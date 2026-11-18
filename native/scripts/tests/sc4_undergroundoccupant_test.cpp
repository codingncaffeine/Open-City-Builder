// cSC4UndergroundOccupant: the data model (init / record / terrain heights / bounding boxes), the geometry builders
// (walls, floor, transformation matrix, custom pieces), the hide-triangle logic, the zipper queries, the messages and
// the two Draw passes, driven through a fake host (the seams) and a recording gd driver for the draw context.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "render/gd/GDriverBase.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DBSegment.h"
#include "sc4/DrawContext.h"
#include "sc4/RenderProperties.h"
#include "sc4/UndergroundOccupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) <= 1e-5f)

using namespace sc4;

namespace {

// ---- fakes --------------------------------------------------------------------------------------------------
struct FakeBinding final : IUndergroundTextureBinding {
    std::uint32_t ids[2] = {0, 0};
    int releases = 0;
    const std::uint32_t* TextureIds() override { return ids; }
    void Release() override { ++releases; }
};

struct FakeConnection final : IUndergroundConnectionOccupant {
    int releases = 0;
    void Release() override { ++releases; }
};

struct FakeModelInstance final : s3d::cS3DModelInstanceBasic {
    void GetWorldBounds(cS3DBoundingBox&) override {}
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
};

struct FakeMeshes final : IUndergroundModelMeshes {
    int count = 1;
    s3d::cS3DMaterial material;
    SC4Mesh mesh{};
    int releases = 0;
    int Count() override { return count; }
    const SC4Mesh* Mesh(int) override { return &mesh; }
    s3d::cS3DMaterial* Material(int) override { return &material; }
    void Release() override { ++releases; }
};

// The recording driver the draw context talks to.
struct Driver final : gd::cGDriverBase {
    gd::sGDMode mode{};
    std::vector<std::uint32_t> textures;
    std::vector<std::pair<int, int>> draws;    // primitive, count
    std::vector<std::pair<int, int>> formats;  // format, stride
    int matrices = 0;
    bool GetVideoModeInfo(gd::sGDMode& out) override {
        out = mode;
        return true;
    }
    void DrawArrays(gd::eGDPrimType primitive, int, int count) override {
        draws.push_back({static_cast<int>(primitive), count});
    }
    void InterleavedArrays(gd::eGDVertexFormat format, int stride, const void*) override {
        formats.push_back({static_cast<int>(format), stride});
    }
    void SetTexture(std::uint32_t texture, std::uint32_t) override { textures.push_back(texture); }
    void LoadMatrix(const float*) override { ++matrices; }
};

struct Host final : IUndergroundOccupantHost {
    int statics = 0;
    int prepared = 0;
    bool inCity = false;
    bool connectionPresent = false;
    std::map<std::pair<int, int>, cSC4UndergroundOccupant*> cells;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bindingKeys;
    std::vector<FakeBinding*> bindings;
    std::vector<FakeConnection*> connections;
    FakeMeshes meshes;
    FakeModelInstance* createdModel = nullptr;
    std::vector<std::pair<s3d::cIS3DModelInstance*, int>> rendererAdds;

    ~Host() override {
        for (auto* binding : bindings) delete binding;
        for (auto* connection : connections) delete connection;
        delete createdModel;
    }
    void InitNetworkOccupantStatics() override { ++statics; }
    IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) override {
        bindingKeys.push_back({group, instance});
        auto* binding = new FakeBinding();
        binding->ids[0] = 0x9000u + static_cast<std::uint32_t>(bindings.size());
        bindings.push_back(binding);
        return binding;
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x * 0.0625f));
        cellZ = static_cast<int>(std::floor(z * 0.0625f));
    }
    bool IsCellInCity(int, int) override { return inCity; }
    void PrepareRenderer() override { ++prepared; }
    void AddModelInstanceToRenderer(s3d::cIS3DModelInstance* instance, int layer) override {
        rendererAdds.push_back({instance, layer});
    }
    s3d::cIS3DModelInstance* CreateCustomModel(std::uint32_t) override {
        if (!createdModel) {
            createdModel = new FakeModelInstance();
            createdModel->AddRef();   // the original returns one reference
        }
        return createdModel;
    }
    cSC4UndergroundOccupant* FindUndergroundOccupant(int x, int z, std::uint32_t) override {
        const auto it = cells.find({x, z});
        if (it == cells.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    bool FindUndergroundConnectionOccupant(int, int, std::uint32_t, IUndergroundConnectionOccupant*& out) override {
        if (!connectionPresent) {
            out = nullptr;
            return false;
        }
        auto* connection = new FakeConnection();
        connections.push_back(connection);
        out = connection;
        return true;
    }
    IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance*) override { return &meshes; }
};

// The base class is abstract in the original (three pure virtuals), so the test instantiates this.
struct TestOccupant final : cSC4UndergroundOccupant {
    tCustomGeometryInfo info{};
    std::uint32_t networkType = 4;
    const tCustomGeometryInfo* GetCustomGeometryInfo() const override { return &info; }
    std::uint32_t GetNetworkType() const override { return networkType; }
    std::int32_t GetModelInstanceOffset() const override { return 0x6000000; }
};

// The network occupant seam the recovered base class uses for its persist ref.
struct NetHost final : INetworkOccupantHost {
    int posts = 0;
    void PostMessage(std::uint32_t, cISC4Occupant*, std::uint32_t) override { ++posts; }
    void RegisterWithNetworkManager(cISC4Occupant*) override {}
    bool IntersectionPieceId(std::uint32_t) override { return false; }
    void ReadPersistRef(cIGZIStream&, std::uint32_t, cIGZUnknown*& out) override { out = nullptr; }
    void WritePersistRef(cIGZOStream&, cIGZUnknown*) override {}
};

// The message server the occupant posts its manager notifications through.
struct MessageServer final : cIGZMessageServer2 {
    std::vector<std::uint32_t> posted;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override {
        posted.push_back(message->GetType());
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return false; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// corners of one tile cell (16 m), with the given first corner height
void CellCorners(int cellX, int cellZ, float y0, float (*corners)[3]) {
    const float x = static_cast<float>(cellX * 16);
    const float z = static_cast<float>(cellZ * 16);
    const float xs[4] = {x, x + 16, x + 16, x};
    const float zs[4] = {z, z, z + 16, z + 16};
    for (int i = 0; i < 4; ++i) {
        corners[i][0] = xs[i];
        corners[i][1] = y0 + static_cast<float>(i);
        corners[i][2] = zs[i];
    }
}

const float kCorners[4][3] = {{16.0f, 5.0f, 32.0f}, {32.0f, 6.0f, 32.0f}, {32.0f, 7.0f, 48.0f}, {16.0f, 8.0f, 48.0f}};

void FillGeometryInfo(TestOccupant& occupant, const UndergroundVertex* vertices, const std::int32_t* edge0,
                      const std::int32_t* hinge, float* edgePercentages, float* hingePercentages, int count) {
    occupant.info = tCustomGeometryInfo{};
    occupant.info.vertices = vertices;
    occupant.info.edge0VertIndices = edge0;
    occupant.info.hingeVertIndices = hinge;
    occupant.info.edgeDistancePercentages = edgePercentages;
    occupant.info.hingeDistancePercentages = hingePercentages;
    occupant.info.percentageCount = count;
    occupant.info.textureRecords[0].instance = 0x11111111;   // piece texture
    occupant.info.textureRecords[1].instance = 0x22222222;   // trench texture
}

}  // namespace

int main() {
    Host host;
    NetHost netHost;
    MessageServer server;
    SetUndergroundOccupantHost(&host);
    SetNetworkOccupantHost(&netHost);
    SetDBSegmentMessageServer(&server);

    // geometry info vertices: five points 16 apart along x, the last link closing the loop
    const UndergroundVertex infoVertices[5] = {{0, 0, 0}, {16, 0, 0}, {32, 0, 0}, {48, 0, 0}, {64, 0, 0}};
    const std::int32_t infoEdge0[5] = {0, 1, 2, 3, 4};
    const std::int32_t infoHinge[5] = {1, 2, 3, 4, 0};
    float edgePercentages[8] = {};
    float hingePercentages[8] = {};

    // ---- construction defaults -------------------------------------------------------------------------------
    auto* occupant = new TestOccupant();
    occupant->AddRef();
    CHECK(occupant->GetType() == kOccupantTypeNetwork);
    CHECK(occupant->GetGZIID() == 0);
    CHECK(occupant->PieceId() == 0);
    CHECK(occupant->WallQuadCount() == 0);
    CHECK(occupant->GeometryInfo() == nullptr);
    CHECK(occupant->PieceTexture() == nullptr);
    CHECK(occupant->CustomModel() == nullptr);
    CHECK(!occupant->IsPlaced());   // flags bit 0 is clear
    const cS3DMatrix4& identity = occupant->ModelMatrix();
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) CHECK(identity.m[row][col] == (row == col ? 1.0f : 0.0f));

    // ---- StaticInit / SetupPackedRenderState -----------------------------------------------------------------
    {
        CHECK(cSC4UndergroundOccupant::StaticInit());
        CHECK(host.statics == 1);
        CHECK(host.prepared == 1);
        CHECK(!cSC4UndergroundOccupant::StaticInit());   // already done
        CHECK(host.statics == 1);
        cSC4PackedRenderState expected;
        expected.PackRenderState(true, true, true, false, false, 1, false, 3, -1, -1, -1, -1.0f, 1, -1);
        CHECK(cSC4UndergroundOccupant::TrenchPackedRenderState().value == expected.value);
        CHECK(cSC4UndergroundOccupant::TrenchPackedRenderState().mask == expected.mask);
        expected.PackRenderState(true, true, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);
        CHECK(cSC4UndergroundOccupant::PiecePackedRenderState().value == expected.value);
        expected.PackRenderState(true, true, true, false, true, 0, false, 3, 4, 5, -1, -1.0f, 1, -1);
        CHECK(cSC4UndergroundOccupant::HighlightPackedRenderState().value == expected.value);
        CHECK(cSC4UndergroundOccupant::HighlightPackedRenderState().mask == expected.mask);
    }

    // ---- Init(corners, gziid, info) --------------------------------------------------------------------------
    FillGeometryInfo(*occupant, infoVertices, infoEdge0, infoHinge, edgePercentages, hingePercentages, 5);
    CHECK(occupant->Init(kCorners, kGZIID_cSC4PipeOccupant, &occupant->info));
    CHECK(occupant->IsInitialized());
    CHECK(occupant->GetGZIID() == kGZIID_cSC4PipeOccupant);
    // the geometry info argument is ignored: the virtual result wins (quirk)
    CHECK(occupant->GeometryInfo() == occupant->GetCustomGeometryInfo());
    CHECK(occupant->PieceTexture() != nullptr && occupant->TrenchTexture() != nullptr);
    CHECK(host.bindingKeys.size() == 2);
    CHECK(host.bindingKeys[0].first == kTextureBindingGroupPiece && host.bindingKeys[0].second == 0x22222222);
    CHECK(host.bindingKeys[1].first == kTextureBindingGroupPiece && host.bindingKeys[1].second == 0x11111111);
    // cells: the corner average (24, 40) -> cell (1, 2)
    float position[3];
    occupant->GetPosition(position);
    CHECK_NEAR(position[0], 24.0f);
    CHECK_NEAR(position[1], 5.0f);   // the *lowest* terrain height, not a corner height
    CHECK_NEAR(position[2], 40.0f);
    float heights[4];
    occupant->GetTerrainHeights(heights);
    CHECK(heights[0] == 5 && heights[1] == 6 && heights[2] == 7 && heights[3] == 8);
    // the distance percentage tables: 16/80 for the four short links, 64/128 for the closing one
    CHECK(edgePercentages[0] == 1.0f && hingePercentages[4] == 1.0f);
    for (int i = 1; i < 4; ++i) CHECK_NEAR(edgePercentages[i], 0.2f);
    CHECK_NEAR(edgePercentages[4], 0.5f);
    CHECK_NEAR(hingePercentages[0], 0.5f);
    for (int i = 1; i < 4; ++i) CHECK_NEAR(hingePercentages[i], 0.2f);
    // bounds: min y from the corners, max y from the terrain heights
    float lo[3], hi[3];
    occupant->GetBoundingBox(lo, hi);
    CHECK(lo[0] == 16 && lo[1] == 5 && lo[2] == 32);
    CHECK(hi[0] == 32 && hi[1] == 8 && hi[2] == 48);

    // ---- model y per piece type ------------------------------------------------------------------------------
    cSC4UndergroundOccupant::SetTuningParams(3.0f, true, 10.0f);
    occupant->SetQuadDraw(kUndergroundPieceStraight, 0x01);   // rotation triple (0,2,1)
    CHECK_NEAR(occupant->ModelY(), 3.0f);   // terrain (5+7)/2 min 6 minus the 3 m piece depth
    cSC4UndergroundOccupant::SetTuningParams(3.0f, false, 10.0f);
    occupant->SetQuadDraw(kUndergroundPieceStraight, 0x01);
    CHECK_NEAR(occupant->ModelY(), 16.0f);  // corner (5+7)/2 min 6 plus the 10 m intersection height
    occupant->SetQuadDraw(kUndergroundPieceCrossing, 0x00);
    CHECK_NEAR(occupant->ModelY(), 16.5f);  // sum of the terrain heights / 4 + 10
    cSC4UndergroundOccupant::SetTuningParams(3.0f, true, 10.0f);
    occupant->SetQuadDraw(kUndergroundPieceCrossing, 0x00);
    CHECK_NEAR(occupant->ModelY(), 2.0f);   // lowest terrain 5 - 3
    occupant->SetQuadDraw(0x7777, 0x00);
    CHECK_NEAR(occupant->ModelY(), 15.0f);  // unknown piece: corner 0 + the intersection height

    // ---- GetHookupData ---------------------------------------------------------------------------------------
    {
        float height = 0.0f;
        bool valid = false;
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->GetHookupData(height, valid, 0);
        CHECK(valid && height == occupant->ModelY());
        occupant->SetQuadDraw(kUndergroundPieceCrossing, 0x00);
        occupant->GetHookupData(height, valid, 1);   // edge 1 = corners (0, 3)
        CHECK(valid);
        CHECK_NEAR(height, (5.0f + 8.0f) * 0.5f + 10.0f);
        occupant->SetQuadDraw(0x7777, 0x00);
        occupant->GetHookupData(height, valid, 0);
        CHECK(!valid);
    }

    // ---- wall and floor geometry -----------------------------------------------------------------------------
    {
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->SetTerrainHeights((float[]){1.0f, 2.0f, 3.0f, 4.0f});
        occupant->CreateFloor(true);
        CHECK(occupant->TrenchGeometry().size() == 4);
        const UndergroundVertex& floor0 = occupant->TrenchGeometry()[0];
        CHECK(floor0.x == 16 && floor0.y == 5 && floor0.z == 32);
        CHECK(floor0.color[0] == 0xff && floor0.color[3] == 0);
        std::vector<UndergroundVertex> wall;
        occupant->AddWallQuad(0, 3, wall);
        CHECK(wall.size() == 4);
        CHECK(wall[0].x == 16 && wall[0].y == 1.0f && wall[0].z == 32);   // terrain height, u = v = 0
        CHECK(wall[1].x == 16 && wall[1].y == 5.0f && wall[1].z == 32);   // corner height
        CHECK_NEAR(wall[1].v, (1.0f - 5.0f) * 0.0625f);
        CHECK_NEAR(wall[1].u, 0.0f);
        CHECK(wall[2].x == 16 && wall[2].y == 8.0f && wall[2].z == 48);
        CHECK_NEAR(wall[2].u, 1.0f);
        CHECK_NEAR(wall[2].v, wall[1].v);
        CHECK(wall[3].x == 16 && wall[3].y == 4.0f && wall[3].z == 48);
        CHECK_NEAR(wall[3].u, 1.0f);
        CHECK_NEAR(wall[3].v, 0.0f);
    }

    // ---- hide triangles and walls ----------------------------------------------------------------------------
    {
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->SetQuadFlipAndTriHide();
        bool t0 = true, t1 = true, t2 = true;
        occupant->GetHiddenTriangles(t0, t1, t2);
        CHECK(!t0 && t1 && !t2);
        // flag polarity quirk: hide0 *sets* 0x1000000 while hide1/hide2 *clear* their flags when set
        CHECK(!occupant->HasNetworkFlag(kNetworkFlagHideTriangle0));
        CHECK(!occupant->HasNetworkFlag(kNetworkFlagHideTriangle1));
        CHECK(occupant->HasNetworkFlag(kNetworkFlagHideTriangle2));
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x80);   // flipped straight piece
        occupant->SetQuadFlipAndTriHide();
        occupant->GetHiddenTriangles(t0, t1, t2);
        CHECK(t0 && !t1 && t2);
        occupant->SetQuadDraw(kUndergroundPieceCrossing, 0x00);   // no 0x4238 entry: (0,1,1)
        occupant->SetQuadFlipAndTriHide();
        occupant->GetHiddenTriangles(t0, t1, t2);
        CHECK(!t0 && t1 && t2);

        // walls: piece 0x200 with rotationFlip 0 hides the edges of corners 1 and 2, then walls the other two
        occupant->SetEdgeStore(cSC4EdgeConnectionStore{});
        occupant->ClearNetworkFlag(0x80000000u);
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        host.connectionPresent = false;
        occupant->CreateWalls(false);
        CHECK(occupant->WallQuadCount() == 3);   // the connecting quad + edge 0 + edge 3
        for (std::uint32_t i = 0; i < occupant->WallQuadCount(); ++i)
            CHECK(occupant->WallGeometry(static_cast<int>(i)).size() == 4);
        // a connection piece in the cell means the connecting quad is not added and no edge is hidden, so all four
        // edges get a wall
        host.connectionPresent = true;
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->CreateWalls(false);
        CHECK(occupant->WallQuadCount() == 4);
        CHECK(!host.connections.empty() && host.connections.back()->releases == 1);
        host.connectionPresent = false;

        // a real neighbour connection on edge 3 (+z) removes that wall
        auto* neighbour = new TestOccupant();
        neighbour->AddRef();
        CHECK(neighbour->Init(kCorners, kGZIID_cSC4PipeOccupant, &neighbour->info));
        neighbour->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        host.cells[{1, 3}] = neighbour;
        occupant->SetEdgeType(4, 3, 1);
        occupant->SetNetworkFlag(1u << 4);
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->CreateWalls(false);
        CHECK(occupant->WallQuadCount() == 2);
        // an "imaginary" connection (both pieces have the high flag bit) is walled again
        occupant->SetNetworkFlag(0x80000000u);
        neighbour->SetNetworkFlag(0x80000000u);
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->CreateWalls(false);
        CHECK(occupant->WallQuadCount() == 3);
        occupant->ClearNetworkFlag(1u << 4);
        occupant->ClearNetworkFlag(0x80000000u);
        neighbour->ClearNetworkFlag(0x80000000u);
        host.cells.clear();
        neighbour->Release();
    }

    // ---- flags and messages ----------------------------------------------------------------------------------
    {
        occupant->ClearNetworkFlag(kNetworkFlagUndergroundDirty);
        CHECK(!occupant->ModelDirty());
        occupant->ClearNetworkFlag(0x00010000);   // no change: still clean
        CHECK(!occupant->ModelDirty());
        occupant->SetNetworkFlag(0x00010000);
        CHECK(occupant->ModelDirty());
        CHECK(occupant->HasNetworkFlag(0x00010000));
        occupant->ClearNetworkFlag(0x00010000);
        CHECK(!occupant->HasNetworkFlag(0x00010000));
        CHECK(occupant->ModelDirty());            // the second change keeps it dirty

        server.posted.clear();
        occupant->PostOccupantMessage(kMessageOccupantInserted, 7);
        CHECK(server.posted.size() == 2);
        CHECK(server.posted[0] == kMessageOccupantInserted);
        CHECK(server.posted[1] == kMessageUndergroundInserted);
        server.posted.clear();
        occupant->PostOccupantMessage(kMessageOccupantRemovedFromManager, 7);
        CHECK(server.posted.size() == 2);
        CHECK(server.posted[1] == kMessageUndergroundRemoved);
        server.posted.clear();
        occupant->PostOccupantMessage(kMessageOccupantChanged, 7);
        CHECK(server.posted.size() == 1);   // no manager message for other messages
        server.posted.clear();
    }

    // ---- ModelStateChanged -----------------------------------------------------------------------------------
    {
        host.inCity = false;
        occupant->SetNetworkFlag(kNetworkFlagUndergroundDirty);
        occupant->ModelStateChanged(false);
        CHECK(!occupant->ModelDirty() && !occupant->NeedsModelCreate());
        host.inCity = true;
        host.rendererAdds.clear();
        occupant->SetFlag(2);
        // a neighbour on edge 2 (+x) is told to rebuild
        auto* other = new TestOccupant();
        other->AddRef();
        CHECK(other->Init(kCorners, kGZIID_cSC4PipeOccupant, &other->info));
        other->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        host.cells[{2, 2}] = other;
        occupant->SetEdgeType(4, 2, 1);
        occupant->SetNetworkFlag(1u << 4);
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        CHECK(!other->ModelDirty());
        occupant->ModelStateChanged(true);
        CHECK(occupant->ModelDirty() && occupant->NeedsModelCreate());
        CHECK(other->ModelDirty());
        CHECK(host.rendererAdds.size() == 1);
        CHECK(host.rendererAdds[0].first == nullptr && host.rendererAdds[0].second == 3);
        occupant->ModelStateChanged(true);   // already dirty: nothing happens
        CHECK(host.rendererAdds.size() == 1);
        occupant->ClearNetworkFlag(1u << 4);
        host.cells.clear();
        other->Release();
    }

    // ---- record round trip -----------------------------------------------------------------------------------
    {
        occupant->SetQuadDraw(kUndergroundPiecePipe, 0x82);
        occupant->SetModelAttributeState(0x12, 0x34, 2.5f);
        occupant->SetTerrainHeights((float[]){1.5f, 2.5f, 3.5f, 4.5f});
        occupant->ComputeModelYPosition();
        occupant->CreateFloor(false);
        occupant->CreateWalls(false);
        occupant->CreateTransformationMatrix();
        PortMemoryOStream out;
        CHECK(occupant->Write(out));
        // two version words: the underground one (3) in front of the network occupant's (8)
        CHECK(out.bytes().size() > 4);
        CHECK(out.bytes()[0] == 3 && out.bytes()[1] == 0);
        CHECK(out.bytes()[2] == 8 && out.bytes()[3] == 0);
        auto* restored = new TestOccupant();
        restored->AddRef();
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(restored->Read(in));
        CHECK(restored->GetGZIID() == occupant->GetGZIID());
        CHECK(restored->PieceId() == kUndergroundPiecePipe);
        CHECK(restored->GetRotation() == 2 && restored->GetFlip() == 1);
        CHECK(restored->WallQuadCount() == occupant->WallQuadCount());
        CHECK(restored->WallGeometry(0).size() == occupant->WallGeometry(0).size());
        CHECK(restored->TrenchGeometry().size() == 4);
        CHECK_NEAR(restored->TrenchGeometry()[0].y, occupant->TrenchGeometry()[0].y);
        CHECK_NEAR(restored->ModelY(), occupant->ModelY());
        CHECK(restored->ModelAttributeFlags0() == 0x12 && restored->ModelAttributeFlags1() == 0x34);
        CHECK_NEAR(restored->ModelShade3(), 2.5f);
        bool t0 = true, t1 = false, t2 = false;
        occupant->GetHiddenTriangles(t0, t1, t2);
        bool r0 = false, r1 = false, r2 = false;
        restored->GetHiddenTriangles(r0, r1, r2);
        CHECK(r0 == t0 && r1 == t1 && r2 == t2);
        CHECK(restored->NeedsModelCreate() && !restored->ModelCreated());
        const cS3DMatrix4& a = occupant->ModelMatrix();
        const cS3DMatrix4& b = restored->ModelMatrix();
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) CHECK_NEAR(a.m[row][col], b.m[row][col]);
        restored->Release();
    }

    // ---- transformation matrix -------------------------------------------------------------------------------
    {
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x01);   // rotation 1, no flip
        occupant->CreateTransformationMatrix();
        const cS3DMatrix4& matrix = occupant->ModelMatrix();
        CHECK_NEAR(matrix.m[0][0], 0.0f);
        CHECK_NEAR(matrix.m[0][2], -1.0f);
        CHECK_NEAR(matrix.m[2][0], 1.0f);
        CHECK_NEAR(matrix.m[2][2], 0.0f);
        CHECK_NEAR(matrix.m[1][1], 1.0f);
        CHECK_NEAR(matrix.m[0][3], 24.0f);   // position x
        CHECK_NEAR(matrix.m[1][3], occupant->ModelY());
        CHECK_NEAR(matrix.m[2][3], 40.0f);
        CHECK_NEAR(matrix.m[3][3], 1.0f);
        cS3DTransform transform;
        occupant->GetTransform(transform);
        CHECK_NEAR(transform.translation[0], 24.0f);
        CHECK_NEAR(transform.translation[1], occupant->ModelY());
        CHECK_NEAR(transform.translation[2], 40.0f);
        // flipped: the x row is mirrored
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x81);
        occupant->CreateTransformationMatrix();
        CHECK_NEAR(matrix.m[0][2], 1.0f);
        CHECK_NEAR(matrix.m[0][3], 24.0f);
    }

    // ---- custom piece geometry -------------------------------------------------------------------------------
    {
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        cSC4UndergroundOccupant::SetTuningParams(3.0f, true, 10.0f);
        occupant->SetTerrainHeights((float[]){5.0f, 6.0f, 7.0f, 8.0f});
        occupant->CreateTransformationMatrix();
        const UndergroundVertex source[3] = {{0, 0, 0}, {16, 0, 16}, {16, 16, 16}};
        std::vector<UndergroundVertex> piece;
        occupant->MakeCustomUndergroundPiece(false, false, piece, source, 3, nullptr, 0, 0, nullptr, 0, 0, nullptr);
        CHECK(piece.size() == 3);
        CHECK_NEAR(piece[0].x, 24.0f);  // the model matrix translation (the corner average)
        CHECK_NEAR(piece[0].u, 0.5f);   // (0 + 8) / 16
        CHECK_NEAR(piece[1].u, 1.5f);   // (16 + 8) / 16
        CHECK_NEAR(piece[0].v, 0.5f);   // equal y and z ranges: the z branch is used
        CHECK(piece[0].color[0] == 0xff && piece[0].color[3] == 0xff);
        occupant->MakeCustomUndergroundPiece(true, true, piece, source, 3, nullptr, 0, 0, nullptr, 0, 0, nullptr);
        CHECK_NEAR(piece[0].u, 0.5f);   // 1 - 0.5
        CHECK_NEAR(piece[1].u, -0.5f);  // 1 - 1.5
        CHECK_NEAR(piece[0].v, 0.5f);
        // a hookup neighbour at edge 0: the crossing piece reports edge 2 -> (7 + 8) / 2 + 10 = 17.5
        auto* crossing = new TestOccupant();
        crossing->AddRef();
        CHECK(crossing->Init(kCorners, kGZIID_cSC4PipeOccupant, &crossing->info));
        crossing->SetQuadDraw(kUndergroundPieceCrossing, 0x00);
        crossing->SetTerrainHeights((float[]){5.0f, 6.0f, 7.0f, 8.0f});
        host.cells[{0, 2}] = crossing;   // edge 0 = -x from cell (1,2)
        const std::int32_t hookup[1] = {1};
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->CreateTransformationMatrix();
        occupant->MakeCustomUndergroundPiece(false, false, piece, source, 3, hookup, 1, 0, nullptr, 0, 0, nullptr);
        CHECK(piece.size() == 3);
        CHECK_NEAR(piece[1].y, 17.5f);   // the model y minus (model y - the neighbour's hookup height)
        // edgeToCellOffset (i386 0x00bfb1a0): edge 2 is +x and edge 3 is +z, so both ends of a run find their neighbour
        host.cells.clear();
        host.cells[{2, 2}] = crossing;   // edge 2 = +x; the crossing reports its edge 0 -> (5 + 6) / 2 + 10 = 15.5
        occupant->MakeCustomUndergroundPiece(false, false, piece, source, 3, nullptr, 0, 0, hookup, 1, 2, nullptr);
        CHECK_NEAR(piece[1].y, 15.5f);
        host.cells.clear();
        host.cells[{1, 3}] = crossing;   // edge 3 = +z; the crossing reports its edge 1
        float edge1 = 0.0f;
        bool edge1Valid = false;
        crossing->GetHookupData(edge1, edge1Valid, 1);
        occupant->MakeCustomUndergroundPiece(false, false, piece, source, 3, nullptr, 0, 0, hookup, 1, 3, nullptr);
        CHECK(edge1Valid);
        CHECK_NEAR(piece[1].y, edge1);
        host.cells.clear();
        crossing->Release();
    }

    // ---- zipper queries --------------------------------------------------------------------------------------
    {
        float subwayCorners[4][3];
        CellCorners(1, 2, 5.0f, subwayCorners);
        auto* subway = new TestOccupant();
        auto* p1 = new TestOccupant();
        auto* p2 = new TestOccupant();
        auto* p3 = new TestOccupant();
        auto* p4 = new TestOccupant();
        for (TestOccupant* p : {subway, p1, p2, p3, p4}) p->AddRef();
        CHECK(subway->Init(subwayCorners, kGZIID_cSC4SubwayOccupant, &subway->info));
        subway->SetQuadDraw(kUndergroundPiecePipe, 0x00);
        const int cellXY[4][2] = {{1, 1}, {1, 0}, {1, -1}, {1, 3}};
        TestOccupant* chain[4] = {p1, p2, p3, p4};
        for (int i = 0; i < 4; ++i) {
            float corners[4][3];
            CellCorners(cellXY[i][0], cellXY[i][1], 5.0f, corners);
            CHECK(chain[i]->Init(corners, kGZIID_cSC4SubwayOccupant, &chain[i]->info));
            chain[i]->SetQuadDraw(kUndergroundPiecePipe, 0x00);
        }
        // depth 0: no recursion
        CHECK(subway->GetZipperCandidateCount(subway, 0, 0) == 1);
        host.cells[{1, 1}] = p1;
        host.cells[{1, 0}] = p2;
        host.cells[{1, -1}] = p3;
        host.cells[{1, 3}] = p4;
        CHECK(subway->GetZipperCandidateCount(p1, 1, 4) == 3);   // p1 -> p2 -> p3
        // rotation 0: edge 3 (+z) has one candidate, edge 1 (-z) has three -> the zipper faces +z
        bool param = false;
        CHECK(subway->GetZipperPieceType(param) == 1);
        CHECK(param == true);
        // a connection piece in the cell blocks the zipper query
        host.connectionPresent = true;
        CHECK(subway->GetZipperCandidateCount(subway, 0, 4) == 0);
        CHECK(!subway->GetZipperPieceType(param));
        host.connectionPresent = false;
        host.cells.clear();
        // a pipe (not the subway) has no zipper piece type
        auto* pipeOccupant = new TestOccupant();
        pipeOccupant->AddRef();
        CHECK(pipeOccupant->Init(kCorners, kGZIID_cSC4PipeOccupant, &pipeOccupant->info));
        pipeOccupant->SetQuadDraw(kUndergroundPiecePipe, 0x00);
        CHECK(!pipeOccupant->GetZipperPieceType(param));
        pipeOccupant->Release();
        for (TestOccupant* p : {subway, p1, p2, p3, p4}) p->Release();
    }

    // ---- Draw ------------------------------------------------------------------------------------------------
    {
        Driver driver;
        driver.mode.flags[1] = 3;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext context;
        context.SetDriver(&driver);
        context.SetRenderProperties(&properties);
        context.Init();
        CHECK(context.IsInitialized());
        occupant->SetQuadDraw(kUndergroundPieceStraight, 0x00);
        occupant->SetTerrainHeights((float[]){5.0f, 6.0f, 7.0f, 8.0f});
        occupant->CreateFloor(false);
        occupant->CreateWalls(false);
        CHECK(occupant->TrenchGeometry().size() == 4);
        CHECK(occupant->WallQuadCount() >= 2);
        // pass 0: the trench quad and one draw per wall quad, all triangle strips (6) with format 10
        context.SetRenderPass(0);
        driver.draws.clear();
        driver.textures.clear();
        CHECK(occupant->Draw(&context, false) == 2);
        CHECK(driver.draws.size() == 1 + occupant->WallQuadCount());
        for (const auto& draw : driver.draws) {
            CHECK(draw.first == 6);
            CHECK(draw.second == 4);
        }
        CHECK(driver.textures.size() == 2);
        CHECK(driver.textures[0] == occupant->TrenchTexture()->TextureIds()[0]);
        CHECK(driver.textures[1] == occupant->PieceTexture()->TextureIds()[0]);
        // highlight: nothing to draw in this port (CreateUndergroundModel is not recovered yet), still 1
        driver.draws.clear();
        CHECK(occupant->Draw(&context, true) == 1);
        CHECK(driver.draws.empty());
        // pass 1: the junction model CreateUndergroundModel loaded (the custom model, i386 +0x240), its meshes through the seam
        CHECK(occupant->CustomModel() != nullptr);
        FakeModelInstance instance;
        occupant->SetModelInstance(&instance);
        s3d::SC4DrawAttributes attributes;
        attributes.flags = 0x20000000u;
        instance.SetDrawAttributes(attributes);
        host.meshes = FakeMeshes{};
        host.meshes.material.flags = 1;
        occupant->ClearNetworkFlag(kNetworkFlagUndergroundNoPiece);
        context.SetRenderPass(1);
        driver.draws.clear();
        driver.matrices = 0;
        CHECK(occupant->Draw(&context, false) == 1);
        CHECK(host.meshes.releases == 1);
        CHECK(host.meshes.material.flags == 0x11);
        CHECK(host.meshes.material.b9 == 4 && host.meshes.material.b10 == 5);
        CHECK(driver.matrices == 1);
        CHECK(!occupant->ModelDirty());
        // the "no piece model" flag skips the pass and leaves the dirty flag alone
        occupant->SetNetworkFlag(kNetworkFlagUndergroundNoPiece);
        occupant->ClearNetworkFlag(kNetworkFlagUndergroundDirty);
        occupant->SetNetworkFlag(kNetworkFlagUndergroundDirty);   // toggled: marks the model dirty
        CHECK(occupant->ModelDirty());
        driver.matrices = 0;
        CHECK(occupant->Draw(&context, false) == 1);
        CHECK(driver.matrices == 0);
        CHECK(occupant->ModelDirty());
        occupant->SetModelInstance(nullptr);
    }

    // ---- ChangeZoomLevel -------------------------------------------------------------------------------------
    {
        Driver driver;
        driver.mode.flags[1] = 3;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext context;
        context.SetDriver(&driver);
        context.SetRenderProperties(&properties);
        context.Init();
        context.SetZoomAndRotation(2, 0);
        auto& table = cSC4UndergroundOccupant::ZoomTextureBindings();
        table.clear();
        table.push_back({0x40, nullptr});
        table.push_back({0x80, nullptr});
        occupant->ChangeZoomLevel(&context);
        CHECK(cSC4UndergroundOccupant::CurrentZoom() == 2);
        CHECK(table.size() == 2);
        CHECK(table[0].instanceBase == 0x42 && table[1].instanceBase == 0x82);
        CHECK(table[0].binding != nullptr && table[1].binding != nullptr);
        CHECK(host.bindingKeys.back().first == kTextureBindingGroupZoom);
        CHECK(host.bindingKeys.back().second == 0x82);
        // quirk: the instance bases keep growing on every call
        occupant->ChangeZoomLevel(&context);
        CHECK(table[0].instanceBase == 0x44 && table[1].instanceBase == 0x84);
        CHECK(table[0].binding != nullptr);
        table.clear();
    }

    // ---- accessors -------------------------------------------------------------------------------------------
    {
        FakeModelInstance instance;
        occupant->SetModelInstance(&instance);
        s3d::cIS3DModelInstance* instances[2] = {nullptr, nullptr};
        int count = 0;
        occupant->GetModelInstances(0, 0, instances, 2, count);
        CHECK(count == 1 && instances[0] == &instance);
        occupant->SetModelInstance(nullptr);
        IUndergroundTextureBinding* first = nullptr;
        IUndergroundTextureBinding* second = nullptr;
        occupant->ClearNetworkFlag(0x00030000);
        occupant->GetCurrentTextureBindings(first, second);
        CHECK(first == occupant->PieceTextureAt(0) && second == occupant->PieceTextureAt(1));
        std::uint32_t sort = 0;
        occupant->ClearNetworkFlag(0x06000000);   // the triangle flags the geometry builders set
        occupant->GetSortValue(sort);
        CHECK(sort == kUndergroundDefaultSortValue);
        occupant->SetNetworkFlag(0x04000000);
        occupant->GetSortValue(sort);
        CHECK(sort == occupant->PieceId());
        const cS3DVector3 color = occupant->GetDrawColor();
        CHECK(color.x == 1.0f && color.y == 1.0f && color.z == 1.0f);
        std::uint32_t piece = 0;
        int networkType = 0;
        float queryHeights[4] = {};
        float modelY = 0.0f;
        occupant->GetQueryData(piece, networkType, queryHeights, modelY);
        CHECK(piece == occupant->PieceId());
        CHECK_NEAR(modelY, occupant->ModelY());
        CHECK(queryHeights[0] == occupant->Corner(0).y);
        // IsPlaced needs flags bit 0
        CHECK(!occupant->IsPlaced());
        occupant->SetFlag(1);
        CHECK(occupant->IsPlaced());
        // vertex heights
        float newHeights[4] = {9, 10, 11, 12};
        occupant->SetVertexHeights(newHeights);
        float readHeights[4] = {};
        occupant->GetVertexHeights(readHeights);
        CHECK(readHeights[0] == 9 && readHeights[3] == 12);
        // QueryInterface
        void* out = nullptr;
        CHECK(occupant->QueryInterface(kGZIID_cISC4UndergroundOccupant, &out) && out == occupant);
        out = nullptr;
        CHECK(occupant->QueryInterface(kGZIID_cISC4UndergroundOccupant3, &out) && out == occupant);
        out = nullptr;
        CHECK(occupant->QueryInterface(kGZIID_cISC4Occupant, &out) && out != nullptr);
        out = nullptr;
        CHECK(!occupant->QueryInterface(0x12345678, &out));
        // the no-op overrides
        occupant->AddModel(nullptr, nullptr, 0);
        occupant->CreateHighlight(true);
        occupant->SetHighlight(1, true);
        CHECK(!occupant->HasViewDependentModel());
        CHECK(occupant->GetModelInstanceOffset() == 0x6000000);
        CHECK(occupant->GetNetworkType() == 4);
    }

    // ---- shutdown --------------------------------------------------------------------------------------------
    {
        occupant->SetModelInstance(nullptr);
        occupant->SetCustomModel(host.CreateCustomModel(0));
        CHECK(occupant->CustomModel() != nullptr);
        CHECK(occupant->Shutdown());
        CHECK(occupant->CustomModel() == nullptr);
        CHECK(!occupant->IsInitialized());
        host.createdModel = nullptr;   // released by the occupant; the fake's Release only counts
    }

    occupant->Release();

    SetUndergroundOccupantHost(nullptr);
    SetNetworkOccupantHost(nullptr);
    SetDBSegmentMessageServer(nullptr);
    std::puts(failures ? "sc4_undergroundoccupant: FAILED" : "sc4_undergroundoccupant ok");
    return failures != 0;
}
