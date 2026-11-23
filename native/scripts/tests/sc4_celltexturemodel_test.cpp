// cSC4CellTextureModel: a lot base texture cell drawn on the terrain. Covered: the unlocked cell's quad over the four
// terrain corners is split along the terrain's own diagonal for that cell (cSTETerrainView3D::DrawTexturedTerrainQuad's
// index tables {0,2,3, 0,3,1} and, flipped, {0,2,1, 1,2,3}), so a cell that is not flat shows no ground through its
// centre; a locked lot's flat quad keeps the strip.
#include <cstdio>
#include <cstdint>
#include <vector>

#include "render/gd/GDriverBase.h"
#include "sc4/CellTextureModel.h"
#include "sc4/DrawContext.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct Host final : sc4::ICellTextureModelHost {
    bool locked = false;
    bool flipped = false;
    float corners[4] = {10.0f, 12.0f, 11.0f, 15.0f};   // not planar: the diagonals disagree at the centre
    std::uint32_t frame = 0x5150;
    sc4::CellTextureBinding binding{&frame, 1, false};
    int flipQueries = 0;
    bool LockedLotAltitude(int, int, float& altitude) override { altitude = 20.0f; return locked; }
    void CellCornerAltitudes(int, int, float out[4]) override { for (int i = 0; i < 4; ++i) out[i] = corners[i]; }
    void CellCornerShades(int, int, std::uint8_t out[4]) override { for (int i = 0; i < 4; ++i) out[i] = 0xff; }
    const sc4::CellTextureBinding* TextureBinding(std::uint32_t) override { return &binding; }
    bool CellTriangulationFlipped(int, int) override { ++flipQueries; return flipped; }
};

struct Driver final : gd::cGDriverBase {
    int arrays = 0, elements = 0, lastPrimitive = -1, lastCount = 0;
    std::vector<std::uint16_t> lastIndices;
    void DrawArrays(gd::eGDPrimType primitive, int, int count) override {
        ++arrays;
        lastPrimitive = static_cast<int>(primitive);
        lastCount = count;
    }
    void DrawElements(gd::eGDPrimType primitive, int count, gd::eGDType, const void* indices) override {
        ++elements;
        lastPrimitive = static_cast<int>(primitive);
        lastCount = count;
        const auto* p = static_cast<const std::uint16_t*>(indices);
        lastIndices.assign(p, p + count);
    }
    void InterleavedArrays(gd::eGDVertexFormat, int, const void*) override {}
    void SetTexture(std::uint32_t, std::uint32_t) override {}
};

sc4::LotBaseTextureSpecification Spec() {
    sc4::LotBaseTextureSpecification spec;
    spec.textureId = 0x2500;
    spec.cellX = 37;
    spec.cellZ = 99;
    spec.alpha = 0xff;
    spec.combineMode = 0xff;   // opaque: drawn in the first pass
    spec.frame = 0;
    return spec;
}

int DrawOnce(Host& host, Driver& driver) {
    sc4::SetCellTextureModelHost(&host);
    sc4::cSC4CellTextureModel model;
    model.Init(Spec());
    SC4DrawContext context;
    context.SetDriver(&driver);
    context.Init();
    context.SetZoomAndRotation(4, 0);
    const int result = model.Draw(&context, false);
    sc4::SetCellTextureModelHost(nullptr);
    return result;
}

}  // namespace

int main() {
    {   // a normal cell: the terrain's diagonal (vertex 0 to 3)
        Host host;
        Driver driver;
        CHECK(DrawOnce(host, driver) == 1);
        CHECK(host.flipQueries >= 1);
        CHECK(driver.elements == 1 && driver.arrays == 0);
        CHECK(driver.lastPrimitive == 0 && driver.lastCount == 6);
        CHECK((driver.lastIndices == std::vector<std::uint16_t>{0, 2, 3, 0, 3, 1}));
    }
    {   // a flipped cell: the other diagonal (vertex 1 to 2)
        Host host;
        host.flipped = true;
        Driver driver;
        CHECK(DrawOnce(host, driver) == 1);
        CHECK(driver.elements == 1);
        CHECK((driver.lastIndices == std::vector<std::uint16_t>{0, 2, 1, 1, 2, 3}));
    }
    {   // a locked lot: flat at the lot's altitude, the strip as before
        Host host;
        host.locked = true;
        Driver driver;
        CHECK(DrawOnce(host, driver) == 1);
        CHECK(driver.elements == 0 && driver.arrays == 1);
        CHECK(driver.lastPrimitive == 1 && driver.lastCount == 4);
    }
    if (failures == 0) std::printf("sc4_celltexturemodel: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
