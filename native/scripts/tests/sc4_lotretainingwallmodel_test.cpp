// cSC4LotRetainingWallModel: the wall segment mesh of a locked lot's edge. Everything here is checked against the
// x86_64 disassembly of 0x1001671ac..0x1001688ca (otool -arch x86_64 -tV), not against the Ghidra decompiler.
//
// Covered: layout / defaults from the ctor 0x1001671ac, the two virtual views (QueryInterface 0x1001673d4 answering
// iid 0xa9e2831e and delegating to cS3DModelInstanceBasic), GetWallModel 0x100168850, GetWorldBounds 0x100167452,
// Pick 0x1001674ac, GetSortValue 0x1001674c4 (with and without a texture binding), the metres->cells conversion and
// the four edge spans of SetupVerticesFromBBoxAndWallEdge 0x10016872a, SegmentIsAdjacentToFlatLot 0x100168638 (the
// per-edge one-cell look-back and the locked-flag answer), Init 0x1001686d4 (always true, `changed` from the bounds),
// the four segment shapes of SetupVertices 0x1001677ea (both ends below the lot = nothing, near-below/far-above and
// near-above/far-below = one triangle each, both above = a six-vertex quad), the bounding box rules (the along axis
// shrunk by 0.001, the across axis 0.001..0.002 thick, the six-comparison 0.0001 rebuild test), the empty path's unit
// box, SetupTextures 0x1001676f8 (per-zoom id, the 0x695c00c6 retry), DoZoomChange 0x10016762a (one-shot packed
// render state), SetupPackedRenderState 0x100167692 (its arguments), SetTextureData 0x100168808, the lighting pass
// (the negated edge normal, rgb from cISC4LightingManager vslot 0xc0 and alpha 0xff) and Draw 0x100167510 (the zoom /
// rotation tracking, the two-facing-edges cull, the highlight early-out and the DrawPrims(0, 10, count) submission).
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "sc4/LotRetainingWallModel.h"
#include "sc4/DrawContext.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
#include "sc4/Lot.h"
#pragma clang diagnostic pop

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f * (1.0f + std::fabs(b)); }

// ---- fakes ---------------------------------------------------------------------------------------------------

struct FakeLotManager final : sc4::ILotRetainingWallLotManager {
    sc4::cSC4Lot* lot = nullptr;
    int wantedX = 0, wantedZ = 0;
    bool anyCell = true;
    int calls = 0;
    sc4::cSC4Lot* LotAtCell(int x, int z) override {
        ++calls;
        if (!anyCell && (x != wantedX || z != wantedZ)) return nullptr;
        return lot;
    }
};

struct FakeTerrain final : sc4::ILotRetainingWallTerrain {
    int countX = 8, countZ = 8;
    // a small height table on the x axis at z == 0; everything else is level
    std::vector<float> heightsX;
    int CellCountX() const override { return countX; }
    int CellCountZ() const override { return countZ; }
    float AltitudeAtVertex(int x, int z) const override {
        if (z == 0 && x >= 0 && x < static_cast<int>(heightsX.size())) return heightsX[static_cast<std::size_t>(x)];
        return 0.0f;
    }
};

struct FakeLighting final : sc4::ILotRetainingWallLighting {
    int calls = 0;
    std::uint8_t rgb[3] = {10, 20, 30};
    cS3DVector3 lastNormal{};
    cS3DVector3 lastPosition{};
    void GetColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r, std::uint8_t& g,
                  std::uint8_t& b) override {
        ++calls;
        lastPosition = position;
        lastNormal = normal;
        r = rgb[0];
        g = rgb[1];
        b = rgb[2];
    }
};

struct FakeBinding final : sc4::ILotRetainingWallTextureBinding {
    std::uint32_t id = 0;
    int releases = 0;
    std::uint32_t TextureId() const override { return id; }
    void Release() override { ++releases; }
};

struct FakeTextures final : sc4::ILotRetainingWallTextures {
    std::vector<sc4::SC4TextureBindingKey> keys;
    std::vector<std::uint32_t> produced;   // the binding ids to answer, in call order (0 = fail)
    std::vector<FakeBinding*> bindings;
    std::size_t next = 0;
    std::uint32_t failFirstId = 0;   // when non-zero, the first key with this instance fails
    bool failed = false;
    ~FakeTextures() { for (FakeBinding* b : bindings) delete b; }   // Release only counts

    sc4::ILotRetainingWallTextureBinding* CreateTexture(const sc4::SC4TextureBindingKey& key) override {
        keys.push_back(key);
        if (failFirstId != 0 && key.instance == failFirstId && !failed) {
            failed = true;
            return nullptr;
        }
        auto* binding = new FakeBinding();
        binding->id = key.instance;
        bindings.push_back(binding);
        produced.push_back(key.instance);
        return binding;
    }
};

struct FakeRenderer {
    int drawContextQueries = 0;
};

struct FakeHost final : sc4::ILotRetainingWallModelHost {
    FakeLotManager lots;
    FakeTerrain terrain;
    FakeLighting lighting;
    FakeTextures textures;
    FakeRenderer renderer;
    sc4::ILotRetainingWallLotManager* LotManager() override { return &lots; }
    sc4::ILotRetainingWallTerrain* Terrain() override { return &terrain; }
    sc4::ILotRetainingWallLighting* Lighting() override { return &lighting; }
    sc4::ILotRetainingWallTextures* Textures() override { return &textures; }
    void TouchRendererDrawContext() override { ++renderer.drawContextQueries; }
    void GetVectorForEdge(float* v, int edge) override {
        // SC4ViewUtilities::GetVectorForEdge (0x10017723c), including its negative-index quirk
        static const float kX[4] = {0.0f, 1.0f, 0.0f, -1.0f};
        static const float kZ[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
        int index = edge;
        if (index < 0) index -= 3;   // the "round toward zero then subtract" wrap of the original
        index %= 4;
        if (index < 0) index += 4;
        v[1] = 0.0f;
        if (static_cast<unsigned>(index) < 4u) {
            v[0] = kX[index];
            v[2] = kZ[index];
        }
    }
};

// A recording cIGZGDriver: DrawPrims reaches it through SC4DrawContext.
struct Driver final : gd::cGDriverBase {
    int draws = 0;
    int lastPrimitive = -1;
    int lastCount = 0;
    int lastFormat = -1;
    const void* lastVertices = nullptr;
    std::uint32_t lastTexture = 0;
    int textures = 0;
    void DrawArrays(gd::eGDPrimType primitive, int first, int count) override {
        (void)first;
        ++draws;
        lastPrimitive = static_cast<int>(primitive);
        lastCount = count;
    }
    void InterleavedArrays(gd::eGDVertexFormat format, int stride, const void* data) override {
        (void)stride;
        lastFormat = static_cast<int>(format);
        lastVertices = data;
    }
    void SetTexture(std::uint32_t texture, std::uint32_t) override {
        ++textures;
        lastTexture = texture;
    }
};

// One 2x2-cell lot at (0,0) with altitude 5 and the 32-metre box the occupant would pass.
struct Scene {
    FakeHost host;
    sc4::cSC4Lot lot{0, 0, 2, 2, 0};
    Scene() {
        lot.SetAltitude(5.0f);
        host.lots.lot = &lot;
        sc4::SetLotRetainingWallModelHost(&host);
    }
    ~Scene() { sc4::SetLotRetainingWallModelHost(nullptr); }
    static cS3DBoundingBox Box() {
        cS3DBoundingBox box;
        box.min[0] = 0.0f;
        box.min[1] = 5.0f;
        box.min[2] = 0.0f;
        box.max[0] = 32.0f;
        box.max[1] = 5.0f;
        box.max[2] = 32.0f;
        return box;
    }
};

}  // namespace

int main() {
    // ---- the ctor's defaults -----------------------------------------------------------------------------------
    {
        Scene scene;
        sc4::cSC4LotRetainingWallModel model;
        cS3DBoundingBox box;
        model.GetWorldBounds(box);
        CHECK(Near(box.min[0], 1.0f) && Near(box.min[1], 1.0f) && Near(box.min[2], 1.0f));
        CHECK(Near(box.max[0], 0.0f) && Near(box.max[1], 0.0f) && Near(box.max[2], 0.0f));
        CHECK(model.WallEdge() == -1);
        CHECK(model.SegmentsBuilt() == 0);
        CHECK(model.ZoomLevel() == 0xff && model.ViewRotation() == 0xff);
        CHECK(model.TextureId() == 0);
        CHECK(model.Vertices().empty());
        CHECK(!model.Pick(nullptr));
        std::uint32_t sort = 0;
        model.GetSortValue(sort);
        CHECK(sort == sc4::kRetainingWallSortValueNoTexture);
        CHECK(model.GetWallModel() == static_cast<sc4::cISC4LotRetainingWallModel*>(&model));
    }

    // ---- QueryInterface: the wall interface, the base's model-instance iid, and a miss -------------------------
    {
        Scene scene;
        sc4::cSC4LotRetainingWallModel model;
        void* out = nullptr;
        CHECK(model.QueryInterface(sc4::kGZIID_cISC4LotRetainingWallModel, &out));
        CHECK(out == static_cast<sc4::cISC4LotRetainingWallModel*>(&model));
        out = nullptr;
        CHECK(model.QueryInterface(s3d::kGZIID_cIS3DModelInstance, &out));
        CHECK(out != nullptr);
        CHECK(!model.QueryInterface(0x12345678u, &out));
    }

    // ---- SetupVerticesFromBBoxAndWallEdge: the four edges of a 2x2 lot ---------------------------------------
    {
        const cS3DBoundingBox box = Scene::Box();
        struct Expected { int startX, startZ, endX, endZ; };
        const Expected expected[4] = {
            {0, 0, 2, 0},   // edge 0: along x at z0
            {2, 0, 2, 2},   // edge 1: along z at x1+1
            {0, 2, 2, 2},   // edge 2: along x at z1+1
            {0, 0, 0, 2},   // edge 3: along z at x0
        };
        for (int edge = 0; edge < 4; ++edge) {
            sc4::SC4PointInt start, end;
            sc4::cSC4LotRetainingWallModel::SetupVerticesFromBBoxAndWallEdge(box, edge, start, end);
            CHECK(start.x == expected[edge].startX && start.z == expected[edge].startZ);
            CHECK(end.x == expected[edge].endX && end.z == expected[edge].endZ);
        }
    }

    // ---- SegmentIsAdjacentToFlatLot --------------------------------------------------------------------------
    {
        Scene scene;
        sc4::cSC4LotRetainingWallModel model;
        bool changed = false;
        model.Init(Scene::Box(), 0, changed);   // edge 0 (flat lot: no segments, but the edge is set)
        // edge 0 looks one cell back in z, so (x, 0) is tested at (x, -1): out of bounds
        CHECK(!model.SegmentIsAdjacentToFlatLot(0, 0, &scene.host.lots, &scene.host.terrain));
        // edge 3 looks one cell back in x; (1, 0) tests (0, 0), which is the lot
        bool changed3 = false;
        model.Init(Scene::Box(), 3, changed3);
        scene.lot.SetFlags(0);
        CHECK(!model.SegmentIsAdjacentToFlatLot(1, 0, &scene.host.lots, &scene.host.terrain));
        scene.lot.SetFlags(1);   // locked
        CHECK(model.SegmentIsAdjacentToFlatLot(1, 0, &scene.host.lots, &scene.host.terrain));
        // outside the terrain: edge 3 looks at (x-1, z), so x must stay below countX after the step back
        scene.host.terrain.countX = 8;
        CHECK(!model.SegmentIsAdjacentToFlatLot(9, 0, &scene.host.lots, &scene.host.terrain));
        scene.host.terrain.countZ = 4;
        CHECK(!model.SegmentIsAdjacentToFlatLot(1, 4, &scene.host.lots, &scene.host.terrain));
        // no lot at the cell
        scene.host.lots.anyCell = false;
        scene.host.lots.wantedX = 42;
        CHECK(!model.SegmentIsAdjacentToFlatLot(1, 0, &scene.host.lots, &scene.host.terrain));
    }

    // ---- Init: every call answers true; the flat lot builds nothing and reports a unit box -------------------
    {
        Scene scene;
        scene.host.terrain.heightsX = {4.0f, 4.5f, 4.0f};
        sc4::cSC4LotRetainingWallModel model;
        bool changed = true;
        CHECK(model.Init(Scene::Box(), 0, changed));
        CHECK(model.WallEdge() == 0);
        CHECK(model.SegmentsBuilt() == 0);
        CHECK(!changed);
        CHECK(model.Vertices().empty());
        cS3DBoundingBox box;
        model.GetWorldBounds(box);
        CHECK(Near(box.min[0], 0.0f) && Near(box.min[1], 4.0f) && Near(box.min[2], 0.0f));
        CHECK(Near(box.max[0], 1.0f) && Near(box.max[1], 5.0f) && Near(box.max[2], 1.0f));
        CHECK(scene.host.lighting.calls == 0);   // nothing to light
    }

    // ---- the near-below / far-above triangle (edge 0) ---------------------------------------------------------
    {
        Scene scene;
        scene.host.terrain.heightsX = {4.0f, 4.5f, 6.0f};
        sc4::cSC4LotRetainingWallModel model;
        bool changed = false;
        CHECK(model.Init(Scene::Box(), 0, changed));
        CHECK(changed);
        CHECK(model.SegmentsBuilt() == 1);
        const auto& v = model.Vertices();
        CHECK(v.size() == 3);
        if (v.size() == 3) {
            // the first cell is skipped, the second one builds (worldX 16, lot altitude 5, terrain 4.5 -> 6.0)
            CHECK(Near(v[0].x, 16.0f) && Near(v[0].y, 4.3f) && Near(v[0].z, 0.0f));
            CHECK(Near(v[1].x, 32.0f) && Near(v[1].y, 5.002f) && Near(v[1].z, 0.0f));
            CHECK(Near(v[2].x, 32.0f) && Near(v[2].y, 6.2f) && Near(v[2].z, 0.0f));
            CHECK(Near(v[0].u, 1.0f) && Near(v[0].v, 4.3f / 16.0f));
            CHECK(Near(v[2].u, 2.0f) && Near(v[2].v, 6.2f / 16.0f));
            CHECK(v[0].color[3] == 0xff && v[1].color[3] == 0xff && v[2].color[3] == 0xff);
            CHECK(v[0].color[0] == 10 && v[0].color[1] == 20 && v[0].color[2] == 30);
        }
        cS3DBoundingBox box;
        model.GetWorldBounds(box);
        // along x: 16..32 shrunk by 0.001; across z: 0.001..0.002; y: 4.5..6
        CHECK(Near(box.min[0], 16.001f) && Near(box.min[1], 4.5f) && Near(box.min[2], 0.001f));
        CHECK(Near(box.max[0], 31.999f) && Near(box.max[1], 6.0f) && Near(box.max[2], 0.002f));
        // lighting: the negated edge-0 vector and the first vertex position
        CHECK(scene.host.lighting.calls == 3);
        CHECK(Near(scene.host.lighting.lastNormal.x, 0.0f) && Near(scene.host.lighting.lastNormal.y, 0.0f) &&
              Near(scene.host.lighting.lastNormal.z, 1.0f));
    }

    // ---- the near-above / far-below triangle (edge 0) ---------------------------------------------------------
    {
        Scene scene;
        scene.host.terrain.heightsX = {6.0f, 4.0f, 4.0f};
        sc4::cSC4LotRetainingWallModel model;
        bool changed = false;
        CHECK(model.Init(Scene::Box(), 0, changed));
        CHECK(changed);
        CHECK(model.SegmentsBuilt() == 1);
        const auto& v = model.Vertices();
        CHECK(v.size() == 3);
        if (v.size() == 3) {
            CHECK(Near(v[0].x, 0.0f) && Near(v[0].y, 5.002f) && Near(v[0].z, 0.0f));
            CHECK(Near(v[1].x, 16.0f) && Near(v[1].y, 3.8f) && Near(v[1].z, 0.0f));
            CHECK(Near(v[2].x, 0.0f) && Near(v[2].y, 6.2f) && Near(v[2].z, 0.0f));
        }
        cS3DBoundingBox box;
        model.GetWorldBounds(box);
        CHECK(Near(box.min[0], 0.001f) && Near(box.min[1], 4.0f) && Near(box.min[2], 0.001f));
        CHECK(Near(box.max[0], 15.999f) && Near(box.max[1], 6.0f) && Near(box.max[2], 0.002f));
    }

    // ---- both ends above the lot: the six-vertex quad --------------------------------------------------------
    {
        Scene scene;
        scene.host.terrain.heightsX = {6.0f, 7.0f, 7.0f};
        sc4::cSC4LotRetainingWallModel model;
        bool changed = false;
        CHECK(model.Init(Scene::Box(), 0, changed));
        CHECK(changed);
        CHECK(model.SegmentsBuilt() == 2);
        const auto& v = model.Vertices();
        CHECK(v.size() == 12);
        if (v.size() == 12) {
            CHECK(Near(v[0].x, 0.0f) && Near(v[0].y, 5.002f) && Near(v[0].z, 0.0f));
            CHECK(Near(v[1].x, 16.0f) && Near(v[1].y, 5.002f) && Near(v[1].z, 0.0f));
            CHECK(Near(v[2].x, 0.0f) && Near(v[2].y, 6.2f) && Near(v[2].z, 0.0f));
            CHECK(Near(v[3].x, 0.0f) && Near(v[3].y, 6.2f) && Near(v[3].z, 0.0f));
            CHECK(Near(v[4].x, 16.0f) && Near(v[4].y, 5.002f) && Near(v[4].z, 0.0f));
            CHECK(Near(v[5].x, 16.0f) && Near(v[5].y, 7.2f) && Near(v[5].z, 0.0f));
            // second cell: x 16 -> 32, heights 7.0 / 7.0
            CHECK(Near(v[6].x, 16.0f) && Near(v[6].y, 5.002f));
            CHECK(Near(v[11].x, 32.0f) && Near(v[11].y, 7.2f));
        }
    }

    // ---- a z-aligned edge (edge 1): the fixed coordinate is x = (x1+1)*16 --------------------------------
    {
        struct ZTerrain final : sc4::ILotRetainingWallTerrain {
            int CellCountX() const override { return 8; }
            int CellCountZ() const override { return 8; }
            float AltitudeAtVertex(int x, int z) const override {
                if (x == 2) return z == 0 ? 6.0f : 7.0f;   // the wall's fixed cell (x1+1)
                return 4.0f;
            }
        };
        struct ZHost final : sc4::ILotRetainingWallModelHost {
            ZTerrain terrain;
            FakeTerrain unusedTerrain;
            FakeLotManager lots;
            FakeLighting lighting;
            FakeTextures textures;
            sc4::ILotRetainingWallLotManager* LotManager() override { return &lots; }
            sc4::ILotRetainingWallTerrain* Terrain() override { return &terrain; }
            sc4::ILotRetainingWallLighting* Lighting() override { return &lighting; }
            sc4::ILotRetainingWallTextures* Textures() override { return &textures; }
            void TouchRendererDrawContext() override {}
            void GetVectorForEdge(float* v, int edge) override {
                static const float kX[4] = {0.0f, 1.0f, 0.0f, -1.0f};
                static const float kZ[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
                v[1] = 0.0f;
                if (edge >= 0 && edge < 4) {
                    v[0] = kX[edge];
                    v[2] = kZ[edge];
                }
            }
        };
        sc4::cSC4Lot lot{0, 0, 2, 2, 0};
        lot.SetAltitude(5.0f);
        ZHost zHost;
        zHost.lots.lot = &lot;
        sc4::SetLotRetainingWallModelHost(&zHost);

        sc4::cSC4LotRetainingWallModel model;
        bool changed = false;
        CHECK(model.Init(Scene::Box(), 1, changed));
        CHECK(model.SegmentsBuilt() == 2);
        const auto& v = model.Vertices();
        CHECK(v.size() == 12);
        if (v.size() == 12) {
            CHECK(Near(v[0].x, 32.0f) && Near(v[0].y, 5.002f) && Near(v[0].z, 0.0f));
            CHECK(Near(v[1].x, 32.0f) && Near(v[1].y, 5.002f) && Near(v[1].z, 16.0f));
            CHECK(Near(v[2].x, 32.0f) && Near(v[2].y, 6.2f) && Near(v[2].z, 0.0f));
            // u comes from z for a z-aligned wall
            CHECK(Near(v[0].u, 0.0f) && Near(v[1].u, 1.0f));
        }
        cS3DBoundingBox box;
        model.GetWorldBounds(box);
        CHECK(Near(box.min[0], 32.001f) && Near(box.min[1], 5.0f) && Near(box.min[2], 0.001f));
        CHECK(Near(box.max[0], 32.002f) && Near(box.max[1], 7.0f) && Near(box.max[2], 31.999f));
        sc4::SetLotRetainingWallModelHost(nullptr);
    }

    // ---- the bounds-difference test: a second build with the same terrain reports changed = false ------------
    {
        Scene scene;
        scene.host.terrain.heightsX = {6.0f, 7.0f, 7.0f};
        sc4::cSC4LotRetainingWallModel model;
        bool changed = true;
        model.Init(Scene::Box(), 0, changed);
        CHECK(changed);
        bool again = true;
        model.Init(Scene::Box(), 0, again);
        CHECK(!again);   // same box: the six 0.0001 comparisons match
    }

    // ---- no lot at the wall cell: nothing is built ----------------------------------------------------------
    {
        Scene scene;
        scene.host.lots.anyCell = false;
        scene.host.lots.wantedX = 99;
        sc4::cSC4LotRetainingWallModel model;
        // Quirk kept: the no-lot early-out leaves the caller's `changed` untouched (it is only written by the
        // build path's 0x1001684d7 store).
        bool changed = false;
        CHECK(model.Init(Scene::Box(), 0, changed));
        CHECK(!changed);
        CHECK(model.Vertices().empty());
        CHECK(scene.host.lighting.calls == 0);
    }

    // ---- SetTextureData + SetupTextures: per-zoom id, the retry step, the release of the old binding ----------
    {
        Scene scene;
        sc4::cSC4LotRetainingWallModel model;
        const std::uint32_t ids[5] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u};
        model.SetTextureData(ids, 4.0f);
        CHECK(sc4::LotRetainingWallStaticInitialized() == false);
        model.DoZoomChange(0xff, 2);
        CHECK(model.ZoomLevel() == 2);
        CHECK(sc4::LotRetainingWallStaticInitialized());
        CHECK(scene.host.renderer.drawContextQueries == 1);   // the discarded GetDrawContext call
        CHECK(scene.host.textures.keys.size() == 1);
        if (!scene.host.textures.keys.empty()) {
            const auto& key = scene.host.textures.keys[0];
            CHECK(key.type == sc4::kRetainingWallTextureType);
            CHECK(key.group == sc4::kRetainingWallTextureGroup);
            CHECK(key.instance == 0x33333333u);
        }
        CHECK(model.TextureId() == 0x33333333u);
        std::uint32_t sort = 0;
        model.GetSortValue(sort);
        CHECK(sort == (0x33333333u | sc4::kRetainingWallSortValueMask));
        // the same zoom level does nothing
        model.DoZoomChange(2, 2);
        CHECK(scene.host.textures.keys.size() == 1);
        // a different zoom binds its own id and releases the previous binding
        FakeBinding* previous = scene.host.textures.bindings.empty() ? nullptr : scene.host.textures.bindings[0];
        model.DoZoomChange(2, 0);
        CHECK(scene.host.textures.keys.size() == 2);
        CHECK(previous && previous->releases == 1);
        CHECK(model.TextureId() == 0x11111111u);
        // no texture at all for that zoom: no binding
        const std::uint32_t zero[5] = {0, 0, 0, 0, 0};
        model.SetTextureData(zero, 1.0f);
        model.DoZoomChange(0, 4);
        CHECK(model.TextureId() == 0);
        CHECK(scene.host.textures.keys.size() == 2);
        // the retry: the id that fails to bind is bumped by 0x695c00c6 and tried again
        const std::uint32_t one[5] = {0x70000000u, 0, 0, 0, 0};
        model.SetTextureData(one, 1.0f);
        scene.host.textures.failFirstId = 0x70000000u;
        model.DoZoomChange(4, 0);
        CHECK(scene.host.textures.keys.size() == 4);
        if (scene.host.textures.keys.size() == 4) {
            CHECK(scene.host.textures.keys[3].instance == 0x70000000u + sc4::kRetainingWallTextureRetryStep);
        }
        CHECK(model.TextureId() == 0x70000000u + sc4::kRetainingWallTextureRetryStep);
    }

    // ---- the packed render state's arguments ----------------------------------------------------------------
    {
        cSC4PackedRenderState expected;
        expected.PackRenderState(true, true, true, false, false, 1, false, 3, -1, -1, -1, -1.0f, -1, -1);
        const cSC4PackedRenderState& actual = sc4::LotRetainingWallPackedRenderState();
        CHECK(actual.value == expected.value);
        CHECK(actual.mask == expected.mask);
    }

    // ---- Draw: zoom/rotation tracking, the edge cull, the highlight early-out, the submission ----------------
    {
        Scene scene;
        scene.host.terrain.heightsX = {6.0f, 7.0f, 7.0f};
        sc4::cSC4LotRetainingWallModel model;
        bool changed = false;
        model.Init(Scene::Box(), 0, changed);
        const std::uint32_t ids[5] = {0x01020304u, 0, 0, 0, 0};
        model.SetTextureData(ids, 1.0f);

        SC4DrawContext context;
        Driver driver;
        context.SetDriver(&driver);
        context.Init();
        context.SetZoomAndRotation(0, 0);   // edge 0 is drawn at rotation 0

        CHECK(model.Draw(&context, true) == 1);   // highlight: no state change, no draw
        CHECK(driver.draws == 0);
        CHECK(model.ZoomLevel() == 0xff);

        int litBefore = scene.host.lighting.calls;
        CHECK(model.Draw(&context, false) == 1);
        CHECK(model.ZoomLevel() == 0);
        CHECK(model.ViewRotation() == 0);
        CHECK(scene.host.lighting.calls == litBefore + 12);   // the rotation change re-lights all 12 vertices
        CHECK(driver.draws == 1);
        CHECK(driver.lastPrimitive == 0 && driver.lastFormat == 0x0a && driver.lastCount == 12);
        CHECK(driver.lastVertices == model.Vertices().data());
        CHECK(driver.textures == 1 && driver.lastTexture == 0x01020304u);

        // the same draw again: no zoom change, no rotation change, one more submission and no relight
        litBefore = scene.host.lighting.calls;
        CHECK(model.Draw(&context, false) == 1);
        CHECK(driver.draws == 2);
        CHECK(scene.host.lighting.calls == litBefore);

        // a different rotation rebuilds nothing but re-lights, and edge 0 is culled at rotation 2
        litBefore = scene.host.lighting.calls;
        context.SetZoomAndRotation(0, 2);
        CHECK(model.Draw(&context, false) == 1);
        CHECK(model.ViewRotation() == 2);
        CHECK(scene.host.lighting.calls == litBefore + 12);
        CHECK(driver.draws == 2);   // culled

        // a new zoom level rebinds the texture (no texture for zoom 1 -> no binding, so no draw)
        context.SetZoomAndRotation(1, 2);
        CHECK(model.Draw(&context, false) == 1);
        CHECK(model.ZoomLevel() == 1);
        CHECK(model.TextureId() == 0);
        CHECK(driver.draws == 2);
    }

    if (failures == 0) std::printf("sc4_lotretainingwallmodel: ok\n");
    else std::printf("sc4_lotretainingwallmodel: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
