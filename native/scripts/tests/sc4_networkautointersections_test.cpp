// The original's hash_map iteration order (nSC4Networks::cHashMapOrder: identity hash, new keys at the front of their bucket,
// the buckets regrown to the next SGI prime with each old bucket moved front first) and IdentifyHighwayIntersections, which
// takes the first auto-placed rule in that order on a dragged cell that already holds a network.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rz/cRZIniFile.h"
#include "rz/cRZString.h"
#include "sc4/NetworkIntersectionRules.h"
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
using namespace sc4::nSC4Networks;

namespace sc4 {
struct NetworkToolTestAccess {
    static void SetWorldCache(cSC4NetworkTool& t, INetworkWorldCache* cache) { t.worldCache_ = cache; }
    static void SetCity(cSC4NetworkTool& t, int width) {
        t.cityWidth_ = t.cityHeight_ = width;
        t.widthPlusOne_ = t.heightPlusOne_ = width + 1;
    }
    static void SetDrag(cSC4NetworkTool& t, const std::vector<SC4PointU>& cells) {
        t.cells_ = cells;
        t.steps_.clear();
        for (std::uint32_t i = 0; i < cells.size(); ++i) t.steps_.push_back(tDraggedStep{i, i, 0});
    }
    static int Identify(cSC4NetworkTool& t, int steps, std::uint32_t type) { return t.IdentifyHighwayIntersections(steps, type); }
    static std::vector<tIntersection>& Intersections(cSC4NetworkTool& t) { return t.intersections_; }
    static std::size_t Potentials(cSC4NetworkTool& t) { return t.unknown10_.size(); }
    static std::uint32_t PotentialType(cSC4NetworkTool& t, std::size_t i) { return t.unknown10_[i].networkFlags; }
};
}  // namespace sc4

namespace {

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
    const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t) override { return nullptr; }
    cSC4NetworkCellInfo* GetCellInfo(std::uint32_t) override { return nullptr; }
    cIGZUnknown* GetTuningProperty(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return nullptr; }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
};

// Two one-cell auto rules over a highway N-S cell, keys 0x03 and 0xc4 (both bucket 3 of 193), told apart by their step
// offsets, and one rule that is not auto-placed.
const char* kRules =
    "[HighwayIntersectionInfo_0x00000003]\n"
    "CellLayout = A<\n"
    "CellLayout = ^\n"
    "CheckType = A - highway: 0x02000200\n"
    "AutoPlace = yes\n"
    "StepOffsets = 1, 2\n"
    "[HighwayIntersectionInfo_0x000000c4]\n"
    "CellLayout = A<\n"
    "CellLayout = ^\n"
    "CheckType = A - highway: 0x02000200\n"
    "AutoPlace = yes\n"
    "StepOffsets = 3, 4\n"
    "[HighwayIntersectionInfo_0x00000001]\n"
    "CellLayout = A<\n"
    "CellLayout = ^\n"
    "CheckType = A - highway: 0x02000200\n"
    "StepOffsets = 5, 6\n";

cSC4NetworkCellInfo* MakeCell(cSC4NetworkWorldCache& cache, std::uint32_t x, std::uint32_t z) {
    cSC4NetworkCellInfo* cell = cache.CreateCell(x + (z << 16));
    cell->x = x;
    cell->z = z;
    const std::int32_t v0 = static_cast<std::int32_t>(z * 65 + x);
    cell->vertexBaseHeights[0] = v0;
    cell->vertexBaseHeights[1] = v0 + 65;
    cell->vertexBaseHeights[2] = v0 + 66;
    cell->vertexBaseHeights[3] = v0 + 1;
    return cell;
}

std::size_t IndexOf(const std::vector<std::uint32_t>& keys, std::uint32_t key) {
    return static_cast<std::size_t>(std::find(keys.begin(), keys.end(), key) - keys.begin());
}

}  // namespace

int main() {
    // ---- the hash_map order ----
    {
        cHashMapOrder order;
        CHECK(order.BucketCount() == 193);
        order.Insert(5);
        order.Insert(5 + 193 * 389);   // the same bucket at 193 and at 389 buckets
        order.Insert(2);
        order.Insert(5);               // already there: nothing moves
        std::vector<std::uint32_t> keys = order.Keys();
        CHECK(keys.size() == 3 && keys[0] == 2 && keys[1] == 5 + 193 * 389 && keys[2] == 5);
        // fill to 193 keys, then the 194th grows the table to 389 and reverses the shared bucket
        for (std::uint32_t k = 1000; order.Size() < 193; ++k) order.Insert(k);
        CHECK(order.BucketCount() == 193);
        keys = order.Keys();
        CHECK(IndexOf(keys, 5 + 193 * 389) < IndexOf(keys, 5));
        order.Insert(999999);
        CHECK(order.BucketCount() == 389 && order.Size() == 194);
        keys = order.Keys();
        CHECK(IndexOf(keys, 5) < IndexOf(keys, 5 + 193 * 389));
        // clear keeps the buckets
        order.Clear();
        CHECK(order.BucketCount() == 389 && order.Keys().empty());
    }

    // ---- IdentifyHighwayIntersections ----
    Host host;
    SetNetworkToolHost(&host);
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/sc4_networkautointersections_rules.ini";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs(kRules, f);
        std::fclose(f);
    }
    {
        cRZIniFile ini{cRZString(path.c_str())};
        cSC4NetworkTool::ReloadIntersectionRules(ini);
    }
    const IntersectionRuleMap& rules = cSC4NetworkTool::IntersectionRules();
    CHECK(rules.size() == 3);
    const std::vector<std::uint32_t> keys = rules.order.Keys();
    CHECK(keys.size() == 3);
    CHECK(IndexOf(keys, 1) < IndexOf(keys, 0xc4) && IndexOf(keys, 0xc4) < IndexOf(keys, 3));   // 0xc4 went in after 3

    auto* tool = new cSC4NetworkTool(kNetworkHighway);
    tool->AddRef();
    cSC4NetworkWorldCache cache;
    NetworkToolTestAccess::SetWorldCache(*tool, &cache);
    NetworkToolTestAccess::SetCity(*tool, 64);
    auto* existing = new cSC4NetworkOccupant();
    existing->AddRef();
    cSC4NetworkCellInfo* crossing = MakeCell(cache, 10, 10);
    crossing->networkFlags = 0x4;
    crossing->tileNeighborhoods[kNetworkHighway] = 0x02000200;
    crossing->occupant = existing;
    cSC4NetworkCellInfo* after = MakeCell(cache, 10, 11);
    after->networkFlags = 0x4;
    after->tileNeighborhoods[kNetworkHighway] = 0x02000200;

    // the drag's last step crosses: the run stays open to the end, kept with the type 4
    NetworkToolTestAccess::SetDrag(*tool, {SC4PointU{10, 9}, SC4PointU{10, 10}});
    CHECK(NetworkToolTestAccess::Identify(*tool, 2, kNetworkHighway) == 2);
    std::vector<tIntersection>& found = NetworkToolTestAccess::Intersections(*tool);
    CHECK(found.size() == 1);
    if (!found.empty()) {
        CHECK(found[0].ruleId == 0xc4);   // the first auto rule in the hash order, not the lowest id
        CHECK(found[0].x == 10 && found[0].z == 10 && found[0].step == 1 && found[0].stepA == 4 && found[0].stepB == 5);
    }
    CHECK(NetworkToolTestAccess::Potentials(*tool) == 1 && NetworkToolTestAccess::PotentialType(*tool, 0) == 4);
    CHECK(crossing->unknown5a == 1);   // MarkIntersection

    // a step after the crossing that does not cross closes the run with the drag's type
    crossing->unknown5a = 0;
    NetworkToolTestAccess::SetDrag(*tool, {SC4PointU{10, 10}, SC4PointU{10, 11}});
    NetworkToolTestAccess::Identify(*tool, 2, kNetworkHighway);
    CHECK(found.size() == 1 && found[0].step == 0);
    CHECK(NetworkToolTestAccess::Potentials(*tool) == 2 && NetworkToolTestAccess::PotentialType(*tool, 1) == kNetworkHighway);

    // a cell without a network occupant is never tested
    crossing->occupant = nullptr;
    NetworkToolTestAccess::SetDrag(*tool, {SC4PointU{10, 10}});
    NetworkToolTestAccess::Identify(*tool, 1, kNetworkHighway);
    CHECK(found.empty());

    existing->Release();
    tool->Release();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sc4_networkautointersections: all passed\n");
    return 0;
}
