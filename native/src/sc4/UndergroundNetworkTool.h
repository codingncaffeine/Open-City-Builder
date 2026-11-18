// cSC4UndergroundNetworkTool — the buried-network drawing tool (water pipes and subway tubes). 37 symbols at
// 0x100192f96..0x1001a9acc, vtable 0x100a81070 (83 slots + the two pure-virtual slots 0x298 / 0x2a0). It is
// the cSC4NetworkTool of a buried network: it teaches the drag solver the depth of the pieces
// (GetNetworkHeightForTerrainHeight), inserts the vertical constraints an underground pipe needs
// (InsertRangeConstraintsForCell / InsertNetworkSpecificConstraintsForCell), builds the cSC4UndergroundOccupant
// of each solved tile (CreateNewUndergroundOccupant), propagates the solved cells through a drag
// (PropagateFromCell) and keeps the preview/highlight occupants of the cells the cursor touches
// (AddCellPreview / ClearCellPreviews).
//
// The class itself is abstract: slots 0x298 and 0x2a0 are null in its vtable. The two concrete tools fill them
// and are NOT recovered yet:
//   cSC4PipeTool   (vtable 0x100a7b050): NetworkGZIID 0x1001937ca = 0x49c05b9f,
//                                        CreateNetworkOccupant 0x1001930c0
//   cSC4SubwayTool (vtable 0x100a7ac80): NetworkGZIID 0x1001930ae = 0xca16374f,
//                                        CreateNetworkOccupant 0x100192b10
// (both create a cSC4PipeOccupant / cSC4SubwayOccupant, which are not recovered either). A test or the app
// supplies a concrete subclass; scripts/tests/sc4_undergroundnetworktool_test.cpp does.
//
// Object layout (0x690 bytes; base cSC4NetworkTool is 0x638):
//   +0x000 cSC4NetworkTool
//   +0x638 bool inited          (Init/Shutdown guard; Init sets it *before* the base Init runs)
//   +0x63c float networkDepth   (property 0xa96e5641; every terrain height the tool uses is reduced by it)
//   +0x640 float tuningA        (property 0x496e564c; only the partial Update... reads it, as a clearance)
//   +0x644 bool  tuningFlag     (property 0x496e5a36; handed to cSC4UndergroundOccupant::SetTuningParams)
//   +0x648 float tuningB        (property 0x096e5653; the other half of that clearance)
//   +0x660 std::list<cSC4UndergroundOccupant*> previewList_    (created by AddCellPreview)
//   +0x678 std::list<cSC4UndergroundOccupant*> highlightList_  (cells that already had an occupant)
//
// The four tuning values come from the network placement exemplar (type 0x6534284a, group 0x084344e0, instance
// = kNetworkPlacementExemplarIDs[type], property 0xa52160f5) read by ReloadNetworkSpecificInfo (0x1001a7c7e),
// which the base Init calls through the virtual slot +0x288.
//
// Quirks kept on purpose (addresses are the original functions):
//  * the destructor slots are `ud2` traps (0x1001a9ab2 / 0x1001a9ab8): the tool is never destroyed. The port's
//    destructor aborts the same way; tests must not delete a tool.
//  * GetNetworkHeightForTerrainHeight subtracts the depth even for a terrain height below it (0x1001930a0).
//  * InsertRangeConstraintsForCell bypasses the underground constraints for a buildable cell and calls the base
//    implementation instead (0x1001a7643 tail call).
//  * InsertNetworkSpecificConstraintsForCell adds 100000 to every vertex id before the equality constraints when
//    the cell is buildable, and skips both when the sum looks negative as an int (0x1001a7898).
//  * SetQuadTrianglesToDraw clears the three hide bits, then sets bit 0 for hidden triangle 0 but bits 1 and 2
//    for the *visible* triangles (opposite polarity, 0x1001a8500).
//  * CreateNewUndergroundOccupant sets network flag 0x80000000 on a fresh occupant but clears it on one it found
//    again, and only the byte at +0x5c of the cell record keeps the occupant manager handle (0x1001a7362).
//  * AddCellPreview adds the occupant to the preview list when it was just created and to the highlight list when
//    the cell already had one (0x1001a7e56); the occupant manager only sees the created ones.
//  * PropagateFromCell only stops at a straight piece after the fourth level (0x1001a913c); earlier levels always
//    recurse, so a short drag can run through a straight tile of any length.
//  * CreateCellInfosForCurrentPlacementTiles subtracts placement +0x40 for a new cell but placement +0x14 for an
//    existing one (0x1001a7a0d / 0x1001a7b6b), and makes the preferred range of an existing cell a point at
//    min(vertex height, terrain height) (0x1001a7b89).
//  * UpdateOccupantHighlights is the identity function and DestroyHilightedOccupants / ClearOccupantHighlights /
//    UpdateTerrainHeights / CheckForReservedTiles / ApplyPlaneConstraintToRegularCells are empty (the linker
//    folded them onto trivial bodies: 0x1001a9ac4, 0x1001a9acc, 0x100192f96, 0x1001937c0, 0x1001a71e0,
//    0x1001a78e8).
//
// Seams (see docs/SEAMS.md): the terrain (height of a vertex / of a cell), the occupant manager (cell query,
// add and remove), the resource manager (the tuning property holder), the world cache (already a seam of
// cSC4NetworkTool, extended with the two entry points this tool calls) and the vertical constraint satisfier
// (same, extended with the seven calls this tool makes). The base-class InsertRangeConstraintsForCell
// (0x1001b37e4) is not recovered and stays a seam; InsertOccupantsForSolvedCells and ClearCellPreviews are called on the
// recovered base. Everything the seams stand for is one original call, noted on the method.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/Occupant.h"
#include "sc4/SCProperty.h"
#include "sc4/TileNeighborhood.h"
#include "sc4/UndergroundConnectionOccupant.h"
#include "sc4/UndergroundOccupant.h"

namespace sc4 {

// The two interface ids the drag loop asks the occupants for (0x1001a7d3e / 0x1001a81d6).
constexpr std::uint32_t kGZIID_cISC4UndergroundConnectionOccupant = 0x8a139184;
// kGZIID_cISC4UndergroundOccupant (0xa821ef94), its second id (0xa9764c8c) and the id SetQuadTrianglesToDraw
// queries (0x4a10afc2) live in UndergroundOccupant.h.

// The network tuning exemplar ReloadNetworkSpecificInfo reads (0x1001a7c7e: key {0x6534284a, 0x084344e0,
// exemplarId}, property 0xa52160f5) and the four properties it copies out of it.
constexpr std::uint32_t kUndergroundTuningType = 0x6534284a;
constexpr std::uint32_t kUndergroundTuningGroup = 0x084344e0;
constexpr std::uint32_t kUndergroundTuningProperty = 0xa52160f5;
constexpr std::uint32_t kUndergroundPropNetworkDepth = 0xa96e5641;   // -> +0x63c
constexpr std::uint32_t kUndergroundPropTuningA = 0x496e564c;        // -> +0x640
constexpr std::uint32_t kUndergroundPropTuningB = 0x096e5653;        // -> +0x648
constexpr std::uint32_t kUndergroundPropTuningFlag = 0x496e5a36;     // -> +0x644

// The network flags the tool sets on the occupants it builds (cSC4NetworkOccupant +0x188).
constexpr std::uint32_t kUndergroundToolFlagBuilt = 0x80000000;      // CreateNewUndergroundOccupant
constexpr std::uint32_t kUndergroundToolFlagTranslucent = 0x00004000;   // cleared on insertion
constexpr std::uint32_t kUndergroundToolFlagUsable = 0x10000000;        // set on insertion
constexpr std::uint32_t kUndergroundToolFlagNoPiece = 0x00200000;       // StartPlacing leaves these alone

// kNextX / kNextZ @ 0x100a1a170 / 0x100a1a180 (PropagateFromCell's edge -> neighbour table; note the order,
// which is not the cSC4TileNeighborhoodCompact edge order used elsewhere).
constexpr std::int32_t kUndergroundNextX[4] = {-1, 0, 1, 0};
constexpr std::int32_t kUndergroundNextZ[4] = {0, -1, 0, 1};

// Every engine global / unrecovered neighbour the recovered functions reach. One method per original call,
// with the vtable slot or address in the comment; the test supplies a fake.
class IUndergroundNetworkToolHost {
public:
    virtual ~IUndergroundNetworkToolHost() = default;
    // SL::spTerrain slot +0x68: the terrain height under a cell vertex id (0x1001a7c30, 0x1001a7a0d, ...).
    virtual float TerrainHeight(std::uint32_t vertexId) = 0;
    // SL::spTerrain slot +0x88: the four corner heights of a cell (0x1001a85e2, partial).
    virtual void TerrainCellHeights(int x, int z, float* heights) = 0;
    // SL::spOccupantManager slot +0x148: every occupant covering (x, z). Each element is returned with one
    // reference the caller releases (0x1001a7d3e / 0x1001a81d6).
    virtual void QueryCellOccupants(int x, int z, std::list<cISC4Occupant*>& out) = 0;
    // SL::spOccupantManager slot +0xa0: add the occupant; its result is the cell record's +0x5c handle
    // (0x1001a7362 / 0x1001a7218 / 0x1001a7e56). `param458` is the tool's +0x458 value (1).
    virtual std::uint32_t AddOccupantToManager(cISC4Occupant* occupant, int param458) = 0;
    // SL::spOccupantManager slot +0xa8: remove it again (0x1001a80ec: (occupant, 0, param458)).
    virtual void RemoveOccupantFromManager(cISC4Occupant* occupant, bool keep, int param458) = 0;
    // SL::spRM (0x100b21280) slot +0x18 with key {kUndergroundTuningType, kUndergroundTuningGroup, exemplarId}
    // and property kUndergroundTuningProperty; then vslot +0x30 of the resource gives the property holder the
    // four GetPropertyValue calls read. Returned with one reference, or null; the caller Releases it
    // (0x1001a7c7e).
    virtual cISCPropertyHolder* AcquireNetworkTuningProperties(std::uint32_t exemplarId) = 0;
    // cSC4NetworkTool::InsertRangeConstraintsForCell @ 0x1001b37e4 (not recovered): the base constraint pass a
    // buildable underground cell takes (0x1001a7643).
    virtual void InsertRangeConstraintsForCellBase(std::uint32_t cellKey) = 0;
};
void SetUndergroundNetworkToolHost(IUndergroundNetworkToolHost* host);
IUndergroundNetworkToolHost* GetUndergroundNetworkToolHost();

class cSC4UndergroundNetworkTool : public cSC4NetworkTool {
public:
    explicit cSC4UndergroundNetworkTool(std::uint32_t networkType);   // 0x1001a74c8
    // 0x1001a9ab2 / 0x1001a9ab8 are `push rbp; mov rbp,rsp; ud2`; see the quirk note above.
    ~cSC4UndergroundNetworkTool() override;                           // traps

    // --- cISC4NetworkTool overrides (original slot in the comment) ---
    bool Init() override;                                              // [3]  0x1001a754c
    bool Shutdown() override;                                          // [4]  0x1001a756a
    void ClearCellPreviews();                                          // [16] 0x1001a80ec
    void ClearOccupantHighlights();                                    // [17] 0x100192f96 (empty)
    bool StopPickAtWater() override;                                   // [46] 0x100192f9e (false)
    void CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4NetworkOccupant** out, float (*heights)[3],
                               std::uint32_t networkType);             // [56] 0x1001a71e6
    virtual void InsertOccupantsForSolvedCells(std::vector<tSolvedCell>& cells,
                                               std::list<cSC4NetworkOccupant*>* out, bool preview);  // [57]
    bool CanTilesBeSupportedOnTerrain(std::uint32_t networkType);      // [58] 0x1001937b8 (true)
    void UpdateTerrainHeights(std::vector<std::uint32_t>& heights);    // [59] 0x1001937c0 (empty)
    virtual void AddCellPreview(cSC4NetworkCellInfo& cell, const tSolvedCell& solved);   // [60]
    int UpdateOccupantHighlights(int value);                           // [61] identity
    void DestroyHilightedOccupants();                                  // [62] empty
    bool AllowBridges() override;                                      // [63] 0x100193078 (false)
    bool AllowTunnels() override;                                      // [64] 0x100193080 (false)
    bool IsStoppedByWater() override;                                  // [65] 0x100193088 (false)
    bool ShouldAutoConnect() override;                                 // [66] 0x100193090 (true)
    bool TestForAutoConnectOccupant(int x, int z);                     // [67] 0x1001a93a6
    void CheckForReservedTiles(int& cell, std::uint32_t networkType, bool flag);   // [69] empty
    bool CheckForGoodTerrain(int& failureReason);                      // [71] 0x100193098 (true)
    void StartPlacing() override;                                      // [72] 0x1001a8310
    void CreateCellInfosForCurrentPlacementTiles(std::uint32_t networkType);  // [73] 0x1001a78ee
    virtual float GetNetworkVertexHt(int vertexId) const;              // [75] 0x1001a7c30
    // The same slot under the base port's name (i386 vslot 0x12c): the base insertion reads the pipe heights through it.
    float GetNetworkVertexHeight(std::int32_t vertex) override { return GetNetworkVertexHt(vertex); }
    virtual float GetNetworkHeightForTerrainHeight(float terrainHeight) const;   // [76] 0x1001930a0
    void PropagateSolvedCellsForSmoothing() override;                  // [77] 0x1001a932a
    void InsertRangeConstraintsForCell(std::uint32_t cellKey);         // [78] 0x1001a7616
    void ApplyPlaneConstraintToRegularCells(std::vector<tImmovablePortion>& portions);   // [79] empty
    void InsertNetworkSpecificConstraintsForCell(std::uint32_t cellKey);   // [80] 0x1001a779a
    void ReloadNetworkSpecificInfo(std::uint32_t exemplarId) override;  // [81] 0x1001a7c7e
    void SetQuadTrianglesToDraw(cSC4NetworkOccupant* occupant, const cSC4TileNeighborhoodCompact& compact);  // [82]
    // The port's base slot takes the packed neighbourhood; the base insertion reaches the method above through it.
    void SetQuadTrianglesToDraw(cSC4NetworkOccupant* occupant, std::uint32_t compact) override {
        SetQuadTrianglesToDraw(occupant, cSC4TileNeighborhoodCompact(compact));
    }

    // --- the rest of the class (not interface slots) ---
    bool CreateNewUndergroundOccupant(cSC4NetworkCellInfo* cell, const tSolvedCell& solved,
                                      cSC4UndergroundOccupant*& out, bool& existing);   // 0x1001a7362
    bool FindUndergroundOccupant(int x, int z, cSC4UndergroundOccupant*& out, std::uint32_t gziid);  // 0x1001a7d3e
    // The connection piece of the cell whose network occupant id (i386 vslot 0xc8 GetNetworkOccupantGZIID) is `gziid`,
    // one reference.
    bool FindUndergroundConnectionOccupant(int x, int z, std::uint32_t gziid,
                                           cSC4UndergroundConnectionOccupant*& out);   // 0x1001a81d6
    void PropagateFromCell(cSC4NetworkCellInfo* cell, int depth);      // 0x1001a913c
    // PARTIAL: only the three clears are ported. See the .cpp for the reason.
    void UpdateUndergroundOccupantTerrainHeights(std::vector<SC4PointU> cells,
                                                 std::list<cSC4NetworkOccupant*>& occupants,
                                                 std::uint32_t networkType);   // 0x1001a85e2

    // --- the two pure virtuals of the original (slots 0x298 / 0x2a0) ---
    // The network gziid of the pieces this tool builds (0x1001930ae / 0x1001937ca).
    virtual std::uint32_t NetworkGZIID() = 0;
    // Build the concrete occupant of a cell (0x100192b10 / 0x1001930c0); it also fills the 4x3 corner array.
    virtual void CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4UndergroundOccupant*& out,
                                       float (*heights)[3], std::uint32_t networkType) = 0;

    // port wiring the tests read
    bool Inited() const { return inited_; }                          // +0x638
    float NetworkDepth() const { return networkDepth_; }             // +0x63c
    float TuningA() const { return tuningA_; }                       // +0x640
    bool TuningFlag() const { return tuningFlag_; }                  // +0x644
    float TuningB() const { return tuningB_; }                       // +0x648
    const std::list<cSC4UndergroundOccupant*>& PreviewList() const { return previewList_; }      // +0x660
    const std::list<cSC4UndergroundOccupant*>& HighlightList() const { return highlightList_; }  // +0x678

private:
    bool inited_ = false;                                   // +0x638
    float networkDepth_ = 0.0f;                             // +0x63c
    float tuningA_ = 0.0f;                                  // +0x640
    bool tuningFlag_ = false;                               // +0x644
    std::uint8_t unknown645_[3] = {0, 0, 0};                // +0x645
    float tuningB_ = 0.0f;                                  // +0x648
    std::uint8_t unknown64c_[0x14] = {};                    // +0x64c
    std::list<cSC4UndergroundOccupant*> previewList_;       // +0x660
    std::list<cSC4UndergroundOccupant*> highlightList_;     // +0x678

    // scripts/tests/sc4_undergroundnetworktool_test.cpp reaches the private state (the base friend access is
    // declared in NetworkTool.h).
    friend struct UndergroundNetworkToolTestAccess;
};

// IsStraight @ 0x1001a92df (a file-local helper in the original): true for the six compact neighbourhoods that
// have exactly two connections forming a corner or a straight line.
bool IsUndergroundStraightNeighbourhood(const cSC4NetworkCellInfo& cell, std::uint32_t networkType);

}  // namespace sc4
