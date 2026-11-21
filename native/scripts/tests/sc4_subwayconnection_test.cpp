// The subway's pieces apart from the drag: the subway piece tables and StaticInit (i386 0x0013756a), the subway view's custom
// view (cSC4SubwayCustomView, class 0x4a19bde3), and cSC4SubwayConnectionOccupant on the underground connection base: Init
// (i386 0x0022f916: the cell, the trench floor quad, the cell's piece or a piece laid to a neighbor, never for a loaded
// connection), DoMessage (0x0023197a: pieces inserted on / removed from its cell, its building removed), SetBuildingOccupant
// and the record.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/PipeOccupant.h"
#include "sc4/SubwayConnectionOccupant.h"
#include "sc4/SubwayOccupant.h"
#include "sc4/UndergroundCustomView.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-4f)

using namespace sc4;

namespace {

// A subway piece on a cell (its corners at the cell's 16 m square).
cSC4SubwayOccupant* MakeSubway(int x, int z) {
    auto* piece = new cSC4SubwayOccupant();
    piece->AddRef();
    const float x0 = static_cast<float>(x) * 16.0f, z0 = static_cast<float>(z) * 16.0f;
    const float corners[4][3] = {{x0, 40.0f, z0}, {x0, 40.0f, z0 + 16.0f}, {x0 + 16.0f, 40.0f, z0 + 16.0f}, {x0 + 16.0f, 40.0f, z0}};
    piece->Init(corners);
    return piece;
}

struct FakeHost final : IUndergroundConnectionHost {
    std::map<std::pair<int, int>, cSC4SubwayOccupant*> pieces;
    struct Placement { std::uint32_t type; int x0, z0, x1, z1; };
    std::vector<Placement> placements;
    std::vector<std::pair<cISC4Occupant*, bool>> removed;
    int rendererChanges = 0;
    ~FakeHost() {
        for (auto& entry : pieces) entry.second->Release();
    }

    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x / 16.0f);
        cellZ = static_cast<int>(z / 16.0f);
    }
    void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) override {
        x = static_cast<float>(cellX) * 16.0f;
        z = static_cast<float>(cellZ) * 16.0f;
    }
    float CellWidthX() override { return 16.0f; }
    float CellWidthZ() override { return 16.0f; }
    void CellCornerHeights(int, int, float* heights) override {
        for (int i = 0; i < 4; ++i) heights[i] = 100.0f + static_cast<float>(i);
    }
    cSC4UndergroundOccupant* FindUndergroundOccupant(int cellX, int cellZ, std::uint32_t gziid) override {
        if (gziid != kGZCLSID_cSC4SubwayOccupant) return nullptr;
        auto it = pieces.find({cellX, cellZ});
        if (it == pieces.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    cSC4UndergroundConnectionOccupant* FindUndergroundConnectionOccupant(int, int, std::uint32_t) override { return nullptr; }
    // The network tool lays the line: a piece on each end cell.
    void PlaceNetworkOccupants(std::uint32_t type, int x0, int z0, int x1, int z1) override {
        placements.push_back({type, x0, z0, x1, z1});
        if (!pieces.count({x0, z0})) pieces[{x0, z0}] = MakeSubway(x0, z0);
    }
    void RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t) override { removed.push_back({occupant, shutdown}); }
    void ChangeModelInRenderer(cSC4UndergroundConnectionOccupant*, int) override { ++rendererChanges; }
};

cSC4SubwayConnectionOccupant* MakeConnection(int x, int z) {
    auto* connection = new cSC4SubwayConnectionOccupant();
    connection->AddRef();
    const float position[3] = {static_cast<float>(x) * 16.0f + 8.0f, 50.0f, static_cast<float>(z) * 16.0f + 8.0f};
    connection->SetPosition(position);
    return connection;
}

void TestGeometryTables() {
    const tCustomGeometryInfo& info = cSC4SubwayOccupant::sCustomGeometryInfo;
    CHECK(info.pieceArrays[0] != nullptr && info.pieceArrays[1] != nullptr && info.pieceArrays[2] != nullptr);   // straight, zipper top / bottom
    CHECK(info.pieceArrays[1] != info.pieceArrays[0] && info.pieceArrays[2] != info.pieceArrays[1]);
    CHECK(info.counts[0] == 12 && info.counts[1] == 6 && info.counts[2] == 12 && info.counts[3] == 6);
    CHECK(info.countB0 == 21 && info.percentageCount == 5 && info.countB8 == 11 && info.countBC == 6);
    // the subway's tube is wider than the pipe's (straight piece vertex 2: x -4.197 against -1.679)
    const auto* straight = static_cast<const UndergroundVertex*>(info.pieceArrays[0]);
    CHECK_NEAR(straight[2].x, -4.197f);
    CHECK_NEAR(straight[4].y, 2.971f);
    const auto* zipperTop = static_cast<const UndergroundVertex*>(info.pieceArrays[1]);
    CHECK_NEAR(zipperTop[5].z, 4.0f);   // the top half stops short of the far edge
    CHECK(info.textureRecords[0].instance == 0x095a8d70 && info.textureRecords[1].instance == 0xe95a8d8d);
    CHECK(info.textureRecords[2].instance == 0xe953f219 && info.textureRecords[4].instance == 0xe953f219);
    CHECK(info.textureRecords[5].instance == 0x8a11ce6f && info.textureRecords[8].instance == 0xaa1b0446);
    CHECK(info.textureRecords[11].instance == 0x6a1b0358 && info.textureRecords[16].instance == 0xa961018a);

    cSC4SubwayOccupant::StaticInit();
    const float* piece = cSC4SubwayOccupant::sDiagonalPieceEdgeDistancePercentages;
    const float* cap = cSC4SubwayOccupant::sDiagonalCapEdgeDistancePercentages;
    CHECK(info.edgeDistancePercentages == piece && info.hingeDistancePercentages == cap);
    CHECK_NEAR(piece[0], 1.0f);
    CHECK_NEAR(cap[4], 1.0f);
    for (int u = 1; u < 5; ++u) {
        CHECK(piece[u] > 0.0f && piece[u] < 1.0f);
        CHECK_NEAR(piece[u] + piece[5 - u], 1.0f);
        CHECK_NEAR(cap[4 - u], piece[u]);
    }
}

void TestCustomViews() {
    cISC4CustomView* subway = CreateCustomView(kGZCLSID_cSC4SubwayCustomView);
    CHECK(subway != nullptr);
    if (subway) {
        CHECK(static_cast<cSC4UndergroundCustomView*>(subway)->NetworkGziid() == kGZCLSID_cSC4SubwayOccupant);
        subway->Release();
    }
    cISC4CustomView* pipe = CreateCustomView(kGZCLSID_cSC4PipeCustomView);
    CHECK(pipe != nullptr);
    if (pipe) {
        CHECK(static_cast<cSC4UndergroundCustomView*>(pipe)->NetworkGziid() == kGZCLSID_cSC4PipeOccupant);
        pipe->Release();
    }
    CHECK(CreateCustomView(0x12345678) == nullptr);
}

// A new connection beside a subway: the cell from the position, the trench floor quad 15 below the terrain corners, a piece laid
// from its cell to the neighbor (straight neighbors first), and the new piece of its cell taken as its network piece.
void TestInitJoinsNeighbor() {
    FakeHost host;
    SetUndergroundConnectionHost(&host);
    host.pieces[{8, 99}] = MakeSubway(8, 99);
    cSC4SubwayConnectionOccupant* connection = MakeConnection(7, 99);
    CHECK(connection->Init());
    CHECK(connection->CellX() == 7 && connection->CellZ() == 99);
    CHECK_NEAR(connection->TrenchFloorY(), 35.0f);
    CHECK_NEAR(connection->FloorVertex(0).x, 112.0f);
    CHECK_NEAR(connection->FloorVertex(0).z, 1584.0f);
    CHECK_NEAR(connection->FloorVertex(0).y, 85.0f);
    CHECK_NEAR(connection->FloorVertex(1).z, 1600.0f);
    CHECK_NEAR(connection->FloorVertex(2).x, 128.0f);
    CHECK_NEAR(connection->FloorVertex(2).y, 87.0f);
    CHECK_NEAR(connection->FloorVertex(3).x, 128.0f);
    CHECK_NEAR(connection->FloorVertex(3).z, 1584.0f);
    CHECK_NEAR(connection->FloorVertex(1).v, 1.0f);
    CHECK_NEAR(connection->FloorVertex(3).u, 1.0f);
    CHECK(host.placements.size() == 1);
    if (!host.placements.empty()) {
        const auto& p = host.placements[0];
        CHECK(p.type == kSubwayNetworkType && p.x0 == 7 && p.z0 == 99 && p.x1 == 8 && p.z1 == 99);
    }
    cSC4SubwayOccupant* laid = host.pieces.count({7, 99}) ? host.pieces[{7, 99}] : nullptr;
    CHECK(laid != nullptr);
    CHECK(connection->NetworkOccupant() == static_cast<cISC4Occupant*>(laid));
    float min[3] = {}, max[3] = {};
    connection->GetBoundingBox(min, max);
    CHECK_NEAR(min[0], 112.0f);   // the position widened by the floor quad; the top is the highest terrain corner
    CHECK_NEAR(max[0], 128.0f);
    CHECK_NEAR(min[1], 50.0f);
    CHECK_NEAR(max[1], 103.0f);
    connection->Shutdown();
    connection->Release();
    SetUndergroundConnectionHost(nullptr);
}

// A connection on a cell that already has a piece takes it and lays nothing; a loaded connection never lays a piece.
void TestInitOwnPieceAndLoaded() {
    FakeHost host;
    SetUndergroundConnectionHost(&host);
    host.pieces[{20, 20}] = MakeSubway(20, 20);
    host.pieces[{21, 20}] = MakeSubway(21, 20);
    cSC4SubwayConnectionOccupant* own = MakeConnection(20, 20);
    CHECK(own->Init());
    CHECK(host.placements.empty());
    CHECK(own->NetworkOccupant() == static_cast<cISC4Occupant*>(host.pieces[{20, 20}]));

    // the record of a connection beside a piece, read back: Init keeps to the cell's own piece (none) and lays nothing
    cSC4SubwayConnectionOccupant* fresh = MakeConnection(40, 40);
    PortMemoryOStream out;
    CHECK(fresh->Write(out));
    host.pieces[{41, 40}] = MakeSubway(41, 40);
    auto* loaded = new cSC4SubwayConnectionOccupant();
    loaded->AddRef();
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(loaded->Read(in));
    float position[3] = {};
    loaded->GetPosition(position);
    CHECK_NEAR(position[0], 648.0f);
    CHECK(loaded->Init());
    CHECK(host.placements.empty());
    CHECK(loaded->NetworkOccupant() == nullptr);
    CHECK(loaded->CellX() == 40 && loaded->CellZ() == 40);
    for (auto* c : {own, fresh, loaded}) {
        c->Shutdown();
        c->Release();
    }
    SetUndergroundConnectionHost(nullptr);
}

cRZMessage2Standard* Message(std::uint32_t type, cISC4Occupant* occupant) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetVoid1(occupant);
    return message;
}

// DoMessage: a subway piece inserted on its cell becomes its piece, one on another cell does not; the piece removed is let go;
// the building removed takes the connection out of the occupant manager; losing the building through SetBuildingOccupant too.
void TestMessages() {
    FakeHost host;
    SetUndergroundConnectionHost(&host);
    cSC4SubwayConnectionOccupant* connection = MakeConnection(5, 5);
    CHECK(connection->Init());
    CHECK(connection->NetworkOccupant() == nullptr);
    CHECK(host.placements.empty());   // no neighbor to join

    cSC4SubwayOccupant* elsewhere = MakeSubway(6, 5);
    cRZMessage2Standard* message = Message(kMessageUndergroundInserted, elsewhere);
    connection->DoMessage(message);
    message->Release();
    CHECK(connection->NetworkOccupant() == nullptr);

    cSC4SubwayOccupant* here = MakeSubway(5, 5);
    message = Message(kMessageUndergroundInserted, here);
    connection->DoMessage(message);
    message->Release();
    CHECK(connection->NetworkOccupant() == static_cast<cISC4Occupant*>(here));

    auto* pipe = new cSC4PipeOccupant();   // another network's piece is not this connection's
    pipe->AddRef();
    message = Message(kMessageUndergroundRemoved, pipe);
    connection->DoMessage(message);
    message->Release();
    CHECK(connection->NetworkOccupant() == static_cast<cISC4Occupant*>(here));

    message = Message(kMessageUndergroundRemoved, here);
    connection->DoMessage(message);
    message->Release();
    CHECK(connection->NetworkOccupant() == nullptr);
    CHECK_NEAR(connection->TrenchFloorY(), 35.0f);

    auto* building = new cSC4SubwayOccupant();   // any occupant stands in for the building above
    building->AddRef();
    connection->SetBuildingOccupant(building);
    CHECK(connection->BuildingOccupant() == static_cast<cISC4Occupant*>(building));
    message = Message(kMessageBuildingRemoved, elsewhere);   // not its building
    connection->DoMessage(message);
    message->Release();
    CHECK(host.removed.empty());
    message = Message(kMessageBuildingRemoved, building);
    connection->DoMessage(message);
    message->Release();
    CHECK(host.removed.size() == 1);
    if (!host.removed.empty()) CHECK(host.removed[0].first == static_cast<cISC4Occupant*>(connection) && !host.removed[0].second);
    CHECK(connection->BuildingOccupant() == nullptr);

    cSC4SubwayConnectionOccupant* other = MakeConnection(9, 9);
    other->Init();
    other->SetBuildingOccupant(building);
    other->SetBuildingOccupant(nullptr);
    CHECK(host.removed.size() == 2);
    if (host.removed.size() == 2) CHECK(host.removed[1].first == static_cast<cISC4Occupant*>(other) && host.removed[1].second);

    for (auto* c : {connection, other}) {
        c->Shutdown();
        c->Release();
    }
    for (cIGZUnknown* u : {static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(elsewhere)), static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(here)),
                           static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(pipe)), static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(building))})
        u->Release();
    SetUndergroundConnectionHost(nullptr);
}

}  // namespace

int main() {
    TestGeometryTables();
    TestCustomViews();
    TestInitJoinsNeighbor();
    TestInitOwnPieceAndLoaded();
    TestMessages();
    std::printf(failures ? "FAILED\n" : "sc4_subwayconnection ok\n");
    return failures ? 1 : 0;
}
