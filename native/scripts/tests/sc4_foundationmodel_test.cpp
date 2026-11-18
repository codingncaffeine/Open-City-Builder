// cSC4FoundationModel (sc4/FoundationModel.h): the side and top quads, the reach down to the ground, the texture retry, the sort
// value. The engine services are a fake host.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>

#include "sc4/FoundationModel.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct Binding final : IFoundationTextureBinding {
    explicit Binding(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }
    std::uint32_t id_;
};

struct Host final : IFoundationModelHost {
    std::set<std::uint32_t> known;      // the texture ids that bind
    std::map<int, float> ground, water; // by vertex index
    float defaultGround = 100.0f;
    int cellsX = 63;                    // 64 vertices per row
    int edgeA = 2, edgeB = 3;
    IFoundationTextureBinding* GetBinding(std::uint32_t instance) override { return known.count(instance) ? new Binding(instance * 2) : nullptr; }
    void VisibleEdges(int& a, int& b) override { a = edgeA; b = edgeB; }
    void LightColor(const float normal[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = static_cast<std::uint8_t>(100 + normal[0] * 10);
        g = static_cast<std::uint8_t>(100 + normal[1] * 50);
        b = static_cast<std::uint8_t>(100 + normal[2] * 10);
    }
    int TerrainCellCountX() override { return cellsX; }
    float TerrainAltitude(int vertex) override { auto it = ground.find(vertex); return it == ground.end() ? defaultGround : it->second; }
    float TerrainWaterLevel(int vertex) override { auto it = water.find(vertex); return it == water.end() ? 0.0f : it->second; }
};

cS3DBoundingBox Box(float x0, float y0, float z0, float x1, float y1, float z1) {
    cS3DBoundingBox box;
    box.min[0] = x0; box.min[1] = y0; box.min[2] = z0;
    box.max[0] = x1; box.max[1] = y1; box.max[2] = z1;
    return box;
}

}  // namespace

int main() {
    Host host;
    SetFoundationModelHost(&host);

    {   // geometry: a 32 x 16 box from 104 down to 100 at cells 2..3, 4; the four sides and the top
        auto* model = new cSC4FoundationModel();
        model->AddRef();
        const std::uint32_t side[5] = {0x100, 0x101, 0x102, 0x103, 0x104}, top[5] = {0x200, 0x201, 0x202, 0x203, 0x204};
        model->SetTextureIDs(side, top);
        model->SetTextureWorldSize(16.0f, 32.0f);
        model->SetWorldBounds(Box(32.0f, 100.0f, 64.0f, 64.0f, 104.0f, 80.0f));
        CHECK(model->AdjustHeightToTerrain(true));   // forced: the vertices are built though the ground is not deeper
        CHECK(model->Bounds().min[1] == 100.0f);
        const FoundationVertex* s = model->SideVertices();
        // edge 0 (min z): x1 top, x1 bottom, x0 bottom, x0 top; u = width / 16 = 2, v = height / 16 = 0.25
        CHECK(s[0].x == 64.0f && s[0].y == 104.0f && s[0].z == 64.0f && s[0].u == 0.0f && s[0].v == 0.0f);
        CHECK(s[1].x == 64.0f && s[1].y == 100.0f && s[1].v == 0.25f);
        CHECK(s[2].x == 32.0f && s[2].y == 100.0f && s[2].u == 2.0f && s[2].v == 0.25f);
        CHECK(s[3].x == 32.0f && s[3].y == 104.0f && s[3].u == 2.0f && s[3].v == 0.0f);
        // edge 1 (max x) runs along z with the width's u (quirk)
        CHECK(s[4].x == 64.0f && s[4].z == 80.0f && s[6].z == 64.0f && s[6].u == 2.0f);
        // edge 2 (max z) and edge 3 (min x)
        CHECK(s[8].x == 32.0f && s[8].z == 80.0f && s[10].x == 64.0f && s[10].z == 80.0f);
        CHECK(s[12].x == 32.0f && s[12].z == 64.0f && s[14].z == 80.0f && s[15].y == 104.0f);
        // top: uv in the side texture's world size (quirk): 32 / 16 = 2 by 16 / 16 = 1
        const FoundationVertex* t = model->TopVertices();
        CHECK(t[0].x == 32.0f && t[0].y == 104.0f && t[0].z == 64.0f && t[0].u == 0.0f && t[0].v == 0.0f);
        CHECK(t[2].x == 64.0f && t[2].z == 80.0f && t[2].u == 2.0f && t[2].v == 1.0f);

        // nothing deeper and not forced: no rebuild
        CHECK(!model->AdjustHeightToTerrain(false));
        // a border vertex 20 below the top: the bottom goes down to whole cells (20 + 16 -> 2 cells = 32 below the top)
        host.ground[2 + 64 * 4] = 84.0f;
        CHECK(model->AdjustHeightToTerrain(false));
        CHECK(model->Bounds().min[1] == 104.0f - 32.0f);
        // water above the ground counts as the surface
        host.ground.clear();
        host.water[3 + 64 * 5] = 120.0f;
        CHECK(!model->AdjustHeightToTerrain(false));   // water above the top: nothing below
        model->Release();
    }

    {   // textures: the zoom's id, retried as 0x69f20000 + zoom (side) / 0x0912220f + zoom (top) when it does not bind
        auto* model = new cSC4FoundationModel();
        model->AddRef();
        const std::uint32_t side[5] = {0x100, 0x101, 0x102, 0x103, 0x104}, top[5] = {0x200, 0x201, 0x202, 0x203, 0x204};
        model->SetTextureIDs(side, top);
        std::uint32_t sort = 0;
        model->GetSortValue(sort);
        CHECK(sort == (0xe9862ab6u | 0x2ab6u));
        host.known = {0x102, 0x0912220fu + 2};
        model->DoZoomChange(-1, 2);
        CHECK(model->SideTextureId(2) == 0x102);
        CHECK(model->TopTextureId(2) == 0x0912220fu + 2);
        model->GetSortValue(sort);
        CHECK(sort == ((0x102u * 2) | 0x2ab6u));
        CHECK(model->EdgeA() == 2 && model->EdgeB() == 3);
        host.known.clear();
        model->DoZoomChange(2, 3);
        CHECK(model->SideTextureId(3) == 0x69f20000u + 3);   // retried and kept
        model->GetSortValue(sort);
        CHECK(sort == (0xe9862ab6u | 0x2ab6u));
        model->Release();
    }

    {   // lighting: each side by the light on its edge's outward normal, the top by the light from above
        auto* model = new cSC4FoundationModel();
        model->AddRef();
        model->SetWorldBounds(Box(0.0f, 0.0f, 0.0f, 16.0f, 4.0f, 16.0f));
        model->SetupVertices(true, true);
        const FoundationVertex* s = model->SideVertices();
        CHECK(s[0].color[3] == 0xff && s[15].color[3] == 0xff);
        CHECK(model->TopVertices()[0].color[1] == 150 && model->TopVertices()[3].color[3] == 0xff);
        CHECK(s[0].color[1] == 100);   // a side's normal is horizontal
        model->Release();
    }

    if (failures == 0) std::printf("sc4_foundationmodel: all passed\n");
    return failures == 0 ? 0 : 1;
}
