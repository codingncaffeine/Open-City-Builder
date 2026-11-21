// InsertNetworkOccupant's support structures (AddSupportStructures and its parts): a piece with a support record that stands
// above the terrain gets side lips on its open edges and, up to the type's wall height, retaining walls down to the terrain;
// above it a pylon with the record's footprint instead. A piece level with the terrain, or without a record, gets none.
#include <cmath>
#include <cstdio>
#include <list>
#include <map>
#include <vector>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/NetworkWorldCache.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

using namespace sc4;

namespace sc4 {
struct NetworkToolTestAccess {
    static void Setup(cSC4NetworkTool& t, INetworkWorldCache* cache, const INetworkTypeInfo* info, int width) {
        t.worldCache_ = cache;
        t.cityWidth_ = t.cityHeight_ = width;
        t.widthPlusOne_ = t.heightPlusOne_ = width + 1;
        t.unknown98_ = true;
        t.typeInfo_ = info;
        t.cellSize_ = 16.0f;
        t.placement_.float40[1] = 45.0f;   // the road's wall height
    }
    static void Insert(cSC4NetworkTool& t, cSC4NetworkCellInfo& cell, const tSolvedCell& solved, std::list<cSC4NetworkOccupant*>* out) {
        t.InsertNetworkOccupant(cell, solved, out);
    }
};
}  // namespace sc4

namespace {

struct TypeInfo : INetworkTypeInfo {
    std::uint32_t ViewInputControlId() const override { return 0; }
    std::uint32_t ToolViewId() const override { return 0; }
    std::uint32_t CursorId() const override { return 0; }
    std::uint32_t DirectionalCursorBase() const override { return 0; }
    std::uint32_t DefaultExemplarId() const override { return 0x2821ed93; }
    int PenWidthX() const override { return 1; }
    int PenWidthZ() const override { return 1; }
    const std::vector<cSC4NetworkBridgeTypeInfo>& BridgeTypes() const override { return bridges; }
    const cSC4NetworkPlacementParams& PlacementParams() const override { return params; }
    const cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t id) const override {
        auto it = supports.find(id);
        return it == supports.end() ? nullptr : &it->second;
    }
    std::uint32_t PlacementExemplarId() const override { return 0; }
    std::vector<cSC4NetworkBridgeTypeInfo> bridges;
    cSC4NetworkPlacementParams params{};
    std::map<std::uint32_t, cSC4NetworkSupportStructInfo> supports;
};

struct Host : INetworkToolHost {
    void RegisterShutdownCallback(bool (*)(void*), void*) override {}
    cIGZUnknown* AcquireTerrain() override { return nullptr; }
    float TerrainCellSize() override { return 16.0f; }
    bool TerrainAllowsNetwork(std::uint32_t, std::uint32_t) override { return false; }
    int CityWidth() override { return 64; }
    int CityHeight() override { return 64; }
    cIGZUnknown* AcquireOccupantManager() override { return nullptr; }
    cIGZUnknown* AcquireCityViewModel() override { return nullptr; }
    void AddRenderLayer(INetworkPreview*, std::uint32_t, std::int32_t) override {}
    void RemoveRenderLayer(INetworkPreview*) override {}
    std::uint32_t RendererAltitudeScale() override { return 0; }
    void RegisterMessage(cIGZMessageTarget2*, std::uint32_t) override {}
    void UnregisterMessage(cIGZMessageTarget2*, std::uint32_t) override {}
    cIGZUnknown* CreateNetworkOrFilter(std::uint32_t) override { return nullptr; }
    INetworkPreview* CreatePreview() override { return nullptr; }
    INetworkWorldCache* CreateWorldCache() override { return nullptr; }
    INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return nullptr; }
    const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t type) override { return type == kNetworkRoad ? &road : nullptr; }
    cSC4NetworkCellInfo* GetCellInfo(std::uint32_t) override { return nullptr; }
    cIGZUnknown* GetTuningProperty(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return nullptr; }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    bool HasPrebuiltModelExemplar(std::uint32_t, std::uint32_t) override { return false; }
    float TerrainVertexHeight(std::int32_t vertex) override {
        auto it = terrain.find(vertex);
        return it == terrain.end() ? 0.0f : it->second;
    }
    TypeInfo road;
    std::map<std::int32_t, float> terrain;
};

constexpr std::uint32_t kPiece = 0x00004b00, kLip = 0xbb0200, kWall = 0xbb0000, kColumnSide = 0x8080000, kColumnTop = 0x912220e;

cSC4NetworkCellInfo* MakeCell(Host& host, cSC4NetworkWorldCache& cache, std::uint32_t x, std::uint32_t z, float height, float ground) {
    cSC4NetworkCellInfo* cell = cache.CreateCell(x + (z << 16));
    cell->x = x;
    cell->z = z;
    cell->networkFlags = 0x1;
    const std::int32_t v0 = static_cast<std::int32_t>(z * 65 + x);
    cell->vertexBaseHeights[0] = v0;
    cell->vertexBaseHeights[1] = v0 + 65;
    cell->vertexBaseHeights[2] = v0 + 66;
    cell->vertexBaseHeights[3] = v0 + 1;
    for (int i = 0; i != 4; ++i) {
        cell->vertexHeights[i] = height;
        host.terrain[cell->vertexBaseHeights[i]] = ground;
    }
    return cell;
}

cSC4NetworkOccupant* Insert(cSC4NetworkTool& tool, cSC4NetworkCellInfo& cell, std::uint32_t piece, std::uint8_t rotation) {
    std::list<cSC4NetworkOccupant*> out;
    tSolvedCell solved;
    solved.pieceType = piece;
    solved.rotationFlip = rotation;
    solved.cellKey = cell.x + (cell.z << 16);
    NetworkToolTestAccess::Insert(tool, cell, solved, &out);
    return out.empty() ? nullptr : out.front();
}

int Count(const cSC4NetworkOccupant* occupant, std::uint32_t id) {
    int n = 0;
    for (const SC4AddOnPart& part : occupant->AddOnParts()) n += part.id == id;
    return n;
}

// The lowest and highest corner of the parts with this id.
void Span(const cSC4NetworkOccupant* occupant, std::uint32_t id, float& low, float& high) {
    low = 1e30f;
    high = -1e30f;
    for (const SC4AddOnPart& part : occupant->AddOnParts())
        if (part.id == id)
            for (const auto& c : part.corner) {
                low = std::min(low, c.y);
                high = std::max(high, c.y);
            }
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

}  // namespace

int main() {
    Host host;
    // A straight road piece's record: its west edge (corners 0-1) and its east edge (corners 2-3); a 4 x 4 column footprint
    // for each rotation.
    cSC4NetworkSupportStructInfo record;
    record.edges = {0, 1, 2, 3};
    for (int turn = 0; turn != 4; ++turn) record.footprint.insert(record.footprint.end(), {-2, -2, -2, 2, 2, 2, 2, -2});
    host.road.supports[kPiece] = record;
    SetNetworkToolHost(&host);
    auto* tool = new cSC4NetworkTool(kNetworkRoad);
    tool->AddRef();
    cSC4NetworkWorldCache cache;
    NetworkToolTestAccess::Setup(*tool, &cache, &host.road, 64);

    // ---- level 1: 5 above the terrain, a north-south road with nothing on its sides, so both record edges are open: a lip
    //      and a wall (from 3 below the deck down to the terrain) on each, no pylon ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(host, cache, 10, 10, 20.0f, 15.0f);
        cell->tileNeighborhoods[kNetworkRoad] = 0x02000200;   // connected on sides 1 (-z) and 3 (+z)
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, kPiece, 0);
        CHECK(occupant != nullptr);
        if (occupant) {
            CHECK(occupant->HasNetworkFlag(0x40000000) && occupant->HasNetworkFlag(0x8000000));
            CHECK(Count(occupant, kLip) == 8);    // two strips, four triangles each (both faces)
            CHECK(Count(occupant, kWall) == 8);
            CHECK(Count(occupant, kColumnSide) == 0 && Count(occupant, kColumnTop) == 0);
            float low = 0, high = 0;
            Span(occupant, kLip, low, high);
            CHECK(Near(low, 17.0f) && Near(high, 21.5f));
            Span(occupant, kWall, low, high);
            CHECK(Near(low, 15.0f) && Near(high, 17.0f));
            // the west wall runs along x = 160 (corners 0-1), the east one along x = 176
            int west = 0, east = 0;
            for (const SC4AddOnPart& part : occupant->AddOnParts())
                if (part.id == kWall)
                    for (const auto& c : part.corner) {
                        west += Near(c.x, 160.0f);
                        east += Near(c.x, 176.0f);
                    }
            CHECK(west == 12 && east == 12);
            occupant->Release();
        }
    }

    // ---- an edge with a connection across it takes no lip and its wall starts 0.01 below the deck ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(host, cache, 12, 10, 20.0f, 15.0f);
        cell->tileNeighborhoods[kNetworkRoad] = 0x02000202;   // also connected on side 0 (-x, corners 0-1)
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, kPiece, 0);
        if (occupant) {
            CHECK(Count(occupant, kLip) == 4);
            CHECK(Count(occupant, kWall) == 8);
            float low = 0, high = 0;
            Span(occupant, kWall, low, high);
            CHECK(Near(high, 19.99f));
            occupant->Release();
        }
    }

    // ---- level 2: 50 above the terrain (over the wall height 45): lips and a pylon from 1.3 under the deck to the terrain,
    //      no walls ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(host, cache, 14, 10, 20.0f, -30.0f);
        cell->tileNeighborhoods[kNetworkRoad] = 0x02000200;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, kPiece, 0);
        if (occupant) {
            CHECK(Count(occupant, kWall) == 0);
            CHECK(Count(occupant, kLip) == 8);
            CHECK(Count(occupant, kColumnSide) == 8 && Count(occupant, kColumnTop) == 2);
            float low = 0, high = 0;
            Span(occupant, kColumnSide, low, high);
            CHECK(Near(low, -30.0f) && Near(high, 18.7f));
            // the column stands around the cell's middle (232, 168) at the footprint's +-2
            bool inside = true;
            for (const SC4AddOnPart& part : occupant->AddOnParts())
                if (part.id == kColumnSide)
                    for (const auto& c : part.corner) inside = inside && Near(std::fabs(c.x - 232.0f), 2.0f) && Near(std::fabs(c.z - 168.0f), 2.0f);
            CHECK(inside);
            occupant->Release();
        }
    }

    // ---- a mirrored piece walks its edges the other way: corners 0-1 of the record land on cell corners 3-0 (rotation 1) ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(host, cache, 16, 10, 20.0f, 15.0f);
        cell->tileNeighborhoods[kNetworkRoad] = 0x02000200;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, kPiece, 0x81);
        if (occupant) {
            // rotation 1 mirrored: record corners {1, 0} -> cell corners {3, 0} (the north side, z = 160) and {3, 2} -> {2, 1}
            // (the south side, z = 176)
            int north = 0, south = 0;
            for (const SC4AddOnPart& part : occupant->AddOnParts())
                if (part.id == kWall)
                    for (const auto& c : part.corner) {
                        north += Near(c.z, 160.0f);
                        south += Near(c.z, 176.0f);
                    }
            CHECK(north == 12 && south == 12);
            occupant->Release();
        }
    }

    // ---- a one-sided record (only corners 0-1): unmirrored its wall stands on the west side (x = 160), mirrored on the east
    //      side (corners 3-2, x = 176) ----
    {
        cSC4NetworkSupportStructInfo oneSided = record;
        oneSided.edges = {0, 1};
        host.road.supports[0x00004c00] = oneSided;
        for (std::uint8_t rotation : {std::uint8_t(0), std::uint8_t(0x80)}) {
            cSC4NetworkCellInfo* cell = MakeCell(host, cache, rotation ? 24 : 22, 10, 20.0f, 15.0f);
            cell->tileNeighborhoods[kNetworkRoad] = 0x02000200;
            cSC4NetworkOccupant* occupant = Insert(*tool, *cell, 0x00004c00, rotation);
            if (!occupant) continue;
            const float side = rotation ? 400.0f : 352.0f;
            int on = 0, walls = 0;
            for (const SC4AddOnPart& part : occupant->AddOnParts())
                if (part.id == kWall)
                    for (const auto& c : part.corner) {
                        ++walls;
                        on += Near(c.x, side);
                    }
            CHECK(walls == 12 && on == 12);
            occupant->Release();
        }
    }

    // ---- controls: a piece level with the terrain, and a raised piece without a record, get nothing ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(host, cache, 18, 10, 20.0f, 20.0f);
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, kPiece, 0);
        if (occupant) {
            CHECK(!occupant->HasNetworkFlag(0x40000000));
            CHECK(occupant->AddOnParts().empty());
            occupant->Release();
        }
        cell = MakeCell(host, cache, 20, 10, 20.0f, 0.0f);
        occupant = Insert(*tool, *cell, 0x00005700, 0);
        if (occupant) {
            CHECK(!occupant->HasNetworkFlag(0x40000000));
            CHECK(occupant->AddOnParts().empty());
            occupant->Release();
        }
    }

    tool->Release();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sc4_networksupports: all passed\n");
    return 0;
}
