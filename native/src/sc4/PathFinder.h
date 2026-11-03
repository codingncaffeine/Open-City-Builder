// cSC4PathFinder — the traffic simulator's trip search (interface cISC4PathFinder, iid 0xaa0db766; 55 functions,
// 0x1000d0c62..0x1000d5258 plus the TripNodeHashMap / TripNodePriorityQueue helpers). It runs an A*-style search over
// (cell, travel type, direction) states from a start rectangle to a destination (a lot with free trip capacity, a neighbour
// connection, or a rectangle) and then applies the traffic of the found trip to the network.
//
// Recovered here: the object state and all the setters / getters, Init / Shutdown, the TripNode pool and hash map, the binary-heap
// priority queue, the distance tables, node scoring, AddTripNode, DoSuccessfulTrip and CheckDestinationCapacity.
// NOT recovered (they read the traffic simulator's per-cell connection tables at +0x98, tunnel / ferry maps, congestion curve and
// the lot / occupant managers, none of which exist in src/sc4/TrafficSimulator.*): AtGoal, FindNearestStandardDest, CreateStartNodes,
// FindPath, WasTripReversible, ApplyTraffic, GetCompressedRoute - see the PATHFINDER-* to-do rows.
//
// Original layout of the 0x61e0-byte object: +8 initialised, +0xc refcount, +0x10 traffic simulator, +0x18 goal TripNode, +0x20 heuristic
// weight (float), +0x24 start rect (x0 z0 x1 z1), +0x34 end rect, +0x44 "near start" rect (start rect grown by 4), +0x54 excluded rect,
// +0x64/+0x68 end cell, +0x6c max trip length / trip length (float), +0x70 travel-type mask (ushort) and +0x72 start travel types,
// +0x74 traffic density, +0x78 traffic arrived, +0x7c trip flags, +0x80 route deque*, +0x94 destination filter, +0x98 destination flags,
// +0xa0 destination filter object, +0xa8 destination capacity kind, +0xb0 starting cost array*, +0xb8 float[9] per-travel-type cost
// limits, +0xdc/+0xe0 city size, +0xe4 work time limit, +0xe8 resume flag, +0xec uint[?] congestion counters, +0x110 blocked-type
// mask, +0x112/+0x114 shorts + +0x116 byte (reversal point), +0x118 / +0x130 vector<int> axis distance tables (+0x148 / +0x150 point at
// element 1), +0x158 priority queue, +0x178 hash map.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "gz/cIGZUnknown.h"
#include "sc4/MiscTypes.h"

class cISCPropertyHolder;

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4PathFinder = 0xaa0db766;
constexpr int kCellStepX[4] = {-1, 0, 1, 0};   // kDX 0x100a170f0
constexpr int kCellStepZ[4] = {0, -1, 0, 1};   // kDZ 0x100a17100

// One search state (0x30 bytes). key: bits 0..7 z, 8..15 x, 16..23 travel type, 24..25 direction, 26.. parent's direction,
// bit 28 = start node, bit 31 = the node sits one step (kCellStepX/Z[direction]) outside the key's cell.
struct TripNode {
    std::uint32_t key = 0;            // +0
    TripNode* parent = nullptr;       // +8
    float cost = 0.0f;                // +0x10
    float score = 0.0f;               // +0x14 (cost + heuristic)
    std::uint8_t inQueue = 0;         // +0x18
    std::uint8_t closed = 0;          // +0x19
    std::uint8_t direction = 0;       // +0x1a
    std::uint8_t travelType = 0;      // +0x1b
    std::int16_t x = 0, z = 0;        // +0x1c / +0x1e
    TripNode* chain = nullptr;        // +0x20 next node of the bucket
    std::uint32_t bucket = 0;         // +0x28
    std::uint32_t heapIndex = 0;      // +0x2c
};

// 0x1000d3ccc: splits a node key into travel type, x and z (bit 31 shifts the cell one step in the stored direction).
void ParseTripLocation(std::uint32_t key, std::uint8_t& travelType, std::int16_t& x, std::int16_t& z);

// 0x1000d4dd4 / 0x1000d2468 / 0x1000d169c: nodes live in 0x1000-node blocks that are zeroed once; 0xc07 chained buckets keyed by
// (key * 0x9e3779b1) % 0xc07.
class TripNodeHashMap {
public:
    TripNodeHashMap();
    ~TripNodeHashMap();
    TripNode* GetNode(std::uint32_t key);   // creates a zeroed node (inQueue = closed = 0) when missing
    void Clear();                            // zero every block and bucket, restart at the first block
    // all live nodes, bucket order (the original walks the bucket array and chains)
    template <class F> void ForEachBucketNode(F f) const {
        for (std::uint32_t b = 0; b < kBuckets; ++b)
            for (TripNode* n = buckets_[b]; n; n = n->chain) f(n);
    }
    // every node handed out since the last Clear, allocation order (RecalculateNodeScores walks the blocks like this)
    template <class F> void ForEachAllocated(F f) const {
        for (std::size_t b = 0; b < blocks_.size() && int(b) <= blockIndex_; ++b) {
            TripNode* begin = blocks_[b].get();
            TripNode* end = begin + kBlockNodes;
            if (int(b) == blockIndex_) end = next_;
            for (TripNode* n = begin; n != end; ++n) f(n);
        }
    }
    static constexpr std::uint32_t kBuckets = 0xc07;
    static constexpr std::uint32_t kBlockNodes = 0x1000;
private:
    TripNode* buckets_[kBuckets] = {};
    std::vector<std::unique_ptr<TripNode[]>> blocks_;
    TripNode* next_ = nullptr;      // +0x6058
    TripNode* blockEnd_ = nullptr;  // +0x6060
    int blockIndex_ = 0;            // +0x6068
};

// 0x1000d4d44 / 0x1000d25bc / 0x1000d375c / 0x1000d3c72: 1-based binary min-heap on TripNode::score (ties keep the older node
// above), nodes remember their heap slot.
class TripNodePriorityQueue {
public:
    TripNodePriorityQueue();
    void Push(TripNode* node);
    TripNode* Pop();               // the lowest score; the queue must not be empty
    void UpdateNode(TripNode* node);   // only sifts up (scores only ever decrease)
    std::uint32_t Size() const { return count_; }
    bool Empty() const { return count_ == 0; }
private:
    std::vector<TripNode*> heap_;
    std::uint32_t count_ = 0;      // +0x18
    std::uint32_t capacity_ = 0;   // +0x1c
};

struct RouteData { std::uint8_t x = 0, z = 0, travelType = 0; };

// What the path finder needs from the traffic simulator / city (SL::spCity, the traffic simulator at +0x10).
class IPathFinderHost {
public:
    virtual ~IPathFinderHost() = default;
    virtual int CityWidthInCells() = 0;                 // SL::spCity slot 0x2e8
    virtual int CityDepthInCells() = 0;                 // SL::spCity slot 0x2f0
    virtual float TripScaleForDisplay() = 0;            // traffic simulator slot 0x78
    virtual std::uint32_t TripCapacityLimit() = 0;      // traffic simulator slot 0xa0 (no arguments in the call)
    virtual std::uint32_t UsedTripCapacity(cISCPropertyHolder* holder, std::uint32_t kind) = 0;   // 0x1000885b8
    virtual bool NeighborConnectionCost(int x, int z, float& cost) = 0;    // GetTrafficNeighborConnection(x, z)->+4

    // ---- the traffic simulator's cell graph (traffic-network.md §2; pathfinder.md §4.1). Defaults: an empty city. ----
    virtual std::uint16_t Connections(int t, int x, int z) { (void)t; (void)x; (void)z; return 0; }      // conn[t] (in city)
    virtual std::uint16_t Switches(int t, int x, int z) { (void)t; (void)x; (void)z; return 0xffff; }    // switch[t]
    virtual std::uint16_t PriorCar(int side, int x, int z) { (void)side; (void)x; (void)z; return 0xffff; }
    virtual std::uint8_t Wormhole(int x, int z) { (void)x; (void)z; return 0; }
    // The first network-info record of transit network n at the cell: inverse capacity, piece type, extra step cost.
    virtual bool CellNetwork(int n, int x, int z, float& inverseCapacity, std::uint8_t& type, float& extraCost) {
        (void)n; (void)x; (void)z; (void)inverseCapacity; (void)type; (void)extraCost; return false;
    }
    virtual float Volume(int n, int x, int z) { (void)n; (void)x; (void)z; return 0.0f; }
    virtual std::uint32_t TransitSwitchId(int x, int z) { (void)x; (void)z; return 0; }
    virtual float TransitSwitchCost(std::uint32_t id) { (void)id; return 0.0f; }
    // The destination bitmask map, [x + 1][z + 1] (off-map ring included), and the free destination counts per type.
    virtual std::uint8_t DestinationByte(int x, int z) { (void)x; (void)z; return 0; }
    virtual void SetDestinationByte(int x, int z, std::uint8_t value) { (void)x; (void)z; (void)value; }
    virtual std::uint32_t FreeDestinations(int k) { (void)k; return 0; }
    virtual void DecrementFreeDestinations(int k) { (void)k; }
    // The lot covering a cell (GetLot(x, z, false)): its bounding rect and property holder.
    virtual bool LotAtCell(int x, int z, SC4RectInt32& rect, cISCPropertyHolder*& holder) { (void)x; (void)z; (void)rect; holder = nullptr; return false; }
    virtual cISCPropertyHolder* NeighborPool(int t, int x, int z) { (void)t; (void)x; (void)z; return nullptr; }
    virtual std::uint32_t MaxTripCapacity(cISCPropertyHolder* holder, std::uint32_t k) { (void)holder; (void)k; return 0; }
    // Per travel type t: 1 / max speed on network type `type`, and the three tuning flags.
    virtual float InverseMaxSpeed(int t, int type) { (void)t; (void)type; return 3.4028235e38f; }
    virtual bool AffectedByTraffic(int t) { (void)t; return true; }
    virtual bool CanReachDestination(int t) { (void)t; return true; }
    virtual const float* DefaultStartingCosts() { return nullptr; }
    virtual std::uint32_t MaxDistanceOriginToNetwork() { return 1; }
    virtual float NearestDestinationAttractiveness() { return 0.75f; }
    virtual float CongestionSpeedFactor(float ratio) { (void)ratio; return 1.0f; }   // the congestion -> inverse speed curve
    virtual bool StrikeOn() { return false; }
};

class cISC4PathFinder : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                          // 0x18
    virtual bool Shutdown() = 0;                                      // 0x20
    virtual void SetFlag(std::uint32_t flag) = 0;                     // 0x28
    virtual void ClearFlag(std::uint32_t flag) = 0;                   // 0x30
    virtual void ClearAllFlags() = 0;                                 // 0x38
    virtual void SetRouteInfo(std::deque<RouteData>* route) = 0;      // 0x40
    virtual void SetTripDestinationFilter(int filter) = 0;            // 0x48
    virtual void SetDestinationFlag(std::uint8_t flag) = 0;           // 0x50
    virtual void ClearDestinationFlag(std::uint8_t flag) = 0;         // 0x58
    virtual void ClearAllDestinationFlags() = 0;                      // 0x60
    virtual void SetTravelTypeFlag(std::uint16_t flag, bool alsoStart) = 0;   // 0x68
    virtual void ClearTravelTypeFlag(std::uint16_t flag) = 0;         // 0x70
    virtual void ClearAllTravelTypeFlags() = 0;                       // 0x78
    virtual void SetStartingCostArray(float* costs) = 0;              // 0x80
    virtual void SetMaxTripLength(float length) = 0;                  // 0x88
    virtual void SetTrafficDensity(std::uint32_t density) = 0;        // 0x90
    virtual void SetStartRect(SC4RectInt32 rect) = 0;                 // 0x98
    virtual void SetEndRect(SC4RectInt32 rect) = 0;                   // 0xa0
    virtual void SetMaxWorkTime(std::uint32_t milliseconds) = 0;      // 0xa8
    virtual void Reset() = 0;                                         // 0xb0
    virtual std::uint32_t GetTrafficArrived() = 0;                    // 0xc0
    virtual float GetTripLength() = 0;                                // 0xc8
    virtual float GetTripLengthScaledForDisplay() = 0;                // 0xd0
    virtual void GetEndCell(int& x, int& z) const = 0;                // 0xd8
    virtual std::uint8_t GetLastTravelType() const = 0;               // 0xe0
    // 1 found, 0 not found, 2 suspended (a work-time limit ran out; call again to resume). pathfinder.md §4.3.
    virtual int FindPath() = 0;
    virtual bool WasTripReversible() = 0;
    virtual void GetCompressedRoute(std::vector<std::uint8_t>& out, std::uint32_t& travelTypes) = 0;
};

class cSC4PathFinder final : public cISC4PathFinder {
public:
    explicit cSC4PathFinder(IPathFinderHost* host);

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x1000d0e86: iid 1 and 0xaa0db766
    std::uint32_t AddRef() override;                                // 0x1000d0ea8
    std::uint32_t Release() override;                               // 0x1000d0eb6 (shuts down, then deletes at 0)

    bool Init() override;
    bool Shutdown() override;
    void SetFlag(std::uint32_t flag) override { tripFlags_ |= flag; }
    void ClearFlag(std::uint32_t flag) override { tripFlags_ &= ~flag; }
    void ClearAllFlags() override { tripFlags_ = 0; }
    void SetRouteInfo(std::deque<RouteData>* route) override { route_ = route; }
    void SetTripDestinationFilter(int filter) override { destinationFilter_ = filter; }
    void SetDestinationFlag(std::uint8_t flag) override { destinationFlags_ = std::uint8_t(destinationFlags_ | flag); }
    void ClearDestinationFlag(std::uint8_t flag) override { destinationFlags_ = std::uint8_t(~flag & destinationFlags_); }
    void ClearAllDestinationFlags() override { destinationFlags_ = 0; }
    void SetTravelTypeFlag(std::uint16_t flag, bool alsoStart) override;
    void ClearTravelTypeFlag(std::uint16_t flag) override;
    void ClearAllTravelTypeFlags() override { travelTypes_ = 0; }   // the original zeroes the dword: the start mask too
    void SetStartingCostArray(float* costs) override { startingCosts_ = costs; }
    void SetMaxTripLength(float length) override { tripLength_ = length; }
    void SetTrafficDensity(std::uint32_t density) override { trafficDensity_ = density; }
    void SetStartRect(SC4RectInt32 rect) override { startRect_ = rect; }
    void SetEndRect(SC4RectInt32 rect) override { endRect_ = rect; }
    void SetMaxWorkTime(std::uint32_t milliseconds) override { maxWorkTime_ = milliseconds; }
    void Reset() override { resume_ = false; }
    std::uint32_t GetTrafficArrived() override { return trafficArrived_; }
    float GetTripLength() override { return tripLength_; }
    float GetTripLengthScaledForDisplay() override { return host_->TripScaleForDisplay() * tripLength_; }
    void GetEndCell(int& x, int& z) const override { x = endCellX_; z = endCellZ_; }
    std::uint8_t GetLastTravelType() const override { return goal_ ? goal_->travelType : 0; }
    int FindPath() override;                                  // 0x006D8FD0 (Windows)
    bool WasTripReversible() override;                        // pathfinder.md §4.16
    void GetCompressedRoute(std::vector<std::uint8_t>& out, std::uint32_t& travelTypes) override;   // §4.15
    // pf2's return-leg setup writes these directly in the original (traffic-simulator.md §4.4).
    SC4RectInt32 StartRect() const { return startRect_; }
    std::uint16_t AllowedTravelTypes() const { return std::uint16_t(travelTypes_ & 0xffff); }
    void SetTravelTypeMasks(std::uint16_t allowed, std::uint16_t start) { travelTypes_ = std::uint32_t(allowed) | (std::uint32_t(start) << 16); }
    const float* StartingCosts() const { return startingCosts_; }
    bool ParkAndRide(int& x, int& z, std::uint8_t& side) const { x = reversalX_; z = reversalZ_; side = reversalDirection_; return x != -2; }
    void SetParkAndRide(int x, int z, std::uint8_t side) { reversalX_ = std::int16_t(x); reversalZ_ = std::int16_t(z); reversalDirection_ = side; }
    std::uint32_t NodesExpanded() const { return expanded_; }

    // ---- search internals (the original's private methods)
    void ReleasePathNodes();                                  // 0x1000d1664
    bool CheckDestinationCapacity(cISCPropertyHolder* holder, std::uint8_t kind);   // 0x1000d1606
    void SetupDistanceTables();                               // 0x1000d3556
    void RecalculateNodeScores();                             // 0x1000d364c
    void DoSuccessfulTrip();                                  // 0x1000d382c
    static std::uint8_t EdgeTo(int fromX, int fromZ, int toX, int toZ);   // 0x1000d3976
    void AddTripNode(int x, int z, std::uint8_t direction, std::uint8_t travelType, float stepCost, TripNode* parent,
                     float limit, bool outside);              // 0x1000d3994
    static float DistanceToRectangle(int x, int z, SC4RectInt32 rect);    // 0x1000d3b32
    bool FindNearestStandardDest();                           // pathfinder.md §4.5
    bool CreateStartNodes();                                  // §4.6
    bool AtGoal(TripNode* node, bool cannotEnd);              // §4.10
    float Heuristic(int x, int z) const;                      // §4.4

    // ---- state access for the host / tests
    TripNodeHashMap& Nodes() { return nodes_; }
    TripNodePriorityQueue& Queue() { return queue_; }
    void SetGoal(TripNode* goal) { goal_ = goal; }
    const std::vector<int>& XDistances() const { return xDistance_; }
    const std::vector<int>& ZDistances() const { return zDistance_; }
    int XDistanceAt(int x) const { return xDistance_[std::size_t(x + 1)]; }
    int ZDistanceAt(int z) const { return zDistance_[std::size_t(z + 1)]; }
    void SetHeuristicWeight(float w) { heuristicWeight_ = w; }
    void SetNearRect(SC4RectInt32 r) { nearRect_ = r; }
    void SetCityCells(int width, int depth) { cityWidth_ = width; cityDepth_ = depth; }
    void SetCostLimit(int travelType, float v) { costLimit_[travelType] = v; }
    std::uint8_t BlockedTypeMask() const { return blockedTypes_; }
    std::uint32_t Counter(int travelType) const { return congestion_[travelType]; }
    std::uint32_t TripFlags() const { return tripFlags_; }
    int DestinationFilter() const { return destinationFilter_; }
    std::uint8_t DestinationFlags() const { return destinationFlags_; }
    std::uint16_t TravelTypes() const { return std::uint16_t(travelTypes_ & 0xffff); }
    std::uint16_t StartTravelTypes() const { return std::uint16_t(travelTypes_ >> 16); }
    bool Initialized() const { return initialized_; }
    int ReversalX() const { return reversalX_; }
    int ReversalZ() const { return reversalZ_; }
    std::uint8_t ReversalDirection() const { return reversalDirection_; }
    void SetPlainEndCell(int x, int z) { endCellX_ = x; endCellZ_ = z; }
    void SetDestinationCapacityKind(std::uint32_t k) { capacityKind_ = k; }
    std::uint32_t DestinationCapacityKind() const { return capacityKind_; }
    void SetTrafficDensityArrived(std::uint32_t v) { trafficArrived_ = v; }
    float TripLengthRaw() const { return tripLength_; }

private:
    IPathFinderHost* host_;
    bool initialized_ = false;                     // +8
    std::uint32_t refCount_ = 0;                   // +0xc
    TripNode* goal_ = nullptr;                     // +0x18
    float heuristicWeight_ = 1.0f;                 // +0x20
    SC4RectInt32 startRect_{}, endRect_{}, nearRect_{}, excludeRect_{};   // +0x24 +0x34 +0x44 +0x54
    int endCellX_ = 0, endCellZ_ = 0;              // +0x64 / +0x68
    float tripLength_ = 0.0f;                      // +0x6c
    std::uint32_t travelTypes_ = 0x00020002;       // +0x70 (low: allowed types, high: start types)
    std::uint32_t trafficDensity_ = 0;             // +0x74
    std::uint32_t trafficArrived_ = 0;             // +0x78
    std::uint32_t tripFlags_ = 0;                  // +0x7c
    std::deque<RouteData>* route_ = nullptr;       // +0x80
    int destinationFilter_ = 3;                    // +0x94
    std::uint8_t destinationFlags_ = 0;            // +0x98
    std::uint32_t capacityKind_ = 0;               // +0xa8
    float* startingCosts_ = nullptr;               // +0xb0
    float costLimit_[9] = {};                      // +0xb8
    int cityWidth_ = 0, cityDepth_ = 0;            // +0xdc / +0xe0
    std::uint32_t maxWorkTime_ = 0;                // +0xe4
    bool resume_ = false;                          // +0xe8
    std::uint32_t congestion_[9] = {};             // +0xec
    std::uint8_t blockedTypes_ = 0;                // +0x110
    std::int16_t reversalX_ = 0, reversalZ_ = 0;   // +0x112 / +0x114
    std::uint8_t reversalDirection_ = 0;           // +0x116
    std::uint32_t expanded_ = 0;                   // nodes expanded by the last FindPath (diagnostics)
    std::vector<int> xDistance_, zDistance_;       // +0x118 / +0x130
    TripNodePriorityQueue queue_;                  // +0x158
    TripNodeHashMap nodes_;                        // +0x178
};

}  // namespace sc4
