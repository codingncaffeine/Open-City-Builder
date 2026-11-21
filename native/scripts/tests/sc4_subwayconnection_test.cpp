// The subway's pieces apart from the drag: the subway piece tables and StaticInit (i386 0x0013756a), the subway view's custom
// view (cSC4SubwayCustomView, class 0x4a19bde3), and cSC4SubwayConnectionOccupant on the underground connection base: Init
// (i386 0x0022f916: the cell, the trench floor quad, the cell's piece or a piece laid to a neighbor, never for a loaded
// connection), DoMessage (0x0023197a: pieces inserted on / removed from its cell, its building removed), SetBuildingOccupant
// and the record. The train paths of the tube pieces (GetPathInfo with the floor heights) and the connection's drawn geometry.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/PathInfo.h"
#include "sc4/PipeOccupant.h"
#include "sc4/SubwayConnectionOccupant.h"
#include "sc4/SubwayOccupant.h"
#include "sc4/UndergroundCustomView.h"
#include "sc4/ViewUtilities.h"

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

// ---- the train paths (cSC4SubwayOccupant::GetAltitude / InitPathBottomQuad / GetPathInfo, i386 0x00136d5c..0x00137320)

// The path file the subway asks for: one train path (type 4) across the cell, high above the tube, and one road path.
struct FakePathHost final : INetworkOccupantPathHost {
    std::vector<cGZPersistResourceKey> asked;
    bool lastUseElevation = true;
    std::uint32_t MapPath(std::uint32_t id) override { return id + 0x10000; }   // the subway must not map its id
    cISC4PathInfo* LoadPathInfo(const cGZPersistResourceKey& key) override {
        asked.push_back(key);
        auto* info = new cSC4PathInfo();
        info->AddRef();
        info->SetUseElevationFlag(true);
        if (tPath* train = info->NewPath(MakePathKey(4, 0, 2))) {
            train->points = {{-8.0f, 999.0f, 0.0f}, {0.0f, 999.0f, 0.0f}, {8.0f, 999.0f, 0.0f}};
            train->length = 1.0f;
        }
        if (tPath* road = info->NewPath(MakePathKey(1, 0, 2))) road->points = {{-8.0f, 999.0f, 4.0f}, {8.0f, 999.0f, 4.0f}};
        resource_ = info;
        info->AddRef();   // the test keeps the resource to look at it
        return info;
    }
    ~FakePathHost() { if (resource_) resource_->Release(); }
    cSC4PathInfo* resource_ = nullptr;
};

// A tube piece on a sloped floor (corner heights 40, 42, 50, 46 for the corners at (x0, z0), (x0, z0+16), (x0+16, z0+16),
// (x0+16, z0)): the train paths come from {0x296678f7, 0xa966883f, piece id | 0x6000000} with no id mapping, the resource is
// made flat, and every point sits half a unit under the floor quad (split on its 0-2 diagonal), the lengths summed again.
void TestTrainPaths() {
    FakePathHost host;
    SetNetworkOccupantPathHost(&host);
    auto* piece = new cSC4SubwayOccupant();
    piece->AddRef();
    const float x0 = 160.0f, z0 = 320.0f;
    const float corners[4][3] = {{x0, 40.0f, z0}, {x0, 42.0f, z0 + 16.0f}, {x0 + 16.0f, 50.0f, z0 + 16.0f}, {x0 + 16.0f, 46.0f, z0}};
    piece->Init(corners);
    piece->SetQuadDraw(0x200, 0);   // a straight piece: the floor quad is the trench floor
    cISC4PathInfo* info = piece->GetPathInfo();
    CHECK(info != nullptr);
    CHECK(host.asked.size() == 1);
    if (!host.asked.empty()) {
        CHECK(host.asked[0].type == 0x296678f7 && host.asked[0].group == 0xa966883f);
        CHECK(host.asked[0].instance == (0x200u | 0x6000000u));
    }
    CHECK(host.resource_ && !host.resource_->UseElevation());
    // GetAltitude on both triangles of the quad
    CHECK_NEAR(piece->GetAltitude(x0 + 8.0f, z0 + 4.0f), 43.5f);   // x run longer: corners 0, 3, 2
    CHECK_NEAR(piece->GetAltitude(x0 + 4.0f, z0 + 8.0f), 42.5f);   // z run longer: corners 0, 1, 2
    CHECK_NEAR(piece->PathBottomVertex(2).y, 50.0f);
    int trainPaths = 0;
    if (info && info->GetPathMap()) {
        for (const auto& [key, path] : *info->GetPathMap()) {
            float length = 0.0f;
            for (std::size_t i = 0; i < path.points.size(); ++i) {
                const cS3DVector3& p = path.points[i];
                CHECK_NEAR(p.y, piece->GetAltitude(p.x, p.z));
                CHECK(p.y < 60.0f);
                if (i) {
                    const cS3DVector3& q = path.points[i - 1];
                    length += std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
                }
            }
            CHECK_NEAR(path.length, length);
            if (((key >> 24) & 0xf) == 4) {
                ++trainPaths;
                CHECK(path.length > 16.0f);   // the slope makes it longer than the flat 16
            }
        }
    }
    CHECK(trainPaths == 1);
    CHECK(piece->GetPathInfo() == info);   // cached
    CHECK(host.asked.size() == 1);
    piece->Shutdown();
    piece->Release();
    SetNetworkOccupantPathHost(nullptr);
}

// ---- the connection's drawing (cSC4UndergroundConnectionOccupant 0x00230aa4..0x00232394)

struct FakeBinding final : IUndergroundTextureBinding {
    explicit FakeBinding(std::uint32_t id) : id_(id) {}
    const std::uint32_t* TextureIds() override { return &id_; }
    void Release() override { delete this; }
    std::uint32_t id_;
};

struct FakeViewHost final : IUndergroundConnectionViewHost {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> textures;   // group, instance
    std::vector<std::uint32_t> models;
    int rotation = 0;
    void PrepareRenderer() override {}
    IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) override {
        textures.push_back({group, instance});
        return new FakeBinding(instance);
    }
    s3d::cIS3DModelInstance* CreateModel(std::uint32_t instance) override {
        models.push_back(instance);
        return nullptr;
    }
    IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance*) override { return nullptr; }
    void GetZoomAndRotation(int& zoom, int& r) override { zoom = 4; r = rotation; }
    void GetCameraViewAngles(float& yaw, float& pitch, float& screenPitch) override { yaw = pitch = screenPitch = 0.0f; }
    cS3DVector3 LightColor(const cS3DVector3&) override { return {1.0f, 1.0f, 1.0f}; }
    cISC4Occupant* FindBuildingOccupant(int, int) override { return nullptr; }
};

bool HasTexture(const FakeViewHost& host, std::uint32_t group, std::uint32_t instance) {
    for (const auto& t : host.textures) if (t.first == group && t.second == instance) return true;
    return false;
}

// A station connection on a cell without a tube piece: the shaft is the 15 wide square from the connection's height (50) down
// to the trench floor (35) and 3 more, at its position; four walls from the terrain (100..103) to the floor, the floor quad; the
// textures and the model of the subway connection's table. A tube piece under it takes the walls and floor away and sets the
// shaft's bottom at its model height. The highlight quad, the model instance and a view rotation.
void TestConnectionDrawing() {
    FakeHost host;
    FakeViewHost view;
    SetUndergroundConnectionHost(&host);
    SetUndergroundConnectionViewHost(&view);
    cSC4SubwayConnectionOccupant* connection = MakeConnection(5, 5);   // position 88, 50, 88
    CHECK(connection->Init());
    CHECK(connection->ModelDirty());
    connection->CreateGeometry();
    const auto& shaft = connection->Piece();
    CHECK(shaft.size() == 10);
    if (shaft.size() == 10) {
        CHECK_NEAR(shaft[0].x, 80.5f);
        CHECK_NEAR(shaft[0].z, 80.5f);
        CHECK_NEAR(shaft[0].y, 50.0f);        // top: the connection's height
        CHECK_NEAR(shaft[1].y, 32.0f);        // bottom: 15 under it and the table's 3
        CHECK_NEAR(shaft[4].x, 95.5f);
        CHECK_NEAR(shaft[4].z, 95.5f);
        CHECK_NEAR(shaft[5].u, 2.0f);         // one u per vertex pair
        CHECK_NEAR(shaft[0].v, 0.0f);
        CHECK_NEAR(shaft[1].v, 18.0f / 15.0f);
        CHECK(shaft[3].color[0] == 0xff && shaft[3].color[3] == 0xff);
    }
    CHECK_NEAR(connection->TrenchFloorY(), 35.0f);
    CHECK(connection->WallQuadCount() == 4);
    const auto& wall = connection->Walls(0);   // corners 1 -> 0
    CHECK(wall.size() == 4);
    if (wall.size() == 4) {
        CHECK_NEAR(wall[0].x, 80.0f);
        CHECK_NEAR(wall[0].z, 96.0f);
        CHECK_NEAR(wall[0].y, 101.0f);
        CHECK_NEAR(wall[1].y, 86.0f);
        CHECK_NEAR(wall[1].v, 15.0f / 16.0f);
        CHECK_NEAR(wall[2].z, 80.0f);
        CHECK_NEAR(wall[2].y, 85.0f);
        CHECK_NEAR(wall[2].v, 15.0f / 16.0f);   // the near corner's depth
        CHECK_NEAR(wall[3].y, 100.0f);
        CHECK_NEAR(wall[3].u, 1.0f);
    }
    CHECK(connection->Trench().size() == 4);
    CHECK(HasTexture(view, kTextureBindingGroupZoom, 0x6a4e6066));
    CHECK(HasTexture(view, kTextureBindingGroupPiece, 0xe95a8d8d));
    CHECK(HasTexture(view, kTextureBindingGroupPiece, 0x095a8d70));
    CHECK(connection->PieceTexture() && connection->PieceTexture()->TextureIds()[0] == 0x6a4e6066);
    CHECK(!view.models.empty() && view.models[0] == 0x0ad80000);
    CHECK_NEAR(connection->ViewMatrix().m[0][3], 88.0f);
    CHECK_NEAR(connection->ViewMatrix().m[1][3], 50.0f);

    connection->SetHighlight(1, false);
    const auto& quad = connection->Highlight();
    CHECK(quad.size() == 4);
    if (quad.size() == 4) {
        CHECK_NEAR(quad[0].y, 101.0f);   // corner 1 first
        CHECK_NEAR(quad[1].y, 100.0f);
        CHECK(quad[0].color[0] == 0x7f && quad[0].color[1] == 0x87 && quad[0].color[2] == 0xe8 && quad[0].color[3] == 0x64);
    }
    connection->SetHighlight(0, false);
    CHECK(connection->Highlight().empty());

    s3d::cIS3DModelInstance* models[2] = {};
    int count = 0;
    connection->GetModelInstances(4, 0, models, 2, count);
    CHECK(count == 1 && models[0] == static_cast<s3d::cIS3DModelInstance*>(static_cast<s3d::cS3DModelInstanceBasic*>(connection)));
    if (models[0]) models[0]->Release();

    // a view rotation rebuilds: the models turn with it
    std::int32_t before[2] = {4, 0}, after[2] = {4, 1};
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(0xa6b79602);
    message->SetVoid1(before);
    message->SetVoid2(after);
    view.rotation = 1;
    connection->DoMessage(message);
    message->Release();
    CHECK_NEAR(connection->ViewMatrix().m[0][0], std::cos(SC4ViewUtilities::GetObjectYRotationAngle(1)));

    // a tube piece under it: no walls or floor, the shaft down to the piece's model height
    cSC4SubwayOccupant* here = MakeSubway(5, 5);
    cRZMessage2Standard* inserted = Message(kMessageUndergroundInserted, here);
    connection->DoMessage(inserted);
    inserted->Release();
    connection->CreateGeometry();
    CHECK(connection->WallQuadCount() == 4 && connection->Walls(0).empty());
    CHECK(connection->Trench().empty());
    CHECK_NEAR(connection->TrenchFloorY(), here->ModelY());
    if (connection->Piece().size() == 10) CHECK_NEAR(connection->Piece()[1].y, here->ModelY() - 3.0f);

    connection->Shutdown();
    connection->Release();
    here->Release();
    SetUndergroundConnectionViewHost(nullptr);
    SetUndergroundConnectionHost(nullptr);
}

}  // namespace

int main() {
    TestGeometryTables();
    TestCustomViews();
    TestInitJoinsNeighbor();
    TestInitOwnPieceAndLoaded();
    TestMessages();
    TestTrainPaths();
    TestConnectionDrawing();
    std::printf(failures ? "FAILED\n" : "sc4_subwayconnection ok\n");
    return failures ? 1 : 0;
}
