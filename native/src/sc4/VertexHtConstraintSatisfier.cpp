// cSC4VertexHtConstraintSatisfier — see VertexHtConstraintSatisfier.h for the layout and quirks.
// Function order follows the original addresses; every body carries the address it was recovered from.
#include "sc4/VertexHtConstraintSatisfier.h"

#include <cfloat>
#include <cstddef>
#include <string>

#include "rz/cRZString.h"

namespace sc4 {

namespace {

// The only engine global this class reaches (the original read SL::spTerrain in DumpConstraints).
IVertexHtConstraintHost* gVertexHtConstraintHost = nullptr;

// 0.01f at 0x100a15928: the tolerance the original allows before declaring a height window empty.
constexpr float kRangeTolerance = 0.01f;

}  // namespace

void SetVertexHtConstraintHost(IVertexHtConstraintHost* host) { gVertexHtConstraintHost = host; }

// tHtRange::Intersect(tHtRange const&, bool&) @ 0x1001bda76 — shrink to the intersection.
bool tHtRange::Intersect(const tHtRange& other, bool& changed) {
    changed = false;
    // 0x1001bda81: UCOMISS other.low, this->low / JBE => only a strictly larger low is taken.
    if (other.low > low) {
        low = other.low;
        changed = true;
    }
    // 0x1001bda97: UCOMISS this->high, other.high / JBE => only a strictly smaller high is taken.
    if (high > other.high) {
        high = other.high;
        changed = true;
    }
    // 0x1001bdaaf: !(low > high) returns true (unordered counts as "not inverted", as UCOMISS+JBE does).
    if (!(low > high)) return true;
    // 0x1001bdabb: a gap of at most 0.01 collapses onto the low bound, anything larger is a failure.
    if (!(low - high > kRangeTolerance)) {
        high = low;
        return true;
    }
    return false;
}

// 0x1001bca8c
cSC4VertexHtConstraintSatisfier::cSC4VertexHtConstraintSatisfier() = default;

// 0x1001bcb6a (D1). D0 (0x1001bccae) and the base-object alias (0x1001bcca4) are compiler-emitted. The
// original's D1 only cleared the equality-set list nodes here; the sets themselves are freed by
// Shutdown. The "never initialized" path also frees them so a misused instance cannot leak.
cSC4VertexHtConstraintSatisfier::~cSC4VertexHtConstraintSatisfier() {
    if (initialized_) {
        Shutdown();
    } else {
        DeleteEqualitySets();
    }
}

// 0x1001bd156: the ctor leaves +8/+9 zero; Init sets the 16-bit word to 0x0101 (initialized AND solved).
bool cSC4VertexHtConstraintSatisfier::Init(float initialHeightLow, float initialHeightHigh) {
    if (initialized_) return false;
    initialized_ = true;
    solveSucceeded_ = true;  // quirk: the original writes 0x0101 here, not just the initialized flag
    initialHeightLow_ = initialHeightLow;
    initialHeightHigh_ = initialHeightHigh;
    return true;
}

// 0x1001bcc74
bool cSC4VertexHtConstraintSatisfier::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    DeleteConstraintsAndOtherTemporaryData();
    solvedHeights_.clear();
    // The original leaves +0xf8 pointing at the freed bucket chain; reset it so GetNext is defined.
    solvedCursor_ = solvedHeights_.end();
    return true;
}

// 0x1001bccdc
void cSC4VertexHtConstraintSatisfier::DeleteConstraintsAndOtherTemporaryData() {
    toSolve_.clear();  // 0x1001bcced
    DeleteEqualitySets();  // the original clears each set's tree twice, then deletes it
    slopeConstraints_.clear();
    smoothnessConstraints_.clear();
    midFixedSmoothnessConstraints_.clear();
    endFixedSmoothnessConstraints_.clear();
    solvedVertList_.clear();
}

void cSC4VertexHtConstraintSatisfier::DeleteEqualitySets() {
    for (std::set<int>* set : equalitySets_) {
        set->clear();
        delete set;
    }
    equalitySets_.clear();
}

// 0x1001bc9c2: a new entry gets the Init window, the hinted height and flag 0x01. The original copies
// the 8 bytes at +0x0c (low/high) in one move and leaves the preferred fields as operator[] zeroed them.
void cSC4VertexHtConstraintSatisfier::AddVertexToSolve(std::int32_t vertexId, float height) {
    if (toSolve_.find(vertexId) != toSolve_.end()) return;
    tToSolveVertInfo& info = toSolve_[vertexId];
    info.low = initialHeightLow_;
    info.high = initialHeightHigh_;
    info.height = height;
    info.flags = kFlagRange;
}

// 0x1001bd6b2
void cSC4VertexHtConstraintSatisfier::InsertRangeConstraint(std::int32_t vertexId, float low, float high) {
    auto it = toSolve_.find(vertexId);
    if (it == toSolve_.end()) return;
    it->second.flags |= kFlagRange;
    // The original searches for the same key a second time (0x1001bd6f4).
    it = toSolve_.find(vertexId);
    if (it == toSolve_.end()) {
        // Unreachable in the original (nothing erased the key between the two finds): its branch copies
        // the key plus 16 bytes of uninitialized stack into a fresh entry. Reproduced with those bytes
        // zeroed, as operator[] zero-initializes them.
        tToSolveVertInfo& info = toSolve_[vertexId];
        info.low = low;
        info.high = high;
        info.flags = kFlagRange;
        return;
    }
    // 0x1001bd6fe: window.low = max(window.low, low); 0x1001bd715: window.high = min(window.high, high).
    if (low > it->second.low) it->second.low = low;
    if (high < it->second.high) it->second.high = high;
    // 0x1001bd72c: an inverted window is folded onto its low bound when the gap is <= 0.01.
    if (it->second.low > it->second.high &&
        !(it->second.low - it->second.high > kRangeTolerance)) {
        it->second.high = it->second.low;
    }
}

// 0x1001bd78a
void cSC4VertexHtConstraintSatisfier::InsertPreferredValueConstraint(std::int32_t vertexId, float value) {
    auto it = toSolve_.find(vertexId);
    if (it == toSolve_.end()) return;
    it->second.flags |= kFlagPreferredValue;
    it->second.preferred = value;
}

// 0x1001bd7c2
void cSC4VertexHtConstraintSatisfier::InsertPreferredRangeConstraint(std::int32_t vertexId, float low,
                                                                     float high) {
    auto it = toSolve_.find(vertexId);
    if (it == toSolve_.end()) return;
    it->second.flags |= kFlagPreferredRange;
    it->second.preferredLow = low;
    it->second.preferredHigh = high;
}

// 0x1001bd67c
void cSC4VertexHtConstraintSatisfier::InsertFixedVertex(std::int32_t vertexId, float height) {
    AddVertexToSolve(vertexId, height);
    InsertRangeConstraint(vertexId, height, height);
}

// i386 0x0067b564: a find in the to-solve map (the network tool's support pass asks before adding a vertex).
bool cSC4VertexHtConstraintSatisfier::IsInToSolveSet(std::int32_t vertexId) const {
    return toSolve_.find(vertexId) != toSolve_.end();
}

// 0x1001bd09a: the first equality set (in list order) whose tree contains the vertex.
std::set<int>* cSC4VertexHtConstraintSatisfier::FindEqualitySet(std::int32_t vertexId) const {
    for (std::set<int>* set : equalitySets_) {
        if (set->find(vertexId) != set->end()) return set;
    }
    return nullptr;
}

// 0x1001bd4be: every element of `source` goes into `target` (the original walks source in order).
void cSC4VertexHtConstraintSatisfier::MergeIntoSet(std::set<int>& target, const std::set<int>& source) {
    target.insert(source.begin(), source.end());
}

// 0x1001bd178: flags both vertices (when present), then joins their equality sets.
void cSC4VertexHtConstraintSatisfier::InsertEqualityConstraint(std::int32_t vertexA,
                                                                std::int32_t vertexB) {
    auto itA = toSolve_.find(vertexA);
    if (itA != toSolve_.end()) itA->second.flags |= kFlagEquality;
    auto itB = toSolve_.find(vertexB);
    if (itB != toSolve_.end()) itB->second.flags |= kFlagEquality;

    std::set<int>* setA = FindEqualitySet(vertexA);
    std::set<int>* setB = FindEqualitySet(vertexB);
    if (setA == nullptr) {
        if (setB == nullptr) {
            // 0x1001bd2e0: new set, appended to the list, then A and B inserted in that order.
            auto* set = new std::set<int>;
            equalitySets_.push_back(set);
            set->insert(vertexA);
            set->insert(vertexB);
        } else {
            setB->insert(vertexA);  // 0x1001bd243
        }
    } else if (setB == nullptr) {
        setA->insert(vertexB);  // 0x1001bd293
    } else if (setA != setB) {
        // 0x1001bd200: merge B into A, then drop B from the list and delete it.
        MergeIntoSet(*setA, *setB);
        equalitySets_.remove(setB);
        delete setB;
    }
}

// 0x1001bd80a
void cSC4VertexHtConstraintSatisfier::InsertSlopeConstraint(std::int32_t vertexA,
                                                             std::int32_t vertexB, float maxSlope) {
    if (vertexA == vertexB) return;
    auto itA = toSolve_.find(vertexA);
    if (itA == toSolve_.end()) return;  // the original checks A off first and bails out silently
    itA->second.flags |= kFlagSlope;
    auto itB = toSolve_.find(vertexB);
    if (itB == toSolve_.end()) return;  // A stays flagged
    itB->second.flags |= kFlagSlope;
    for (tSlopeConstraint& c : slopeConstraints_) {
        if ((c.vertexA == vertexA && c.vertexB == vertexB) ||
            (c.vertexA == vertexB && c.vertexB == vertexA)) {
            // Keep the smaller slope: the original skips the store when stored <= new (UCOMISS+JBE,
            // which also skips on NaN), so only a strictly larger stored slope is replaced.
            if (c.maxSlope > maxSlope) c.maxSlope = maxSlope;
            return;
        }
    }
    slopeConstraints_.push_back(tSlopeConstraint{vertexA, vertexB, maxSlope});
}

// 0x1001bd8fc
void cSC4VertexHtConstraintSatisfier::InsertSmoothnessConstraint(std::int32_t vertexA,
                                                                  std::int32_t vertexB,
                                                                  std::int32_t vertexC,
                                                                  float maxSecondDifference) {
    if (vertexC == vertexA || vertexA == vertexB || vertexB == vertexC) return;  // 0x1001bd8fc head
    auto itA = toSolve_.find(vertexA);
    if (itA == toSolve_.end()) return;
    itA->second.flags |= kFlagSmoothness;
    auto itB = toSolve_.find(vertexB);
    if (itB == toSolve_.end()) return;  // A stays flagged
    itB->second.flags |= kFlagSmoothness;
    auto itC = toSolve_.find(vertexC);
    if (itC == toSolve_.end()) return;  // A and B stay flagged
    itC->second.flags |= kFlagSmoothness;
    for (tSmoothnessConstraint& c : smoothnessConstraints_) {
        if (c.vertexB == vertexB &&
            ((c.vertexA == vertexA && c.vertexC == vertexC) ||
             (c.vertexA == vertexC && c.vertexC == vertexA))) {
            // Same UCOMISS+JBE shape as the slope list: only a strictly larger limit is replaced.
            if (c.maxSecondDifference > maxSecondDifference)
                c.maxSecondDifference = maxSecondDifference;
            return;
        }
    }
    smoothnessConstraints_.push_back(
        tSmoothnessConstraint{vertexA, vertexB, vertexC, maxSecondDifference});
}

// 0x1001bc840: merge-or-append a fixed-end smoothness constraint, keyed by (end, middle) exactly (no
// reversed lookup here, unlike AddMidFixed). The merge keeps the intersection of the two windows.
bool cSC4VertexHtConstraintSatisfier::AddEndFixedSmoothnessConstraint(
    const tSmoothConstrEndFixed& constraint) {
    for (tSmoothConstrEndFixed& c : endFixedSmoothnessConstraints_) {
        if (c.vertexEnd == constraint.vertexEnd && c.vertexMid == constraint.vertexMid) {
            float high = c.high;
            if (constraint.high < c.high) {  // 0x1001bc8c5
                c.high = constraint.high;
                high = constraint.high;
            }
            float low = c.low;
            if (c.low < constraint.low) {  // 0x1001bc8d8
                c.low = constraint.low;
                low = constraint.low;
            }
            return high >= low;  // 0x1001bc8ea: SETNC after UCOMISS(high, low); NaN => false
        }
    }
    endFixedSmoothnessConstraints_.push_back(constraint);  // 0x1001bc87d (append)
    return true;
}

// 0x1001bc8fc: same as AddEndFixed, but the key is the unordered pair (endA, endC).
bool cSC4VertexHtConstraintSatisfier::AddMidFixedSmoothnessConstraint(
    const tSmoothConstrMidFixed& constraint) {
    for (tSmoothConstrMidFixed& c : midFixedSmoothnessConstraints_) {
        if ((c.vertexA == constraint.vertexA && c.vertexC == constraint.vertexC) ||
            (c.vertexA == constraint.vertexC && c.vertexC == constraint.vertexA)) {
            float high = c.high;
            if (constraint.high < c.high) {
                c.high = constraint.high;
                high = constraint.high;
            }
            float low = c.low;
            if (c.low < constraint.low) {
                c.low = constraint.low;
                low = constraint.low;
            }
            return high >= low;
        }
    }
    midFixedSmoothnessConstraints_.push_back(constraint);  // 0x1001bc944 (append)
    return true;
}

// 0x1001bdad4: the mark is an append-only vector entry in the original (nothing in the class ever
// reads solvedVertList_ except PickASolution's pop; MarkSolution does not consume it).
bool cSC4VertexHtConstraintSatisfier::MarkPotentiallySolved(std::int32_t vertexId, float height) {
    solvedVertList_.push_back(tSolvedVert{vertexId, height});
    return true;
}

// 0x1001bdb1e
void cSC4VertexHtConstraintSatisfier::MarkSolution(std::int32_t vertexId, float height) {
    solvedHeights_[vertexId] = height;  // 0x1001bdb3f: unordered_map::operator[]
    auto it = toSolve_.find(vertexId);
    if (it != toSolve_.end()) toSolve_.erase(it);  // 0x1001bdb67
}

// 0x1001bda28: the not-found case returns true without writing `changed` in the original (the callers
// then read an uninitialized local); the port always receives false from the caller's initialization.
bool cSC4VertexHtConstraintSatisfier::IntersectVertexRange(std::int32_t vertexId, const tHtRange& range,
                                                           bool& changed) {
    auto it = toSolve_.find(vertexId);
    if (it == toSolve_.end()) return true;
    // The original reinterprets the value's first two floats as the tHtRange and intersects in place.
    tHtRange window{it->second.low, it->second.high};
    const bool ok = window.Intersect(range, changed);
    it->second.low = window.low;
    it->second.high = window.high;
    return ok;
}

// 0x1001bdd50: the float (point) flavour of the dispatch.
bool cSC4VertexHtConstraintSatisfier::UpdateConstraints(std::int32_t vertexId, float height,
                                                        std::uint32_t flags,
                                                        std::set<int>& nextVertices) {
    if ((flags & kFlagEquality) && !UpdateEqualityConstraints(vertexId, height, nextVertices))
        return false;
    if ((flags & kFlagSlope) && !UpdateSlopeConstraints(vertexId, height, nextVertices)) return false;
    if (flags & kFlagSmoothness) {
        if (!UpdateSmoothnessConstraints(vertexId, height)) return false;
        if (!UpdateMidFixedSmoothnessConstraints(vertexId, height, nextVertices)) return false;
        if (!UpdateEndFixedSmoothnessConstraints(vertexId, height, nextVertices)) return false;
    }
    return true;
}

// 0x1001bde06: the range flavour of the dispatch (same order of the checks).
bool cSC4VertexHtConstraintSatisfier::UpdateConstraints(std::int32_t vertexId, const tHtRange& range,
                                                        std::uint32_t flags,
                                                        std::set<int>& nextVertices) {
    if ((flags & kFlagEquality) && !UpdateEqualityConstraints(vertexId, range, nextVertices))
        return false;
    if ((flags & kFlagSlope) && !UpdateSlopeConstraints(vertexId, range, nextVertices)) return false;
    if (flags & kFlagSmoothness) {
        if (!UpdateSmoothnessConstraints(vertexId, range)) return false;
        if (!UpdateMidFixedSmoothnessConstraints(vertexId, range, nextVertices)) return false;
        if (!UpdateEndFixedSmoothnessConstraints(vertexId, range, nextVertices)) return false;
    }
    return true;
}

// 0x1001be298: an equality set forces every member onto the solved height. The original has no
// null-check on the map lookup (a member is always present, see the header note); the port returns
// false instead of faulting if that invariant is ever broken.
bool cSC4VertexHtConstraintSatisfier::UpdateEqualityConstraints(std::int32_t vertexId, float height,
                                                               std::set<int>& nextVertices) {
    std::set<int>* set = FindEqualitySet(vertexId);
    if (set == nullptr) return true;
    if (!set->empty()) {
        for (int member : *set) {
            auto it = toSolve_.find(member);
            if (it == toSolve_.end()) return false;  // hardening: the original dereferences null
            tToSolveVertInfo& info = it->second;
            if (member != vertexId) {
                bool changed = false;
                // 0x1001be325: low = max(low, height); 0x1001be341: high = min(high, height).
                if (height > info.low) {
                    info.low = height;
                    changed = true;
                }
                if (height < info.high) {
                    info.high = height;
                    changed = true;
                }
                // 0x1001be359: inverted by more than 0.01 => hard failure (the set stays in place).
                if (info.high < info.low) {
                    if (info.low - info.high > kRangeTolerance) return false;
                    info.high = info.low;
                }
                if (changed) nextVertices.insert(member);
            }
            info.flags &= ~kFlagEquality;  // 0x1001be3e7 (also for the solved member itself)
        }
    }
    // 0x1001be430: the set is always removed and deleted after the pass (even when it was empty).
    equalitySets_.remove(set);
    delete set;
    return true;
}

// 0x1001beb14 (i386 0x0014b102): the range flavour intersects every other member's window with the
// vertex's range and keeps the set and the equality flags: like every range flavour it narrows windows
// without consuming the constraint, which the float flavour applies (and removes) once a member is
// solved.
bool cSC4VertexHtConstraintSatisfier::UpdateEqualityConstraints(std::int32_t vertexId,
                                                                const tHtRange& range,
                                                                std::set<int>& nextVertices) {
    std::set<int>* set = FindEqualitySet(vertexId);
    if (set == nullptr) return true;
    for (int member : *set) {
        if (member == vertexId) continue;
        bool changed = false;
        if (!IntersectVertexRange(member, range, changed)) return false;
        if (changed) nextVertices.insert(member);
    }
    return true;
}

// 0x1001be488
bool cSC4VertexHtConstraintSatisfier::UpdateSlopeConstraints(std::int32_t vertexId, float height,
                                                             std::set<int>& nextVertices) {
    for (auto it = slopeConstraints_.begin(); it != slopeConstraints_.end();) {
        int other;
        if (it->vertexA == vertexId) {
            other = it->vertexB;
        } else if (it->vertexB == vertexId) {
            other = it->vertexA;
        } else {
            ++it;
            continue;
        }
        if (other < 0) {  // 0x1001be4f1: negative ids are skipped (the constraint stays)
            ++it;
            continue;
        }
        // 0x1001be505: [height - slope, slope + height].
        tHtRange range{height - it->maxSlope, it->maxSlope + height};
        bool changed = false;  // the original's local is left uninitialized when `other` is missing
        if (!IntersectVertexRange(other, range, changed)) return false;
        if (changed) nextVertices.insert(other);  // 0x1001be577
        it = slopeConstraints_.erase(it);
    }
    return true;
}

// 0x1001bec70
bool cSC4VertexHtConstraintSatisfier::UpdateSlopeConstraints(std::int32_t vertexId,
                                                             const tHtRange& range,
                                                             std::set<int>& nextVertices) {
    for (auto it = slopeConstraints_.begin(); it != slopeConstraints_.end();) {
        int other;
        if (it->vertexA == vertexId) {
            other = it->vertexB;
        } else if (it->vertexB == vertexId) {
            other = it->vertexA;
        } else {
            ++it;
            continue;
        }
        if (other < 0) {
            ++it;
            continue;
        }
        // i386 0x0014ae02: [range.low - slope, range.high + slope]; the constraint stays for the solved pass.
        tHtRange shifted{range.low - it->maxSlope, range.high + it->maxSlope};
        bool changed = false;
        if (!IntersectVertexRange(other, shifted, changed)) return false;
        if (changed) nextVertices.insert(other);
        ++it;
    }
    return true;
}

// 0x1001be60c: project a smoothness constraint through the solved END (AddEndFixed, keyed by the
// unsolved end + middle), through the solved MIDDLE (AddMidFixed, keyed by both ends) or through the
// solved other END. The original removes the smoothness constraint once the projection is accepted.
bool cSC4VertexHtConstraintSatisfier::UpdateSmoothnessConstraints(std::int32_t vertexId, float height) {
    for (auto it = smoothnessConstraints_.begin(); it != smoothnessConstraints_.end();) {
        if (it->vertexA == vertexId) {
            // 0x1001be6b8: key (C, B), window [-limit - h, limit - h].
            const tSmoothConstrEndFixed c{it->vertexC, it->vertexB,
                                          -it->maxSecondDifference - height,
                                          it->maxSecondDifference - height};
            if (!AddEndFixedSmoothnessConstraint(c)) return false;
        } else if (it->vertexB == vertexId) {
            // 0x1001be676: key (A, C), hA + hC in [2h - limit, 2h + limit].
            const tSmoothConstrMidFixed c{it->vertexA, it->vertexC,
                                          (height + height) - it->maxSecondDifference,
                                          (height + height) + it->maxSecondDifference};
            if (!AddMidFixedSmoothnessConstraint(c)) return false;
        } else if (it->vertexC == vertexId) {
            // 0x1001be6ac: key (A, B), same window as the A branch.
            const tSmoothConstrEndFixed c{it->vertexA, it->vertexB,
                                          -it->maxSecondDifference - height,
                                          it->maxSecondDifference - height};
            if (!AddEndFixedSmoothnessConstraint(c)) return false;
        } else {
            ++it;
            continue;
        }
        it = smoothnessConstraints_.erase(it);  // 0x1001be6e9
    }
    return true;
}

// 0x1001bedbe: the range flavour; the same three projections, with the solved window's bounds in place
// of the solved height. The mid projection reflects the range as [2*low - limit, 2*high + limit] and
// the end projection stores [-limit - range.high, limit - range.low].
bool cSC4VertexHtConstraintSatisfier::UpdateSmoothnessConstraints(std::int32_t vertexId,
                                                                  const tHtRange& range) {
    for (auto it = smoothnessConstraints_.begin(); it != smoothnessConstraints_.end();) {
        if (it->vertexA == vertexId) {
            const tSmoothConstrEndFixed c{it->vertexC, it->vertexB,
                                          -it->maxSecondDifference - range.high,
                                          it->maxSecondDifference - range.low};
            if (!AddEndFixedSmoothnessConstraint(c)) return false;
        } else if (it->vertexB == vertexId) {
            const tSmoothConstrMidFixed c{it->vertexA, it->vertexC,
                                          (range.low + range.low) - it->maxSecondDifference,
                                          (range.high + range.high) + it->maxSecondDifference};
            if (!AddMidFixedSmoothnessConstraint(c)) return false;
        } else if (it->vertexC == vertexId) {
            const tSmoothConstrEndFixed c{it->vertexA, it->vertexB,
                                          -it->maxSecondDifference - range.high,
                                          it->maxSecondDifference - range.low};
            if (!AddEndFixedSmoothnessConstraint(c)) return false;
        }
        ++it;   // the range flavour keeps the constraint for the solved height's pass
    }
    return true;
}

// 0x1001be734
bool cSC4VertexHtConstraintSatisfier::UpdateMidFixedSmoothnessConstraints(
    std::int32_t vertexId, float height, std::set<int>& nextVertices) {
    for (auto it = midFixedSmoothnessConstraints_.begin(); it != midFixedSmoothnessConstraints_.end();) {
        int other;
        if (it->vertexA == vertexId) {
            other = it->vertexC;
        } else if (it->vertexC == vertexId) {
            other = it->vertexA;
        } else {
            ++it;
            continue;
        }
        if (other < 0) {
            ++it;
            continue;
        }
        // 0x1001be7a3: [storedLow - h, storedHigh - h].
        tHtRange range{it->low - height, it->high - height};
        bool changed = false;
        if (!IntersectVertexRange(other, range, changed)) return false;
        if (changed) nextVertices.insert(other);
        it = midFixedSmoothnessConstraints_.erase(it);  // 0x1001be848
    }
    return true;
}

// 0x1001beeb8
bool cSC4VertexHtConstraintSatisfier::UpdateMidFixedSmoothnessConstraints(
    std::int32_t vertexId, const tHtRange& range, std::set<int>& nextVertices) {
    for (auto it = midFixedSmoothnessConstraints_.begin(); it != midFixedSmoothnessConstraints_.end();) {
        int other;
        if (it->vertexA == vertexId) {
            other = it->vertexC;
        } else if (it->vertexC == vertexId) {
            other = it->vertexA;
        } else {
            ++it;
            continue;
        }
        if (other < 0) {
            ++it;
            continue;
        }
        // 0x1001beeb8: [storedLow - range.high, storedHigh - range.low].
        tHtRange shifted{it->low - range.high, it->high - range.low};
        bool changed = false;
        if (!IntersectVertexRange(other, shifted, changed)) return false;
        if (changed) nextVertices.insert(other);
        ++it;   // kept (range flavour)
    }
    return true;
}

// 0x1001be8ba
bool cSC4VertexHtConstraintSatisfier::UpdateEndFixedSmoothnessConstraints(
    std::int32_t vertexId, float height, std::set<int>& nextVertices) {
    for (auto it = endFixedSmoothnessConstraints_.begin(); it != endFixedSmoothnessConstraints_.end();) {
        if (it->vertexEnd == vertexId) {
            // 0x1001be917: the middle's window is [(h - high) * -0.5, (h - low) * -0.5].
            const tHtRange range{(it->high - height) * -0.5f, (it->low - height) * -0.5f};
            bool changed = false;
            if (!IntersectVertexRange(it->vertexMid, range, changed)) return false;
            if (changed) nextVertices.insert(it->vertexMid);
        } else if (it->vertexMid == vertexId) {
            // 0x1001be9c1: the end's window is [low + 2h, high + 2h]. The original forms 2h first
            // (XMM1 = h; ADDSS XMM1,XMM1) and adds it in one step, so keep that association.
            const tHtRange range{it->low + (height + height), it->high + (height + height)};
            bool changed = false;
            if (!IntersectVertexRange(it->vertexEnd, range, changed)) return false;
            if (changed) nextVertices.insert(it->vertexEnd);
        } else {
            ++it;
            continue;
        }
        it = endFixedSmoothnessConstraints_.erase(it);  // 0x1001bea4d
    }
    return true;
}

// 0x1001bf006
bool cSC4VertexHtConstraintSatisfier::UpdateEndFixedSmoothnessConstraints(
    std::int32_t vertexId, const tHtRange& range, std::set<int>& nextVertices) {
    for (auto it = endFixedSmoothnessConstraints_.begin(); it != endFixedSmoothnessConstraints_.end();) {
        if (it->vertexEnd == vertexId) {
            // i386 0x0014b18c: [(high - range.low) * -0.5, (low - range.high) * -0.5] (FLOAT_00a707c4 = -0.5).
            const tHtRange middle_range{(it->high - range.low) * -0.5f,
                                        (it->low - range.high) * -0.5f};
            bool changed = false;
            if (!IntersectVertexRange(it->vertexMid, middle_range, changed)) return false;
            if (changed) nextVertices.insert(it->vertexMid);
        } else if (it->vertexMid == vertexId) {
            // 0x1001be9c1 (range): [2*range.low + low, 2*range.high + high].
            const tHtRange end_range{range.low + range.low + it->low,
                                     range.high + range.high + it->high};
            bool changed = false;
            if (!IntersectVertexRange(it->vertexEnd, end_range, changed)) return false;
            if (changed) nextVertices.insert(it->vertexEnd);
        }
        ++it;   // kept (range flavour)
    }
    return true;
}

// 0x1001bdbd8: pop a marked solution (the vector holds the point windows found while propagating);
// otherwise take the vertex with the narrowest window and snap it to one height. The original checks
// the vector first, so a "potentially solved" mark always wins over the narrowest-window scan.
bool cSC4VertexHtConstraintSatisfier::PickASolution(std::int32_t& vertexId, float& height,
                                                    bool& propagate) {
    propagate = false;
    if (!solvedVertList_.empty()) {
        vertexId = solvedVertList_.back().vertex;
        height = solvedVertList_.back().height;
        solvedVertList_.pop_back();
        return true;
    }
    // 0x1001bdc13: the scan starts at the map's first bucket-chain node, i.e. begin() on libc++.
    auto it = toSolve_.begin();
    if (it == toSolve_.end()) return false;  // original dereferences a null node (unreachable in Solve)
    float bestWidth = FLT_MAX;
    std::int32_t bestVertex = 0;
    tToSolveVertInfo* best = nullptr;
    for (; it != toSolve_.end(); ++it) {
        const float width = it->second.high - it->second.low;  // 0x1001bdc2a
        if (width < 0.0f) {  // 0x1001bdc37: an already-inverted window aborts the solve
            failedVertex_ = it->first;
            return false;
        }
        if (width < bestWidth) {
            bestVertex = it->first;
            best = &it->second;
            bestWidth = width;
        }
        if (bestWidth == 0.0f) break;  // 0x1001bdc4d: stop at an exact point
    }
    if (best == nullptr) return false;  // every width was NaN; the original would dereference null
    vertexId = bestVertex;

    float low = best->low;
    float high = best->high;
    float chosen;
    if ((best->flags & kFlagPreferredValue) && high >= best->preferred &&
        best->preferred >= low) {
        // 0x1001bdd49: the preferred value wins outright when it lies inside the window.
        chosen = best->preferred;
    } else {
        if (best->flags & kFlagPreferredRange) {
            // 0x1001bdcae: shrink to [max(low, prefLow), min(high, prefHigh)]. UCOMISS treats
            // unordered comparisons as "take the window bound", reproduced by the <= forms below.
            const float newLow = (best->preferredLow <= low) ? low : best->preferredLow;
            const float newHigh = (high <= best->preferredHigh) ? high : best->preferredHigh;
            if (!(newLow > newHigh)) {
                low = newLow;
                high = newHigh;
            } else if (!(newLow - newHigh > kRangeTolerance)) {
                // 0x1001bdcf0: inverted by at most 0.01 => collapse onto the new low bound.
                low = newLow;
                high = newLow;
            }
            // A larger inversion ignores the preferred range entirely.
        }
        // 0x1001bdd06: clamp the hinted height into the (possibly preferred-narrowed) window.
        if (best->height > high) {
            chosen = high;
        } else {
            chosen = (low > best->height) ? low : best->height;  // MAXSS(low, height)
        }
    }
    propagate = (best->low != best->high);  // 0x1001bdd22: was the window still a range?
    height = chosen;
    best->high = chosen;  // 0x1001bdd38/0x1001bdd3d: low = high = chosen
    best->low = chosen;
    best->flags |= kFlagSolved;
    return true;
}

// 0x1001bdad4 / 0x1001bdbd8 helper: the "potentially solved" vector is read by PickASolution only, so
// a mark can make a vertex be solved before the narrowest-window scan reaches it.
bool cSC4VertexHtConstraintSatisfier::DoInitialConstraintPropagation() {
    std::set<int> vertices;  // the original collects the keys from toSolve_'s bucket chain
    for (const auto& entry : toSolve_) vertices.insert(entry.first);
    return PropagateRangeConstraintsOfVerts(vertices);
}

// 0x1001bcece: the ping-pong worklist. Each round drains `current` into the other set; the current set
// is cleared in place and the two swap.
bool cSC4VertexHtConstraintSatisfier::PropagateRangeConstraintsOfVerts(std::set<int>& vertices) {
    if (vertices.empty()) return true;  // 0x1001bcef2
    std::set<int> scratch;
    std::set<int>* current = &vertices;
    std::set<int>* pending = &scratch;
    do {
        for (std::set<int>::const_iterator it = current->begin(); it != current->end(); ++it) {
            const std::int32_t vertexId = *it;
            auto entry = toSolve_.find(vertexId);
            if (entry == toSolve_.end()) {
                // 0x1001bcf48: the original bails out without recording a failed vertex here.
                return false;
            }
            tToSolveVertInfo& info = entry->second;
            if (info.high < info.low) {  // 0x1001bcf58: inverted window => failure
                failedVertex_ = vertexId;
                return false;
            }
            if (info.low != info.high) {
                // 0x1001bcfa8: the window is still a range; pass it (low/high) to the range dispatch.
                const tHtRange range{info.low, info.high};
                if (!UpdateConstraints(vertexId, range, info.flags, *pending)) {
                    failedVertex_ = vertexId;
                    return false;
                }
            } else {
                // 0x1001bcf72: a point window goes through the float dispatch.
                if (!UpdateConstraints(vertexId, info.low, info.flags, *pending)) {
                    failedVertex_ = vertexId;
                    return false;
                }
                if (!(info.flags & kFlagSolved)) {  // 0x1001bcf88
                    info.flags |= kFlagSolved;
                    MarkPotentiallySolved(vertexId, info.low);
                }
            }
        }
        current->clear();  // 0x1001bd005
        std::swap(current, pending);
    } while (!current->empty());
    return true;
}

// 0x1001bdeac: park the cursor on the solved map's first bucket-chain node (begin()).
void cSC4VertexHtConstraintSatisfier::RewindSolvedVertexList() {
    solvedCursor_ = solvedHeights_.begin();
}

// 0x1001bd0ea: the solved height, or false when the vertex never got solved.
bool cSC4VertexHtConstraintSatisfier::GetHeight(std::int32_t vertexId, float& height) const {
    const auto it = solvedHeights_.find(vertexId);
    if (it == solvedHeights_.end()) return false;
    height = it->second;
    return true;
}

// 0x1001bd11c: walk the solved map in bucket-chain order, exactly like the original's node pointer.
bool cSC4VertexHtConstraintSatisfier::GetNextSolvedVertexAndItsHeight(std::int32_t& vertexId,
                                                                     float& height) {
    if (solvedCursor_ == solvedHeights_.end()) return false;
    vertexId = solvedCursor_->first;
    height = solvedCursor_->second;
    ++solvedCursor_;  // the original advances to node->next (null at the end, i.e. end())
    return true;
}

// 0x1001bdebe
bool cSC4VertexHtConstraintSatisfier::Solve() {
    std::set<int> propagationSet;
    bool ok = DoInitialConstraintPropagation();
    solveSucceeded_ = ok;
    if (ok) {
        std::size_t count = toSolve_.size();  // 0x1001bdf01 loads the map's node count
        if (count != 0) {
            bool done = false;
            while (!done) {
                // 0x1001bdffd: defensive "more than INT_MAX windows" path (sets solveSucceeded_ = 0).
                if (static_cast<std::int32_t>(count) <= 0) {
                    solveSucceeded_ = false;
                    break;
                }
                std::int32_t vertexId = 0;
                float height = 0.0f;
                bool propagate = false;
                ok = PickASolution(vertexId, height, propagate);
                solveSucceeded_ = ok;
                if (!ok) break;
                if (propagate) {  // 0x1001bdf2c
                    propagationSet.insert(vertexId);
                    ok = PropagateRangeConstraintsOfVerts(propagationSet);
                    solveSucceeded_ = ok;
                }
                if (!ok) break;
                MarkSolution(vertexId, height);  // 0x1001bdfe0
                count = toSolve_.size();
                if (!solveSucceeded_) break;
                if (toSolve_.size() == 0) done = true;  // 0x1001bdff5: all solved, clean success
            }
        }
    }
    DeleteConstraintsAndOtherTemporaryData();  // 0x1001be001
    if (!solveSucceeded_) DumpConstraints();    // 0x1001be012
    return solveSucceeded_;
}

// 0x1001be05e: debug-only. The original formats the equality sets into a local cRZString (Sprintf
// " %d,%d", then appends " >" per set) and throws the text away — the print/log sink was compiled out.
// The observable work is the two SL::spTerrain->GetVertex calls per vertex (the first supplies x, the
// second z) and the empty walks over the slope and end-fixed lists.
void cSC4VertexHtConstraintSatisfier::DumpConstraints() {
    cRZString line;
    std::string text;  // the original's accumulator is a cRZString; its contents are discarded
    for (std::set<int>* set : equalitySets_) {
        line = cRZString();  // the accumulator is re-initialized for every set
        for (int vertex : *set) {
            if (gVertexHtConstraintHost == nullptr) continue;  // no terrain: nothing observable
            std::int32_t x = 0;
            std::int32_t z = 0;
            gVertexHtConstraintHost->GetVertex(vertex, x, z);
            const std::int32_t firstX = x;
            gVertexHtConstraintHost->GetVertex(vertex, x, z);
            line.Sprintf(" %d,%d", firstX, z);
            text.append(line.ToChar());
        }
        text.append(" >");
    }
    // The original walks the slope and end-fixed lists here with an empty body; kept so the two
    // traversals exist, but they have no observable effect.
    for (const tSlopeConstraint& constraint : slopeConstraints_) {
        (void)constraint;
    }
    for (const tSmoothConstrEndFixed& constraint : endFixedSmoothnessConstraints_) {
        (void)constraint;
    }
}

}  // namespace sc4
