// The underground (pipe) view: cSC4ComplexViewOccupantFilter, cSC4WaterOccupantFilter, cSC4UndergroundCustomView, the
// pipe's piece tables (StaticInit's distance tables) and CreateUndergroundModel for the pipe pieces.
#include <cstdio>
#include <list>
#include <map>
#include <vector>

#include "sc4/ComplexViewOccupantFilter.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/PipeOccupant.h"
#include "sc4/UndergroundCustomView.h"

using namespace sc4;

namespace {
int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

constexpr std::uint32_t kRoadFlag = 0x0001, kPipeFlag = 0x0010, kSubwayFlag = 0x0080;

cSC4NetworkOccupant* MakePiece(std::uint32_t flags) {
    auto* piece = new cSC4NetworkOccupant();
    piece->AddRef();
    piece->SetNetworkFlag(flags);
    return piece;
}

struct LotHost final : IComplexViewFilterHost {
    int state = -1;
    int LotStateAt(int, int) override { return state; }
};

void TestComplexFilter() {
    LotHost lots;
    SetComplexViewFilterHost(&lots);
    cSC4NetworkOccupant* road = MakePiece(kRoadFlag);
    cSC4NetworkOccupant* pipe = MakePiece(kPipeFlag);
    cSC4NetworkOccupant* subway = MakePiece(kSubwayFlag);

    // The pipe view (SetDataView, underground mode 1): every type, every network but subways.
    cSC4ComplexViewOccupantFilter pipeView;
    pipeView.AddRef();
    pipeView.SetIncludeUnlistedTypes(true);
    pipeView.SetIncludeUnlistedNetworks(true);
    pipeView.NetworkTypes().push_back(7);
    CHECK(pipeView.IsOccupantIncluded(road));
    CHECK(pipeView.IsOccupantIncluded(pipe));
    CHECK(!pipeView.IsOccupantIncluded(subway));

    // The normal view's list (mode 0): pipes and subways out.
    cSC4ComplexViewOccupantFilter surface;
    surface.AddRef();
    surface.SetIncludeUnlistedTypes(true);
    surface.SetIncludeUnlistedNetworks(true);
    surface.NetworkTypes().push_back(4);
    surface.NetworkTypes().push_back(7);
    CHECK(surface.IsOccupantIncluded(road));
    CHECK(!surface.IsOccupantIncluded(pipe));

    // A listed type with the default off: only that type.
    cSC4ComplexViewOccupantFilter listed;
    listed.AddRef();
    listed.OccupantTypes().push_back(0xc772bf98);
    listed.SetIncludeUnlistedNetworks(true);
    CHECK(listed.IsOccupantIncluded(road));
    listed.OccupantTypes().clear();
    CHECK(!listed.IsOccupantIncluded(road));

    // The water filter forces the surface pieces back in (a filter that drops every network piece).
    cSC4ComplexViewOccupantFilter forced;
    forced.AddRef();
    forced.SetIncludeUnlistedTypes(true);
    forced.NetworkTypes().push_back(0);   // the road listed, unlisted networks off: the road would be dropped...
    forced.SetIncludeUnlistedNetworks(true);
    CHECK(!forced.IsOccupantIncluded(road));
    auto* water = new cSC4WaterOccupantFilter();
    water->AddRef();
    forced.SetForceIncludePipeOccupants(water);
    CHECK(forced.IsOccupantIncluded(road));   // ...the water filter keeps every surface piece
    CHECK(!water->IsOccupantIncluded(pipe));  // a pipe is no surface piece and has no water property
    forced.SetForceIncludePipeOccupants(nullptr);
    water->Release();

    // A lot texture whose lot is not built answers the plain-lots flag; a built one goes by its type.
    cSC4LotBaseTextureOccupant texture;
    texture.AddRef();
    cSC4ComplexViewOccupantFilter plain;
    plain.AddRef();
    lots.state = -1;
    CHECK(plain.IsOccupantIncluded(&texture));   // default: plain lots kept
    plain.SetIncludePlainLots(false);
    CHECK(!plain.IsOccupantIncluded(&texture));
    lots.state = 1;                               // a lot not built yet (state < 2) is still plain
    plain.SetIncludeUnlistedTypes(true);
    CHECK(!plain.IsOccupantIncluded(&texture));
    plain.SetIncludeUnlistedTypes(false);
    lots.state = 2;
    CHECK(!plain.IsOccupantIncluded(&texture));  // built: no listed type, default off
    plain.SetIncludeUnlistedTypes(true);
    CHECK(plain.IsOccupantIncluded(&texture));
    SetComplexViewFilterHost(nullptr);
    road->Release();
    pipe->Release();
    subway->Release();
}

struct ViewHost final : IUndergroundCustomViewHost {
    std::map<std::pair<int, int>, std::vector<cISC4Occupant*>> cells;
    int CityCellCountX() override { return 2; }
    int CityCellCountZ() override { return 2; }
    void CellOccupants(int x, int z, std::list<cISC4Occupant*>& out) override {
        for (cISC4Occupant* occupant : cells[{x, z}]) {
            occupant->AddRef();
            out.push_back(occupant);
        }
    }
    cIGZMessageServer2* MessageServer() override { return nullptr; }
};

void TestUndergroundCustomView() {
    ViewHost host;
    SetUndergroundCustomViewHost(&host);
    cSC4NetworkOccupant* road = MakePiece(kRoadFlag | 0x10000000);   // usable
    cSC4NetworkOccupant* building = MakePiece(kRoadFlag);            // still under construction
    cSC4NetworkOccupant* node = MakePiece(kRoadFlag | 0x200000);     // a lot connection node: left alone
    cSC4NetworkOccupant* pipe = MakePiece(kPipeFlag);                // not a surface piece
    host.cells[{0, 0}] = {road, pipe};
    host.cells[{1, 1}] = {building, node};
    cISC4CustomView* view = CreateCustomView(kGZCLSID_cSC4PipeCustomView);
    CHECK(view != nullptr);
    CHECK(CreateCustomView(0x12345678) == nullptr);
    if (view) {
        CHECK(view->InitCustomView());
        CHECK(road->HasNetworkFlag(0x4000));
        CHECK(road->Corner(0).color[3] == 0x40);        // SetTranslucency(true, 0x40) after the flag's 0x80
        CHECK(building->HasNetworkFlag(0x4000));
        CHECK(!node->HasNetworkFlag(0x4000));
        CHECK(!pipe->HasNetworkFlag(0x4000));
        CHECK(static_cast<cSC4UndergroundCustomView*>(view)->TranslucentCount() == 2);
        CHECK(view->ShutdownCustomView());
        CHECK(!road->HasNetworkFlag(0x4000));            // usable: opaque again
        CHECK(road->Corner(0).color[3] == 0xff);
        CHECK(building->HasNetworkFlag(0x4000));         // under construction: stays translucent
        CHECK(static_cast<cSC4UndergroundCustomView*>(view)->TranslucentCount() == 0);
        view->Release();
    }
    SetUndergroundCustomViewHost(nullptr);
    for (cSC4NetworkOccupant* piece : {road, building, node, pipe}) piece->Release();
}

void TestPipeTables() {
    cSC4PipeOccupant::StaticInit();
    const float* piece = cSC4PipeOccupant::sDiagonalPieceEdgeDistancePercentages;
    const float* cap = cSC4PipeOccupant::sDiagonalCapEdgeDistancePercentages;
    CHECK(piece[0] == 1.0f);
    CHECK(cap[4] == 1.0f);
    for (int u = 1; u < 5; ++u) {
        CHECK(piece[u] > 0.0f && piece[u] < 1.0f);
        CHECK(piece[u] == cap[4 - u]);
    }
    const tCustomGeometryInfo& info = cSC4PipeOccupant::sCustomGeometryInfo;
    CHECK(info.counts[0] == 12 && info.counts[1] == 6 && info.countB0 == 21 && info.percentageCount == 5);
    CHECK(info.textureRecords[0].instance == 0x095a8d70);   // the wall texture
    CHECK(info.textureRecords[1].instance == 0xe95a8d8d);   // the trench texture
    CHECK(info.pieceArrays[1] == nullptr && info.pieceArrays[2] == nullptr);   // pipes have no zipper pieces
}

struct PieceHost final : IUndergroundOccupantHost {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bindings;
    struct Binding final : IUndergroundTextureBinding {
        std::uint32_t id;
        explicit Binding(std::uint32_t i) : id(i) {}
        const std::uint32_t* TextureIds() override { return &id; }
        void Release() override { delete this; }
    };
    void InitNetworkOccupantStatics() override {}
    IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) override {
        bindings.emplace_back(group, instance);
        return instance ? new Binding(instance) : nullptr;
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x / 16.0f);
        cellZ = static_cast<int>(z / 16.0f);
    }
    bool IsCellInCity(int, int) override { return true; }
    void PrepareRenderer() override {}
    void ChangeModelInRenderer(cSC4UndergroundOccupant*, s3d::cIS3DModelInstance*, int) override {}
    s3d::cIS3DModelInstance* CreateCustomModel(std::uint32_t instance) override {
        customAsked = instance;
        return nullptr;
    }
    cSC4UndergroundOccupant* FindUndergroundOccupant(int, int, std::uint32_t) override { return nullptr; }
    bool FindUndergroundConnectionOccupant(int, int, std::uint32_t, IUndergroundConnectionOccupant*& out) override {
        out = nullptr;
        return false;
    }
    IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance*) override { return nullptr; }
    std::uint32_t customAsked = 0;
};

void TestCreateUndergroundModel() {
    PieceHost host;
    SetUndergroundOccupantHost(&host);
    const float corners[4][3] = {{32, 0, 32}, {32, 0, 48}, {48, 0, 48}, {48, 0, 32}};
    auto* pipe = new cSC4PipeOccupant();
    pipe->AddRef();
    pipe->Init(corners);
    // The straight piece: piece textures from records 2, 3, 4 (group 0x1abe787d), then the wall and trench again.
    pipe->SetQuadDraw(0x4b00, 0);
    host.bindings.clear();
    pipe->CreateUndergroundModel(0, false, false);
    CHECK(!pipe->PieceGeometry().empty());
    CHECK(pipe->PieceGeometry2().empty());
    CHECK(pipe->PieceTextureAt(0) && pipe->PieceTextureAt(0)->TextureIds()[0] == 0x8a1b1d4a);
    CHECK(pipe->PieceTextureAt(2) && pipe->PieceTextureAt(2)->TextureIds()[0] == 0xea38125b);
    CHECK(pipe->TrenchTexture() && pipe->TrenchTexture()->TextureIds()[0] == 0xe95a8d8d);
    CHECK(pipe->PieceTexture() && pipe->PieceTexture()->TextureIds()[0] == 0x095a8d70);
    bool zoomGroup = false;
    for (const auto& [group, instance] : host.bindings)
        if (group == kTextureBindingGroupZoom && instance == 0x8a1b1d4a) zoomGroup = true;
    CHECK(zoomGroup);
    CHECK(host.customAsked == 0);
    CHECK(pipe->ModelDirty());
    // The straight cap: cap top and cap end.
    pipe->SetQuadDraw(0x300, 0);
    pipe->CreateUndergroundModel(1, false, false);
    CHECK(!pipe->PieceGeometry().empty());
    CHECK(!pipe->PieceGeometry2().empty());
    // The diagonal piece: one half; the double diagonal (0x14e00): two halves.
    pipe->SetQuadDraw(0xa00, 0);
    pipe->CreateUndergroundModel(2, false, false);
    CHECK(!pipe->PieceGeometry().empty());
    CHECK(pipe->PieceGeometry2().empty());
    CHECK(pipe->PieceTextureAt(0) && pipe->PieceTextureAt(0)->TextureIds()[0] == 0xea1b27f1);
    pipe->SetQuadDraw(0x14e00, 0);
    pipe->CreateUndergroundModel(3, false, false);
    CHECK(!pipe->PieceGeometry().empty());
    CHECK(!pipe->PieceGeometry2().empty());
    CHECK(pipe->GetRotation() == 3);   // the second half's turn is undone
    // A piece the tables do not cover asks for its custom model (offset 0x6000000 | piece).
    pipe->SetQuadDraw(0x1234, 0);
    pipe->CreateUndergroundModel(0, false, false);
    CHECK(pipe->PieceGeometry().empty());
    CHECK(host.customAsked == (0x6000000u | 0x1234u));
    pipe->Release();
    SetUndergroundOccupantHost(nullptr);
}
}  // namespace

int main() {
    TestComplexFilter();
    TestUndergroundCustomView();
    TestPipeTables();
    TestCreateUndergroundModel();
    if (g_failures) {
        std::printf("FAILED %d\n", g_failures);
        return 1;
    }
    std::printf("sc4_undergroundview: all checks passed\n");
    return 0;
}
