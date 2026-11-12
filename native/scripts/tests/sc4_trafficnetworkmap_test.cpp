// cSC4TrafficNetworkMap: tile storage, the per-cell volume/congestion accumulation, intersections,
// train junctions, surrounding-road and geometry queries, view tracking and shutdown.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "sc4/TrafficNetworkMap.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

struct FakeOccupant;

// cISC4Occupant implementation used by the seams that hand occupants back.
struct FakeBase : cISC4Occupant {
    FakeOccupant* owner = nullptr;
    int refs = 1;
    SC4CellRect cells{0, 0, 0, 0};
    float position[3] = {0, 0, 0};
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float* xyz) override { for (int i = 0; i < 3; ++i) xyz[i] = position[i]; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect& r) override { r = cells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

struct FakePathInfo : ITrafficNetworkMapPathInfo {
    bool intersections[8] = {};
    int stopPoints = 1;
    std::vector<std::uint32_t> junctionKeys;
    std::uint8_t junctionEntry = 3;
    bool IsIntersection(std::uint32_t type) override { return type < 8 && intersections[type]; }
    int SnapPointToAnyPath(cS3DVector3& point, std::uint32_t, std::uint32_t*) override {
        point.y = 42.0f;
        return 0;
    }
    int GetStopPointCount(std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t) override { return stopPoints; }
    bool GetJunctionKeys(std::uint32_t pathType, std::uint8_t, std::vector<std::uint32_t>& keys) override {
        if ((pathType != 3 && pathType != 7) || junctionKeys.empty()) return false;
        keys = junctionKeys;
        return true;
    }
    void MakeTunnelPaths(ITrafficNetworkMapOccupant*, ITrafficNetworkMapOccupant*) override {}
};

struct FakeOccupant : ITrafficNetworkMapOccupant {
    FakeBase base;
    FakePathInfo pathInfo;
    int refs = 1;
    std::uint32_t flags = 0x10000001u;   // usable + one road flag
    std::uint32_t types = 0;             // bit 0 = street
    std::uint32_t cellX = 0, cellZ = 0;
    float altitude = 42.0f;
    bool usable = true;
    bool connects = true;
    int releases = 0;
    FakeOccupant() { base.owner = this; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0xa821ef94u || iid == 1u) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool HasNetworkFlag(std::uint32_t f) const override { return (flags & f) == f; }
    bool HasAnyNetworkFlag(std::uint32_t f) const override { return (flags & f) != 0; }
    void SetNetworkFlag(std::uint32_t f) override { flags |= f; }
    void ClearNetworkFlag(std::uint32_t f) override { flags &= ~f; }
    bool ConnectsToNeighbor(int, std::uint32_t) const override { return connects; }
    cISC4Occupant* AsOccupant() override { return &base; }
    ITrafficNetworkMapPathInfo* GetPathInfo() override { return &pathInfo; }
    void ReleasePathInfo() override { ++releases; }
    bool PathConnectionExists(std::uint32_t, std::uint8_t, std::uint8_t) const override { return true; }
    bool IsIntersection() const override { return false; }
    float GetAltitude(float, float) const override { return altitude; }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { x = cellX; z = cellZ; }
    bool IsOfType(std::uint32_t type) const override { return type < 32 && ((types >> type) & 1u) != 0; }
    bool IsUsable() const override { return usable; }
};

struct FakeAutomaton : ITrafficNetworkMapAutomaton {
    int refs = 1;
    bool removed = false;
    std::uint32_t lane = 0;
    std::uint32_t pathType = 0;
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool IsRemoved() override { return removed; }
    std::uint32_t GetLane() override { return lane; }
    std::uint32_t PathTypeWord() const override { return pathType; }
};

struct FakeProp : ITrafficNetworkMapProp {
    int refs = 1;
    std::uint32_t group = 0x5002;
    std::uint32_t property = 0x4a70d491;
    int state = 0;
    int orientation = 0;
    bool flag80 = true;
    float position[3] = {0, 0, 0};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool IsOccupantGroup(std::uint32_t g) override { return g == group; }
    bool HasProperty(std::uint32_t id) override { return id == property; }
    int GetState() override { return state; }
    void SetState(int value) override { state = value; }
    void GetPosition(float* xyz) override { for (int i = 0; i < 3; ++i) xyz[i] = position[i]; }
    int GetOrientation() override { return orientation; }
    bool IsFlagSet(std::uint32_t f) override { return f == 0x80 && flag80; }
};

struct FakeLot : ITrafficNetworkMapLot {
    bool livable = true;
    std::uint8_t wealth = 1;
    std::uint16_t travelPopulation = 7;
    std::int32_t rect[4] = {1, 3, 1, 3};
    void GetBoundingRect(std::int32_t r[4]) override { std::memcpy(r, rect, sizeof rect); }
    bool IsLivable() override { return livable; }
    std::uint8_t OccupantWealth() override { return wealth; }
    std::uint16_t TravelPopulation(std::uint32_t) override { return travelPopulation; }
};

struct FakeBuilding : ITrafficNetworkMapBuilding {
    std::uint32_t purpose = 2;
    std::uint32_t wealth = 1;
    std::uint32_t Purpose() override { return purpose; }
    std::uint32_t Wealth() override { return wealth; }
};

struct Host : ITrafficNetworkMapHost {
    int cellsX = 8, cellsZ = 4;
    bool active = true, terrain = true;
    float viewPosition[3] = {0, 0, 0};
    float viewTarget[3] = {0, 0, 0};
    std::vector<cISC4Occupant*> box;
    std::vector<ITrafficNetworkMapProp*> props;
    FakeLot* lotAtCell = nullptr;
    FakeBuilding* building = nullptr;
    int activeEdges = 0;
    int subscribed = 0, unsubscribed = 0;
    int CityCellsX() override { return cellsX; }
    int CityCellsZ() override { return cellsZ; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    bool SimulationActive() override { return active; }
    ITrafficNetworkMapLot* LotAtCell(int, int) override { return lotAtCell; }
    ITrafficNetworkMapLot* LotOf(cISC4Occupant*) override { return lotAtCell; }
    std::vector<cISC4Occupant*> NetworkOccupantsInBox(const float*, const float*) override { return box; }
    void ReleaseOccupants(const std::vector<cISC4Occupant*>& list) override {
        for (cISC4Occupant* occupant : list) occupant->Release();
    }
    std::vector<ITrafficNetworkMapProp*> PropsAtCell(int, int) override { return props; }
    bool HasTerrain() override { return terrain; }
    float TerrainHeightAt(float, float) override { return 7.0f; }
    ITrafficNetworkMapBuilding* LotBuilding(ITrafficNetworkMapLot*) override { return building; }
    int ActiveEdgesForBuilding(std::uint32_t, std::uint8_t) override { return activeEdges; }
    void GetViewPosition(float& x, float& y, float& z) override { x = viewPosition[0]; y = viewPosition[1]; z = viewPosition[2]; }
    void GetViewTarget(float& x, float& y, float& z) override { x = viewTarget[0]; y = viewTarget[1]; z = viewTarget[2]; }
    void SubscribeMessage(std::uint32_t) override { ++subscribed; }
    void UnsubscribeMessage(std::uint32_t) override { ++unsubscribed; }
    void MakeTunnelPaths(ITrafficNetworkMapPathInfo*, ITrafficNetworkMapOccupant*, ITrafficNetworkMapOccupant*) override {}
};

bool FakeBase::QueryInterface(std::uint32_t iid, void** out) {
    if (!owner || iid != 0xa821ef94u) return 0;
    *out = owner;
    owner->AddRef();
    return true;
}

void InitStreet(FakeOccupant& occupant, std::uint32_t x, std::uint32_t z) {
    occupant.types = 1u << 0;   // street
    occupant.cellX = x;
    occupant.cellZ = z;
    occupant.base.cells = SC4CellRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(z),
                                      static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)};
    occupant.base.position[0] = static_cast<float>(x) * 16.0f + 8.0f;
    occupant.base.position[1] = 30.0f;
    occupant.base.position[2] = static_cast<float>(z) * 16.0f + 8.0f;
}

}  // namespace

int main() {
    Host host;
    TrafficNetworkMapTuning tuning;
    tuning.stoplightPeriodA = 10.0f;
    tuning.stoplightPeriodB = 12.0f;
    cSC4TrafficNetworkMap map(&host, &tuning);

    // ---- Init ----
    CHECK(!map.Initialized());
    CHECK(map.Init());
    CHECK(map.Initialized());
    CHECK(map.CellsX() == 8 && map.CellsZ() == 4);
    CHECK(map.TileCount() == 0);
    CHECK(host.subscribed == 10);   // nine message ids plus the animation tick id
    CHECK(map.SafeGetTileInfoAtCell(-1, 0) == nullptr);
    CHECK(map.SafeGetTileInfoAtCell(8, 0) == nullptr);

    // ---- tile storage / insert ----
    FakeOccupant street;
    InitStreet(street, 2, 3);
    CHECK(map.HandleInsertNetworkOccupant(&street));
    CHECK(map.UnusableRoads().size() == 1);
    CHECK(street.refs == 2);
    CHECK(!map.HandleInsertNetworkOccupant(&street));   // already present
    CHECK(street.refs == 2);

    CHECK(map.HandleNetworkOccupantUsable(&street, false));
    CHECK(map.TileCount() == 1);
    CHECK(map.GetTileInfoAtCell(2, 3) != nullptr);
    CHECK(map.GetTileInfoAtCell(2, 3)->occupant == &street);
    CHECK(map.GetTileInfoByNetwork(&street) != nullptr);
    CHECK(map.FindNetworkOccupant(2, 3) == &street);
    CHECK(map.FindNetworkOccupant(2, 3, 0x10000000u, false) == &street);
    CHECK(map.FindNetworkOccupant(2, 3, 0x00000002u, false) == nullptr);
    CHECK(map.ConnectionExists(2, 3, 0, 0, 1));

    float position[3] = {2 * 16.0f + 8.0f, 30.0f, 3 * 16.0f + 8.0f};
    CHECK(map.FindNetworkOccupant(position) == &street);
    CHECK(map.FindNetworkTile(2, 3, 20.0f, 40.0f) != nullptr);
    CHECK(map.FindNetworkTile(2, 3, 40.0f, 50.0f) == nullptr);

    // a second occupant on the same cell chains in front of the first
    FakeOccupant street2;
    InitStreet(street2, 2, 3);
    CHECK(map.HandleNetworkOccupantUsable(&street2, false));
    CHECK(map.TileCount() == 2);
    CHECK(map.GetTileInfoAtCell(2, 3)->occupant == &street2);
    CHECK(map.GetTileInfoAtCell(2, 3)->next->occupant == &street);
    CHECK(map.FindNetworkOccupant(2, 3) == &street2);

    // ---- intersections: the street becomes a stoplight junction ----
    street.pathInfo.intersections[kPathTypeCar] = true;
    map.AddIntersection(&street);
    CHECK(map.GetIntersectionMap().size() == 1);
    const TrafficIntersectionArray& intersection = map.GetIntersectionMap().at(&street);
    CHECK(intersection.approaches[0].type == 2);
    CHECK(intersection.approaches[0].otherDuration == 10.0f);
    CHECK(intersection.approaches[0].green == 0);          // group A starts on the north/south edges
    CHECK(intersection.approaches[1].green == 1);
    CHECK(intersection.approaches[0].timeLeft == 10.0f);

    FakeProp prop;
    host.props.push_back(&prop);
    map.UpdateIntersection(&street);   // InitIntersectionProps pulls the prop from the host
    CHECK(intersection.props.size() == 1);
    map.UpdateIntersectionProps(&street, const_cast<TrafficIntersectionArray&>(intersection), false);
    CHECK(prop.state == 2);            // approach 0 is red for vehicles

    TrafficIntersectionInfo* first = nullptr;
    map.TickSingleIntersection(&street, const_cast<TrafficIntersectionArray&>(intersection), 0.25f);
    first = const_cast<TrafficIntersectionInfo*>(&intersection.approaches[0]);
    CHECK(std::fabs(first->timeLeft - 9.75f) < 1e-4f);
    first->green = 1;
    first->timeLeft = 0.5f;
    map.UpdateIntersectionProps(&street, const_cast<TrafficIntersectionArray&>(intersection), false);
    CHECK(prop.state == 1);            // green and about to switch
    first->timeLeft = 0.1f;
    map.TickSingleIntersection(&street, const_cast<TrafficIntersectionArray&>(intersection), 0.2f);
    CHECK(first->green == 0);
    CHECK(first->timeLeft == first->otherDuration);
    CHECK(intersection.dirty == 0);

    // ---- train junctions ----
    FakeOccupant train;
    InitStreet(train, 5, 1);
    train.types = 0;   // not a road type; the junction comes from the path info
    train.pathInfo.junctionKeys = {11u, 22u};
    map.AddTrainJunction(&train);
    CHECK(map.TrainJunctions().size() == 1);
    CHECK(map.GetTrainJunctionPath(&train, 0) == 11u);
    CHECK(map.ToggleTrainJunction(&train));
    CHECK(map.GetTrainJunctionPath(&train, 0) == 22u);
    std::vector<std::uint32_t> keys;
    std::uint16_t cursor = 0;
    CHECK(map.GetTrainJunctionKeys(&train, 0, keys, cursor) == 2);
    CHECK(keys.size() == 2 && keys[0] == 11u && keys[1] == 22u && cursor == 1);
    map.RemoveTrainJunction(&train);
    CHECK(map.TrainJunctions().empty());

    // ---- volume / congestion counters ----
    std::uint32_t* laneTick = map.GetLaneTick(&street, 0, 4, 2, 7);
    CHECK(laneTick != nullptr);
    CHECK(*laneTick == 0);
    *laneTick = 5;
    CHECK(map.GetLaneTick(&street, 0, 4, 2, 7) == laneTick);
    CHECK(*map.GetAutosPerSecond(&street, 0, 4, 2, 7) == 0);
    std::uint32_t* automataCount = map.GetAutomataCount(&street, 0, 4, 2, 7);
    *automataCount = 3;
    CHECK(*map.GetAutomataCount(&street, 0, (4u << 24) | (2u << 8) | 7u) == 3);

    FakeAutomaton automaton;
    automaton.lane = (4u << 24) | 3u;
    automaton.pathType = 2;
    CHECK(map.EnterNetwork(&automaton, &street));
    CHECK(automaton.refs == 2);
    CHECK(*map.GetAutomataCount(&street, 0, automaton.lane) == 1);
    CHECK(map.GetAutomataCountForLane(&street, automaton.lane) == 1);
    CHECK(!map.EnterNetwork(&automaton, &street));   // already in the list
    // A later arrival goes in front of the earlier one: FindLeaderInTile scans from an automaton towards the end of the
    // list, i.e. over the automata that entered the tile before it (Mac i386 0x001ea0da pushes at the front).
    FakeAutomaton later;
    later.lane = automaton.lane;
    later.pathType = 2;
    CHECK(map.EnterNetwork(&later, &street));
    {
        const TrafficTileInfo* listTile = map.GetTileInfoByNetwork(&street);
        CHECK(listTile && listTile->automata.size() == 2);
        CHECK(listTile && listTile->automata.front() == &later && listTile->automata.back() == &automaton);
    }
    CHECK(map.ExitNetwork(&later, &street, later.lane));
    CHECK(map.ExitNetwork(&automaton, &street, automaton.lane));
    CHECK(automaton.refs == 1);
    CHECK(*map.GetAutomataCount(&street, 0, automaton.lane) == 0);

    // ---- occupancy from neighbouring buildings ----
    FakeLot lot;
    FakeBuilding building;
    host.lotAtCell = &lot;
    host.building = &building;
    TrafficTileInfo* streetTile = map.GetTileInfoByNetwork(&street);
    map.UpdateRoadNeighborInfo(&street, streetTile);
    CHECK(streetTile->edgeOccupancy[0] == 7);   // west neighbour at (1,3)
    CHECK(streetTile->occupancyMask == kTrafficFootprintMaskZ[0]);
    CHECK(streetTile->buildingMask == kTrafficFootprintMaskX[1]);
    CHECK(streetTile->roadMask == kTrafficBuildingTypeMask[1]);
    map.UpdateNetworkOccupancyInfo(&street, streetTile);
    CHECK(streetTile->edgeOccupancy[0] == 7);

    // the aggregate frequency table has one slot per packed key; the two getters share the entry
    std::uint32_t* aggregate = map.GetAutomataCount(&street, 7);
    CHECK(aggregate != nullptr && *aggregate == 0);
    *aggregate = 2;
    CHECK(map.GetAutomataCount(&street, 7) == aggregate);
    std::uint32_t* autos = map.GetAutosPerSecond(&street, 7);
    CHECK(autos != nullptr && *autos == 0);
    CHECK(map.GetAutosPerSecond(&street, 7) == autos);
    // a path type outside 1..12 is clamped to slot 0 (kept quirk)
    CHECK(map.GetLaneTick(&street, 0, 0, 1, 2) != nullptr);
    CHECK(map.GetLaneTick(&street, 0, 13, 1, 2) != nullptr);

    // ---- weighted edge choice ----
    std::uint8_t edges[4] = {0xff, 0xff, 0xff, 0xff};
    const float noWeights[4] = {0, 0, 0, 0};
    CHECK(map.ChooseWeightedEdge(noWeights, edges) == 0);
    const float secondOnly[4] = {0, 1, 0, 0};
    CHECK(map.ChooseWeightedEdge(secondOnly, edges) == 1 && edges[0] == 1);
    const float firstOnly[4] = {1, 0, 0, 0};
    CHECK(map.ChooseWeightedEdge(firstOnly, edges) == 1 && edges[0] == 0);

    // ---- geometry ----
    cS3DVector3 point{40.0f, 5.0f, 40.0f};
    CHECK(map.ConstrainPointToNetworkTile(point, 0x1f4f));
    CHECK(point.x == 40.0f && point.z == 48.0f && point.y == 42.0f);
    CHECK(map.GetRoadAltitude(40.0f, 56.0f) == 42.0f);   // snaps to the street tile's altitude
    cS3DVector3 onTile{40.0f, 42.0f, 56.0f};
    CHECK(map.ConstrainVectorToNetworkTile(onTile, onTile, 0x1f4f));   // already on the network

    // ---- a step off the road is turned along the tile edge it crossed (Mac i386 0x001e9154) ----
    {
        // the street tile is cell (2, 3): x 32..48, z 48..64, road altitude 42; nothing else around it carries a road
        const cS3DVector3 from{40.0f, 42.0f, 56.0f};
        cS3DVector3 north{44.0f, 42.0f, 45.0f};                 // 3 past the low z edge, heading +x
        CHECK(map.ConstrainVectorToNetworkTile(from, north, 0x1f4f));
        CHECK(north.x == 47.0f && north.z == 48.0f && north.y == 42.0f);
        cSC4TrafficNetworkMap& mapRef = map;
        cS3DVector3 south{44.0f, 42.0f, 66.0f};                 // 2 past the high z edge: the point stops just inside it
        CHECK(mapRef.ConstrainVectorToNetworkTile(from, south, 0x1f4f));
        CHECK(south.x == 46.0f && std::fabs(south.z - 63.99f) < 1e-4f);
        cS3DVector3 west{30.0f, 42.0f, 54.0f};                  // 2 past the low x edge, heading -z
        CHECK(map.ConstrainVectorToNetworkTile(from, west, 0x1f4f));
        CHECK(west.x == 32.0f && west.z == 52.0f);
        // a corner step whose two turned points both land off the road: clamped into the starting tile, and the answer is false
        const cS3DVector3 corner{34.0f, 42.0f, 50.0f};
        cS3DVector3 out{30.0f, 9.0f, 46.0f};
        CHECK(!map.ConstrainVectorToNetworkTile(corner, out, 0x1f4f));
        CHECK(out.x == 32.0f && out.z == 48.0f && out.y == 42.0f);
        // a step inside a tile without a road crosses no edge: the nearest road tile around the point takes it
        const cS3DVector3 offRoad{40.0f, 5.0f, 40.0f};
        cS3DVector3 stillOff{41.0f, 5.0f, 41.0f};
        CHECK(map.ConstrainVectorToNetworkTile(offRoad, stillOff, 0x1f4f));
        CHECK(stillOff.x == 41.0f && stillOff.z == 48.0f && stillOff.y == 42.0f);
        // networks the tile does not carry do not hold the step
        cS3DVector3 other{44.0f, 42.0f, 45.0f};
        CHECK(!map.ConstrainVectorToNetworkTile(from, other, 0x2));
    }

    // ---- surrounding roads ----
    FakeOccupant near;   // top-left inside the rectangle -> rejected as "surrounding"
    InitStreet(near, 2, 3);
    FakeOccupant far;
    InitStreet(far, 6, 3);
    host.box = {&near.base, &far.base};
    std::vector<ITrafficNetworkMapOccupant*> roads;
    CHECK(map.GetSurroundingRoads(roads, SC4RectInt32{2, 3, 2, 3}, 1, 1, 0x10000000u));
    CHECK(roads.size() == 1 && roads.front() == &far);
    for (ITrafficNetworkMapOccupant* road : roads) road->Release();
    host.box.clear();
    CHECK(map.GetSurroundingRoads(roads, &near.base, 1, 1, 0x10000000u) == false);

    // ---- view tracking ----
    host.viewPosition[0] = 2 * 16.0f;
    host.viewPosition[2] = 3 * 16.0f;
    host.viewTarget[0] = 4 * 16.0f;
    host.viewTarget[2] = 1 * 16.0f;
    CHECK(map.HandleViewChanged());
    const SC4RectInt32& visible = map.VisibleRect();
    CHECK(visible.x0 == 0 && visible.x1 == 7 && visible.z0 == 0 && visible.z1 == 3);

    // ---- animation tick drives the occupancy refresh and the path-info GC ----
    building.purpose = 0;   // no traffic source, but the edge occupancy keeps its previous value
    street.releases = 0;
    host.viewPosition[0] = -1000.0f;   // move the visible rectangle so the street cell is outside
    host.viewPosition[2] = -1000.0f;
    host.viewTarget[0] = -1000.0f;
    host.viewTarget[2] = -1000.0f;
    CHECK(map.HandleViewChanged());
    map.DoMessage2(kTrafficMessageTileOccupancyDirty, 0, 0.0f);
    map.DoAnimationTick(0, 1000.0f);
    CHECK(street.releases >= 1);       // GarbageCollectPathInfo releases the cached path info

    // ---- removal ----
    CHECK(map.HandleRemoveNetworkOccupant(&street));
    CHECK(map.TileCount() == 1);   // street2 remains
    CHECK(map.GetIntersectionMap().count(&street) == 0);
    CHECK(map.UnusableRoads().empty());

    // ---- shutdown ----
    CHECK(map.Shutdown());
    CHECK(!map.Initialized());
    CHECK(map.TileCount() == 0);
    CHECK(map.GetIntersectionMap().empty());

    // ---- COM surface (Release destroys the object below two references) ----
    auto* heap = new cSC4TrafficNetworkMap(&host, &tuning);
    void* iface = nullptr;
    CHECK(heap->QueryInterface(kGZIID_cISC4TrafficNetworkMap, &iface));
    CHECK(iface == heap && heap->RefCount() == 2);
    CHECK(heap->Release() == 1);
    CHECK(heap->Release() == 0);

    if (failures == 0) std::printf("sc4_trafficnetworkmap: ok\n");
    return failures == 0 ? 0 : 1;
}
