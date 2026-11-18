// SC4DrawContext: render-state diffing, texture stages, the model-view matrix, mesh submission and the
// RenderModelInstance / DrawBoundingBox paths, driven through a recording cIGZGDriver.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sc4/DrawContext.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
#include "sc4/Occupant.h"
#pragma clang diagnostic pop

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

std::string I(long long v) { return std::to_string(v); }
std::string F(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.6g", static_cast<double>(v));
    return buffer;
}
bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f * (1.0f + std::fabs(b)); }

struct Driver final : gd::cGDriverBase {
    std::vector<std::string> calls;
    std::vector<std::array<float, 16>> matrices;
    std::vector<std::array<std::int32_t, 3>> drawArrays;
    struct ElementCall { std::int32_t primitive, count, type; const void* indices; };
    std::vector<ElementCall> drawElements;
    struct InterleavedCall { std::int32_t format, stride; const void* data; };
    std::vector<InterleavedCall> interleaved;
    gd::sGDMode reportedMode{};

    void DrawArrays(gd::eGDPrimType primitive, int first, int count) override {
        drawArrays.push_back({static_cast<std::int32_t>(primitive), first, count});
    }
    void DrawElements(gd::eGDPrimType primitive, int count, gd::eGDType type, const void* indices) override {
        drawElements.push_back({static_cast<std::int32_t>(primitive), count, static_cast<std::int32_t>(type), indices});
    }
    void InterleavedArrays(gd::eGDVertexFormat format, int stride, const void* data) override {
        interleaved.push_back({static_cast<std::int32_t>(format), stride, data});
    }
    void ColorMask(bool enable) override { calls.push_back("ColorMask(" + I(enable) + ")"); cGDriverBase::ColorMask(enable); }
    void DepthMask(bool enable) override { calls.push_back("DepthMask(" + I(enable) + ")"); cGDriverBase::DepthMask(enable); }
    void DepthFunc(gd::eGDTestFunc func) override { calls.push_back("DepthFunc(" + I(func) + ")"); cGDriverBase::DepthFunc(func); }
    void BlendFunc(gd::eGDBlend source, gd::eGDBlend destination) override {
        calls.push_back("BlendFunc(" + I(source) + "," + I(destination) + ")");
        cGDriverBase::BlendFunc(source, destination);
    }
    void AlphaFunc(gd::eGDTestFunc func, float reference) override {
        calls.push_back("AlphaFunc(" + I(func) + "," + F(reference) + ")");
        cGDriverBase::AlphaFunc(func, reference);
    }
    void Enable(gd::eGDDriverState state) override { calls.push_back("Enable(" + I(state) + ")"); cGDriverBase::Enable(state); }
    void Disable(gd::eGDDriverState state) override { calls.push_back("Disable(" + I(state) + ")"); cGDriverBase::Disable(state); }
    void TexStage(std::uint32_t stage) override { calls.push_back("TexStage(" + I(stage) + ")"); cGDriverBase::TexStage(stage); }
    void TexEnv(gd::eGDTextureEnvTarget target, gd::eGDTextureEnvParamType param, gd::eGDTextureEnvModeParam mode) override {
        calls.push_back("TexEnv(" + I(target) + "," + I(param) + "," + I(mode) + ")");
        cGDriverBase::TexEnv(target, param, mode);
    }
    void TexEnv(gd::eGDTextureEnvTarget target, gd::eGDTextureEnvParamType param, const float* values) override {
        calls.push_back("TexEnvColor(" + I(target) + "," + I(param) + "," + F(values[0]) + "," + F(values[1]) + "," +
                        F(values[2]) + "," + F(values[3]) + ")");
    }
    void TexParameter(gd::eGDTextureTarget target, gd::eGDTextureParamType param, gd::eGDTextureParam value) override {
        calls.push_back("TexParameter(" + I(target) + "," + I(param) + "," + I(value) + ")");
    }
    void TexStageCoord(gd::eGDTexCoordSource source) override { calls.push_back("TexStageCoord(" + I(source) + ")"); }
    void TexStageMatrix(const float* matrix, std::uint32_t a, std::uint32_t b, gd::eGDTexMatFlags flags) override {
        calls.push_back("TexStageMatrix(" + I(a) + "," + I(b) + "," + I(flags) + (matrix ? ",m" : ",null") + ")");
    }
    void SetTexture(std::uint32_t texture, std::uint32_t stage) override {
        calls.push_back("SetTexture(" + I(texture) + "," + I(stage) + ")");
    }
    void SetCombiner(const gd::cGDCombiner& combiner, std::uint32_t stage) override {
        calls.push_back("SetCombiner(" + I(combiner.word0) + "," + I(combiner.word1) + "," + I(combiner.word2) + "," + I(stage) + ")");
    }
    void ColorMultiplier(float r, float g, float b) override {
        calls.push_back("ColorMultiplier(" + F(r) + "," + F(g) + "," + F(b) + ")");
    }
    void AlphaMultiplier(float a) override { calls.push_back("AlphaMultiplier(" + F(a) + ")"); }
    void Fog(gd::eGDFogParamType param, gd::eGDFogParam value) override { calls.push_back("Fog(" + I(param) + "," + I(value) + ")"); }
    void Fog(gd::eGDFogParamType param, const float* values) override { calls.push_back("Fog(" + I(param) + "," + F(values[0]) + ")"); }
    void MatrixMode(gd::eGDMatrixTarget target) override { calls.push_back("MatrixMode(" + I(target) + ")"); }
    void LoadMatrix(const float* matrix) override {
        std::array<float, 16> copy{};
        std::memcpy(copy.data(), matrix, sizeof copy);
        matrices.push_back(copy);
        calls.push_back("LoadMatrix");
    }
    bool GetVideoModeInfo(gd::sGDMode& mode) override {
        mode = reportedMode;
        return true;
    }
    void Reset() {
        calls.clear();
        matrices.clear();
        drawArrays.clear();
        drawElements.clear();
        interleaved.clear();
    }
    bool Has(const std::string& call) const {
        for (const std::string& c : calls)
            if (c == call) return true;
        return false;
    }
    int Count(const std::string& call) const {
        int n = 0;
        for (const std::string& c : calls)
            if (c == call) ++n;
        return n;
    }
    int CountPrefix(const std::string& prefix) const {
        int n = 0;
        for (const std::string& c : calls)
            if (c.rfind(prefix, 0) == 0) ++n;
        return n;
    }
    const std::string& Last() const {
        static const std::string empty;
        return calls.empty() ? empty : calls.back();
    }
};

struct FakeViewUtilities final : IS4ViewUtilities {
    float pixelsPerWorld = 1.0f;
    float ViewportUnitsPerWorld() const override { return pixelsPerWorld; }
    void SetPixelsPerWorld(float value) { pixelsPerWorld = value; }
};

struct FakeLighting final : IS4LightingManager {
    cS3DVector3 global{0.2f, 0.4f, 0.6f};
    cS3DVector3 model{0.5f, 0.25f, 0.125f};
    bool modelLightAvailable = true;
    bool modelLightCalled = false;
    s3d::cIS3DModelInstance* lastInstance = nullptr;
    void GetGlobalLight(cS3DVector3& out) override { out = global; }
    bool GetModelLight(s3d::cIS3DModelInstance* instance, cS3DVector3& out) override {
        modelLightCalled = true;
        lastInstance = instance;
        out = model;
        return modelLightAvailable;
    }
};

// One vertex group, one primitive list and (optionally) an index buffer, i.e. what RenderMesh walks.
struct MeshFixture {
    SC4MeshVertexGroup vertices;
    SC4MeshDrawRecord records[2] = {{0, 0, 3}, {0, 3, 6}};
    SC4MeshDrawList draws;
    const std::uint16_t* indexPointer = nullptr;
    SC4Mesh mesh;
    std::uint16_t indices[8] = {0, 1, 2, 2, 3, 0, 4, 5};
    int vertexData = 0;

    MeshFixture() {
        vertices.format = 10;
        vertices.count = 4;
        vertices.data = &vertexData;
        draws.begin = records;
        draws.end = records + 2;
        mesh.vertices = &vertices;
        mesh.draws = &draws;
    }
    void WithIndices() {
        indexPointer = indices;
        mesh.indexGroup = &indexPointer;
    }
};

struct FakeMeshBuilder final : IS3DMeshBuilder {
    struct BoxCall { cS3DVector3 corners[8]; };
    int clears = 0;
    bool directionSet = false;
    cS3DVector3 direction{};
    bool colorSet = false;
    cS3DVector4 color{};
    std::vector<BoxCall> boxes;
    SC4Mesh emitted;
    SC4MeshVertexGroup emittedVertices;
    SC4MeshDrawRecord emittedRecord{0, 0, 36};
    SC4MeshDrawList emittedDraws;

    FakeMeshBuilder() {
        emittedVertices.format = 10;
        emittedVertices.count = 24;
        emittedVertices.data = this;
        emittedDraws.begin = &emittedRecord;
        emittedDraws.end = &emittedRecord + 1;
        emitted.vertices = &emittedVertices;
        emitted.draws = &emittedDraws;
    }
    void Clear() override { ++clears; }
    void SetDirLight(const cS3DVector3& value) override { directionSet = true; direction = value; }
    void SetColor(const cS3DVector4& value) override { colorSet = true; color = value; }
    void AddBoxMesh(const cS3DVector3 corners[8]) override {
        BoxCall call{};
        std::memcpy(call.corners, corners, sizeof call.corners);
        boxes.push_back(call);
    }
    const SC4Mesh* Mesh() const override { return &emitted; }
};

struct FakeMeshBuilderHost final : IMeshBuilderHost {
    FakeMeshBuilder builder;
    IS3DMeshBuilder* BoxMeshBuilder() override { return &builder; }
};

// A cIS3DModelInstance with a fixed transform / bounds and a settable owner.
class FakeInstance final : public s3d::cIS3DModelInstance {
public:
    cIGZUnknown* owner = nullptr;
    cS3DBoundingBox bounds;
    cS3DTransform transform;
    s3d::SC4DrawAttributes attributes;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != s3d::kGZIID_cIS3DModelInstance) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refCount; }
    std::uint32_t Release() override { return --refCount; }
    cIGZUnknown* GetOwner() override { return owner; }
    void SetOwner(cIGZUnknown* value) override { owner = value; }
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetWorldBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& value) override { transform = value; }
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 1; }
    int GetProperties() override { return 2; }
    void SetProperties(int) override {}
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return attributes; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes& value) override { attributes = value; }
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }

private:
    std::uint32_t refCount = 0;
};

// cISC4Occupant with just GetBoundingBox filled in (the debug path of RenderModelInstance queries it).
class FakeOccupant final : public sc4::cISC4Occupant {
public:
    cS3DBoundingBox bounds;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != sc4::kGZIID_cISC4Occupant) return false;
        *out = static_cast<cISC4Occupant*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float*) override {}
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        std::memcpy(minXyz, bounds.min, sizeof bounds.min);
        std::memcpy(maxXyz, bounds.max, sizeof bounds.max);
    }
    void GetBoundingCityCells(sc4::SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    sc4::cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    sc4::cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(sc4::cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

// A two-layer material whose texture records start with the texture id RenderModelInstance reads.
struct MaterialFixture {
    std::uint32_t textureIds[2] = {111, 222};
    const void* textureBindings[2] = {&textureIds[0], &textureIds[1]};   // binding -> record -> id
    s3d::cS3DMaterial material{};

    MaterialFixture() {
        material.flags = 0x100 | 0x80 | 0x02 | 0x01 | 0x10 | 0x08 | 0x20;   // colour/depth/alpha/blend/cull/layers
        material.b6 = 3;      // depth func
        material.b7 = 4;      // alpha func
        material.b8 = 255;    // alpha
        material.b9 = 4;      // blend source
        material.b10 = 5;     // blend destination
        material.layerCount = 2;
        material.layers[0].textureId = 111;
        material.layers[0].texture = reinterpret_cast<std::uint64_t>(&textureBindings[0]);
        material.layers[0].sampler[0] = 3;
        material.layers[0].sampler[1] = 3;
        material.layers[0].sampler[2] = 1;
        material.layers[0].sampler[3] = 7;
        material.layers[0].used = 1;
        material.layers[0].pad15 = 5;
        material.layers[0].scale = 1.0f;
        material.layers[1].textureId = 222;
        material.layers[1].texture = reinterpret_cast<std::uint64_t>(&textureBindings[1]);
        material.layers[1].sampler[0] = 2;
        material.layers[1].sampler[1] = 1;
        material.layers[1].sampler[2] = 2;
        material.layers[1].sampler[3] = 3;
        material.layers[1].used = 4;      // texture combiner
        material.layers[1].pad15 = 0;
        material.layers[1].pad16 = 1;     // scaled texture transform
        material.layers[1].scale = 4.0f;
    }
};

}  // namespace

int main() {
    // ---- construction ----------------------------------------------------------------------------------
    {
        SC4DrawContext dc;
        CHECK(!dc.IsInitialized());
        CHECK(dc.StateWord() == 0);
        CHECK(!dc.GetLighting());
        CHECK(dc.Stages().empty());
        CHECK(dc.HighlightColors().size() == 11);
        for (const cS3DVector4& color : dc.HighlightColors())
            CHECK(color.x == 1.0f && color.y == 1.0f && color.z == 1.0f && color.w == 1.0f);
        float matrix[16] = {};
        dc.GetModelViewMatrix(matrix);
        for (float value : matrix) CHECK(value == 0.0f);
    }

    // ---- Init: device capabilities, stage count, highlight colours and the default state ----------------
    {
        Driver driver;
        driver.reportedMode = gd::sGDMode{};
        driver.reportedMode.flags[1] = 3;
        reinterpret_cast<std::uint8_t*>(&driver.reportedMode)[0x29] = 0x0b;
        reinterpret_cast<std::uint8_t*>(&driver.reportedMode)[0x2a] = 0x0c;
        sc4::cSC4RenderProperties properties;
        FakeViewUtilities view;
        FakeLighting lighting;
        SC4DrawContext dc;
        dc.SetDriver(&driver);
        dc.SetRenderProperties(&properties);
        dc.SetViewUtilities(&view);
        dc.SetLightingManager(&lighting);
        dc.Init();
        CHECK(dc.IsInitialized());
        CHECK(dc.MaxTextureStages() == 3);
        CHECK(dc.Stages().size() == 3);
        CHECK(dc.DeviceCapability29() == 0x0b && dc.DeviceCapability2a() == 0x0c);

        const std::vector<cS3DVector4>& colors = dc.HighlightColors();
        CHECK(colors[0].x == 1.0f && colors[0].y == 1.0f && colors[0].z == 1.0f && colors[0].w == 1.0f);
        CHECK(colors[1].x == 0.0f && colors[1].y == 1.0f && colors[1].z == 0.0f);
        CHECK(colors[2].x == 1.0f && colors[2].y == 0.0f && colors[2].z == 0.0f);
        CHECK(Near(colors[3].x, 0.2f) && Near(colors[3].y, 0.2f) && Near(colors[3].z, 0.4f) && Near(colors[3].w, 0.6f));
        CHECK(Near(colors[4].x, 0.66f) && Near(colors[4].w, 0.7f));
        CHECK(Near(colors[5].x, 0.66f) && colors[5].w == 1.0f);
        CHECK(Near(colors[7].y, 0.37f));
        CHECK(colors[8].x == colors[6].x && colors[8].y == colors[6].y && colors[8].z == colors[6].z);   // quirk
        CHECK(Near(colors[9].y, 0.7f) && Near(colors[9].z, 0.2f) && Near(colors[9].w, 0.3f));
        CHECK(Near(colors[10].x, 0.7f) && Near(colors[10].z, 0.2f));

        // Init only packs the two render states; the driver sees nothing until a state is applied
        CHECK(dc.StateWord() == 0);
        CHECK(driver.calls.empty());
        dc.SetDefaultRenderState();
        cSC4PackedRenderState expected;
        expected.PackRenderState(true, true, true, false, false, 0, false, 3, 4, 5, 1, 0.5f, 1, 1);
        CHECK(dc.StateWord() == expected.value);
        CHECK(driver.Has("ColorMask(1)") && driver.Has("DepthMask(1)"));
        CHECK(driver.Has("Enable(1)") && driver.Has("DepthFunc(3)") && driver.Has("BlendFunc(4,5)"));
        CHECK(driver.CountPrefix("AlphaFunc(1,") == 1);
        // both TexEnv words are sent (the default state names TexEnv 1 as well), then stage 0 again
        CHECK(driver.Count("TexEnv(0,0,1)") == 2);
        CHECK(driver.Has("TexStage(1)") && driver.Has("TexStage(0)"));
        // fields that were already 0 are not touched
        CHECK(!driver.Has("Enable(5)") && !driver.Has("Disable(0)") && !driver.Has("Disable(4)"));
        for (const SC4DrawContext::cStageInfo& stage : dc.Stages()) {
            // the tail of SetRenderState has already pushed stage 0 back to repeat wrapping / bilinear
            CHECK(stage.texture == -1 && stage.texCoord == -1);
            CHECK(stage.combiner.word0 == 0x01030001u && stage.combiner.word1 == 0x01030001u);
            CHECK(stage.combiner.word2 == 0x00010000u);
            CHECK(!stage.transform && stage.texMatrix[0] == 1.0f && stage.texMatrix[5] == 1.0f && stage.texMatrix[10] == 1.0f);
        }
        CHECK(dc.Stages()[0].wrapU == 3 && dc.Stages()[0].wrapV == 3 && dc.Stages()[0].minFilter == 1);
        CHECK(dc.Stages()[0].magFilter == 7);
        CHECK(driver.Has("TexParameter(0,2,3)") && driver.Has("TexParameter(0,3,3)"));
        CHECK(driver.Has("TexParameter(0,0,1)") && driver.Has("TexParameter(0,1,7)"));
        SC4DeviceFlags flags;
        CHECK(FirstRenderStateMismatch(driver.Mirror(), dc.StateWord(), flags).empty());
        dc.Init();   // idempotent
        CHECK(dc.Stages().size() == 3);
        dc.Shutdown();
        CHECK(!dc.IsInitialized());
        CHECK(dc.StateWord() != 0);
    }

    // ---- camera: transposed projection, model-view from the camera's view transform ---------------------
    {
        Driver local;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        s3d::cS3DCamera camera;
        camera.SetOrtho(-2.0f, 2.0f, 1.0f, -1.0f, 0.5f, 100.0f);
        camera.SetPosition(cS3DVector3{1.0f, 2.0f, 3.0f});
        dc.SetCamera(&camera);
        CHECK(local.Has("MatrixMode(1)") && local.Has("MatrixMode(0)"));
        CHECK(local.matrices.size() == 2);
        const float* projection = camera.ProjectionMatrix();
        bool transposed = true;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                if (local.matrices[0][row * 4 + col] != projection[col * 4 + row]) transposed = false;
        CHECK(transposed);
        CHECK(Near(local.matrices[1][0], 1.0f) && Near(local.matrices[1][5], 1.0f) && Near(local.matrices[1][10], 1.0f));
        CHECK(Near(local.matrices[1][12], -1.0f) && Near(local.matrices[1][13], -2.0f) && Near(local.matrices[1][14], -3.0f));
        CHECK(local.matrices[1][15] == 1.0f);
        CHECK(local.matrices[1][3] == 0.0f && local.matrices[1][7] == 0.0f && local.matrices[1][11] == 0.0f);
        float matrix[16] = {};
        dc.GetModelViewMatrix(matrix);
        CHECK(std::memcmp(matrix, local.matrices[1].data(), sizeof matrix) == 0);
        CHECK(dc.ViewTransform().translation[0] == -1.0f && dc.ViewTransform().translation[2] == -3.0f);

        // scale flag: the rotation is multiplied by the view scale (1 / the camera scale)
        cS3DTransform& localXform = camera.LocalXform();
        localXform.flags = static_cast<std::uint8_t>(localXform.flags | nS3D::kTransformScale);
        localXform.scale = 0.5f;
        localXform.changeCounter = static_cast<std::uint8_t>(localXform.changeCounter + 1);
        dc.SetModelViewTransformChanged();
        dc.ResetModelViewTransform();
        CHECK(Near(local.matrices.back()[0], 2.0f) && Near(local.matrices.back()[5], 2.0f));
        CHECK(Near(local.matrices.back()[12], -2.0f));   // the view translation is scaled, the world one is not
    }

    // ---- state flags -----------------------------------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        local.Reset();
        dc.EnableColorMaskFlag(true);
        CHECK(local.Has("ColorMask(1)") && (dc.StateWord() & 1u) != 0);
        dc.EnableColorMaskFlag(true);
        CHECK(local.Count("ColorMask(1)") == 1);
        dc.EnableColorMaskFlag(false);
        CHECK(local.Has("ColorMask(0)") && (dc.StateWord() & 1u) == 0);
        dc.EnableDepthTestFlag(true);
        dc.EnableDepthMaskFlag(true);
        dc.EnableCullFaceFlag(true);
        CHECK(local.Has("Enable(1)") && local.Has("DepthMask(1)") && local.Has("Enable(3)"));
        CHECK((dc.StateWord() & (4u | 2u | 0x40u)) == (4u | 2u | 0x40u));
        dc.EnableAlphaTestFlag(true);
        CHECK(local.Has("Enable(0)"));
        dc.EnableBlendStateFlag(true);
        CHECK(local.Has("Enable(4)"));
        properties.SetBoolValue(8, true);
        properties.SetBoolValue(7, true);
        dc.EnableAlphaTestFlag(false);
        dc.EnableBlendStateFlag(false);
        dc.EnableAlphaTestFlag(true);
        dc.EnableBlendStateFlag(true);
        CHECK(!local.Mirror().alphaTest && !local.Mirror().blend);
        CHECK((dc.StateWord() & 8u) == 0 && (dc.StateWord() & 0x10u) == 0);

        local.Reset();
        dc.EnableTextureStateFlag(true, 0);
        CHECK(local.Has("TexStage(0)") && local.Has("Enable(5)"));
        local.Reset();
        dc.EnableTextureStateFlag(true, 1);
        CHECK(local.Has("TexStage(1)") && (dc.StateWord() & 0x10000000u) != 0);
        local.Reset();
        dc.EnableTextureStateFlag(true, 2);
        CHECK(local.Has("TexStage(2)") && local.Has("Enable(5)"));
        local.Reset();
        dc.EnableTextureStateFlag(false, 1);
        CHECK(local.Has("TexStage(1)") && local.Has("Disable(5)") && (dc.StateWord() & 0x10000000u) == 0);
        local.Reset();
        dc.EnableTextureStateFlag(false, 0);
        CHECK(local.Has("TexStage(0)") && local.Has("Disable(5)") && (dc.StateWord() & 0x20u) == 0);
        // with the second stage disabled in the render properties the stage-1 half is skipped
        properties.SetBoolValue(8, false);
        properties.SetBoolValue(7, false);
        properties.SetBoolValue(11, false);
        local.Reset();
        dc.EnableTextureStateFlag(true, 1);
        CHECK((dc.StateWord() & 0x10000000u) != 0);   // the cached bit is still set
        CHECK(!local.Has("TexStage(1)"));             // but stage 1 is never selected
    }

    // ---- depth / blend / alpha functions ----------------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.SetDepthFunc(3);
        CHECK(local.Has("DepthFunc(3)") && ((dc.StateWord() >> 7) & 7u) == 3);
        local.Reset();
        dc.SetDepthFunc(3);
        CHECK(local.calls.empty());
        dc.SetDepthFunc(-1);
        CHECK(local.calls.empty());
        dc.SetBlendFunc(4, 5);
        CHECK(local.Has("BlendFunc(4,5)") && ((dc.StateWord() >> 13) & 0xfu) == 4 && ((dc.StateWord() >> 17) & 0xfu) == 5);
        local.Reset();
        dc.SetBlendFunc(-1, -1);
        CHECK(local.calls.empty());
        dc.SetBlendFunc(2, -1);
        CHECK(local.Has("BlendFunc(2,5)") && ((dc.StateWord() >> 13) & 0xfu) == 2 && ((dc.StateWord() >> 17) & 0xfu) == 5);
        dc.SetAlphaFunc(4, 0.0f);
        CHECK(local.Last() == "AlphaFunc(4,0)");
        CHECK(((dc.StateWord() >> 10) & 7u) == 4 && ((dc.StateWord() >> 21) & 0xfu) == 0);
        local.Reset();
        dc.SetAlphaFunc(4, 0.0f);   // same function and the same quantised reference
        CHECK(local.calls.empty());
        dc.SetAlphaFunc(-1, 0.5f);  // reference only; the effective function is sent
        CHECK(local.Last() == "AlphaFunc(4,0.5)" && local.calls.size() == 1);
        // a reference that is not a multiple of 1/15 never round-trips, so it is re-sent every time
        local.Reset();
        dc.SetAlphaFunc(-1, 0.5f);
        CHECK(local.calls.size() == 1);
    }

    // ---- render-state diffing and the stage-0 reset tail ------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.Init();
        cSC4PackedRenderState state;
        state.PackRenderState(true, true, true, false, true, 2, true, 3, 4, 5, 1, 0.5f, 1, 1);
        local.Reset();
        dc.SetRenderState(state);
        CHECK(dc.StateWord() == state.value);
        CHECK(local.Has("TexStage(1)") && local.Has("Enable(5)"));
        SC4DeviceFlags flags;
        CHECK(FirstRenderStateMismatch(local.Mirror(), dc.StateWord(), flags).empty());
        local.Reset();
        dc.SetRenderState(state);
        CHECK(local.calls.empty());   // no state change and the stage-0 reset is already in place
        cSC4PackedRenderState noBlend = state;
        noBlend.value &= ~0x10u;
        local.Reset();
        dc.SetRenderState(noBlend);
        CHECK(local.Has("Disable(4)"));
        CHECK(local.Count("Enable(5)") == 0);
        cSC4PackedRenderState loose;
        loose.PackRenderState(false, false, false, false, false, 0, false, -1, -1, -1, -1, -1.0f, -1, -1);
        const std::uint32_t before = dc.StateWord();
        dc.SetRenderState(loose);
        CHECK(((dc.StateWord() >> 7) & 7u) == ((before >> 7) & 7u));   // -1 depth func kept
        CHECK(FirstRenderStateMismatch(local.Mirror(), dc.StateWord(), flags).empty());
        cSC4PackedRenderState stage1;
        stage1.PackRenderState(true, true, true, false, true, 2, false, 3, 4, 5, 1, 0.5f, 1, 3);
        local.Reset();
        dc.SetRenderState(stage1);
        CHECK(local.Has("TexStage(1)") && local.Has("TexEnv(0,0,3)"));
    }

    // ---- default / unilateral default render state ------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.Init();
        cSC4PackedRenderState other;
        other.PackRenderState(false, false, false, false, false, 0, false, 7, 0, 0, 0, 0.0f, 0, 0);
        dc.SetRenderState(other);
        local.Reset();
        dc.SetDefaultRenderState();
        cSC4PackedRenderState expected;
        expected.PackRenderState(true, true, true, false, false, 0, false, 3, 4, 5, 1, 0.5f, 1, 1);
        CHECK(dc.StateWord() == expected.value);
        CHECK(local.Has("Enable(1)"));
        local.Reset();
        dc.SetDefaultRenderStateUnilaterally();
        CHECK(dc.StateWord() == expected.value);
        CHECK(local.Count("TexStage(0)") >= 2 && local.Count("TexStage(1)") >= 1);
        CHECK(local.CountPrefix("TexStageMatrix(3,2,1,null)") == 2);   // one per stage
        CHECK(local.Has("Enable(1)") && local.Has("DepthFunc(3)"));
        for (const SC4DrawContext::cStageInfo& stage : dc.Stages()) CHECK(stage.texture == -1 && !stage.transform);
    }

    // ---- textures: stage 0 gating, wrap/filter overrides, combiners, transforms -------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.Init();
        dc.SetTexture(111, 0);
        dc.SetTexCoord(5, 0);
        CHECK(local.Has("SetTexture(111,0)") && local.Has("TexStageCoord(5)"));
        CHECK(dc.Stages()[0].texture == 111 && dc.Stages()[0].texCoord == 5);
        local.Reset();
        dc.SetTexture(111, 0);   // SetTexture is not cached: the driver is called again
        CHECK(local.Has("SetTexture(111,0)") && !local.Has("TexStageCoord(5)"));
        dc.SetTexWrapModes(2, 1, 0);
        CHECK(local.Has("TexParameter(0,2,2)") && local.Has("TexParameter(0,3,1)"));
        local.Reset();
        properties.SetBoolValue(21, true);   // kForceTextureRepeat
        dc.SetTexWrapModes(1, 1, 0);
        CHECK(local.Has("TexParameter(0,2,3)") && local.Has("TexParameter(0,3,3)"));
        properties.SetBoolValue(21, false);
        local.Reset();
        dc.SetTexFiltering(1, 7, 0);
        CHECK(local.Has("TexParameter(0,0,1)") && local.Has("TexParameter(0,1,7)"));
        local.Reset();
        properties.SetBoolValue(5, true);    // kForcePointSampling
        dc.SetTexFiltering(2, 2, 0);
        CHECK(local.Has("TexParameter(0,0,0)") && local.Has("TexParameter(0,1,0)"));
        properties.SetBoolValue(5, false);
        properties.SetBoolValue(6, true);    // kForceBilinear
        local.Reset();
        dc.SetTexFiltering(3, 3, 0);
        CHECK(local.Has("TexParameter(0,0,1)") && local.Has("TexParameter(0,1,7)"));
        properties.SetBoolValue(6, false);

        dc.SetTexEnvMode(2, 0);
        CHECK(((dc.StateWord() >> 25) & 7u) == 2 && local.Has("TexEnv(0,0,2)"));
        CHECK(dc.Stages()[0].combiner.word2 == 0x00010000u);   // only the combiner's high half is forced
        gd::cGDCombiner combiner{7, 8, 9};
        dc.SetTexCombiner(combiner, 0);
        CHECK(local.Last() == "SetCombiner(7,8,9,0)");
        CHECK(((dc.StateWord() >> 25) & 7u) == 4);
        CHECK(dc.Stages()[0].combiner.word0 == 7 && dc.Stages()[0].combiner.word2 == 9);
        dc.SetTexEnvMode(3, 1);
        CHECK(((dc.StateWord() >> 29) & 7u) == 3);
        dc.SetTexCombiner(combiner, 1);
        CHECK(((dc.StateWord() >> 29) & 7u) == 4);

        local.Reset();
        dc.SetTexColor(cS3DVector4{0.25f, 0.5f, 0.75f, 1.0f});
        CHECK(local.Last() == "TexEnvColor(0,1,0.25,0.5,0.75,1)");
        cS3DMatrix4 matrix;
        float* m = &matrix.m[0][0];
        for (int i = 0; i < 16; ++i) m[i] = static_cast<float>(i + 1);
        local.Reset();
        dc.SetTexTransform4(matrix, 1);
        CHECK(local.Has("TexStageMatrix(4,2,1,m)") && local.Has("TexStageCoord(16)"));
        CHECK(dc.Stages()[1].transform && dc.Stages()[1].texMatrix[0] == 1.0f && dc.Stages()[1].texMatrix[15] == 16.0f);
        CHECK(dc.Stages()[0].texMatrix[0] == 1.0f);   // stage 0 does not cache the matrix
        local.Reset();
        dc.ClearTexTransform(1);
        CHECK(local.Has("TexStageMatrix(3,2,1,null)") && !dc.Stages()[1].transform);
        local.Reset();
        dc.ClearTexTransform(1);   // nothing installed: no calls
        CHECK(local.calls.empty());
        dc.SetTexTransform4(matrix, 0);
        CHECK(dc.Stages()[0].transform);
    }

    // ---- fog -------------------------------------------------------------------------------------------
    {
        Driver local;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetFog(true, nullptr, 10.0f, 100.0f);
        CHECK(local.Has("Enable(6)") && local.Has("Fog(0,2)"));
        CHECK(local.Has("Fog(3,10)") && local.Has("Fog(4,100)"));
        local.Reset();
        dc.SetFog(true, nullptr, 10.0f, 100.0f);
        CHECK(local.calls.empty());
        const float color[3] = {0.5f, 0.25f, 0.125f};
        dc.SetFog(false, color, 5.0f, 50.0f);
        CHECK(local.Has("Disable(6)") && local.Has("Fog(1,0.5)") && local.Has("Fog(3,5)") && local.Has("Fog(4,50)"));
        local.Reset();
        dc.SetFog(false, color, 5.0f, 50.0f);
        CHECK(local.calls.empty());
        // FLT_MAX means "leave alone"
        dc.SetFog(false, nullptr, 3.4028235e38f, 3.4028235e38f);
        CHECK(local.calls.empty());
    }

    // ---- lighting / shading / transparency -------------------------------------------------------------
    {
        Driver local;
        FakeLighting lighting;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetLightingManager(&lighting);
        dc.SetLighting(true);
        CHECK(local.Last() == "ColorMultiplier(0.2,0.4,0.6)" && dc.GetLighting());
        local.Reset();
        dc.SetLighting(true);
        CHECK(local.calls.empty());
        dc.SetLighting(false);
        CHECK(local.Last() == "ColorMultiplier(1,1,1)" && !dc.GetLighting());

        FakeInstance instance;
        dc.SetLighting(true);   // ambient = the lighting manager's global light
        lighting.global = cS3DVector3{0.1f, 0.2f, 0.3f};
        dc.SetLighting(false);
        dc.SetLighting(true);
        dc.SetShade(cS3DVector3{0.5f, 0.5f, 0.5f});
        CHECK(local.Last() == "ColorMultiplier(0.05,0.1,0.15)");
        dc.ResetShade();
        CHECK(local.Last() == "ColorMultiplier(0.1,0.2,0.3)");
        local.Reset();
        dc.ResetShade();
        CHECK(local.calls.empty());
        dc.SetSelfLitShade(cS3DVector3{0.25f, 0.5f, 0.75f});
        CHECK(local.Last() == "ColorMultiplier(0.25,0.5,0.75)");
        cS3DVector3 shade{2.0f, 2.0f, 2.0f};
        dc.SetModelShade(&instance, &shade);
        CHECK(local.Last() == "ColorMultiplier(1,0.5,0.25)");
        CHECK(lighting.modelLightCalled && lighting.lastInstance == &instance);
        lighting.modelLightAvailable = false;
        dc.SetModelShade(&instance, nullptr);
        CHECK(local.Last() == "ColorMultiplier(0.1,0.2,0.3)");
        dc.ResetTransparency();
        CHECK(local.Last() == "AlphaMultiplier(1)");
        dc.SetTransparency(0.25f);
        CHECK(local.Last() == "AlphaMultiplier(0.25)");
    }

    // ---- primitive submission --------------------------------------------------------------------------
    {
        Driver local;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        int vertices = 0;
        dc.SetVertexBuffer(10, 4, &vertices);
        CHECK(local.interleaved.size() == 1 && local.interleaved[0].format == 10 && local.interleaved[0].stride == 0);
        CHECK(local.interleaved[0].data == &vertices);
        dc.DrawPrims(6, 10, 4, &vertices);
        CHECK(local.interleaved.back().data == &vertices && local.drawArrays.back()[0] == 6 && local.drawArrays.back()[2] == 4);
        dc.DrawPrims(6, 2, 8);
        CHECK(local.drawArrays.back()[1] == 2 && local.drawArrays.back()[2] == 8);
        std::uint16_t indices[16] = {0, 1, 2, 3};
        dc.SetIndexBuffer(16, indices);
        dc.DrawPrimsIndexed(6, 1, 3);   // "first" is a vertex offset, not an index count
        CHECK(local.drawElements.back().primitive == 6 && local.drawElements.back().count == 3);
        CHECK(local.drawElements.back().type == 3 && local.drawElements.back().indices == indices + 1);
        dc.DrawPrimsIndexed(6, 4, indices);
        CHECK(local.drawElements.back().indices == indices && local.drawElements.back().count == 4);
    }

    // ---- RenderMesh ------------------------------------------------------------------------------------
    {
        Driver local;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        MeshFixture mesh;   // no index group -> DrawArrays
        dc.RenderMesh(mesh.mesh);
        CHECK(local.interleaved.size() == 1 && local.interleaved[0].format == 10);
        CHECK(local.drawArrays.size() == 2);
        CHECK(local.drawArrays[0][0] == 0 && local.drawArrays[0][1] == 0 && local.drawArrays[0][2] == 3);
        CHECK(local.drawArrays[1][1] == 3 && local.drawArrays[1][2] == 6);
        CHECK(local.drawElements.empty());
        local.Reset();
        mesh.vertices.count = 0;   // an empty vertex group draws nothing
        dc.RenderMesh(mesh.mesh);
        CHECK(local.interleaved.empty() && local.drawArrays.empty());
        mesh.vertices.count = 4;
        mesh.WithIndices();
        dc.RenderMesh(mesh.mesh);
        CHECK(local.drawElements.size() == 2);
        CHECK(local.drawElements[0].indices == mesh.indices && local.drawElements[0].count == 3);
        CHECK(local.drawElements[1].indices == mesh.indices + 3 && local.drawElements[1].count == 6);
        dc.DrawPrimsIndexed(4, 2, 1);
        CHECK(local.drawElements.back().indices == mesh.indices + 2);
    }

    // ---- SetModelTransform (both overloads) ------------------------------------------------------------
    {
        Driver local;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        s3d::cS3DCamera camera;
        camera.SetPosition(cS3DVector3{0.0f, 0.0f, 0.0f});
        dc.SetCamera(&camera);
        local.Reset();
        cS3DTransform model;
        model.flags = nS3D::kTransformTranslation;
        model.translation[0] = 4.0f;
        model.translation[1] = 5.0f;
        model.translation[2] = 6.0f;
        dc.SetModelTransform(model);
        CHECK(local.matrices.size() == 1);
        CHECK(Near(local.matrices[0][12], 4.0f) && Near(local.matrices[0][13], 5.0f) && Near(local.matrices[0][14], 6.0f));
        CHECK(dc.ModelTransform().translation[0] == 4.0f && dc.ModelTransform().flags == nS3D::kTransformTranslation);
        CHECK(dc.ModelTransform().changeCounter == 1);
        // Row-major cS3DMatrix4 (translation in m[i][3]); i386 0x0051299a hands the GL driver its transpose.
        cS3DMatrix4 matrix;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) matrix.m[r][c] = r == c ? 1.0f : 0.0f;
        matrix.m[0][1] = 0.5f;
        matrix.m[0][3] = 1.0f;
        matrix.m[1][3] = 2.0f;
        matrix.m[2][3] = 3.0f;
        local.Reset();
        dc.SetModelTransform(matrix);
        CHECK(local.matrices.size() == 1);
        CHECK(Near(local.matrices[0][12], 1.0f) && Near(local.matrices[0][13], 2.0f) && Near(local.matrices[0][14], 3.0f));
        CHECK(Near(local.matrices[0][3], 0.0f) && Near(local.matrices[0][7], 0.0f) && Near(local.matrices[0][11], 0.0f));
        CHECK(Near(local.matrices[0][4], 0.5f) && Near(local.matrices[0][1], 0.0f) && Near(local.matrices[0][15], 1.0f));
        CHECK(dc.ModelTransform().translation[0] == 4.0f);   // the matrix overload keeps the stored transform
    }

    // ---- SetRenderState(cS3DMaterial, SC4DrawAttributes) -----------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.Init();
        MaterialFixture fixture;
        s3d::SC4DrawAttributes attributes;
        attributes.flags = 0;
        attributes.shade[0] = attributes.shade[1] = attributes.shade[2] = attributes.shade[3] = 1.0f;
        local.Reset();
        dc.SetRenderState(fixture.material, &attributes);
        CHECK(local.Has("SetTexture(111,0)") && local.Has("SetTexture(222,1)"));
        CHECK(local.Has("TexStageCoord(5)") && local.Has("TexStageCoord(0)"));
        CHECK(local.Has("SetCombiner(" + I(kSC4DefaultTexCombiner.word0) + "," + I(kSC4DefaultTexCombiner.word1) +
                        ",0,1)"));
        CHECK(local.Has("TexStageMatrix(4,2,1,m)") && dc.Stages()[1].transform);
        CHECK(Near(dc.Stages()[1].texMatrix[0], 0.25f) && Near(dc.Stages()[1].texMatrix[5], -0.25f));
        CHECK((dc.StateWord() & renderstate::kColorWrite) != 0);
        CHECK((dc.StateWord() & renderstate::kDepthWrite) != 0);
        CHECK((dc.StateWord() & renderstate::kCull) != 0);
        CHECK((dc.StateWord() & renderstate::kTexture1) != 0);   // two layers
        // translucency: attribute flag 2 with shade alpha < 1 scales the alpha reference
        attributes.flags = 2;
        attributes.shade[3] = 0.5f;
        local.Reset();
        dc.SetRenderState(fixture.material, &attributes);
        CHECK(local.Has("AlphaMultiplier(0.5)"));
        CHECK(((dc.StateWord() >> 21) & 0xfu) == 8);   // 1.0 * 0.5 * 15 = 7.5 -> 8
        // attribute flag 8 self-lights with the shade alpha and suppresses translucency
        attributes.flags = 8;
        local.Reset();
        dc.SetRenderState(fixture.material, &attributes);
        CHECK(local.Has("ColorMultiplier(0.5,0.5,0.5)"));
        CHECK(((dc.StateWord() >> 13) & 0xfu) == 4 && ((dc.StateWord() >> 17) & 0xfu) == 5);
        // attribute flag 0x20 forces the depth function to 4
        attributes.flags = 0x20;
        dc.SetRenderState(fixture.material, &attributes);
        CHECK(((dc.StateWord() >> 7) & 7u) == 4);
        // attribute flags 0x30 suppress the material's depth flags
        attributes.flags = 0x30;
        dc.SetRenderState(fixture.material, &attributes);
        CHECK((dc.StateWord() & renderstate::kDepthWrite) == 0 && (dc.StateWord() & renderstate::kDepthTest) == 0);
    }

    // ---- highlight render states -----------------------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.Init();
        local.Reset();
        dc.SetRenderStateHighlight(2);
        CHECK(local.Has("TexEnvColor(0,1,1,0,0,1)"));
        CHECK(local.Has("SetCombiner(" + I(kSC4HighlightTexCombiner.word0) + "," + I(kSC4HighlightTexCombiner.word1) + ",0,0)"));
        CHECK(((dc.StateWord() >> 25) & 7u) == 4);
        CHECK(dc.Stages()[0].combiner.word0 == kSC4HighlightTexCombiner.word0);
        MaterialFixture fixture;
        s3d::SC4DrawAttributes attributes;
        attributes.flags = 2u << 8;   // highlight type 2
        dc.SetRenderStateHighlight(fixture.material, &attributes);
        CHECK(local.Has("TexEnvColor(0,1,1,0,0,1)"));
        CHECK(local.Has("SetTexture(111,0)") && local.Has("TexStageCoord(5)"));
    }

    // ---- RenderModelInstance ---------------------------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        FakeLighting lighting;
        FakeViewUtilities view;
        FakeMeshBuilderHost meshHost;
        SetMeshBuilderHost(&meshHost);
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.SetLightingManager(&lighting);
        dc.SetViewUtilities(&view);
        dc.Init();
        s3d::cS3DCamera camera;
        dc.SetCamera(&camera);

        FakeInstance instance;
        instance.transform = cS3DTransform{};
        instance.bounds.min[0] = instance.bounds.min[1] = instance.bounds.min[2] = -2.0f;
        instance.bounds.max[0] = instance.bounds.max[1] = instance.bounds.max[2] = 2.0f;
        instance.attributes.flags = 0;
        instance.attributes.shade[0] = 0.5f;
        instance.attributes.shade[1] = 0.25f;
        instance.attributes.shade[2] = 0.125f;
        instance.attributes.shade[3] = 1.0f;

        MeshFixture fixture;   // the mesh records below point into this fixture, so it must outlive them
        s3d::cS3DMeshAndMaterial meshes;
        meshes.Meshes().resize(2);
        meshes.Materials().resize(2);
        {
            s3d::cS3DMaterial material;
            std::memset(&material, 0, sizeof material);
            material.flags = 0x100 | 0x02 | 0x01;
            material.layerCount = 1;
            for (int i = 0; i < 2; ++i) {
                std::memcpy(meshes.Material(i).data(), &material, sizeof material);
                std::memcpy(meshes.Mesh(i).data(), &fixture.mesh, sizeof(SC4Mesh));
            }
        }

        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, false);
        CHECK(local.matrices.size() == 1);
        CHECK(local.Has("ColorMultiplier(0.25,0.0625,0.015625)"));   // model light * shade
        CHECK(local.interleaved.size() == 2 && local.drawArrays.size() == 4);
        CHECK(local.Has("AlphaMultiplier(1)"));

        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, true);
        CHECK(local.Has("TexEnvColor(0,1,1,1,1,1)"));   // highlight type 0 is white
        CHECK(local.Has("ColorMultiplier(0.5,0.25,0.125)"));   // self-lit with the attribute shade
        instance.attributes.flags = 8;   // attribute flag 8: invisible in the highlight pass
        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, true);
        CHECK(local.calls.empty());
        instance.attributes.flags = 0;

        // culling by screen size: 4 world units * 1 pixel/unit = 4 < 6 (kRenderModelThreshold)
        properties.SetBoolValue(30, true);
        view.SetPixelsPerWorld(1.0f);
        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, false);
        CHECK(local.matrices.empty() && local.drawArrays.empty());
        view.SetPixelsPerWorld(4.0f);
        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, false);
        CHECK(local.drawArrays.size() == 4);
        properties.SetBoolValue(30, false);

        // second-stage emulation: kUseSecondStage off and a two-layer material
        {
            s3d::cS3DMaterial twoLayer;
            std::memset(&twoLayer, 0, sizeof twoLayer);
            twoLayer.flags = 0x100 | 0x02 | 0x01 | 0x20;
            twoLayer.layerCount = 2;
            twoLayer.layers[0].sampler[0] = 1;
            twoLayer.layers[0].sampler[1] = 1;
            twoLayer.layers[1].sampler[0] = 2;
            twoLayer.layers[1].sampler[1] = 2;
            twoLayer.layers[0].used = 1;
            twoLayer.layers[1].used = 1;
            for (int i = 0; i < 2; ++i) std::memcpy(meshes.Material(i).data(), &twoLayer, sizeof twoLayer);
            properties.SetBoolValue(11, false);
            local.Reset();
            dc.RenderModelInstance(&instance, &meshes, &instance.attributes, false);
            CHECK(local.Has("DepthFunc(2)"));   // cached as 1, the driver is told 2 (quirk)
            CHECK(local.Count("DepthFunc(2)") == 2);
            CHECK(local.drawArrays.size() == 8);   // two meshes, each drawn twice
            properties.SetBoolValue(11, true);
        }

        // debug bounding boxes: world bounds in red, then the owner occupant's box in blue
        properties.SetBoolValue(26, true);
        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, false);
        CHECK(meshHost.builder.boxes.size() == 1);   // no owner: only the world-bounds box
        CHECK(meshHost.builder.color.x == 1.0f && meshHost.builder.color.z == 0.0f);
        CHECK(Near(meshHost.builder.direction.x, 0.25f) && Near(meshHost.builder.direction.y, 0.5f) &&
              Near(meshHost.builder.direction.z, 1.0f));
        CHECK(Near(meshHost.builder.boxes[0].corners[0].x, -2.0f));
        FakeOccupant occupant;
        occupant.bounds.min[0] = occupant.bounds.min[1] = occupant.bounds.min[2] = -1.0f;
        occupant.bounds.max[0] = occupant.bounds.max[1] = occupant.bounds.max[2] = 1.0f;
        instance.owner = &occupant;
        local.Reset();
        dc.RenderModelInstance(&instance, &meshes, &instance.attributes, false);
        CHECK(meshHost.builder.boxes.size() == 3);   // the world box again, then the occupant box
        CHECK(meshHost.builder.color.z == 1.0f && meshHost.builder.color.w == 0.5f);
        CHECK(Near(meshHost.builder.boxes[2].corners[0].x, -1.0f));
        instance.owner = nullptr;
        properties.SetBoolValue(26, false);
        SetMeshBuilderHost(nullptr);
    }

    // ---- DrawBoundingBox -------------------------------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        dc.Init();
        FakeMeshBuilderHost host;
        SetMeshBuilderHost(&host);
        cS3DBoundingBox box;
        box.min[0] = box.min[1] = box.min[2] = -1.0f;
        box.max[0] = box.max[1] = box.max[2] = 1.0f;
        dc.DrawBoundingBox(box, cS3DVector4{0.5f, 0.75f, 1.0f, 1.0f});
        CHECK(host.builder.clears == 1 && host.builder.boxes.size() == 1);
        CHECK(host.builder.color.x == 0.5f && host.builder.color.y == 0.75f && host.builder.color.w == 1.0f);
        CHECK(host.builder.directionSet);
        CHECK(Near(host.builder.boxes[0].corners[0].x, -1.0f) && Near(host.builder.boxes[0].corners[7].z, 1.0f));
        CHECK(local.interleaved.size() == 1 && local.drawArrays.size() == 1);
        CHECK(local.drawArrays[0][2] == 36);
        cS3DBoundingBox inverted;
        inverted.min[0] = 1.0f;
        inverted.max[0] = -1.0f;
        local.Reset();
        dc.DrawBoundingBox(inverted, cS3DVector4{1, 1, 1, 1});
        CHECK(local.calls.empty() && host.builder.boxes.size() == 1);
        SetMeshBuilderHost(nullptr);
        local.Reset();
        dc.DrawBoundingBox(box, cS3DVector4{1, 1, 1, 1});   // no host: nothing happens
        CHECK(local.calls.empty());
    }

    // ---- SetDepthOffset --------------------------------------------------------------------------------
    {
        Driver local;
        sc4::cSC4RenderProperties properties;
        SC4DrawContext dc;
        dc.SetDriver(&local);
        dc.SetRenderProperties(&properties);
        s3d::cS3DCamera camera;
        camera.SetOrtho(-1, 1, 1, -1, 0.1f, 10.0f);
        dc.SetCamera(&camera);
        const float before = camera.ProjectionMatrix()[11];
        local.Reset();
        dc.SetDepthOffset(3);   // kDepthOffset defaults to 2: the camera is told 3 * 2 / 65536
        CHECK(local.Has("MatrixMode(1)") && local.matrices.size() == 1);
        const float after = camera.ProjectionMatrix()[11];
        CHECK(std::fabs((after - before) - (-6.0f / 65536.0f)) < 1e-6f);
        bool transposed = true;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                if (local.matrices[0][row * 4 + col] != camera.ProjectionMatrix()[col * 4 + row]) transposed = false;
        CHECK(transposed);
        local.Reset();
        dc.SetDepthOffset(3);
        CHECK(local.calls.empty());
        properties.SetIntValue(16, 5);
        dc.SetDepthOffset(4);
        CHECK(local.matrices.size() == 1);
        CHECK(std::fabs((camera.ProjectionMatrix()[11] - before) - (-(4 * 5) / 65536.0f)) < 1e-6f);
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::puts("OK");
    return 0;
}
