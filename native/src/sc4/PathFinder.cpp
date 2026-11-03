#include "sc4/PathFinder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "sc4/GeometryUtilities.h"

namespace sc4 {

// ---- ParseTripLocation ----------------------------------------------------------------------------------------------------

// 0x1000d3ccc
void ParseTripLocation(std::uint32_t key, std::uint8_t& travelType, std::int16_t& x, std::int16_t& z) {
    travelType = std::uint8_t(key >> 16);
    x = std::int16_t((key >> 8) & 0xff);
    z = std::int16_t(key & 0xff);
    if (std::int32_t(key) < 0) {
        const std::uint32_t direction = (key >> 24) & 3;
        x = std::int16_t(x + kCellStepX[direction]);
        z = std::int16_t(z + kCellStepZ[direction]);
    }
}

// ---- TripNodeHashMap ------------------------------------------------------------------------------------------------------

// 0x1000d4dd4: one zeroed block of 0x1000 nodes to start with.
TripNodeHashMap::TripNodeHashMap() {
    blocks_.emplace_back(new TripNode[kBlockNodes]());
    next_ = blocks_[0].get();
    blockEnd_ = next_ + kBlockNodes;
}
TripNodeHashMap::~TripNodeHashMap() = default;   // 0x1000d4e86

// 0x1000d2468: the node for `key`, created (zeroed, location parsed) at the head of its bucket if missing.
TripNode* TripNodeHashMap::GetNode(std::uint32_t key) {
    const std::uint32_t bucket = std::uint32_t((std::uint64_t(key) * 0x9e3779b1ull) % kBuckets);
    for (TripNode* n = buckets_[bucket]; n; n = n->chain)
        if (n->key == key) return n;
    if (next_ == blockEnd_) {
        ++blockIndex_;
        if (blocks_.size() == std::size_t(blockIndex_)) blocks_.emplace_back(new TripNode[kBlockNodes]());
        next_ = blocks_[std::size_t(blockIndex_)].get();
        blockEnd_ = next_ + kBlockNodes;
    }
    TripNode* node = next_++;
    node->chain = buckets_[bucket];
    buckets_[bucket] = node;
    node->bucket = bucket;
    node->key = key;
    node->inQueue = 0;
    node->closed = 0;
    ParseTripLocation(key, node->travelType, node->x, node->z);
    return node;
}

// 0x1000d169c: zero every block (the used ones keep their memory), rewind to block 0 and empty the buckets.
void TripNodeHashMap::Clear() {
    for (std::size_t b = 0; b < blocks_.size() && int(b) <= blockIndex_; ++b) std::fill(blocks_[b].get(), blocks_[b].get() + kBlockNodes, TripNode{});
    next_ = blocks_[0].get();
    blockEnd_ = next_ + kBlockNodes;
    blockIndex_ = 0;
    std::fill(std::begin(buckets_), std::end(buckets_), nullptr);
}

// ---- TripNodePriorityQueue ------------------------------------------------------------------------------------------------

// 0x1000d4d44: 100 slots, slot 0 unused.
TripNodePriorityQueue::TripNodePriorityQueue() : heap_(100, nullptr), count_(0), capacity_(99) {}

// 0x1000d25bc
void TripNodePriorityQueue::Push(TripNode* node) {
    std::uint32_t index = ++count_;
    if (capacity_ < index) {
        heap_.resize(std::size_t(index) * 2, nullptr);
        capacity_ = std::uint32_t(heap_.size()) - 1;
    }
    for (;;) {
        const std::uint32_t parent = index >> 1;
        if (parent == 0) break;
        TripNode* above = heap_[parent];
        if (above->score <= node->score) break;
        heap_[index] = above;
        above->heapIndex = index;
        index = parent;
    }
    heap_[index] = node;
    node->heapIndex = index;
}

// 0x1000d375c: move the last node to the top and sift it down (the smaller child wins, the right child only when strictly smaller).
TripNode* TripNodePriorityQueue::Pop() {
    TripNode* top = heap_[1];
    if (count_ == 1) {
        count_ = 0;
        return top;
    }
    heap_[1] = heap_[count_];
    heap_[1]->heapIndex = 1;
    --count_;
    if (count_ > 1) {
        std::uint32_t i = 1;
        const std::uint32_t n = count_;
        do {
            std::uint32_t left = i * 2;
            if (n < left) return top;
            TripNode* current = heap_[i];
            std::uint32_t best = left;
            if (current->score <= heap_[left]->score) best = i;
            const std::uint32_t right = left | 1;
            if (right <= n && heap_[right]->score < heap_[best]->score) best = right;
            if (best == i) return top;
            heap_[i] = heap_[best];
            heap_[i]->heapIndex = i;
            heap_[best] = current;
            current->heapIndex = best;
            i = best;
        } while (i < n);
    }
    return top;
}

// 0x1000d3c72
void TripNodePriorityQueue::UpdateNode(TripNode* node) {
    std::uint32_t index = node->heapIndex;
    for (;;) {
        const std::uint32_t parent = index >> 1;
        if (parent == 0) break;
        TripNode* above = heap_[parent];
        if (above->score <= node->score) break;
        heap_[index] = above;
        above->heapIndex = index;
        index = parent;
    }
    heap_[index] = node;
    node->heapIndex = index;
}

// ---- cSC4PathFinder -------------------------------------------------------------------------------------------------------

cSC4PathFinder::cSC4PathFinder(IPathFinderHost* host) : host_(host) {}   // 0x1000d0c62

bool cSC4PathFinder::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cISC4PathFinder) return false;
    if (out) *out = static_cast<cISC4PathFinder*>(this);
    AddRef();
    return true;
}
std::uint32_t cSC4PathFinder::AddRef() { return ++refCount_; }
// 0x1000d0eb6: the last reference shuts the finder down and destroys it.
std::uint32_t cSC4PathFinder::Release() {
    if (refCount_ < 2) {
        if (initialized_) Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// 0x1000d0ef2: reads the city size and sizes the two distance tables (width + 2 / depth + 2 entries, element 1 is cell 0).
bool cSC4PathFinder::Init() {
    if (initialized_) return false;
    initialized_ = true;
    cityWidth_ = host_->CityWidthInCells();
    cityDepth_ = host_->CityDepthInCells();
    xDistance_.assign(std::size_t(cityWidth_) + 2, 0);
    zDistance_.assign(std::size_t(cityDepth_) + 2, 0);
    return true;
}

// 0x1000d10ca
bool cSC4PathFinder::Shutdown() {
    if (!initialized_) return false;
    initialized_ = false;
    ReleasePathNodes();
    return true;
}

// 0x1000d1664
void cSC4PathFinder::ReleasePathNodes() {
    nodes_.Clear();
    while (!queue_.Empty()) queue_.Pop();
    goal_ = nullptr;
}

// 0x1000d5190 / 0x1000d51ae
void cSC4PathFinder::SetTravelTypeFlag(std::uint16_t flag, bool alsoStart) {
    std::uint16_t allowed = std::uint16_t(travelTypes_ & 0xffff) | flag;
    std::uint16_t start = std::uint16_t(travelTypes_ >> 16);
    if (alsoStart) start = std::uint16_t(start | flag);
    travelTypes_ = std::uint32_t(allowed) | (std::uint32_t(start) << 16);
}
void cSC4PathFinder::ClearTravelTypeFlag(std::uint16_t flag) {
    std::uint16_t allowed = std::uint16_t((travelTypes_ & 0xffff) & ~flag);
    std::uint16_t start = std::uint16_t((travelTypes_ >> 16) & ~flag);
    travelTypes_ = std::uint32_t(allowed) | (std::uint32_t(start) << 16);
}

// 0x1000d1606: does the destination lot still have free trip capacity of this kind? Records how many trips fit and the kind.
// pathfinder.md §4.10.1: max = GetMaxTripCapacity(holder, k) (slot 20), used = the arrivals this cycle; the capacity is only
// looked at here (the caller consumes it when it applies the trips).
bool cSC4PathFinder::CheckDestinationCapacity(cISCPropertyHolder* holder, std::uint8_t kind) {
    const std::uint32_t limit = host_->MaxTripCapacity(holder, kind);
    if (limit == 0) return false;
    const std::uint32_t used = host_->UsedTripCapacity(holder, kind);
    if (used >= limit) return false;
    trafficArrived_ = std::min(trafficDensity_, limit - used);
    capacityKind_ = kind;
    return true;
}

namespace {
// One axis of SetupDistanceTables: element i + 1 holds the distance of coordinate i to [lo, hi]; entries are clamped to the table
// (the original writes past it when the rectangle leaves the city).
void FillAxis(std::vector<int>& table, int lo, int hi, int size) {
    auto at = [&](int coordinate) -> int* {
        const long index = long(coordinate) + 1;
        return index >= 0 && index < long(table.size()) ? &table[std::size_t(index)] : nullptr;
    };
    int next = lo;
    if (lo >= 0) {
        for (int c = -1; c < lo; ++c)
            if (int* p = at(c)) *p = lo - c;
    } else {
        next = -1;
    }
    for (int c = next; c <= hi; ++c)
        if (int* p = at(c)) *p = 0;
    next = hi + 1;
    for (int c = next; c < size + 1 && c <= size; ++c)
        if (int* p = at(c)) *p = c - hi;
}
}  // namespace

// 0x1000d3556: the heuristic tables - how many cells each x / z lies from the end rectangle.
void cSC4PathFinder::SetupDistanceTables() {
    FillAxis(xDistance_, endRect_.x0, endRect_.x1, cityWidth_);
    FillAxis(zDistance_, endRect_.z0, endRect_.z1, cityDepth_);
}

// 0x1000d364c: recompute every allocated node's score from its cost and the (new) heuristic; queued nodes sift up.
void cSC4PathFinder::RecalculateNodeScores() {
    nodes_.ForEachAllocated([&](TripNode* n) {
        float h = 0.0f;
        const bool outsideX = std::uint32_t(nearRect_.x1 - nearRect_.x0) < std::uint32_t(n->x - nearRect_.x0);
        const bool outsideZ = std::uint32_t(nearRect_.z1 - nearRect_.z0) < std::uint32_t(n->z - nearRect_.z0);
        if (outsideX || outsideZ) h = float(zDistance_[std::size_t(n->z + 1)] + xDistance_[std::size_t(n->x + 1)]) * heuristicWeight_;
        n->score = h + n->cost;
        if (n->inQueue) queue_.UpdateNode(n);
    });
}

// 0x1000d3976: direction code from one cell to another (0 west, 1 north, 2 east, 3 south).
std::uint8_t cSC4PathFinder::EdgeTo(int fromX, int fromZ, int toX, int toZ) {
    if (toX < fromX) return 0;
    if (toX > fromX) return 2;
    return fromZ <= toZ ? 3 : 1;
}

// 0x1000d3994: relax the state (x, z, direction, travel type) reached from `parent`.
void cSC4PathFinder::AddTripNode(int x, int z, std::uint8_t direction, std::uint8_t travelType, float stepCost, TripNode* parent,
                                 float limit, bool outside) {
    if ((blockedTypes_ >> (travelType & 0x1f)) & 1) return;
    std::uint32_t key = std::uint32_t(z & 0xff) | (std::uint32_t(x & 0xff) << 8) | (std::uint32_t(direction) << 24) |
                        (std::uint32_t(travelType) << 16) | (std::uint32_t(parent->direction) << 26);
    if (outside) key |= 0x80000000u;
    if (parent->parent && parent->parent->key == key) return;   // straight back where we came from
    const float newCost = stepCost + parent->cost;
    if (newCost <= limit || (tripFlags_ & 1)) {
        TripNode* node = nodes_.GetNode(key);
        if ((!node->inQueue || newCost < node->cost) && (!node->closed || newCost < node->cost)) {
            node->parent = parent;
            node->closed = 0;
            node->cost = newCost;
            float h = 0.0f;
            if (x < nearRect_.x0 || z < nearRect_.z0 || nearRect_.x1 < x || nearRect_.z1 < z)
                h = float(zDistance_[std::size_t(z + 1)] + xDistance_[std::size_t(x + 1)]);
            node->score = newCost + h * heuristicWeight_;
            node->direction = direction;
            if (node->inQueue) {
                queue_.UpdateNode(node);
            } else {
                queue_.Push(node);
                node->inQueue = 1;
            }
        }
    } else {
        // over the cost limit: count it, and after 64 such nodes stop trying this travel type
        const std::uint32_t seen = ++congestion_[travelType];
        if (seen > 0x40) blockedTypes_ = std::uint8_t(blockedTypes_ | (1u << (travelType & 0x1f)));
    }
}

// 0x1000d3b32: distance (in cells, corner distances Euclidean) from a cell to a rectangle.
float cSC4PathFinder::DistanceToRectangle(int x, int z, SC4RectInt32 r) {
    if (!(x > r.x1 || x < r.x0 || z < r.z0 || z > r.z1)) return 0.0f;
    if (r.x0 <= x && x <= r.x1) return float(r.z0 <= z ? z - r.z1 : r.z0 - z);
    int cornerX;
    if (r.x1 < x) {
        if (r.z0 <= z && z <= r.z1) return float(x - r.x1);
        cornerX = r.x1;
    } else {
        cornerX = r.x0;
        if (r.z0 <= z && z <= r.z1) return float(r.x0 - x);
    }
    const int cornerZ = z < r.z0 ? r.z0 : r.z1;
    const float a[3] = {float(x), 0.0f, float(z)};
    const float b[3] = {float(cornerX), 0.0f, float(cornerZ)};
    return SC4GeometryUtilities::LengthOfLine(a, b);
}

// 0x1000d382c: the goal node has been reached - total cost, where the walk changes from foot (type 1) to a vehicle, the cost
// of the start node taken off, neighbour-connection toll, and the route as a list of cells.
void cSC4PathFinder::DoSuccessfulTrip() {
    TripNode* goal = goal_;
    tripLength_ = goal->cost;
    TripNode* cur = goal;
    for (TripNode* p = goal->parent; p; p = p->parent) {
        if (p->travelType == 1 && cur->travelType != 1) {
            reversalX_ = cur->x;
            reversalZ_ = cur->z;
            TripNode* next = cur->parent;
            std::uint8_t direction;
            if (next->x < cur->x) direction = 0;
            else if (next->x > cur->x) direction = 2;
            else direction = cur->z <= next->z ? 3 : 1;
            reversalDirection_ = direction;
        }
        cur = p;
    }
    float length = tripLength_ - cur->cost;
    if (length < 0.0f) length = 0.0f;
    tripLength_ = length;
    if ((destinationFilter_ == 0 || destinationFilter_ == 3) && capacityKind_ != 3) {   // Windows: filters 0 and 3 (§4.13)
        if (std::uint32_t(int(goal->x)) >= std::uint32_t(cityWidth_) || std::uint32_t(int(goal->z)) >= std::uint32_t(cityDepth_)) {
            float toll = 0.0f;
            if (host_->NeighborConnectionCost(goal->x, goal->z, toll)) tripLength_ += toll;
        }
    }
    if (route_) {
        route_->clear();
        for (TripNode* n = goal; n; n = n->parent) {
            RouteData data;
            data.x = std::uint8_t(n->x);
            data.z = std::uint8_t(n->z);
            data.travelType = n->travelType;
            route_->push_front(data);
        }
    }
}

// ---- the search (pathfinder.md §4.3 - §4.16) --------------------------------------------------------------------------------

namespace {
constexpr int kNetworkOfType[9] = {0, 1, 1, 2, 1, 2, 3, 4, 5};
constexpr bool kProhibitSharpTurns[9] = {false, false, false, true, false, true, false, true, true};
constexpr bool kLegalStartingSubtype[13] = {true, true, false, true, false, false, true, true, true, true, true, true, false};
constexpr bool kLegalEndingSubtype[13] = {true, false, false, true, false, false, true, false, false, false, true, true, false};
const float kZeroCosts[9] = {};

bool InRect(const SC4RectInt32& r, int x, int z) { return r.x0 <= x && x <= r.x1 && r.z0 <= z && z <= r.z1; }
}  // namespace

// §4.4: 0 inside the search rect (start rect + 4), else the integer distance of the tables.
float cSC4PathFinder::Heuristic(int x, int z) const {
    if (InRect(nearRect_, x, z)) return 0.0f;
    auto axis = [](const std::vector<int>& table, int lo, int hi, int i) {
        const long index = long(i) + 1;
        if (index >= 0 && index < long(table.size())) return table[std::size_t(index)];
        return i < lo ? lo - i : (i > hi ? i - hi : 0);
    };
    return float(axis(xDistance_, endRect_.x0, endRect_.x1, x) + axis(zDistance_, endRect_.z0, endRect_.z1, z));
}

// §4.5: the nearest cell, ring by ring around the start rect, whose destination byte has a wanted type; its lot becomes the end
// rect (an off-map winner: the cell itself).
bool cSC4PathFinder::FindNearestStandardDest() {
    bool any = false;
    for (int k = 0; k < 4; ++k)
        if (((destinationFlags_ >> k) & 1) && host_->FreeDestinations(k) != 0) any = true;
    if (!any) return false;
    SC4RectInt32 bounds{-1, -1, cityWidth_, cityDepth_};
    if (startRect_.x0 < 0) { bounds.x0 = 0; excludeRect_ = {-1, -1, -1, cityDepth_}; }
    else if (startRect_.x1 == cityWidth_) { bounds.x1 = cityWidth_ - 1; excludeRect_ = {cityWidth_, -1, cityWidth_, cityDepth_}; }
    else if (startRect_.z0 < 0) { bounds.z0 = 0; excludeRect_ = {-1, -1, cityWidth_, -1}; }
    else if (startRect_.z1 == cityDepth_) { bounds.z1 = cityDepth_ - 1; excludeRect_ = {-1, cityDepth_, cityWidth_, cityDepth_}; }
    SC4RectInt32 r = startRect_;
    std::vector<std::pair<int, int>> candidates;
    for (;;) {
        const bool left = r.x0 > bounds.x0, right = r.x1 < bounds.x1, top = r.z0 > bounds.z0, bottom = r.z1 < bounds.z1;
        if (left) --r.x0;
        if (right) ++r.x1;
        if (top) --r.z0;
        if (bottom) ++r.z1;
        if (!left && !right && !top && !bottom) return false;
        candidates.clear();
        auto consider = [&](int x, int z) {
            if ((host_->DestinationByte(x, z) & destinationFlags_) == 0 || InRect(excludeRect_, x, z)) return;
            candidates.emplace_back(x, z);
        };
        if (left) for (int z = r.z0; z <= r.z1; ++z) consider(r.x0, z);
        if (right) for (int z = r.z0; z <= r.z1; ++z) consider(r.x1, z);
        if (top) for (int x = r.x0; x <= r.x1; ++x) consider(x, r.z0);
        if (bottom) for (int x = r.x0; x <= r.x1; ++x) consider(x, r.z1);
        if (candidates.empty()) continue;
        auto inCity = [&](int x, int z) { return unsigned(x) < unsigned(cityWidth_) && unsigned(z) < unsigned(cityDepth_); };
        bool anyInCity = false;
        for (auto& c : candidates) anyInCity = anyInCity || inCity(c.first, c.second);
        int best = -1, bestDistance = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const int x = candidates[i].first, z = candidates[i].second;
            if (anyInCity && !inCity(x, z)) continue;
            const int dx = x < startRect_.x0 ? startRect_.x0 - x : (x > startRect_.x1 ? x - startRect_.x1 : 0);
            const int dz = z < startRect_.z0 ? startRect_.z0 - z : (z > startRect_.z1 ? z - startRect_.z1 : 0);
            if (best < 0 || dx + dz < bestDistance) { best = int(i); bestDistance = dx + dz; }
        }
        const int x = candidates[std::size_t(best)].first, z = candidates[std::size_t(best)].second;
        if (inCity(x, z)) {
            cISCPropertyHolder* holder = nullptr;
            SC4RectInt32 lot;
            if (!host_->LotAtCell(x, z, lot, holder)) return false;
            endRect_ = lot;
        } else {
            endRect_ = {x, z, x, z};
        }
        return true;
    }
}

// §4.6: rings 1..MaxDistanceOriginToNetwork around the start rect (clamped to the city, cells diagonal to the rect skipped), one
// start node per (cell, starting travel type, allowed entry side with exits). The ferry-arrival fallback for an off-map start is
// not modelled (no ferry terminals natively).
bool cSC4PathFinder::CreateStartNodes() {
    const float* costs = startingCosts_ ? startingCosts_ : host_->DefaultStartingCosts();
    if (!costs) costs = kZeroCosts;
    const int x0 = startRect_.x0, z0 = startRect_.z0;
    const bool startInCity = unsigned(x0) < unsigned(cityWidth_) && unsigned(z0) < unsigned(cityDepth_);
    bool sides[4] = {true, true, true, true};
    if (!startInCity) {
        int only = 3;
        if (x0 < 0) only = 0;
        else if (x0 >= cityWidth_) only = 2;
        else if (z0 < 0) only = 1;
        for (int s = 0; s < 4; ++s) sides[s] = s == only;
    }
    SC4RectInt32 lotRect;
    cISCPropertyHolder* lotHolder = nullptr;
    const bool lotStart = startInCity && host_->LotAtCell(x0, z0, lotRect, lotHolder);
    const std::uint16_t startMask = StartTravelTypes();
    bool created = false;
    auto visit = [&](int cx, int cz) {
        const bool inX = startRect_.x0 <= cx && cx <= startRect_.x1, inZ = startRect_.z0 <= cz && cz <= startRect_.z1;
        if (!inX && !inZ) return;
        for (int n = 0; n < 6; ++n) {
            float inverse = 0, extra = 0;
            std::uint8_t type = 0;
            if (!host_->CellNetwork(n, cx, cz, inverse, type, extra)) continue;
            if (lotStart && !(type < 13 && kLegalStartingSubtype[type])) continue;
            for (int t = 0; t < 9; ++t) {
                if (!((startMask >> t) & 1) || kNetworkOfType[t] != n) continue;
                const std::uint16_t conn = host_->Connections(t, cx, cz);
                for (int s = 0; s < 4; ++s) {
                    if (!sides[s] || ((conn >> (4 * s)) & 0xf) == 0) continue;
                    const std::uint32_t key = ((0x10u | std::uint32_t(s)) << 24) | (std::uint32_t(t) << 16) | (std::uint32_t(cx & 0xff) << 8) |
                                              std::uint32_t(cz & 0xff);
                    TripNode* node = nodes_.GetNode(key);
                    node->cost = costs[t];
                    node->score = Heuristic(cx, cz) * heuristicWeight_ + node->cost;
                    node->parent = nullptr;
                    node->closed = 0;
                    node->direction = std::uint8_t(s);
                    node->inQueue = 1;
                    queue_.Push(node);
                    created = true;
                }
            }
        }
    };
    SC4RectInt32 ring = startRect_;
    const std::uint32_t rings = host_->MaxDistanceOriginToNetwork();
    for (std::uint32_t d = 1; d <= rings; ++d) {
        ring.x0 = std::max(ring.x0 - 1, 0);
        ring.z0 = std::max(ring.z0 - 1, 0);
        ring.x1 = std::min(ring.x1 + 1, cityWidth_ - 1);
        ring.z1 = std::min(ring.z1 + 1, cityDepth_ - 1);
        for (int x = ring.x0; x <= ring.x1; ++x) visit(x, ring.z0);
        for (int z = ring.z0 + 1; z < ring.z1; ++z) {
            visit(ring.x0, z);
            if (ring.x1 != ring.x0) visit(ring.x1, z);
        }
        if (ring.z1 != ring.z0)
            for (int x = ring.x0; x <= ring.x1; ++x) visit(x, ring.z1);
    }
    return created;
}

// §4.10: filter 0 = a neighbouring lot (or an off-map pool) with free capacity of a wanted type; filter 3 = the end rect or a cell
// next to it; filters 1, 2 (no occupant filter can be set) and 4 never end.
bool cSC4PathFinder::AtGoal(TripNode* node, bool cannotEnd) {
    const int x = node->x, z = node->z;
    auto inCity = [&](int cx, int cz) { return unsigned(cx) < unsigned(cityWidth_) && unsigned(cz) < unsigned(cityDepth_); };
    if (destinationFilter_ == 0) {
        if (inCity(x, z) || InRect(startRect_, x, z) || InRect(excludeRect_, x, z)) {
            if (cannotEnd) return false;
            for (int d = 0; d < 4; ++d) {
                const int nx = x + kCellStepX[d], nz = z + kCellStepZ[d];
                if (!inCity(nx, nz) || InRect(startRect_, nx, nz)) continue;
                const std::uint8_t bits = host_->DestinationByte(nx, nz);
                if ((bits & destinationFlags_) == 0) continue;
                SC4RectInt32 lot;
                cISCPropertyHolder* holder = nullptr;
                if (!host_->LotAtCell(nx, nz, lot, holder) || !holder) continue;
                for (int k = 0; k < 4; ++k) {
                    if (!(((bits & destinationFlags_) >> k) & 1)) continue;
                    if (CheckDestinationCapacity(holder, std::uint8_t(k))) {
                        endCellX_ = nx;
                        endCellZ_ = nz;
                        return true;
                    }
                    for (int lx = lot.x0; lx <= lot.x1; ++lx)
                        for (int lz = lot.z0; lz <= lot.z1; ++lz)
                            host_->SetDestinationByte(lx, lz, std::uint8_t(host_->DestinationByte(lx, lz) & ~(1u << k)));
                    if (host_->FreeDestinations(k) != 0) host_->DecrementFreeDestinations(k);
                }
            }
            return false;
        }
        cISCPropertyHolder* pool = host_->NeighborPool(node->travelType, x, z);
        if (!pool) return false;
        const std::uint8_t bits = host_->DestinationByte(x, z);
        for (int k = 0; k < 4; ++k) {
            if (!(((bits & destinationFlags_) >> k) & 1)) continue;
            if (CheckDestinationCapacity(pool, std::uint8_t(k))) {
                endCellX_ = x;
                endCellZ_ = z;
                return true;
            }
            host_->SetDestinationByte(x, z, std::uint8_t(host_->DestinationByte(x, z) & ~(1u << k)));
        }
        return false;
    }
    if (destinationFilter_ == 3) {
        if (InRect(endRect_, x, z) && (!cannotEnd || !inCity(x, z))) {
            endCellX_ = x;
            endCellZ_ = z;
            trafficArrived_ = trafficDensity_;
            return true;
        }
        if (inCity(endRect_.x0, endRect_.z0)) {
            for (int d = 0; d < 4; ++d) {
                const int nx = x + kCellStepX[d], nz = z + kCellStepZ[d];
                if (InRect(endRect_, nx, nz) && (!cannotEnd || !inCity(nx, nz))) {
                    endCellX_ = nx;
                    endCellZ_ = nz;
                    trafficArrived_ = trafficDensity_;
                    return true;
                }
            }
        }
    }
    return false;
}

// §4.4 / §4.8: modified A* from the start nodes; 1 found, 0 not found, 2 suspended (only with a max work time).
int cSC4PathFinder::FindPath() {
    if (!initialized_) return 0;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(maxWorkTime_);
    trafficArrived_ = 0;
    if (!(tripFlags_ & 8)) reversalX_ = reversalZ_ = -2;
    const float* costs = startingCosts_ ? startingCosts_ : host_->DefaultStartingCosts();
    if (!costs) costs = kZeroCosts;
    for (int t = 0; t < 9; ++t) costLimit_[t] = costs[t] + tripLength_;
    if (!resume_) {
        if (!(tripFlags_ & 4)) {
            excludeRect_ = {-2, -2, -2, -2};
            ReleasePathNodes();
        }
        goal_ = nullptr;
        std::fill(std::begin(congestion_), std::end(congestion_), 0u);
        blockedTypes_ = 0;
    }
    const bool strike = host_->StrikeOn();
    const std::uint16_t switchMask = strike ? std::uint16_t(TravelTypes() & 0x33) : TravelTypes();
    if (!resume_) {
        nearRect_ = {startRect_.x0 - 4, startRect_.z0 - 4, startRect_.x1 + 4, startRect_.z1 + 4};
        if (destinationFilter_ == 0) {
            heuristicWeight_ = host_->NearestDestinationAttractiveness();
            if (!FindNearestStandardDest()) return 0;
            SetupDistanceTables();
            if (tripFlags_ & 4) RecalculateNodeScores();
        } else {
            SetupDistanceTables();
            heuristicWeight_ = destinationFilter_ == 3 ? host_->NearestDestinationAttractiveness() : 1.0f;
        }
        if (StartTravelTypes() == 0) return 0;
        if (!(tripFlags_ & 4) && !CreateStartNodes()) return 0;
    }
    resume_ = false;
    expanded_ = 0;
    std::uint32_t counter = 0;
    auto inCity = [&](int cx, int cz) { return unsigned(cx) < unsigned(cityWidth_) && unsigned(cz) < unsigned(cityDepth_); };
    // The step cost onto cell (bx, bz) for type t on network n (§4.8.2 a); false when the cell has no such network.
    auto stepCost = [&](int t, int bx, int bz, float& cost) {
        float inverse = 0, extra = 0;
        std::uint8_t type = 0;
        if (!host_->CellNetwork(kNetworkOfType[t], bx, bz, inverse, type, extra)) return false;
        const float v = host_->InverseMaxSpeed(t, type);
        if (host_->AffectedByTraffic(t)) {
            const float q = host_->Volume(kNetworkOfType[t], bx, bz) * inverse;
            cost = host_->CongestionSpeedFactor(q) * v + extra;
        } else {
            cost = v + extra;
        }
        return true;
    };
    while (!queue_.Empty()) {
        TripNode* c = queue_.Pop();
        const int t = c->travelType, x = c->x, z = c->z, s = c->direction & 3;
        const bool here = inCity(x, z);
        // §4.8.1 goal test (skipped on a transit switch cell)
        const bool canReach = t < 9 && host_->CanReachDestination(t);
        bool cannotEnd = !canReach;
        if (canReach && here) {
            float inverse = 0, extra = 0;
            std::uint8_t type = 0;
            if (host_->CellNetwork(kNetworkOfType[t], x, z, inverse, type, extra)) cannotEnd = !(type < 13 && kLegalEndingSubtype[type]);
        }
        if (!(here && host_->TransitSwitchId(x, z) != 0) && AtGoal(c, here ? cannotEnd : !canReach)) {
            goal_ = c;
            queue_.Push(c);
            DoSuccessfulTrip();
            return 1;
        }
        if (here && t < 9) {
            ++expanded_;
            // §4.8.2 moves along the network
            TripNode* p = c->parent;
            std::uint8_t exits = std::uint8_t((host_->Connections(t, x, z) >> (4 * s)) & 0xf);
            if (kProhibitSharpTurns[t] && p) exits = std::uint8_t(exits & ~((1u << s) | (1u << (p->direction & 3))));
            const bool carCheck = kNetworkOfType[t] == 1 && p;
            const std::uint16_t prior = carCheck ? host_->PriorCar(s, x, z) : 0;
            const std::uint16_t switches = host_->Switches(t, x, z);
            for (int d = 0; d < 4; ++d) {
                const int bx = x + kCellStepX[d], bz = z + kCellStepZ[d];
                const std::uint8_t entry = std::uint8_t((d + 2) & 3);
                if ((exits >> d) & 1) {
                    if (inCity(bx, bz)) {
                        float cost = 0;
                        if (stepCost(t, bx, bz, cost)) {
                            if (!carCheck || ((prior >> (4 * d + (p->direction & 3))) & 1))
                                AddTripNode(bx, bz, entry, std::uint8_t(t), cost, c, costLimit_[t], false);
                        } else if (const std::uint32_t id = host_->TransitSwitchId(bx, bz)) {
                            AddTripNode(bx, bz, entry, std::uint8_t(t), host_->TransitSwitchCost(id), c, costLimit_[t], false);
                        }
                    } else {
                        // leaving the map: an exit node keyed on this cell, its cost the node's own heuristic term
                        AddTripNode(x, z, std::uint8_t(d), std::uint8_t(t), c->score - c->cost, c, costLimit_[t], true);
                    }
                }
                // (b) switch moves
                const int t2 = (switches >> (4 * d)) & 0xf;
                if (t2 > 8 || !((switchMask >> t2) & 1) || !inCity(bx, bz)) continue;
                if (const std::uint32_t id = host_->TransitSwitchId(bx, bz)) {
                    AddTripNode(bx, bz, entry, std::uint8_t(t2), host_->TransitSwitchCost(id), c, costLimit_[t2], false);
                } else {
                    float cost = 0;
                    if (stepCost(t2, bx, bz, cost)) AddTripNode(bx, bz, entry, std::uint8_t(t2), cost, c, costLimit_[t2], false);
                }
            }
            // §4.8.3 wormholes: tunnels and ferries are not modelled natively yet; the subway-station bit is never set.
            // §4.8.4 park-and-ride return
            if ((tripFlags_ & 8) && x == reversalX_ && z == reversalZ_) {
                const int bx = reversalX_ + kCellStepX[reversalDirection_ & 3], bz = reversalZ_ + kCellStepZ[reversalDirection_ & 3];
                AddTripNode(bx, bz, EdgeTo(reversalX_, reversalZ_, bx, bz), 1, 0.0f, c, costLimit_[t], !inCity(bx, bz));
            }
        }
        // §4.8.5 close and time check
        c->closed = 1;
        c->inQueue = 0;
        if (maxWorkTime_ != 0) {
            counter += 1024;
            if ((counter & 0x3ff) == 0 && Clock::now() >= deadline) {
                resume_ = true;
                return 2;
            }
        }
    }
    // §4.12 exhausted: with flag 2, filter 3 and a route deque, the stored node nearest the end rect
    if ((tripFlags_ & 2) && destinationFilter_ == 3 && route_) {
        TripNode* best = nullptr;
        float bestDistance = 0;
        nodes_.ForEachBucketNode([&](TripNode* n) {
            const float d = DistanceToRectangle(n->x, n->z, endRect_);
            if (!best || d < bestDistance) { best = n; bestDistance = d; }
        });
        if (best) {
            goal_ = best;
            DoSuccessfulTrip();
            trafficArrived_ = 0;
            return 1;
        }
    }
    return 0;
}

// §4.16: can the route be driven backwards (then it is counted in both directions)?
bool cSC4PathFinder::WasTripReversible() {
    if (!goal_) return true;
    auto inCity = [&](int cx, int cz) { return unsigned(cx) < unsigned(cityWidth_) && unsigned(cz) < unsigned(cityDepth_); };
    int e = -1;
    int next = goal_->travelType;
    for (TripNode* c = goal_; c->parent; c = c->parent) {
        TripNode* p = c->parent;
        const bool tunnels = inCity(c->x, c->z) && inCity(p->x, p->z) && (host_->Wormhole(c->x, c->z) & 2) && (host_->Wormhole(p->x, p->z) & 2);
        if (!inCity(c->x, c->z) || !inCity(p->x, p->z) || (c->x == p->x && c->z == p->z) || tunnels) {
            e = -1;
        } else {
            if (e != -1 && c->travelType < 9) {
                if (next != c->travelType) {
                    if (((host_->Switches(c->travelType, c->x, c->z) >> (4 * e)) & 0xf) == 0xf) return false;
                } else if (p->travelType == c->travelType) {
                    if (!((host_->Connections(c->travelType, c->x, c->z) >> (4 * e + (c->direction & 3))) & 1)) return false;
                }
            }
            // EdgeTo(c -> p): the side of p through which a reverse trip enters it
            e = c->x < p->x ? 0 : (c->x > p->x ? 2 : (c->z >= p->z ? 3 : 1));
        }
        next = c->travelType;
    }
    return true;
}

// §4.15: header (root type, x, z), then runs of unit steps [t | 0x40, count, packed 2-bit codes] and jumps [t | f, x, z].
void cSC4PathFinder::GetCompressedRoute(std::vector<std::uint8_t>& out, std::uint32_t& travelTypes) {
    out.clear();
    travelTypes = 0;
    if (!goal_) return;
    std::vector<TripNode*> nodes;
    for (TripNode* n = goal_; n; n = n->parent) nodes.push_back(n);
    std::reverse(nodes.begin(), nodes.end());
    auto inCity = [&](int cx, int cz) { return unsigned(cx) < unsigned(cityWidth_) && unsigned(cz) < unsigned(cityDepth_); };
    // an off-map node is written as the in-city cell next to it (GetNeighborConnectionInnerCell)
    auto inner = [&](int& cx, int& cz) {
        if (cx < 0) cx = 0;
        else if (cx >= cityWidth_) cx = cityWidth_ - 1;
        else if (cz < 0) cz = 0;
        else if (cz >= cityDepth_) cz = cityDepth_ - 1;
    };
    TripNode* root = nodes.front();
    int hx = root->x, hz = root->z;
    if (!inCity(hx, hz)) inner(hx, hz);
    out.push_back(root->travelType);
    out.push_back(std::uint8_t(hx));
    out.push_back(std::uint8_t(hz));
    travelTypes |= 1u << (root->travelType & 0x1f);
    std::vector<std::uint8_t> run;
    std::uint8_t runType = 0;
    auto flush = [&]() {
        if (run.empty()) return;
        out.push_back(std::uint8_t(runType | 0x40));
        out.push_back(std::uint8_t(run.size()));
        for (std::size_t i = 0; i < run.size(); i += 4) {
            std::uint8_t packed = 0;
            for (std::size_t j = 0; j < 4 && i + j < run.size(); ++j) packed = std::uint8_t(packed | (run[i + j] << (2 * j)));
            out.push_back(packed);
        }
        run.clear();
    };
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        TripNode* prev = nodes[i - 1];
        TripNode* n = nodes[i];
        travelTypes |= 1u << (n->travelType & 0x1f);
        const int dx = n->x - prev->x, dz = n->z - prev->z;
        if (std::abs(dx) + std::abs(dz) == 1) {
            const std::uint8_t code = dx == -1 ? 0 : dz == -1 ? 1 : dx == 1 ? 2 : 3;
            if (!run.empty() && (n->travelType != runType || run.size() == 255)) flush();
            if (run.empty()) runType = n->travelType;
            run.push_back(code);
        } else {
            flush();
            const bool ferry = !inCity(prev->x, prev->z) || (host_->Wormhole(prev->x, prev->z) & 4);
            int jx = n->x, jz = n->z;
            if (!inCity(jx, jz)) inner(jx, jz);
            out.push_back(std::uint8_t(n->travelType | (ferry ? 0x80 : 0)));
            out.push_back(std::uint8_t(jx));
            out.push_back(std::uint8_t(jz));
        }
    }
    flush();
}

}  // namespace sc4
