// cSTESurfaceWater over a real cSTETerrain: depth queries, flooding altitude, navigability and the bodies of water, the messages, the stream.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <list>
#include <map>
#include <set>
#include <tuple>

#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SurfaceWater.h"
#include "rz/PortMemoryStreams.h"

using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct FakeNetwork final : IWaterNetworkOccupant {
    bool bridge = true, usable = false, bridgeOk = true;
    float height = 100.0f;
    std::uint8_t mask = 0;
    float where[3] = {0, 0, 0};
    bool HasNetworkFlag(std::uint32_t flag) override { return bridge && flag == 0x40000; }
    bool IsUsable() override { return usable; }
    void GetPosition(float xyz[3]) override { for (int i = 0; i < 3; ++i) xyz[i] = where[i]; }
    std::uint8_t ActiveEdges() override { return mask; }
    bool PassageCeiling(float& out) override { out = height; return bridgeOk; }
};
struct FakeCellOccupant final : IWaterCellOccupant {
    int x = 0, z = 0;
    bool preview = false;
    void GetCell(int& ox, int& oz) override { ox = x; oz = z; }
    bool IsPreview() override { return preview; }
};
struct GridCity final : ISimGridCity {
    std::int32_t CitySizeXInCells() const override { return 4; }
    std::int32_t CitySizeZInCells() const override { return 4; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};
struct Host final : ISurfaceWaterHost {
    cSTETerrain* terrain = nullptr;
    bool initialized = true;
    int added = 0, removedN = 0, ticks = 0, unticks = 0;
    std::map<std::pair<int, int>, FakeNetwork*> network;
    std::map<std::pair<int, int>, FakeCellOccupant*> cellOccupants;
    std::set<std::pair<int, int>> outside;
    const void* lotHere = nullptr;
    std::set<std::tuple<int, int, int>> connections;
    int created = 0, deleted = 0;
    bool NeighborHasConnection(std::uint32_t t, int x, int z) override { return connections.count({int(t), x, z}) != 0; }
    void NeighborDeleteConnection(std::uint32_t t, int x, int z) override { ++deleted; connections.erase({int(t), x, z}); }
    void NeighborCreateConnection(std::uint32_t t, int x, int z) override { ++created; connections.insert({int(t), x, z}); }
    FakeNetwork* built = nullptr;
    FakeCellOccupant* asCell = nullptr;
    cSTETerrain* Terrain() override { return terrain; }
    bool CityInitialized() override { return initialized; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = static_cast<int>(x * 0.0625f); cz = static_cast<int>(z * 0.0625f); }
    int CitySizeX() override { return 4; }
    int CitySizeZ() override { return 4; }
    bool CellInCity(int x, int z) override { return outside.count({x, z}) == 0; }
    void AddNotifications(cSTESurfaceWater&) override { ++added; }
    void RemoveNotifications(cSTESurfaceWater&) override { ++removedN; }
    void RegisterTicks(cSTESurfaceWater&) override { ++ticks; }
    void UnregisterTicks(cSTESurfaceWater&) override { ++unticks; }
    IWaterNetworkOccupant* NetworkOccupantAt(int x, int z, std::uint32_t, bool) override {
        auto it = network.find({x, z});
        return it == network.end() ? nullptr : it->second;
    }
    IWaterNetworkOccupant* AsNetworkOccupant(cIGZUnknown*) override { return built; }
    IWaterCellOccupant* AsCellOccupant(cIGZUnknown*) override { return asCell; }
    const void* LotAtCell(int, int) override { return lotHere; }
    void OccupantsInCell(int x, int z, std::uint32_t group, std::vector<IWaterCellOccupant*>& out) override {
        if (group != kWaterOccupantGroup) return;
        auto it = cellOccupants.find({x, z});
        if (it != cellOccupants.end()) out.push_back(it->second);
    }
};
}  // namespace

int main() {
    // 4x4 cells, altitude 40 on the ground and a 2 x 2 hole of altitude 0 in the middle (vertices 1..2): sea level 20
    cSTETerrain terrain;
    TerrainTuning tuning;
    tuning.seaLevel = 20.0f;
    terrain.Init(4, 4, tuning);
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 5; ++x) terrain.Vertices()[terrain.GetVertIndex(x, z)].altitude = (x >= 1 && x <= 3 && z >= 1 && z <= 3) ? 0.0f : 40.0f;
    GridCity gridCity;
    SetSimGridCity(&gridCity);
    Host host;
    host.terrain = &terrain;
    auto* water = new cSTESurfaceWater(&host);
    water->AddRef();
    CHECK(water->GetGZCLSID() == 0x0b8da7db);
    void* out = nullptr;
    CHECK(water->QueryInterface(0xab8da759, &out) && out == static_cast<cIGZSerializable*>(water));
    water->Release();
    CHECK(water->QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(water));
    water->Release();
    CHECK(!water->QueryInterface(0x12345678, &out));

    CHECK(water->Init() && water->Initialized() && host.added == 1 && host.ticks == 1);
    CHECK(water->Init());                                                  // the second Init is a no-op
    CHECK(host.added == 1);
    CHECK(water->Info().size() == 25 && water->GetWaterDepthAtVertex(terrain.GetVertIndex(2, 2)) == 20.0f);
    CHECK(water->GetWaterDepthAtVertex(0) == -20.0f);                      // land is negative
    CHECK(terrain.WaterDepth()[terrain.GetVertIndex(2, 2)] == 20.0f && terrain.WaterDepth()[0] == 0.0f);   // the terrain keeps land at zero
    CHECK(water->GetBodyOfWaterMap() != nullptr && water->GetBodyOfWaterMap()->GetInstanceID() == 0x4be3618b);

    // depth of a cell: the shallowest corner, never negative
    CHECK(water->GetWaterDepthForCell(1, 1) == 20.0f);                     // all four corners are in the hole
    CHECK(water->GetWaterDepthForCell(0, 0) == 0.0f);                      // all dry (negative depths clamp to zero)
    CHECK(water->GetWaterDepthForCell(0, 1) == 0.0f);                      // one dry corner column: the shallowest wins
    CHECK(water->HasDraftClearance(1, 1) && !water->HasDraftClearance(0, 0));
    CHECK(water->GetWaterType(2, 2) == 3 && water->GetWaterType(0, 0) == 0);   // 20 + 0 is not above the sea level (20)
    water->Info()[terrain.GetVertIndex(2, 2)].depth = 25.0f;
    CHECK(water->GetWaterType(2, 2) == 2);                                 // a lake above the sea level
    water->Info()[terrain.GetVertIndex(2, 2)].depth = 20.0f;

    // flat water: wet and one velocity component zero
    water->Info()[6].velocity = {1.0f, 2.0f, 3.0f};
    CHECK(!water->IsFlatWater(0));
    water->Info()[terrain.GetVertIndex(2, 2)].velocity = {0.0f, 1.0f, 1.0f};
    CHECK(water->IsFlatWater(terrain.GetVertIndex(2, 2)));
    water->Info()[terrain.GetVertIndex(2, 2)].velocity = {1.0f, 1.0f, 1.0f};
    CHECK(!water->IsFlatWater(terrain.GetVertIndex(2, 2)));

    // flooding altitude: the highest wet surface around the vertex, at least the sea level
    CHECK(water->AltitudeAtWhichFloodingWillOccur(2, 2) == 20.0f);
    water->Info()[terrain.GetVertIndex(3, 2)].depth = 35.0f;               // a wet neighbour whose surface is at 0 + 35
    CHECK(water->AltitudeAtWhichFloodingWillOccur(2, 2) == 35.0f);
    water->Info()[terrain.GetVertIndex(3, 2)].depth = 20.0f;

    // the velocity accessor hands out the vertex' vector
    CHECK(&water->GetWaterVelocity(2, 2) == &water->Info()[terrain.GetVertIndex(2, 2)].velocity);

    // the creek spring raises the first flow channel
    water->CreateSpring(3);
    CHECK(water->Info()[3].flow[0] == 1.0f);

    // navigability: a cell is open when the water over its corners is deeper than 5 and nothing blocks it
    CHECK(water->IsNavigable(1, 1, 0, nullptr) && !water->IsNavigable(0, 0, 0, nullptr));
    host.outside.insert({1, 1});
    CHECK(!water->IsNavigable(1, 1, 0, nullptr));
    host.outside.clear();
    int lotA = 1, lotB = 2;
    host.lotHere = &lotA;
    CHECK(!water->IsNavigable(1, 1, 0, &lotB) && water->IsNavigable(1, 1, 0, &lotA) && water->IsNavigable(1, 1, 0, nullptr));
    host.lotHere = nullptr;
    FakeNetwork bridge;
    host.network[{1, 1}] = &bridge;
    bridge.height = 41.0f;                                                 // surface 0 + 20: clearance 21 >= 20
    CHECK(water->IsNavigable(1, 1, 0, nullptr));
    bridge.height = 30.0f;
    CHECK(!water->IsNavigable(1, 1, 0, nullptr));
    bridge.bridgeOk = false;
    CHECK(!water->IsNavigable(1, 1, 0, nullptr));
    host.network.clear();
    FakeCellOccupant blocker;
    blocker.x = 1; blocker.z = 1;
    host.cellOccupants[{1, 1}] = &blocker;
    CHECK(!water->IsNavigable(1, 1, 0, nullptr));
    blocker.z = 2;                                                         // same column, other row: not on the cell
    CHECK(water->IsNavigable(1, 1, 0, nullptr));
    host.cellOccupants.clear();
    CHECK(water->IsNavigable(cS3DVector3{20.0f, 0.0f, 20.0f}, 0, nullptr));   // position -> cell (1, 1)

    // bodies of water: the middle cells form one body numbered 1; a second hole elsewhere is a second body
    water->UpdateBodiesOfWater();
    auto* bodies = water->GetBodyOfWaterMap();
    CHECK(bodies->GetTractValue(1, 1) == 1 && bodies->GetTractValue(2, 2) == 1 && bodies->GetTractValue(0, 0) == 0);

    // the neighbour scan: a run of flat water at an edge gets a connection one cell outside the map; shallow-water ends are moved inward
    water->UpdateNeighborConnections();
    CHECK(host.created == 0);                                              // the hole does not touch the edges of the 4x4 map
    {
        cSTETerrain big;
        TerrainTuning t3;
        t3.seaLevel = 20.0f;
        big.Init(8, 8, t3);
        for (int z = 0; z < 9; ++z)
            for (int x = 0; x < 9; ++x) big.Vertices()[big.GetVertIndex(x, z)].altitude = (x >= 1 && x <= 6) ? 0.0f : 40.0f;   // a channel 6 vertices wide, open to both ends in z
        Host h3;
        h3.terrain = &big;
        auto* w3 = new cSTESurfaceWater(&h3);
        w3->AddRef();
        w3->Init();
        for (auto& info : w3->Info()) info.velocity = {0.0f, 0.0f, 0.0f};
        h3.connections.clear();
        w3->UpdateNeighborConnections();
        int south = 0, north = 0;
        for (const auto& c : h3.connections) {
            CHECK(std::get<0>(c) == 9);
            if (std::get<2>(c) == -1) ++south;
            if (std::get<2>(c) == 8) ++north;
        }
        CHECK(south == 1 && north == 1);                                   // one connection on each open edge, none on the walls
        CHECK(h3.created == 2);
        // a second scan finds them and neither creates nor deletes
        h3.created = h3.deleted = 0;
        w3->UpdateNeighborConnections();
        CHECK(h3.created == 0);
        // a bridge-free but blocked cell deletes the connection under it
        h3.outside.insert({3, 0});
        w3->UpdateNeighborConnections();
        CHECK(h3.deleted >= 1);
        w3->Release();
    }

    // downhill search and basins
    CHECK(water->FindLocalMinimumOrWater(terrain.GetVertIndex(2, 2)) == terrain.GetVertIndex(2, 2));      // already water
    {
        const int dry = terrain.GetVertIndex(0, 0);
        CHECK(water->Info()[dry].depth <= 0.0f);
        CHECK(water->FindLocalMinimumOrWater(dry) == terrain.GetVertIndex(1, 1));   // downhill into the hole, where the water begins
    }
    {
        std::list<int> basin, rim, spill;
        water->GetBasinVertices(terrain.GetVertIndex(2, 2), 0.0f, 10.0f, basin, rim, spill);
        CHECK(basin.size() >= 9);                                          // the 3x3 hole at altitude 0 (the seed may repeat)
        CHECK(std::find(basin.begin(), basin.end(), terrain.GetVertIndex(1, 1)) != basin.end());
        CHECK(!rim.empty());                                               // the walls at altitude 40 are visited and stay on the rim
        for (int v : rim) CHECK(terrain.Vertices()[static_cast<std::size_t>(v)].altitude > 10.0f);
        for (int v : basin) if (terrain.Vertices()[static_cast<std::size_t>(v)].altitude <= 10.0f) CHECK(std::find(rim.begin(), rim.end(), v) == rim.end());
        water->GetBasinVertices(terrain.GetVertIndex(2, 2), 0.0f, -5.0f, basin, rim, spill);
        CHECK(basin.empty());                                              // the seed is above the limit
    }
    {
        TerrainBrushContext context;
        TerrainRect rect{};
        CHECK(!water->Rain(terrain.GetVertIndex(2, 2), context, rect, 1.0f));      // static mode: no per-vertex brush
        CHECK(rect.left == INT_MAX && rect.bottom == INT_MIN);            // (no texture: nothing changed)
        water->SetPerVertexMode(true);
        CHECK(water->Rain(terrain.GetVertIndex(2, 2), context, rect, 1.0f));
        water->SetPerVertexMode(false);
        const float before = water->Info()[5].flow[1];
        water->AddRain(5, 2.5f);
        CHECK(water->Info()[5].flow[1] == before + 2.5f);
    }

    // the flood step: a spring on a hill floods downhill and the buffers swap
    {
        cSTETerrain slope;
        TerrainTuning t2;
        t2.seaLevel = -100.0f;
        slope.Init(4, 4, t2);
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 5; ++x) slope.Vertices()[slope.GetVertIndex(x, z)].altitude = 100.0f - 10.0f * x;
        Host h2;
        h2.terrain = &slope;
        auto* w2 = new cSTESurfaceWater(&h2);
        w2->AddRef();
        w2->Init();
        const int source = slope.GetVertIndex(1, 2);
        w2->Info()[source].flow[0] = 5.0f;                                 // a spring
        w2->UpdateFloodWaters();                                           // the first step only seeds the new buffer with the spring
        CHECK(w2->Info()[source].flow[2] >= 5.0f);
        CHECK(w2->Info()[slope.GetVertIndex(2, 2)].flow[2] == 0.0f);
        w2->UpdateFloodWaters();                                           // the second step shares it downhill
        CHECK(w2->Info()[slope.GetVertIndex(2, 2)].flow[1] > 0.0f);        // to the right (lower)
        CHECK(w2->Info()[slope.GetVertIndex(0, 2)].flow[1] == 0.0f);       // not uphill
        CHECK(w2->Info()[source].velocity.x > 0.0f);                       // the flow points downhill (+x)
        CHECK(w2->Info()[source].depth >= 1.0f);                           // back on buffer 0: the visible depth is the (rounded up) average
        CHECK(slope.WaterDepth().size() == 25 && slope.WaterDepth()[static_cast<std::size_t>(source)] == w2->Info()[source].depth);
        w2->Release();
    }

    // messages
    auto* messagePtr = new cRZMessage2Standard();
    messagePtr->AddRef();
    cRZMessage2Standard& message = *messagePtr;
    message.SetType(0x66956814);
    water->DoMessage(&message);
    CHECK(!water->BodiesDirty());
    FakeCellOccupant mover;
    mover.x = 1; mover.z = 1;
    host.asCell = &mover;
    message.SetType(0x99ef1142);
    message.SetData3(0x2890d4de);
    int dummy = 0;
    message.SetVoid1(&dummy);
    water->DoMessage(&message);
    CHECK(water->BodiesDirty());                                           // an occupant on water marks the bodies stale
    bodies->SetTractValue(1, 1, 7);
    message.SetType(0x66956814);
    water->DoMessage(&message);
    CHECK(!water->BodiesDirty() && bodies->GetTractValue(1, 1) == 1);      // rebuilt
    mover.x = 0; mover.z = 0;
    message.SetType(0x99ef1143);
    water->DoMessage(&message);
    CHECK(!water->BodiesDirty());                                          // dry cell: nothing
    message.SetData3(5);
    mover.x = 1; mover.z = 1;
    water->DoMessage(&message);
    CHECK(!water->BodiesDirty());                                          // another group: ignored
    host.asCell = nullptr;
    water->DoMessage(&message);                                            // no cell view: ignored
    message.SetType(0x86ad10ee);
    water->SetPerVertexMode(false);
    water->DoMessage(&message);                                            // animation only in the per-vertex mode

    // a built bridge piece that joins nothing rebuilds the bodies; one joined to another bridge does not
    FakeNetwork piece;
    piece.mask = 0x2;                                                      // direction 1: (x, z - 1)
    piece.where[0] = 40.0f; piece.where[2] = 40.0f;                        // cell (2, 2)
    host.built = &piece;
    message.SetType(0x89d3e9f3);
    message.SetData1(1);
    bodies->SetTractValue(2, 2, 9);
    water->DoMessage(&message);
    CHECK(bodies->GetTractValue(2, 2) == 1);
    FakeNetwork neighbour;
    host.network[{2, 1}] = &neighbour;
    bodies->SetTractValue(2, 2, 9);
    water->DoMessage(&message);
    CHECK(bodies->GetTractValue(2, 2) == 9);                               // joined: untouched
    neighbour.usable = true;
    water->DoMessage(&message);
    CHECK(bodies->GetTractValue(2, 2) == 1);                               // a finished (usable) neighbour does not hold it back
    host.network.clear();
    message.SetData1(0);
    bodies->SetTractValue(2, 2, 9);
    water->DoMessage(&message);
    CHECK(bodies->GetTractValue(2, 2) == 9);                               // no data: ignored

    // the record round trip
    water->Info()[7].depth = 3.5f; water->Info()[7].flow[1] = 0.25f; water->Info()[7].velocity = {1, 2, 3}; water->Info()[7].sediment = 0.75f;
    PortMemoryOStream o;
    water->WriteWaterRecord(o);
    PortMemoryIStream in(o.bytes().data(), o.bytes().size());
    host.terrain = &terrain;
    auto* copy = new cSTESurfaceWater(&host);
    copy->AddRef();
    copy->Init();
    copy->ReadWaterRecord(in);
    CHECK(copy->Info()[7].depth == 3.5f && copy->Info()[7].flow[1] == 0.25f && copy->Info()[7].velocity.z == 3.0f && copy->Info()[7].sediment == 0.75f);
    CHECK(copy->Info()[0].depth == water->Info()[0].depth);
    CHECK(terrain.Sediment()[7] == 0.75f && terrain.WaterDepth()[7] == 3.5f);
    copy->Release();

    // shutdown unhooks the services once
    CHECK(water->Shutdown() && host.unticks >= 1 && host.removedN >= 1 && !water->Initialized());
    CHECK(water->Shutdown());
    water->Release();
    message.Release();
    SetSimGridCity(nullptr);
    std::puts(failures ? "sc4_surfacewater: FAILED" : "sc4_surfacewater ok");
    return failures != 0;
}
