// InsertNetworkOccupant's pre-built model pieces: a solved piece whose exemplar exists becomes a pre-built model occupant
// (no quad triangles) with the support model of the type's placement block at the offset and rotation the piece's edge
// pattern selects; a piece without one stays a plain quad occupant. Also the two multi-type adjacency checks and the bridge
// term of GetCostOfSolution.
#include <cmath>
#include <cstdio>
#include <list>
#include <map>
#include <vector>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
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
    static void SetWorldCache(cSC4NetworkTool& t, INetworkWorldCache* cache) { t.worldCache_ = cache; }
    static void SetCity(cSC4NetworkTool& t, int width) {
        t.cityWidth_ = t.cityHeight_ = width;
        t.widthPlusOne_ = t.heightPlusOne_ = width + 1;
    }
    static void SetPlacing(cSC4NetworkTool& t, bool placing) { t.unknown98_ = placing; }
    static void Insert(cSC4NetworkTool& t, cSC4NetworkCellInfo& cell, const tSolvedCell& solved, std::list<cSC4NetworkOccupant*>* out) {
        t.InsertNetworkOccupant(cell, solved, out);
    }
    static bool Vertically(cSC4NetworkTool& t, std::uint32_t x, std::uint32_t z, std::uint32_t flags) {
        return t.AdjacentVerticallyToMultitype(x, z, flags);
    }
    static bool Horizontally(cSC4NetworkTool& t, std::uint32_t x, std::uint32_t z, std::uint32_t flags) {
        return t.AdjacentHorizontallyToMultitype(x, z, flags);
    }
    static std::vector<tBridge>& Bridges(cSC4NetworkTool& t) { return t.bridges_; }
    static cSC4NetworkPlacementParams& Placement(cSC4NetworkTool& t) { return t.placement_; }
};
}  // namespace sc4

namespace {

struct TypeInfo : INetworkTypeInfo {
    std::uint32_t ViewInputControlId() const override { return 0; }
    std::uint32_t ToolViewId() const override { return 0; }
    std::uint32_t CursorId() const override { return 0; }
    std::uint32_t DirectionalCursorBase() const override { return 0; }
    std::uint32_t DefaultExemplarId() const override { return group; }
    int PenWidthX() const override { return 2; }
    int PenWidthZ() const override { return 2; }
    const std::vector<cSC4NetworkBridgeTypeInfo>& BridgeTypes() const override { return bridges; }
    const cSC4NetworkPlacementParams& PlacementParams() const override { return params; }
    const cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t) const override { return nullptr; }
    std::uint32_t PlacementExemplarId() const override { return 0; }
    std::uint32_t group = 0;
    std::vector<cSC4NetworkBridgeTypeInfo> bridges;
    cSC4NetworkPlacementParams params{};
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
    const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t type) override {
        auto it = types.find(type);
        return it == types.end() ? nullptr : &it->second;
    }
    cSC4NetworkCellInfo* GetCellInfo(std::uint32_t) override { return nullptr; }
    cIGZUnknown* GetTuningProperty(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return nullptr; }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    bool HasPrebuiltModelExemplar(std::uint32_t type, std::uint32_t piece) override {
        asked.push_back(type);
        return type == prebuiltType && piece == prebuiltPiece;
    }
    std::map<std::uint32_t, TypeInfo> types;
    std::uint32_t prebuiltType = 2, prebuiltPiece = 0x02001500;
    std::vector<std::uint32_t> asked;
};

cSC4NetworkCellInfo* MakeCell(cSC4NetworkWorldCache& cache, std::uint32_t x, std::uint32_t z, std::uint32_t flags, float height) {
    cSC4NetworkCellInfo* cell = cache.CreateCell(x + (z << 16));
    cell->x = x;
    cell->z = z;
    cell->networkFlags = flags;
    const std::int32_t v0 = static_cast<std::int32_t>(z * 65 + x);
    cell->vertexBaseHeights[0] = v0;
    cell->vertexBaseHeights[1] = v0 + 65;
    cell->vertexBaseHeights[2] = v0 + 66;
    cell->vertexBaseHeights[3] = v0 + 1;
    for (float& h : cell->vertexHeights) h = height;
    return cell;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Inserts the solved piece on the cell and returns the occupant (one reference: the caller releases it).
cSC4NetworkOccupant* Insert(cSC4NetworkTool& tool, cSC4NetworkCellInfo& cell, std::uint32_t piece, std::uint8_t rotation) {
    std::list<cSC4NetworkOccupant*> out;
    tSolvedCell solved;
    solved.pieceType = piece;
    solved.rotationFlip = rotation;
    solved.cellKey = cell.x + (cell.z << 16);
    NetworkToolTestAccess::Insert(tool, cell, solved, &out);
    return out.empty() ? nullptr : out.front();
}

}  // namespace

int main() {
    Host host;
    host.types[kNetworkHighway].group = 0xa8434037;
    host.types[kNetworkHighway].params.listD0 = {0x0aaa0001, 0x0aaa0002};
    host.types[kNetworkRoad].group = 0x2821ed93;
    SetNetworkToolHost(&host);
    auto* tool = new cSC4NetworkTool(kNetworkHighway);
    tool->AddRef();
    cSC4NetworkWorldCache cache;
    NetworkToolTestAccess::SetWorldCache(*tool, &cache);
    NetworkToolTestAccess::SetCity(*tool, 64);
    NetworkToolTestAccess::SetPlacing(*tool, true);

    // ---- a straight two-wide highway piece (edges W and E kind 2 with the group edge on S): support model under the cell's
    //      middle, 8 along x, no rotation; the pre-built occupant draws no quad triangles ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(cache, 10, 11, 0x4, 20.0f);
        cell->tileNeighborhoods[kNetworkHighway] = 0x00040200;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, 0x02001500, 0);
        auto* prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant);
        CHECK(prebuilt != nullptr);
        if (prebuilt) {
            CHECK(prebuilt->SupportModelId() == 0x0aaa0001u);   // the style index (0) picks the first
            CHECK(Near(prebuilt->SupportRotation(), 0.0f));
            CHECK(Near(prebuilt->SupportOffset().x, 8.0f) && Near(prebuilt->SupportOffset().z, 0.0f));
            float position[3] = {0, 0, 0};
            prebuilt->GetPosition(position);
            CHECK(Near(prebuilt->SupportOffset().y, 20.0f - position[1]));
            CHECK(!occupant->HasNetworkFlag(0x2000000) && !occupant->HasNetworkFlag(0x4000000));
            CHECK(occupant->HasNetworkFlag(0x8000000));
            CHECK(occupant->HasNetworkFlag(0x1000000));   // two neighboring edges (E and the group edge S): mask 6
        }
        if (occupant) occupant->Release();
    }

    // ---- a diagonal two-wide piece: split diagonal flag, a quarter-turn-by-half support at the cell's middle ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(cache, 20, 20, 0x4, 10.0f);
        cell->tileNeighborhoods[kNetworkHighway] = 0x01030103;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, 0x02001500, 0);
        auto* prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant);
        CHECK(prebuilt != nullptr);
        if (prebuilt) {
            CHECK(occupant->HasNetworkFlag(0x1000000));
            CHECK(Near(prebuilt->SupportRotation(), 0.785398f));
            CHECK(Near(prebuilt->SupportOffset().x, 0.0f) && Near(prebuilt->SupportOffset().z, 0.0f));
        }
        if (occupant) occupant->Release();
    }

    // ---- the T pattern 0x2040200 on an even row takes a support only beside another network type ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(cache, 30, 30, 0x4, 5.0f);
        cell->tileNeighborhoods[kNetworkHighway] = 0x02040200;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, 0x02001500, 0);
        auto* prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant);
        CHECK(prebuilt && prebuilt->SupportModelId() == 0);
        if (occupant) occupant->Release();
        CHECK(!NetworkToolTestAccess::Vertically(*tool, 30, 30, 0x4));
        MakeCell(cache, 30, 31, 0x1, 5.0f);   // a road below
        CHECK(NetworkToolTestAccess::Vertically(*tool, 30, 30, 0x4));
        CHECK(!NetworkToolTestAccess::Vertically(*tool, 30, 30, 0x5));   // the road is one of the cell's own types
        CHECK(!NetworkToolTestAccess::Horizontally(*tool, 30, 30, 0x4));
        MakeCell(cache, 29, 30, 0x1, 5.0f);   // a road left of it
        CHECK(NetworkToolTestAccess::Horizontally(*tool, 30, 30, 0x4));
        occupant = Insert(*tool, *cell, 0x02001500, 0);
        prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant);
        CHECK(prebuilt && prebuilt->SupportModelId() == 0x0aaa0001u && Near(prebuilt->SupportOffset().x, 8.0f));
        if (occupant) occupant->Release();
    }

    // ---- a piece without a pre-built exemplar (control): a plain quad occupant with its triangles ----
    {
        cSC4NetworkCellInfo* cell = MakeCell(cache, 40, 40, 0x4, 0.0f);
        cell->tileNeighborhoods[kNetworkHighway] = 0x00040200;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, 0x02001600, 0);
        CHECK(occupant != nullptr && dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant) == nullptr);
        if (occupant) {
            CHECK(occupant->HasNetworkFlag(0x2000000) && occupant->HasNetworkFlag(0x4000000));
            occupant->Release();
        }
    }

    // ---- a road piece of a type with no support models: pre-built, no support model ----
    {
        host.prebuiltType = kNetworkRoad;
        host.prebuiltPiece = 0x00004b00;
        cSC4NetworkCellInfo* cell = MakeCell(cache, 50, 50, 0x1, 0.0f);
        cell->tileNeighborhoods[kNetworkRoad] = 0x00020002;
        cSC4NetworkOccupant* occupant = Insert(*tool, *cell, 0x00004b00, 0);
        auto* prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant);
        CHECK(prebuilt && prebuilt->SupportModelId() == 0);
        if (occupant) occupant->Release();
    }

    // ---- GetCostOfSolution: a bridge costs its type's fixed cost plus its steps times (its cost per step + the network's bridge
    //      cost per cell) ----
    {
        cSC4NetworkBridgeTypeInfo type;
        type.unknown150[0] = 1000;
        type.unknown150[1] = 50;
        host.types[kNetworkHighway].bridges = {cSC4NetworkBridgeTypeInfo{}, type};
        NetworkToolTestAccess::Placement(*tool).unknownA8[6] = 10;
        NetworkToolTestAccess::Placement(*tool).floatA8[8] = 0.0f;
        CHECK(tool->GetCostOfSolution() == 0);
        tBridge bridge;
        bridge.networkType = kNetworkHighway;
        bridge.startStep = 3;
        bridge.endStep = 7;
        bridge.bridgeType = 1;
        NetworkToolTestAccess::Bridges(*tool).push_back(bridge);
        CHECK(tool->GetCostOfSolution() == 1000 + 4 * (50 + 10));
        NetworkToolTestAccess::Bridges(*tool).clear();
    }

    tool->Release();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sc4_networkprebuiltpieces: all passed\n");
    return 0;
}
