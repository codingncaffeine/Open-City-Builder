// cSC4CellTextureModel: a lot base texture cell drawn on the terrain. Covered: the unlocked cell's quad over the four
// terrain corners is split along the terrain's own diagonal for that cell (cSTETerrainView3D::DrawTexturedTerrainQuad's
// index tables {0,2,3, 0,3,1} and, flipped, {0,2,1, 1,2,3}), so a cell that is not flat shows no ground through its
// centre; a locked lot's flat quad keeps the strip, its corners lit by the lighting manager (straight up) with the spec's
// alpha; the combine colour is the spec colour times the global light's red, and combine mode 3 under render property 29
// draws as mode 1 at half alpha.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "render/gd/GDriverBase.h"
#include "sc4/CellTextureModel.h"
#include "sc4/DrawContext.h"
#include "sc4/RenderProperties.h"

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
    int lightQueries = 0;
    float normalY = 0.0f;
    bool LightColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t rgb[3]) override {
        ++lightQueries;
        normalY = normal.y;
        rgb[0] = static_cast<std::uint8_t>(position.x - 592.0f);   // cell 37: x 592 / 608
        rgb[1] = static_cast<std::uint8_t>(position.y);            // the lot's altitude, 20
        rgb[2] = 9;
        return true;
    }
};

struct Lighting final : IS4LightingManager {
    void GetGlobalLight(cS3DVector3& out) override { out = cS3DVector3{0.5f, 0.9f, 0.9f}; }
    bool GetModelLight(s3d::cIS3DModelInstance*, cS3DVector3&) override { return false; }
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
    float texColor[4] = {-1, -1, -1, -1};
    void TexEnv(gd::eGDTextureEnvTarget, gd::eGDTextureEnvParamType, const float* values) override {
        for (int i = 0; i < 4; ++i) texColor[i] = values[i];
    }
    using gd::cGDriverBase::TexEnv;
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
    {   // the locked lot's corners: the lighting manager's colour at each corner, straight up, with the spec's alpha
        Host host;
        host.locked = true;
        Driver driver;
        sc4::SetCellTextureModelHost(&host);
        sc4::cSC4CellTextureModel model;
        sc4::LotBaseTextureSpecification spec = Spec();
        spec.alpha = 0xc0;
        spec.combineMode = 0xff;
        model.Init(spec);
        SC4DrawContext context;
        context.SetDriver(&driver);
        context.Init();
        context.SetZoomAndRotation(4, 0);
        context.SetRenderPass(1);
        model.Draw(&context, false);
        // lit on the zoom change and again on the rotation change (both on the first draw, as the original)
        CHECK(host.lightQueries == 8 && host.normalY == 1.0f);
        CHECK(model.VertexColor(0)[0] == 0 && model.VertexColor(1)[0] == 16 && model.VertexColor(3)[0] == 16);
        CHECK(model.VertexColor(2)[1] == 20 && model.VertexColor(2)[2] == 9 && model.VertexColor(2)[3] == 0xc0);
        sc4::SetCellTextureModelHost(nullptr);
    }
    {   // the combine colour: the spec colour times the global light's red (0.5), the alpha as is; mode 3 under render
        // property 29 draws as mode 1 at half the alpha
        Host host;
        Lighting lighting;
        sc4::cSC4RenderProperties properties;
        for (int pass = 0; pass < 2; ++pass) {
            Driver driver;
            sc4::SetCellTextureModelHost(&host);
            sc4::cSC4CellTextureModel model;
            sc4::LotBaseTextureSpecification spec = Spec();
            spec.red = 200;
            spec.green = 100;
            spec.blue = 50;
            spec.alpha = 0x80;
            spec.combineMode = pass == 0 ? 2 : 3;
            model.Init(spec);
            SC4DrawContext context;
            context.SetDriver(&driver);
            context.Init();
            context.SetZoomAndRotation(4, 0);
            context.SetRenderPass(1);
            context.SetLightingManager(&lighting);
            if (pass == 1 && properties.NumBoolProperties() > 29) {
                properties.SetBoolValue(29, true);
                context.SetRenderProperties(&properties);
            }
            model.Draw(&context, false);
            const float k = 1.0f / 255.0f;
            CHECK(std::fabs(driver.texColor[0] - 200 * k * 0.5f) < 1e-4f && std::fabs(driver.texColor[1] - 100 * k * 0.5f) < 1e-4f);
            CHECK(std::fabs(driver.texColor[2] - 50 * k * 0.5f) < 1e-4f);
            CHECK(std::fabs(driver.texColor[3] - (pass == 0 ? 0x80 : 0x40) * k) < 1e-4f);
            sc4::SetCellTextureModelHost(nullptr);
        }
        CHECK(properties.NumBoolProperties() > 29);
    }
    if (failures == 0) std::printf("sc4_celltexturemodel: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
