// cSC4VertexHtConstraintSatisfier — recovered from Sim City 4 Deluxe Editionsub 1.2.1.
// Exercises the solver end to end: the Init/Shutdown handshake, every constraint insert and its
// quirks (duplicate/minimum handling, ignored unknown vertices, the 0.01 range fold), the arithmetic
// of every constraint projection (slope, smoothness through either end and through the middle,
// preferred value/range clamping), the equality-set merge/application, the solved-vertex walk, the
// failure paths and the debug dump's terrain queries.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sc4/VertexHtConstraintSatisfier.h"

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);              \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace sc4;

namespace {

// Records the SL::spTerrain slot +0x50 calls DumpConstraints makes.
class RecordingTerrainHost final : public IVertexHtConstraintHost {
public:
    int calls = 0;
    std::vector<int> order;
    std::map<int, std::pair<int, int>> positions;

    void GetVertex(std::int32_t index, std::int32_t& x, std::int32_t& z) override {
        ++calls;
        order.push_back(index);
        const auto it = positions.find(index);
        if (it != positions.end()) {
            x = it->second.first;
            z = it->second.second;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// tHtRange::Intersect
// ---------------------------------------------------------------------------------------------
void TestHtRangeIntersect() {
    {
        tHtRange w{0.0f, 10.0f};
        bool changed = false;
        CHECK(w.Intersect(tHtRange{2.0f, 8.0f}, changed));
        CHECK(changed);
        CHECK(w.low == 2.0f && w.high == 8.0f);
    }
    {
        tHtRange w{0.0f, 10.0f};
        bool changed = true;
        CHECK(w.Intersect(tHtRange{0.0f, 10.0f}, changed));
        CHECK(!changed);  // equal bounds do not count as a move (the original's != test)
        CHECK(w.low == 0.0f && w.high == 10.0f);
    }
    {
        tHtRange w{0.0f, 10.0f};
        bool changed = false;
        CHECK(w.Intersect(tHtRange{10.0f, 10.0f}, changed));
        CHECK(w.low == 10.0f && w.high == 10.0f);
    }
    {
        // Empty by more than the tolerance: false, but both bounds were already overwritten.
        tHtRange w{0.0f, 10.0f};
        bool changed = false;
        CHECK(!w.Intersect(tHtRange{11.0f, 12.0f}, changed));
        CHECK(changed);
        CHECK(w.low == 11.0f && w.high == 10.0f);
    }
    {
        // Empty by at most 0.01: collapsed onto the new low bound.
        tHtRange w{0.0f, 10.0f};
        bool changed = false;
        CHECK(w.Intersect(tHtRange{10.005f, 20.0f}, changed));
        CHECK(w.low == 10.005f && w.high == 10.005f);
    }
    {
        // NaN bounds behave like UCOMISS: the NaN low moves nothing, but the (ordered) high still
        // narrows the window.
        tHtRange w{0.0f, 10.0f};
        bool changed = false;
        const float nan = std::nanf("");
        CHECK(w.Intersect(tHtRange{nan, 5.0f}, changed));
        CHECK(changed);
        CHECK(w.low == 0.0f && w.high == 5.0f);
    }
    {
        // Both NaN: neither bound moves (unordered comparisons are not taken).
        tHtRange w{0.0f, 10.0f};
        bool changed = true;
        const float nan = std::nanf("");
        CHECK(w.Intersect(tHtRange{nan, nan}, changed));
        CHECK(!changed);
        CHECK(w.low == 0.0f && w.high == 10.0f);
    }
}

// ---------------------------------------------------------------------------------------------
// Init / Shutdown / solved heights
// ---------------------------------------------------------------------------------------------
void TestLifecycle() {
    SetVertexHtConstraintHost(nullptr);
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(!solver.IsInitialized());
    CHECK(solver.Init(0.0f, 100.0f));
    CHECK(solver.IsInitialized());
    // Quirk: Init writes 0x0101, so a fresh solver already reports "solved".
    CHECK(solver.SolveSucceeded());
    CHECK(!solver.Init(0.0f, 100.0f));  // double Init is refused without touching the state

    float height = 0.0f;
    CHECK(!solver.GetHeight(7, height));
    solver.AddVertexToSolve(7, 5.0f);
    CHECK(!solver.GetHeight(7, height));  // live, but not solved yet

    CHECK(solver.Shutdown());
    CHECK(!solver.IsInitialized());
    CHECK(!solver.GetHeight(7, height));
    CHECK(solver.Shutdown());  // second Shutdown is a no-op that still returns true
}

void TestFixedVertexSolve() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.InsertFixedVertex(1, 12.5f);
    CHECK(solver.Solve());
    float height = 0.0f;
    CHECK(solver.GetHeight(1, height));
    CHECK(height == 12.5f);  // fixed vertices are reported bit-exactly
    CHECK(solver.FailedVertex() == 0);
    // A second pass keeps the first answer and solves the new vertex too.
    solver.InsertFixedVertex(2, 87.25f);
    CHECK(solver.Solve());
    CHECK(solver.GetHeight(1, height));
    CHECK(height == 12.5f);
    CHECK(solver.GetHeight(2, height));
    CHECK(height == 87.25f);
    CHECK(solver.Shutdown());
    CHECK(!solver.GetHeight(1, height));  // Shutdown drops the solved map
}

// The initial window comes from Init: a vertex hinted above it is clamped into it.
void TestInitialWindowClampsHint() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(3.0f, 7.0f));
    solver.AddVertexToSolve(1, 100.0f);
    CHECK(solver.Solve());
    float height = 0.0f;
    CHECK(solver.GetHeight(1, height));
    CHECK(height == 7.0f);
}

// ---------------------------------------------------------------------------------------------
// Slope constraint: a fixed vertex tightens its neighbour's window; the hint is then clamped.
// ---------------------------------------------------------------------------------------------
void TestSlopeProjection() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 5.0f);
    solver.AddVertexToSolve(2, 5.0f);
    solver.InsertFixedVertex(1, 10.0f);
    solver.InsertSlopeConstraint(1, 2, 3.0f);
    CHECK(solver.Solve());
    float height = 0.0f;
    CHECK(solver.GetHeight(1, height));
    CHECK(height == 10.0f);
    CHECK(solver.GetHeight(2, height));
    CHECK(height == 7.0f);  // clamp(5, 10 - 3, 10 + 3)
}

// A repeated slope constraint keeps the smaller limit; an A == B insert is ignored.
void TestSlopeDuplicateKeepsMinimum() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 0.0f);
    solver.InsertFixedVertex(1, 10.0f);
    solver.InsertSlopeConstraint(1, 2, 8.0f);
    solver.InsertSlopeConstraint(2, 1, 2.0f);  // reversed pair narrows the stored slope to 2
    solver.InsertSlopeConstraint(1, 2, 9.0f);  // does not widen it again
    solver.InsertSlopeConstraint(2, 2, 0.0f);  // A == B: ignored
    CHECK(solver.Solve());
    float height = 0.0f;
    CHECK(solver.GetHeight(2, height));
    CHECK(height == 8.0f);  // clamp(0, 10 - 2, 10 + 2)
}

// The range flavour of the slope projection: a solved *window* [0,10] widens its neighbour's allowed
// window to [0 - slope, 10 + slope]; the neighbour then clamps its hint into it.
void TestSlopeRangeProjection() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(-100.0f, 100.0f));
    solver.AddVertexToSolve(2, 0.0f);
    solver.AddVertexToSolve(3, -50.0f);
    solver.InsertRangeConstraint(2, 0.0f, 10.0f);
    solver.InsertSlopeConstraint(2, 3, 3.0f);
    CHECK(solver.Solve());
    float h2 = 0.0f, h3 = 0.0f;
    CHECK(solver.GetHeight(2, h2));
    CHECK(solver.GetHeight(3, h3));
    CHECK(h2 == 0.0f);
    CHECK(h3 == -3.0f);  // clamp(-50, -100, 100 intersected with [0 - 3, 10 + 3])
}

// The mid-fixed smoothness projection through two point windows: with both ends fixed (0 and 5), the
// third vertex's window becomes [2*5 - 0 - 1, 2*5 - 0 + 1] and its hint clamps onto the low bound.
void TestSmoothnessMidFixedThroughPoints() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(-100.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);   // middle (smallest id => processed first)
    solver.AddVertexToSolve(2, 0.0f);   // end A
    solver.AddVertexToSolve(3, -50.0f); // end C, hinted far below
    solver.InsertFixedVertex(1, 5.0f);
    solver.InsertFixedVertex(2, 0.0f);
    solver.InsertSmoothnessConstraint(2, 1, 3, 1.0f);
    CHECK(solver.Solve());
    float h1 = 0.0f, h2 = 0.0f, h3 = 0.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(solver.GetHeight(3, h3));
    CHECK(h1 == 5.0f && h2 == 0.0f);
    CHECK(h3 == 9.0f);  // 2*5 - 0 - 1
    CHECK(h2 - 2.0f * h1 + h3 == -1.0f);
}

// ---------------------------------------------------------------------------------------------
// Preferred value / preferred range
// ---------------------------------------------------------------------------------------------
void TestPreferredValue() {
    {
        cSC4VertexHtConstraintSatisfier solver;
        CHECK(solver.Init(0.0f, 100.0f));
        solver.AddVertexToSolve(1, 5.0f);
        solver.InsertPreferredValueConstraint(1, 42.0f);
        CHECK(solver.Solve());
        float height = 0.0f;
        CHECK(solver.GetHeight(1, height));
        CHECK(height == 42.0f);
    }
    {
        // Outside the window: the preferred value is ignored and the hint is clamped instead.
        cSC4VertexHtConstraintSatisfier solver;
        CHECK(solver.Init(0.0f, 100.0f));
        solver.AddVertexToSolve(1, 5.0f);
        solver.InsertPreferredValueConstraint(1, 200.0f);
        CHECK(solver.Solve());
        float height = 0.0f;
        CHECK(solver.GetHeight(1, height));
        CHECK(height == 5.0f);
    }
    {
        // On the window bounds the preferred value still wins.
        cSC4VertexHtConstraintSatisfier solver;
        CHECK(solver.Init(0.0f, 100.0f));
        solver.AddVertexToSolve(1, 5.0f);
        solver.InsertPreferredValueConstraint(1, 100.0f);
        CHECK(solver.Solve());
        float height = 0.0f;
        CHECK(solver.GetHeight(1, height));
        CHECK(height == 100.0f);
    }
}

void TestPreferredRange() {
    {
        cSC4VertexHtConstraintSatisfier solver;
        CHECK(solver.Init(0.0f, 100.0f));
        solver.AddVertexToSolve(1, 5.0f);
        solver.InsertPreferredRangeConstraint(1, 40.0f, 60.0f);
        CHECK(solver.Solve());
        float height = 0.0f;
        CHECK(solver.GetHeight(1, height));
        CHECK(height == 40.0f);  // clamp(5, 40, 60)
    }
    {
        // An inverted preferred range by more than 0.01 is ignored.
        cSC4VertexHtConstraintSatisfier solver;
        CHECK(solver.Init(0.0f, 100.0f));
        solver.AddVertexToSolve(1, 5.0f);
        solver.InsertPreferredRangeConstraint(1, 60.0f, 50.0f);
        CHECK(solver.Solve());
        float height = 0.0f;
        CHECK(solver.GetHeight(1, height));
        CHECK(height == 5.0f);
    }
    {
        // Inverted by at most 0.01: the preferred window collapses onto its low bound.
        cSC4VertexHtConstraintSatisfier solver;
        CHECK(solver.Init(0.0f, 100.0f));
        solver.AddVertexToSolve(1, 5.0f);
        solver.InsertPreferredRangeConstraint(1, 50.005f, 50.0f);
        CHECK(solver.Solve());
        float height = 0.0f;
        CHECK(solver.GetHeight(1, height));
        CHECK(height == 50.005f);
    }
}

// ---------------------------------------------------------------------------------------------
// Range insert: intersection, the 0.01 fold and the inverted-window failure
// ---------------------------------------------------------------------------------------------
void TestRangeConstraintFold() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 5.0f);
    solver.InsertRangeConstraint(1, 10.0f, 9.995f);  // inverted by 0.005 -> folds to [10, 10]
    CHECK(solver.Solve());
    float height = 0.0f;
    CHECK(solver.GetHeight(1, height));
    CHECK(height == 10.0f);
}

void TestInvertedRangeFailsSolve() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 5.0f);
    solver.InsertRangeConstraint(1, 10.0f, 8.0f);  // inverted by 2 -> kept, then rejected
    CHECK(!solver.Solve());
    CHECK(!solver.SolveSucceeded());
    float height = 0.0f;
    CHECK(!solver.GetHeight(1, height));
    CHECK(solver.FailedVertex() == 1);
}

// ---------------------------------------------------------------------------------------------
// Equality constraints: transitively merged sets, applied in one propagation pass
// ---------------------------------------------------------------------------------------------
void TestEqualityTransitive() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 5.0f);
    solver.AddVertexToSolve(2, 5.0f);
    solver.AddVertexToSolve(3, 5.0f);
    solver.InsertEqualityConstraint(1, 2);
    solver.InsertEqualityConstraint(2, 3);  // merges {2,3} into {1,2}
    solver.InsertFixedVertex(1, 9.0f);
    CHECK(solver.Solve());
    for (int vertex = 1; vertex <= 3; ++vertex) {
        float height = 0.0f;
        CHECK(solver.GetHeight(vertex, height));
        CHECK(height == 9.0f);
    }
}

// Two free vertices with different hints: the equality set survives the range pass (i386 0x0014b102
// keeps it) and the first solved height is copied to the other (the float flavour, 0x0014b8dc).
void TestEqualityHoldsWithoutFixedVertex() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 10.0f);
    solver.AddVertexToSolve(2, 40.0f);
    solver.InsertEqualityConstraint(1, 2);
    CHECK(solver.Solve());
    float h1 = -1.0f, h2 = -2.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(h1 == h2);
    CHECK(h1 == 10.0f || h1 == 40.0f);
}

// A slope constraint between two free vertices survives the range pass (i386 0x0014ae02) and bounds the
// second vertex once the first one is solved.
void TestSlopeHoldsAfterRangePass() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 50.0f);
    solver.InsertSlopeConstraint(1, 2, 5.0f);
    CHECK(solver.Solve());
    float h1 = -1.0f, h2 = -1.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(std::fabs(h1 - h2) <= 5.0f);
}

void TestEqualityConflictFails() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 0.0f);
    solver.InsertFixedVertex(1, 10.0f);
    solver.InsertFixedVertex(2, 20.0f);
    solver.InsertEqualityConstraint(1, 2);
    CHECK(!solver.Solve());
    CHECK(solver.FailedVertex() == 1);
}

// Equality constraints created for vertices that were never added are kept but never fire (the
// initial worklist is empty), and Solve still reports success with nothing to do.
void TestEqualityForUnknownVertices() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.InsertEqualityConstraint(5, 6);
    CHECK(solver.Solve());
    CHECK(solver.FailedVertex() == 0);
}

// ---------------------------------------------------------------------------------------------
// Smoothness: when the MIDDLE is solved, both ends are reflected: hEnd = 2*hMid - hOtherEnd +- limit.
// ---------------------------------------------------------------------------------------------
// Exact projection through the middle: a fixed at 10 and the middle fixed at 10 give the free end the
// window [2*10 - 10 - 2, 2*10 - 10 + 2] = [8, 12]; its hint (-100) is clamped to 8.
void TestSmoothnessEndFixedThroughMiddle() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 0.0f);
    solver.AddVertexToSolve(3, -100.0f);
    solver.InsertFixedVertex(1, 10.0f);
    solver.InsertFixedVertex(2, 10.0f);
    solver.InsertSmoothnessConstraint(1, 2, 3, 2.0f);
    CHECK(solver.Solve());

    float h1 = 0.0f, h2 = 0.0f, h3 = 0.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(solver.GetHeight(3, h3));
    CHECK(h1 == 10.0f && h2 == 10.0f);
    CHECK(h3 == 8.0f);  // 2*h2 - h1 - limit
}

// The projection through an end that is still a range only narrows the other end ([0,22] here) and
// stays in place (i386 0x0014af82 never erases), so when the first end is solved the second is pulled
// to within the limit: one end keeps its hint 0, the other lands on 2*10 - 0 - 2 = 18.
void TestSmoothnessHoldsAfterRangeProjection() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 0.0f);
    solver.AddVertexToSolve(3, 0.0f);
    solver.InsertFixedVertex(2, 10.0f);
    solver.InsertSmoothnessConstraint(1, 2, 3, 2.0f);
    CHECK(solver.Solve());

    float h1 = 0.0f, h2 = 0.0f, h3 = 0.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(solver.GetHeight(3, h3));
    CHECK(h2 == 10.0f);
    CHECK(std::min(h1, h3) == 0.0f && std::max(h1, h3) == 18.0f);
    CHECK(std::fabs(h1 - 2.0f * h2 + h3) <= 2.0f);
}

// With both smoothness ends fixed (10 and 20) the end-fixed projection survives the middle's range pass
// and narrows the middle to [14, 16] when the second end is reached; the hint 33 is clamped to 16.
void TestSmoothnessFixedEndsBoundTheMiddle() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 33.0f);
    solver.AddVertexToSolve(3, 0.0f);
    solver.InsertFixedVertex(1, 10.0f);
    solver.InsertFixedVertex(3, 20.0f);
    solver.InsertSmoothnessConstraint(1, 2, 3, 2.0f);
    CHECK(solver.Solve());
    float h1 = 0.0f, h2 = 0.0f, h3 = 0.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(solver.GetHeight(3, h3));
    CHECK(h1 == 10.0f && h3 == 20.0f);
    CHECK(h2 == 16.0f);
    CHECK(std::fabs(h1 - 2.0f * h2 + h3) <= 2.0f);
}

// A repeated smoothness constraint keeps the smaller second-difference limit.
void TestSmoothnessDuplicateKeepsMinimum() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 0.0f);
    solver.AddVertexToSolve(3, 100.0f);  // far above the projected window, so the limit is visible
    solver.InsertSmoothnessConstraint(1, 2, 3, 8.0f);
    solver.InsertSmoothnessConstraint(3, 2, 1, 2.0f);  // reversed ends: same triple, narrows to 2
    solver.InsertSmoothnessConstraint(1, 2, 3, 9.0f);  // does not widen it again
    solver.InsertSmoothnessConstraint(1, 1, 3, 0.0f);  // repeated vertex: ignored
    solver.InsertFixedVertex(2, 10.0f);
    CHECK(solver.Solve());
    float h1 = 0.0f, h2 = 0.0f, h3 = 0.0f;
    CHECK(solver.GetHeight(1, h1));
    CHECK(solver.GetHeight(2, h2));
    CHECK(solver.GetHeight(3, h3));
    CHECK(h1 == 0.0f && h2 == 10.0f);
    // Vertex 1's range projects onto vertex 3 as [2*10 - 100 - limit, 2*10 - 0 + limit]; with the
    // kept limit of 2 that is [-82, 22], so the hint 100 lands on 22 (a limit of 8 would give 28).
    CHECK(h3 == 22.0f);
    CHECK(h1 - 2.0f * h2 + h3 == 2.0f);
}

// ---------------------------------------------------------------------------------------------
// Solved-vertex enumeration (RewindSolvedVertexList / GetNextSolvedVertexAndItsHeight)
// ---------------------------------------------------------------------------------------------
void TestSolvedVertexWalk() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.InsertFixedVertex(1, 1.5f);
    solver.InsertFixedVertex(2, 2.5f);
    solver.InsertFixedVertex(3, 3.5f);
    CHECK(solver.Solve());

    auto walk = [&solver]() {
        std::vector<std::pair<int, float>> out;
        solver.RewindSolvedVertexList();
        std::int32_t vertex = 0;
        float height = 0.0f;
        while (solver.GetNextSolvedVertexAndItsHeight(vertex, height)) {
            out.emplace_back(vertex, height);
        }
        return out;
    };
    const auto first = walk();
    CHECK(first.size() == 3);
    // Rewinding replays exactly the same (hash-map bucket chain) order.
    CHECK(first == walk());

    std::set<int> seen;
    for (const auto& entry : first) {
        seen.insert(entry.first);
        CHECK(entry.second == static_cast<float>(entry.first) + 0.5f);
    }
    CHECK(seen == (std::set<int>{1, 2, 3}));
}

// ---------------------------------------------------------------------------------------------
// IsInToSolveSet: true while the vertex has a live window; Solve consumes the windows.
// ---------------------------------------------------------------------------------------------
void TestIsInToSolveSet() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    CHECK(!solver.IsInToSolveSet(7));
    solver.AddVertexToSolve(7, 3.0f);
    CHECK(solver.IsInToSolveSet(7));
    CHECK(!solver.IsInToSolveSet(8));
    solver.InsertFixedVertex(8, 4.0f);
    CHECK(solver.IsInToSolveSet(8));
    CHECK(solver.Solve());
    CHECK(!solver.IsInToSolveSet(7));
    CHECK(!solver.IsInToSolveSet(8));
}

// ---------------------------------------------------------------------------------------------
// DumpConstraints: reached on a failed Solve; two terrain GetVertex calls per equality-set member.
// ---------------------------------------------------------------------------------------------
void TestDumpConstraintsQueriesTerrain() {
    RecordingTerrainHost host;
    host.positions[1] = {11, 12};
    host.positions[2] = {21, 22};
    SetVertexHtConstraintHost(&host);

    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.AddVertexToSolve(1, 0.0f);
    solver.AddVertexToSolve(2, 0.0f);
    solver.InsertFixedVertex(1, 10.0f);
    solver.InsertRangeConstraint(2, 0.0f, 5.0f);
    solver.InsertEqualityConstraint(1, 2);  // leaves {1,2} in the list when the merge fails

    // With the sets still alive the dump asks the terrain for every member's coordinates: twice per
    // vertex (the first call supplies x, the second z, as the original does).
    solver.DumpConstraints();
    CHECK(host.calls == 4);

    std::set<int> queried(host.order.begin(), host.order.end());
    CHECK(queried == (std::set<int>{1, 2}));

    // Quirk: Solve() cleans the constraint lists up BEFORE calling DumpConstraints (0x1001be001 calls
    // DeleteConstraintsAndOtherTemporaryData, 0x1001be015 the dump), so the failed-solve dump always
    // sees empty lists and queries nothing.
    CHECK(!solver.Solve());  // 10 does not fit into 2's [0,5]
    CHECK(host.calls == 4);

    // Without a terrain the dump must be harmless.
    SetVertexHtConstraintHost(nullptr);
    cSC4VertexHtConstraintSatisfier second;
    CHECK(second.Init(0.0f, 100.0f));
    second.AddVertexToSolve(1, 0.0f);
    second.AddVertexToSolve(2, 0.0f);
    second.InsertFixedVertex(1, 10.0f);
    second.InsertRangeConstraint(2, 0.0f, 5.0f);
    second.InsertEqualityConstraint(1, 2);
    second.DumpConstraints();
    CHECK(!second.Solve());
}

// ---------------------------------------------------------------------------------------------
// Inserts for unknown vertices must be ignored (except InsertFixedVertex, which adds one)
// ---------------------------------------------------------------------------------------------
void TestUnknownVertexInserts() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(0.0f, 100.0f));
    solver.InsertRangeConstraint(9, 1.0f, 2.0f);
    solver.InsertPreferredValueConstraint(9, 3.0f);
    solver.InsertPreferredRangeConstraint(9, 4.0f, 5.0f);
    solver.InsertSlopeConstraint(9, 10, 1.0f);
    solver.InsertSmoothnessConstraint(9, 10, 11, 1.0f);
    CHECK(solver.Solve());  // nothing was added, so there is nothing to solve
    float height = 0.0f;
    CHECK(!solver.GetHeight(9, height));

    // InsertFixedVertex is the exception: it adds the vertex first.
    solver.InsertFixedVertex(9, 42.0f);
    CHECK(solver.Solve());
    CHECK(solver.GetHeight(9, height));
    CHECK(height == 42.0f);
}

// Smaller initial hint than the window: the solver keeps the hint when nothing constrains it.
void TestUnconstrainedVertexKeepsHint() {
    cSC4VertexHtConstraintSatisfier solver;
    CHECK(solver.Init(-50.0f, 50.0f));
    solver.AddVertexToSolve(4, 3.75f);
    CHECK(solver.Solve());
    float height = 0.0f;
    CHECK(solver.GetHeight(4, height));
    CHECK(height == 3.75f);
}

}  // namespace

int main() {
    TestHtRangeIntersect();
    TestLifecycle();
    TestFixedVertexSolve();
    TestInitialWindowClampsHint();
    TestSlopeProjection();
    TestSlopeDuplicateKeepsMinimum();
    TestSlopeRangeProjection();
    TestSmoothnessMidFixedThroughPoints();
    TestPreferredValue();
    TestPreferredRange();
    TestRangeConstraintFold();
    TestInvertedRangeFailsSolve();
    TestEqualityTransitive();
    TestEqualityHoldsWithoutFixedVertex();
    TestSlopeHoldsAfterRangePass();
    TestEqualityConflictFails();
    TestEqualityForUnknownVertices();
    TestSmoothnessEndFixedThroughMiddle();
    TestSmoothnessHoldsAfterRangeProjection();
    TestSmoothnessFixedEndsBoundTheMiddle();
    TestSmoothnessDuplicateKeepsMinimum();
    TestSolvedVertexWalk();
    TestIsInToSolveSet();
    TestDumpConstraintsQueriesTerrain();
    TestUnknownVertexInserts();
    TestUnconstrainedVertexKeepsHint();
    SetVertexHtConstraintHost(nullptr);
    if (failures == 0) {
        std::printf("sc4_vertexhtconstraintsatisfier: ok\n");
        return 0;
    }
    std::printf("sc4_vertexhtconstraintsatisfier: %d failure(s)\n", failures);
    return 1;
}
