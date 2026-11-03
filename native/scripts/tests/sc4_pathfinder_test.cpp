// cSC4PathFinder: trip node hash map / priority queue, distance tables, node relaxation, successful-trip bookkeeping, setters.
#include <cmath>
#include <cstdio>
#include <set>

#include "sc4/PathFinder.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {
struct Host : IPathFinderHost {
    int w = 20, d = 10;
    std::uint32_t limit = 10, used = 3;
    float toll = 5.0f;
    bool hasToll = true;
    int CityWidthInCells() override { return w; }
    int CityDepthInCells() override { return d; }
    float TripScaleForDisplay() override { return 2.0f; }
    std::uint32_t TripCapacityLimit() override { return limit; }
    std::uint32_t UsedTripCapacity(cISCPropertyHolder*, std::uint32_t) override { return used; }
    bool NeighborConnectionCost(int, int, float& c) override { c = toll; return hasToll; }
    std::uint32_t MaxTripCapacity(cISCPropertyHolder*, std::uint32_t) override { return limit; }
};

// A 12 x 9 city with one east-west road on row 5 from x = 1 to 8 (car travel type 1, network type 0 Road), a start lot at
// (0, 5) and a destination lot at (9, 5) with free capacity of type 0 (destination byte bit 0).
struct RoadHost : IPathFinderHost {
    std::uint16_t conn[12][9] = {};
    bool road[12][9] = {};
    int CityWidthInCells() override { return 12; }
    int CityDepthInCells() override { return 9; }
    float TripScaleForDisplay() override { return 1.0f; }
    std::uint32_t TripCapacityLimit() override { return 0; }
    std::uint32_t UsedTripCapacity(cISCPropertyHolder*, std::uint32_t) override { return 0; }
    bool NeighborConnectionCost(int, int, float& c) override { c = 0; return true; }
    bool In(int x, int z) const { return unsigned(x) < 12 && unsigned(z) < 9; }
    std::uint16_t Connections(int t, int x, int z) override { return t == 1 && In(x, z) ? conn[x][z] : 0; }
    bool CellNetwork(int n, int x, int z, float& inverse, std::uint8_t& type, float& extra) override {
        if (n != 1 || !In(x, z) || !road[x][z]) return false;
        inverse = 0.001f; type = 0; extra = 0.0f;
        return true;
    }
    std::uint8_t DestinationByte(int x, int z) override { return x == 9 && z == 5 ? 1 : 0; }
    std::uint32_t FreeDestinations(int k) override { return k == 0 ? 1 : 0; }
    bool LotAtCell(int x, int z, SC4RectInt32& rect, cISCPropertyHolder*& holder) override {
        if ((x == 9 || x == 0) && z == 5) { rect = {x, z, x, z}; holder = reinterpret_cast<cISCPropertyHolder*>(this); return true; }
        holder = nullptr;
        return false;
    }
    std::uint32_t MaxTripCapacity(cISCPropertyHolder*, std::uint32_t) override { return 40; }
    float InverseMaxSpeed(int t, int) override { return t == 1 ? 0.5f : 3.4028235e38f; }
    bool AffectedByTraffic(int) override { return false; }
};
}  // namespace

int main() {
    // --- ParseTripLocation ---
    {
        std::uint8_t t; std::int16_t x, z;
        ParseTripLocation((3u << 16) | (7u << 8) | 9u, t, x, z);
        CHECK(t == 3 && x == 7 && z == 9);
        ParseTripLocation(0x80000000u | (1u << 24) | (3u << 16) | (7u << 8) | 9u, t, x, z);   // direction 1: dz = -1
        CHECK(t == 3 && x == 7 && z == 8);
        ParseTripLocation(0x80000000u | (2u << 24) | (7u << 8) | 0u, t, x, z);                  // direction 2: dx = +1
        CHECK(x == 8 && z == 0);
        ParseTripLocation(0x80000000u | (0u << 24) | (0u << 8) | 0u, t, x, z);                  // direction 0 at the corner: x = -1
        CHECK(x == -1 && z == 0);
    }
    // --- hash map ---
    {
        TripNodeHashMap map;
        TripNode* a = map.GetNode(0x10203);
        CHECK(a->key == 0x10203 && a->travelType == 1 && a->x == 2 && a->z == 3 && !a->inQueue && !a->closed);
        CHECK(map.GetNode(0x10203) == a);
        std::set<TripNode*> seen;
        for (std::uint32_t i = 0; i < 9000; ++i) seen.insert(map.GetNode(i * 7 + 1));   // crosses two block boundaries
        CHECK(seen.size() == 9000);
        int count = 0;
        map.ForEachAllocated([&](TripNode*) { ++count; });
        CHECK(count == 9001 || count == 9000);   // 0x10203 may coincide with none of them
        int chained = 0;
        map.ForEachBucketNode([&](TripNode*) { ++chained; });
        CHECK(chained == count);
        a->cost = 4.0f;
        map.Clear();
        int after = 0;
        map.ForEachAllocated([&](TripNode*) { ++after; });
        CHECK(after == 0);
        TripNode* b = map.GetNode(0x10203);
        CHECK(b->cost == 0.0f && b->x == 2);
    }
    // --- priority queue ---
    {
        TripNodePriorityQueue q;
        std::vector<TripNode> nodes(500);
        std::uint32_t seed = 7;
        for (auto& n : nodes) {
            seed = seed * 1664525u + 1013904223u;
            n.score = float(seed >> 20);
            q.Push(&n);
        }
        CHECK(q.Size() == 500);
        // lowering a score re-sifts
        nodes[250].score = -1.0f;
        q.UpdateNode(&nodes[250]);
        TripNode* first = q.Pop();
        CHECK(first == &nodes[250]);
        float last = -2.0f;
        bool sorted = true;
        while (!q.Empty()) {
            TripNode* n = q.Pop();
            if (n->score < last) sorted = false;
            last = n->score;
        }
        CHECK(sorted);
        TripNode single;
        single.score = 1.0f;
        q.Push(&single);
        CHECK(q.Pop() == &single && q.Empty());
    }
    // --- finder basics ---
    Host host;
    cSC4PathFinder* finder = new cSC4PathFinder(&host);
    finder->AddRef();
    void* out = nullptr;
    CHECK(finder->QueryInterface(1, &out) && finder->QueryInterface(kGZIID_cISC4PathFinder, &out) && !finder->QueryInterface(5, &out));
    finder->Release();
    finder->Release();
    CHECK(finder->Init() && !finder->Init());
    CHECK(finder->XDistances().size() == 22 && finder->ZDistances().size() == 12);
    CHECK(finder->TravelTypes() == 2 && finder->StartTravelTypes() == 2);
    finder->SetTravelTypeFlag(0x10, false);
    CHECK(finder->TravelTypes() == 0x12 && finder->StartTravelTypes() == 2);
    finder->SetTravelTypeFlag(0x20, true);
    CHECK(finder->TravelTypes() == 0x32 && finder->StartTravelTypes() == 0x22);
    finder->ClearTravelTypeFlag(0x02);
    CHECK(finder->TravelTypes() == 0x30 && finder->StartTravelTypes() == 0x20);
    finder->ClearAllTravelTypeFlags();
    CHECK(finder->TravelTypes() == 0 && finder->StartTravelTypes() == 0);
    finder->SetFlag(5); finder->SetFlag(2); finder->ClearFlag(1);
    CHECK(finder->TripFlags() == 6);
    finder->ClearAllFlags();
    CHECK(finder->TripFlags() == 0);
    finder->SetDestinationFlag(0x3); finder->SetDestinationFlag(0x8); finder->ClearDestinationFlag(0x1);
    CHECK(finder->DestinationFlags() == 0xa);
    finder->ClearAllDestinationFlags();
    CHECK(finder->DestinationFlags() == 0 && finder->DestinationFilter() == 3);
    finder->SetTripDestinationFilter(0);
    CHECK(finder->DestinationFilter() == 0);
    finder->SetMaxTripLength(12.5f);
    CHECK(finder->GetTripLength() == 12.5f && finder->GetTripLengthScaledForDisplay() == 25.0f);
    int ex = 0, ez = 0;
    finder->SetPlainEndCell(4, 6);
    finder->GetEndCell(ex, ez);
    CHECK(ex == 4 && ez == 6);
    CHECK(finder->GetLastTravelType() == 0);
    // capacity
    finder->SetTrafficDensity(100);
    CHECK(finder->CheckDestinationCapacity(nullptr, 2) && finder->GetTrafficArrived() == 7 && finder->DestinationCapacityKind() == 2);
    finder->SetTrafficDensity(4);
    CHECK(finder->CheckDestinationCapacity(nullptr, 1) && finder->GetTrafficArrived() == 4);
    host.used = 10;
    CHECK(!finder->CheckDestinationCapacity(nullptr, 1));
    host.used = 11;
    CHECK(!finder->CheckDestinationCapacity(nullptr, 1));   // over the limit: no wrap-around
    host.limit = 0;
    CHECK(!finder->CheckDestinationCapacity(nullptr, 1));
    host.limit = 10; host.used = 3;

    // --- distance tables: end rectangle x 5..8, z 2..3 in a 20 x 10 city ---
    finder->SetEndRect({5, 2, 8, 3});
    finder->SetupDistanceTables();
    CHECK(finder->XDistanceAt(-1) == 6 && finder->XDistanceAt(0) == 5 && finder->XDistanceAt(4) == 1 && finder->XDistanceAt(5) == 0 &&
          finder->XDistanceAt(8) == 0 && finder->XDistanceAt(9) == 1 && finder->XDistanceAt(20) == 12);
    CHECK(finder->ZDistanceAt(-1) == 3 && finder->ZDistanceAt(1) == 1 && finder->ZDistanceAt(2) == 0 && finder->ZDistanceAt(3) == 0 &&
          finder->ZDistanceAt(4) == 1 && finder->ZDistanceAt(10) == 7);
    finder->SetEndRect({-1, -1, 3, 3});   // starts outside the city: everything up to hi is zero
    finder->SetupDistanceTables();
    CHECK(finder->XDistanceAt(-1) == 0 && finder->XDistanceAt(3) == 0 && finder->XDistanceAt(4) == 1);

    // --- AddTripNode / relaxation ---
    finder->SetEndRect({10, 5, 10, 5});
    finder->SetupDistanceTables();
    finder->SetHeuristicWeight(2.0f);
    finder->SetNearRect({0, 0, 1, 1});
    TripNode* start = finder->Nodes().GetNode(0x10000000u | (2u << 16) | (3u << 8) | 3u);   // car (type 2) at (3, 3)
    start->cost = 1.0f;
    start->direction = 2;
    finder->AddTripNode(4, 3, 2, 2, 1.5f, start, 100.0f, false);
    const std::uint32_t childKey = 3u | (4u << 8) | (2u << 24) | (2u << 16) | (2u << 26);
    TripNode* child = finder->Nodes().GetNode(childKey);
    CHECK(child->parent == start && child->cost == 2.5f && child->inQueue && child->direction == 2);
    // heuristic: (|10-4| + |5-3|) = 6 + 2 = 8, times 2.0
    CHECK(child->score == 2.5f + 8.0f * 2.0f);
    CHECK(finder->Queue().Size() == 1);
    // a cheaper route to the same state updates in place; a dearer one is ignored
    finder->AddTripNode(4, 3, 2, 2, 0.5f, start, 100.0f, false);
    CHECK(child->cost == 1.5f && finder->Queue().Size() == 1);
    finder->AddTripNode(4, 3, 2, 2, 9.0f, start, 100.0f, false);
    CHECK(child->cost == 1.5f);
    // inside the near rectangle there is no heuristic
    TripNode* near = finder->Nodes().GetNode(0x10000000u | (2u << 16) | (1u << 8) | 1u);
    near->cost = 0;
    finder->AddTripNode(0, 1, 0, 2, 2.0f, near, 100.0f, false);
    TripNode* nearChild = finder->Nodes().GetNode(1u | (0u << 8) | (0u << 24) | (2u << 16));
    CHECK(nearChild->score == 2.0f && nearChild->inQueue);
    // the outside flag sets bit 31 of the key
    finder->AddTripNode(5, 3, 3, 2, 1.0f, start, 100.0f, true);
    CHECK(finder->Nodes().GetNode(0x80000000u | 3u | (5u << 8) | (3u << 24) | (2u << 16) | (2u << 26))->parent == start);
    // not straight back to the grandparent's key
    TripNode* mid = child;
    mid->parent = start;
    start->key = 0;
    // over the limit: counted, and after 0x41 failures the travel type is blocked
    for (int i = 0; i < 0x41; ++i) finder->AddTripNode(9, 9, 0, 4, 500.0f, start, 10.0f, false);
    CHECK(finder->Counter(4) == 0x41 && (finder->BlockedTypeMask() & 0x10));
    std::uint32_t before = finder->Queue().Size();
    finder->AddTripNode(9, 9, 0, 4, 0.0f, start, 10.0f, false);
    CHECK(finder->Queue().Size() == before);   // blocked type: ignored
    // with flag 1 the limit is ignored
    finder->SetFlag(1);
    finder->AddTripNode(9, 8, 0, 3, 900.0f, start, 10.0f, false);
    CHECK(finder->Queue().Size() == before + 1);
    finder->ClearFlag(1);

    // --- RecalculateNodeScores ---
    finder->SetEndRect({0, 0, 0, 0});
    finder->SetupDistanceTables();
    finder->SetHeuristicWeight(1.0f);
    finder->RecalculateNodeScores();
    CHECK(child->score == child->cost + float(finder->XDistanceAt(4) + finder->ZDistanceAt(3)));   // 4 + 3 more than the cost
    CHECK(nearChild->score == nearChild->cost);
    // popping now gives the smallest score
    float lastScore = -1.0f;
    bool ordered = true;
    while (!finder->Queue().Empty()) {
        TripNode* n = finder->Queue().Pop();
        if (n->score < lastScore) ordered = false;
        lastScore = n->score;
    }
    CHECK(ordered);

    // --- EdgeTo / DistanceToRectangle ---
    CHECK(cSC4PathFinder::EdgeTo(5, 5, 4, 5) == 0 && cSC4PathFinder::EdgeTo(5, 5, 6, 0) == 2);
    CHECK(cSC4PathFinder::EdgeTo(5, 5, 5, 6) == 3 && cSC4PathFinder::EdgeTo(5, 5, 5, 5) == 3 && cSC4PathFinder::EdgeTo(5, 5, 5, 4) == 1);
    const SC4RectInt32 rect{10, 10, 14, 12};
    CHECK(cSC4PathFinder::DistanceToRectangle(12, 11, rect) == 0.0f);
    CHECK(cSC4PathFinder::DistanceToRectangle(12, 8, rect) == 2.0f);    // above
    CHECK(cSC4PathFinder::DistanceToRectangle(12, 15, rect) == 3.0f);   // below
    CHECK(cSC4PathFinder::DistanceToRectangle(7, 11, rect) == 3.0f);    // left
    CHECK(cSC4PathFinder::DistanceToRectangle(20, 11, rect) == 6.0f);   // right
    CHECK(std::fabs(cSC4PathFinder::DistanceToRectangle(7, 6, rect) - 5.0f) < 1e-4f);     // corner (10,10): 3,4,5
    CHECK(std::fabs(cSC4PathFinder::DistanceToRectangle(17, 16, rect) - 5.0f) < 1e-4f);   // corner (14,12): 3,4

    // --- DoSuccessfulTrip ---
    {
        TripNodeHashMap& map = finder->Nodes();
        map.Clear();
        auto make = [&](std::uint32_t key, float cost, TripNode* parent) {
            TripNode* n = map.GetNode(key);
            n->cost = cost;
            n->parent = parent;
            return n;
        };
        TripNode* n0 = make(0x20000000u | (2u << 16) | (1u << 8) | 1u, 1.0f, nullptr);      // car at (1,1) - the start
        TripNode* n1 = make((1u << 16) | (2u << 8) | 1u, 3.0f, n0);                          // walk at (2,1)
        TripNode* n2 = make((2u << 16) | (3u << 8) | 1u, 6.0f, n1);                          // car at (3,1)
        TripNode* n3 = make((2u << 16) | (4u << 8) | 1u, 9.0f, n2);                          // car at (4,1)
        std::deque<RouteData> route;
        finder->SetRouteInfo(&route);
        finder->SetGoal(n3);
        finder->SetTripDestinationFilter(3);
        finder->DoSuccessfulTrip();
        CHECK(finder->GetTripLength() == 8.0f);                       // 9 - the start node's 1
        CHECK(finder->GetLastTravelType() == 2);
        CHECK(route.size() == 4 && route[0].x == 1 && route[3].x == 4 && route[1].travelType == 1);
        // the walk -> car change happened at (2,1) ... (3,1): recorded at the node where the walk ends
        CHECK(finder->ReversalX() == 3 && finder->ReversalZ() == 1 && finder->ReversalDirection() == 0);
        // neighbour connection toll: filter 0 and a goal outside the city
        TripNode* edge = make((2u << 16) | (25u << 8) | 1u, 10.0f, n3);
        finder->SetGoal(edge);
        finder->SetTripDestinationFilter(0);
        finder->SetDestinationCapacityKind(1);
        finder->DoSuccessfulTrip();
        CHECK(finder->GetTripLength() == 9.0f + 5.0f);
        host.hasToll = false;
        finder->DoSuccessfulTrip();
        CHECK(finder->GetTripLength() == 9.0f);
        finder->SetDestinationCapacityKind(3);
        host.hasToll = true;
        finder->DoSuccessfulTrip();
        CHECK(finder->GetTripLength() == 9.0f);
        finder->SetRouteInfo(nullptr);
    }
    // --- FindPath over a road (pathfinder.md §4.4 - §4.15) ---
    {
        RoadHost roads;
        for (int x = 1; x <= 8; ++x) {
            roads.road[x][5] = true;
            roads.conn[x][5] = std::uint16_t((1u << (2 + 4 * 0)) | (1u << (0 + 4 * 2)));   // in from the west -> out east; in from the east -> out west
        }
        auto run = [&](RoadHost& host, int& result, std::vector<std::uint8_t>& route, float& length, int& ex, int& ez, bool& reversible) {
            cSC4PathFinder* pf = new cSC4PathFinder(&host);
            pf->AddRef();
            pf->Init();
            pf->SetTrafficDensity(25);
            pf->SetTripDestinationFilter(0);
            pf->ClearAllDestinationFlags();
            pf->SetDestinationFlag(1);
            pf->SetStartRect({0, 5, 0, 5});
            pf->ClearAllFlags();
            pf->ClearAllTravelTypeFlags();
            pf->SetTravelTypeFlag(0x2, true);
            pf->SetMaxTripLength(10.0f);
            result = pf->FindPath();
            std::uint32_t types = 0;
            pf->GetCompressedRoute(route, types);
            length = pf->GetTripLength();
            pf->GetEndCell(ex, ez);
            reversible = pf->WasTripReversible();
            CHECK(result != 1 || (types == 2 && pf->GetTrafficArrived() == 25 && pf->GetLastTravelType() == 1));
            pf->Release();
        };
        int result = 0, ex = 0, ez = 0;
        float length = 0;
        bool reversible = false;
        std::vector<std::uint8_t> route;
        run(roads, result, route, length, ex, ez, reversible);
        CHECK(result == 1 && ex == 9 && ez == 5);
        CHECK(length == 3.5f);   // seven steps (1 -> 8) at 0.5, the start node's cost (0) taken off
        // header: car at (1, 5); one run of seven east steps (code 2), four to a byte, low bits first
        const std::vector<std::uint8_t> expected{1, 1, 5, 0x41, 7, 0xaa, 0x2a};
        CHECK(route == expected);
        CHECK(reversible);
        // planted control: a gap in the road (no exit east at x = 5) leaves the destination unreachable
        RoadHost broken = roads;
        broken.conn[5][5] = std::uint16_t(1u << (0 + 4 * 2));
        run(broken, result, route, length, ex, ez, reversible);
        CHECK(result == 0 && route.empty());
        // a one-way road (east only) is not reversible
        RoadHost oneWay = roads;
        for (int x = 1; x <= 8; ++x) oneWay.conn[x][5] = std::uint16_t(1u << (2 + 4 * 0));
        run(oneWay, result, route, length, ex, ez, reversible);
        CHECK(result == 1 && !reversible);
    }

    // --- shutdown ---
    CHECK(finder->Shutdown() && !finder->Shutdown());
    CHECK(finder->GetLastTravelType() == 0);   // goal released
    finder->Release();   // refcount 1 -> deleted
    if (failures) { std::printf("sc4_pathfinder: %d failure(s)\n", failures); return 1; }
    std::printf("sc4_pathfinder OK\n");
    return 0;
}
