// cSC4VertexHtConstraintSatisfier — the height-constraint solver the network / lot tools run when a
// network piece or a lot pins terrain vertices. It is a small arc-consistency solver:
//
//  * every vertex that takes part is put into the "to solve" map with an allowed height window (the
//    terrain min/max), an initial/preferred height and a set of constraint-type bits;
//  * the constraints (fixed value, range, preferred value, preferred range, equality, slope,
//    smoothness) are inserted as they are discovered and shrink those windows;
//  * Solve() first propagates every window (DoInitialConstraintPropagation), then repeatedly picks the
//    vertex with the narrowest window, snaps it to one height (PickASolutionMarkSolution), propagates
//    that value through the constraints still touching it and starts over, until the map is empty.
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (Aspyr macOS x86_64). Original object layout
// (0x100 bytes; the vtable has just the two destructor entries at 0x100a82ad8):
//   +0x00 vptr
//   +0x08 bool initialized           +0x09 bool solveSucceeded (Init sets the word to 0x0101)
//   +0x0c float initialHeightLow     +0x10 float initialHeightHigh  (AddVertexToSolve seeds the range)
//   +0x14 int  failedVertex          (set by PickASolution / PropagateRangeConstraintsOfVerts)
//   +0x18 std::unordered_map<int,float> solvedHeights       (0x28, the solved answers)
//   +0x40 std::unordered_map<int,tToSolveVertInfo> toSolve  (0x28, the live windows)
//   +0x68 std::list<std::set<int>*> equalitySets            (0x18)
//   +0x80 std::list<tSlopeConstraint> slopeConstraints      (0x18)
//   +0x98 std::list<tSmoothnessConstraint> smoothnessConstraints (0x18)
//   +0xb0 std::list<tSmoothConstrMidFixed> midFixedSmoothness    (0x18)
//   +0xc8 std::list<tSmoothConstrEndFixed> endFixedSmoothness    (0x18)
//   +0xe0 std::vector<tSolvedVert> solvedVertList           (0x18, the "potentially solved" marks)
//   +0xf8 iterator/node pointer into solvedHeights          (the Rewind/GetNext cursor)
//
// The container types are the original's (libc++): the code walks the bucket chains of both hash maps
// directly (DoInitialConstraintPropagation and PickASolution start at the first node; Rewind /
// GetNextSolvedVertexAndItsHeight park a pointer in solvedHeights' own node chain), which is why the
// solved-vertex enumeration order is the hash-map iteration order, not insertion order. The port keeps
// the same traversal through std::unordered_map's iterator so the order still comes from libc++.
//
// Known quirks kept on purpose:
//  * Init() writes 0x0101 to +8, so a fresh object already reports "solved" before Solve runs.
//  * InsertRangeConstraint has a second find() of the same key whose "not found" branch creates the
//    entry (and copies 16 bytes of uninitialized stack); the first find() returning non-null makes it
//    unreachable, but the branch is reproduced (with the garbage bytes zeroed, since reading them is UB).
//  * The range flavours of the Update* passes only narrow windows and keep every constraint (and the
//    equality flags); the float flavours, run when a vertex gets one height, apply a constraint once
//    and remove it (the equality set is deleted, the list entry erased).
//  * A failed equality intersection returns false while leaving the equality set and the partially
//    narrowed windows in place; Solve's cleanup then removes them.
//  * InsertSlopeConstraint/InsertSmoothnessConstraint mark only the vertices whose find() succeeded
//    before bailing out on a missing one.
//  * The smallest-window scan in PickASolution stops as soon as it finds a width of exactly 0, so a
//    later (negative) window is not reported; a NaN width never becomes the best one.
//  * DumpConstraints formats the equality sets into a local cRZString and throws the text away: the
//    original's print sink was compiled out, only its two terrain GetVertex calls per vertex remain.
#pragma once
#include <cstdint>
#include <list>
#include <set>
#include <unordered_map>
#include <vector>

namespace sc4 {

// One allowed height window (a std::pair<float,float> in the original). Intersect() is a separate
// symbol: tHtRange::Intersect(tHtRange const&, bool&) @ 0x1001bda76 (93 bytes).
struct tHtRange {
    float low = 0.0f;   // +0x00
    float high = 0.0f;  // +0x04
    // Shrinks this window to the intersection with `other`, reporting whether either bound moved.
    // Returns false (leaving both windows as they are) when the intersection is empty by more than
    // 0.01; an empty gap of at most 0.01 is collapsed onto the new low bound.
    bool Intersect(const tHtRange& other, bool& changed);
};
static_assert(sizeof(tHtRange) == 8, "tHtRange is a pair of floats");

// SL::spTerrain slot +0x50 = cSTETerrain::GetVertex(int index, int& x, int& z) (vtable 0x100aea6c0,
// +0x50 -> 0x100740e5e). The only engine global the recovered code reaches: DumpConstraints asks the
// terrain for a vertex id's grid coordinates while building its (discarded) debug text. cSTETerrain is
// recovered (src/sc4/cSTETerrain.h), so the real adapter is one call to cSTETerrain::GetVertex.
class IVertexHtConstraintHost {
public:
    virtual ~IVertexHtConstraintHost() = default;
    virtual void GetVertex(std::int32_t index, std::int32_t& x, std::int32_t& z) = 0;
};

// Installs the host used by DumpConstraints (the original read SL::spTerrain directly). Null means
// "no terrain": the dump then skips the coordinate formatting, as there is nothing observable to log.
void SetVertexHtConstraintHost(IVertexHtConstraintHost* host);

// The height-constraint solver. One instance per solving pass: Init -> insert constraints -> Solve ->
// GetHeight (or Rewind/GetNext for the solved list) -> Shutdown.
class cSC4VertexHtConstraintSatisfier {
public:
    // The mapped type of the to-solve map (0x1c bytes in the original, offsets shown; the value lives
    // at hash-node +0x14, so node +0x10 is the key). Created by AddVertexToSolve/operator[] zeroed.
    struct tToSolveVertInfo {
        float low = 0.0f;            // +0x00 window low
        float high = 0.0f;           // +0x04 window high
        float preferred = 0.0f;      // +0x08 preferred value      (InsertPreferredValueConstraint)
        float preferredLow = 0.0f;   // +0x0c preferred range low  (InsertPreferredRangeConstraint)
        float preferredHigh = 0.0f;  // +0x10 preferred range high
        float height = 0.0f;         // +0x14 the initial / hinted height (only AddVertexToSolve sets it)
        std::uint32_t flags = 0;     // +0x18 kFlag*
    };
    static_assert(sizeof(tToSolveVertInfo) == 0x1c, "tToSolveVertInfo strides");
    // A "potentially solved" mark: vector element at +0xe0, 8 bytes.
    struct tSolvedVert {
        std::int32_t vertex = 0;  // +0x00
        float height = 0.0f;      // +0x04
    };
    static_assert(sizeof(tSolvedVert) == 8, "tSolvedVert stride");
    // std::list node payloads: the node is {next, prev, value}, so the value starts at node +0x10.
    struct tSlopeConstraint {          // +0x80 list; 0x0c-byte payload (0x20-byte list node)
        std::int32_t vertexA = 0;      // +0x00
        std::int32_t vertexB = 0;      // +0x04
        float maxSlope = 0.0f;         // +0x08 |hA - hB| <= maxSlope
    };
    struct tSmoothnessConstraint {     // +0x98 list; 0x10-byte payload
        std::int32_t vertexA = 0;      // +0x00 end
        std::int32_t vertexB = 0;      // +0x04 middle
        std::int32_t vertexC = 0;      // +0x08 end
        float maxSecondDifference = 0.0f;  // +0x0c |hA - 2*hB + hC| <= this
    };
    struct tSmoothConstrMidFixed {     // +0xb0 list; 0x10-byte payload
        std::int32_t vertexA = 0;      // +0x00 end
        std::int32_t vertexC = 0;      // +0x04 end
        float low = 0.0f;              // +0x08 allowed hA + hC low
        float high = 0.0f;             // +0x0c allowed hA + hC high
    };
    struct tSmoothConstrEndFixed {     // +0xc8 list; 0x10-byte payload
        std::int32_t vertexEnd = 0;    // +0x00 the end that is not solved yet
        std::int32_t vertexMid = 0;    // +0x04 the middle vertex
        float low = 0.0f;              // +0x08 -limit - (solved end height / range high)
        float high = 0.0f;             // +0x0c +limit - (solved end height / range low)
    };
    static_assert(sizeof(tSlopeConstraint) == 0x0c, "slope payload is three 4-byte fields");
    static_assert(sizeof(tSmoothnessConstraint) == 0x10, "constraint payloads are 16 bytes");
    static_assert(sizeof(tSmoothConstrMidFixed) == 0x10, "constraint payloads are 16 bytes");
    static_assert(sizeof(tSmoothConstrEndFixed) == 0x10, "constraint payloads are 16 bytes");

    enum : std::uint32_t {
        kFlagRange = 0x01,           // +0x18 bit: the window came from InsertRangeConstraint
        kFlagPreferredValue = 0x02,  //     a preferred value was set
        kFlagPreferredRange = 0x04,  //     a preferred range was set
        kFlagEquality = 0x08,        //     the vertex is in an equality set
        kFlagSlope = 0x10,           //     the vertex is in a slope constraint
        kFlagSmoothness = 0x20,      //     the vertex is in a smoothness constraint
        kFlagSolved = 0x40,          //     already marked potentially solved
    };

    cSC4VertexHtConstraintSatisfier();   // 0x1001bca8c
    virtual ~cSC4VertexHtConstraintSatisfier();  // 0x1001bcb6a (D1); 0x1001bcca4/0x1001bccae emitted
    // The solved-vertex cursor points into solvedHeights_, so a copy would alias the source's map.
    cSC4VertexHtConstraintSatisfier(const cSC4VertexHtConstraintSatisfier&) = delete;
    cSC4VertexHtConstraintSatisfier& operator=(const cSC4VertexHtConstraintSatisfier&) = delete;

    // Starts a pass. False (and no state change) when already initialized. The two heights seed every
    // vertex's initial window (AddVertexToSolve copies +0x0c/+0x10 into low/high).
    bool Init(float initialHeightLow, float initialHeightHigh);  // 0x1001bd156
    // Ends a pass: drops the live windows and constraint lists, clears the solved heights. Returns 1.
    bool Shutdown();                                             // 0x1001bcc74
    bool IsInitialized() const { return initialized_; }
    bool SolveSucceeded() const { return solveSucceeded_; }
    // The vertex that made the last propagation/solve fail, or 0 when none was recorded (the field is
    // not initialized by the constructor).
    std::int32_t FailedVertex() const { return failedVertex_; }

    // ---- live windows ---------------------------------------------------------------------------
    // 0x1001bc9c2: adds the vertex with an initial height; a duplicate insert is ignored.
    void AddVertexToSolve(std::int32_t vertexId, float height);
    // 0x1001bd6b2: narrows the vertex's window to [low, high]; ignored for unknown vertices. A window
    // that ends up reversed by at most 0.01 is collapsed onto its low bound.
    void InsertRangeConstraint(std::int32_t vertexId, float low, float high);
    // 0x1001bd78a / 0x1001bd7c2: hints applied by PickASolution (unknown vertices are ignored).
    void InsertPreferredValueConstraint(std::int32_t vertexId, float value);
    void InsertPreferredRangeConstraint(std::int32_t vertexId, float low, float high);
    // 0x1001bd67c: AddVertexToSolve + InsertRangeConstraint(vertexId, h, h).
    void InsertFixedVertex(std::int32_t vertexId, float height);
    // i386 0x0067b564 (IsInToSolveSet(int) const): whether the vertex still has a live window.
    bool IsInToSolveSet(std::int32_t vertexId) const;

    // ---- constraints ----------------------------------------------------------------------------
    // 0x1001bd178: joins the two vertices' equality sets (creating/merging as needed).
    void InsertEqualityConstraint(std::int32_t vertexA, std::int32_t vertexB);
    // 0x1001bd80a: |hA - hB| <= maxSlope; a repeated pair keeps the smaller slope, A == B is ignored.
    void InsertSlopeConstraint(std::int32_t vertexA, std::int32_t vertexB, float maxSlope);
    // 0x1001bd8fc: |hA - 2*hB + hC| <= limit; repeated triples (either end order) keep the smaller
    // limit; any two equal vertex ids are ignored.
    void InsertSmoothnessConstraint(std::int32_t vertexA, std::int32_t vertexB, std::int32_t vertexC,
                                    float maxSecondDifference);

    // ---- solving --------------------------------------------------------------------------------
    // 0x1001bdebe: propagates, then picks/solves vertices until the window map is empty. Returns
    // solveSucceeded_; on failure it dumps the constraints (debug, see DumpConstraints).
    bool Solve();
    // 0x1001bd0ea: the solved height of a vertex, false when the vertex never got solved.
    bool GetHeight(std::int32_t vertexId, float& height) const;
    // 0x1001bdeac / 0x1001bd11c: walks the solved-height map in its bucket-chain order.
    void RewindSolvedVertexList();
    bool GetNextSolvedVertexAndItsHeight(std::int32_t& vertexId, float& height);

    // ---- debug ----------------------------------------------------------------------------------
    // 0x1001be05e: formats the equality sets' vertex coordinates (cRZString::Sprintf " %d,%d") and
    // discards the text; needs IVertexHtConstraintHost for the two GetVertex calls per vertex.
    void DumpConstraints();

private:
    // 0x1001bccdc: clears the to-solve map, deletes every equality set and clears every list/vector.
    void DeleteConstraintsAndOtherTemporaryData();
    // 0x1001bcdac / 0x1001bcece: the initial worklist and its ping-pong propagation.
    bool DoInitialConstraintPropagation();
    bool PropagateRangeConstraintsOfVerts(std::set<int>& vertices);

    // 0x1001bd09a: the equality set containing the vertex (unowned pointer) or null.
    std::set<int>* FindEqualitySet(std::int32_t vertexId) const;
    // 0x1001bd4be: inserts every element of `source` into `target`.
    void MergeIntoSet(std::set<int>& target, const std::set<int>& source);

    // 0x1001bc840 / 0x1001bc8fc: merge-or-append into the two "one end is fixed" lists. They return
    // false when the merged range is empty (gap > 0.01), true otherwise.
    bool AddEndFixedSmoothnessConstraint(const tSmoothConstrEndFixed& constraint);
    bool AddMidFixedSmoothnessConstraint(const tSmoothConstrMidFixed& constraint);
    // 0x1001bdad4 / 0x1001bdb1e: mark the vertex potentially solved / record its final height.
    bool MarkPotentiallySolved(std::int32_t vertexId, float height);
    void MarkSolution(std::int32_t vertexId, float height);
    // 0x1001bdbd8: pops the next candidate solution. `propagate` is set when the window was a range
    // (not already a single value), i.e. the caller must propagate it.
    bool PickASolution(std::int32_t& vertexId, float& height, bool& propagate);
    // 0x1001bda28: intersects a vertex's window; false when the intersection is empty.
    bool IntersectVertexRange(std::int32_t vertexId, const tHtRange& range, bool& changed);

    // 0x1001bdd50 / 0x1001bde06: dispatch on the constraint bits; false as soon as one fails.
    bool UpdateConstraints(std::int32_t vertexId, float height, std::uint32_t flags,
                           std::set<int>& nextVertices);
    bool UpdateConstraints(std::int32_t vertexId, const tHtRange& range, std::uint32_t flags,
                           std::set<int>& nextVertices);
    // The seven constraint kinds (float and range flavours); addresses in the definitions.
    bool UpdateEqualityConstraints(std::int32_t vertexId, float height, std::set<int>& nextVertices);
    bool UpdateEqualityConstraints(std::int32_t vertexId, const tHtRange& range,
                                   std::set<int>& nextVertices);
    bool UpdateSlopeConstraints(std::int32_t vertexId, float height, std::set<int>& nextVertices);
    bool UpdateSlopeConstraints(std::int32_t vertexId, const tHtRange& range,
                                std::set<int>& nextVertices);
    bool UpdateSmoothnessConstraints(std::int32_t vertexId, float height);
    bool UpdateSmoothnessConstraints(std::int32_t vertexId, const tHtRange& range);
    bool UpdateMidFixedSmoothnessConstraints(std::int32_t vertexId, float height,
                                             std::set<int>& nextVertices);
    bool UpdateMidFixedSmoothnessConstraints(std::int32_t vertexId, const tHtRange& range,
                                             std::set<int>& nextVertices);
    bool UpdateEndFixedSmoothnessConstraints(std::int32_t vertexId, float height,
                                             std::set<int>& nextVertices);
    bool UpdateEndFixedSmoothnessConstraints(std::int32_t vertexId, const tHtRange& range,
                                             std::set<int>& nextVertices);

    // Deletes the sets the lists own (Shutdown/DeleteConstraints already do this; the destructor also
    // uses it for the "never initialized" path so a misused instance cannot leak).
    void DeleteEqualitySets();

    bool initialized_ = false;            // +0x08
    bool solveSucceeded_ = false;         // +0x09
    float initialHeightLow_ = 0.0f;       // +0x0c
    float initialHeightHigh_ = 0.0f;      // +0x10
    std::int32_t failedVertex_ = 0;       // +0x14 (uninitialized in the original constructor)
    std::unordered_map<std::int32_t, float> solvedHeights_;             // +0x18
    std::unordered_map<std::int32_t, tToSolveVertInfo> toSolve_;        // +0x40
    std::list<std::set<int>*> equalitySets_;                            // +0x68
    std::list<tSlopeConstraint> slopeConstraints_;                      // +0x80
    std::list<tSmoothnessConstraint> smoothnessConstraints_;            // +0x98
    std::list<tSmoothConstrMidFixed> midFixedSmoothnessConstraints_;    // +0xb0
    std::list<tSmoothConstrEndFixed> endFixedSmoothnessConstraints_;    // +0xc8
    std::vector<tSolvedVert> solvedVertList_;                           // +0xe0
    // +0xf8: the original's node pointer into solvedHeights_' bucket chain. An iterator compares and
    // advances exactly like that pointer on libc++ (begin() is the first node, end() is null).
    std::unordered_map<std::int32_t, float>::const_iterator solvedCursor_ = solvedHeights_.end();
};

}  // namespace sc4
