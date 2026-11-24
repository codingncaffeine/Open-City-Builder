// SC4TrainJunctionView (Mac i386 0x004b3826..0x004b7d54): the cell vectors, the arrow textures and lever models Init takes,
// the arrows per junction (the other paths dimmer, each lifted 0.01 over the last), the arrow texture by the turn, the
// lever above the switched path's midpoint, the brighter cells under the pointer and last clicked.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/DrawContext.h"
#include "sc4/PathInfo.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrainJunctionView.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { const float va_ = (a), vb_ = (b); if (!(std::fabs(va_ - vb_) <= 1e-3f)) { \
    std::printf("FAIL line %d: %s = %g, expected %g\n", __LINE__, #a, double(va_), double(vb_)); ++failures; } } while (0)

using namespace sc4;

namespace {
struct DrawCall {
    enum Kind { Texture, Draw, State, Transparency, ResetTransparency, ModelTransform, ResetModelView, ClearTex } kind;
    int value = 0;
    float alpha = 0.0f;
    std::uint32_t state = 0;
    TrainJunctionVertex vertices[4];
    cS3DTransform transform;
};
std::vector<DrawCall> g_calls;
void Record(DrawCall::Kind kind) { g_calls.push_back(DrawCall{kind}); }
int Count(DrawCall::Kind kind) {
    int n = 0;
    for (const DrawCall& call : g_calls) n += call.kind == kind;
    return n;
}
}  // namespace

SC4DrawContext::SC4DrawContext() {}
SC4DrawContext::~SC4DrawContext() {}
void SC4DrawContext::SetTexture(int texture, int) {
    Record(DrawCall::Texture);
    g_calls.back().value = texture;
}
void SC4DrawContext::DrawPrims(int primitive, int format, int count, const void* vertices) {
    Record(DrawCall::Draw);
    g_calls.back().value = primitive * 100 + format * 10 + count;
    std::memcpy(g_calls.back().vertices, vertices, sizeof(TrainJunctionVertex) * 4);
}
void SC4DrawContext::SetRenderState(const cSC4PackedRenderState& state) {
    Record(DrawCall::State);
    g_calls.back().state = state.value;
}
void SC4DrawContext::SetTransparency(float alpha) {
    Record(DrawCall::Transparency);
    g_calls.back().alpha = alpha;
}
void SC4DrawContext::ResetTransparency() { Record(DrawCall::ResetTransparency); }
void SC4DrawContext::SetModelTransform(const cS3DTransform& transform) {
    Record(DrawCall::ModelTransform);
    g_calls.back().transform = transform;
}
void SC4DrawContext::ResetModelViewTransform() { Record(DrawCall::ResetModelView); }
void SC4DrawContext::ClearTexTransform(int) { Record(DrawCall::ClearTex); }


namespace {

struct FakeModel final : s3d::cIS3DModelInstanceAnimated {
    int refs = 1;
    cIGZUnknown* owner = nullptr;
    int properties = 0, draws = 0, zoom = -1, rotation = -1;
    float rate = 0.0f, opacity = -1.0f, lastUpdate = 0.0f;
    bool translucent = true, selfLit = false;
    cS3DTransform transform{};
    s3d::SC4DrawAttributes attributes{};
    cS3DBoundingBox world{{0.0f, 2.0f, 0.0f}, {1.0f, 8.0f, 1.0f}};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(--refs); }   // the test owns the object
    cIGZUnknown* GetOwner() override { return owner; }
    void SetOwner(cIGZUnknown* value) override { owner = value; }
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox&) override {}
    void GetWorldBounds(cS3DBoundingBox& out) override { out = world; }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& value) override { transform = value; }
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { ++draws; return 1; }
    int GetProperties() override { return properties; }
    void SetProperties(int value) override { properties = value; }
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return attributes; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes& value) override { attributes = value; }
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
    s3d::cIS3DModelInstance* AsModelInstance() override { return this; }
    int SetPosition(const cS3DVector3&) override { return 1; }
    int SetOrientation(const cS3DVector3&) override { return 1; }
    int SetOrientation(const cS3DVector3&, const cS3DVector3&) override { return 1; }
    void ModelPointToWorldPoint(cS3DVector3&) override {}
    void ModelPointToWorldPoint(float*) override {}
    void ModelVectorToWorldVector(cS3DVector3&) override {}
    void ModelVectorToWorldVector(float*) override {}
    int SetZoomAndRotation(int z, int r) override { zoom = z; rotation = r; return 1; }
    int SetFrameset(int) override { return 1; }
    int SetFrame(int) override { return 1; }
    int GetFrame() const override { return 0; }
    int GetFrameCount() const override { return 1; }
    int IncrementFrame() override { return 1; }
    int SetAutoFrameRate(float value) override { rate = value; return 1; }
    int DoAutoFrameUpdate(float elapsed) override { lastUpdate = elapsed; return 1; }
    void SetPlayMode(int) override {}
    const void* GetAnimInfo() const override { return nullptr; }
    int SetOpacity(bool value, float alpha) override { translucent = value; opacity = alpha; return 1; }
    int SetHighlight(int) override { return 1; }
    int SetSelfIlluminated(bool on) override { selfLit = on; return 1; }
};

struct FakeBinding final : IPylonTextureBinding {
    explicit FakeBinding(std::uint32_t value) : id(value) {}
    std::uint32_t TextureId() const override { return id; }
    void Release() override { ++released; }
    std::uint32_t id;
    int released = 0;
};

struct Host final : ITrainJunctionViewHost {
    std::vector<std::unique_ptr<FakeBinding>> bindings;
    FakeModel lever, leverClicked;
    std::map<std::pair<int, int>, ITrafficNetworkMapOccupant*> cells;
    std::vector<std::uint32_t> keys;
    std::uint16_t current = 0;
    cSC4PathInfo* pathInfo = nullptr;
    float terrain = 0.0f;
    bool RendererPresent() override { return true; }
    void RendererGetDrawContext() override {}
    IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) override {
        bindings.push_back(std::make_unique<FakeBinding>(instance));
        return bindings.back().get();
    }
    bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key, s3d::cIS3DModelInstanceAnimated** out) override {
        *out = key.instance == 0x8bb70000 ? &lever : &leverClicked;
        (*out)->AddRef();
        return true;
    }
    bool VisibleCellRect(int rect[4]) override {
        rect[0] = 0; rect[1] = 0; rect[2] = 3; rect[3] = 3;
        return true;
    }
    void CellOccupants(int x, int z, std::vector<ITrafficNetworkMapOccupant*>& out) override {
        auto it = cells.find({x, z});
        if (it != cells.end()) out.push_back(it->second);
    }
    std::size_t TrainJunctionKeys(ITrafficNetworkMapOccupant*, std::uint8_t pathType, std::vector<std::uint32_t>& out,
                                  std::uint16_t& cursor) override {
        if (pathType != 1) return 0;
        out = keys;
        cursor = current;
        return keys.size();
    }
    const cISC4PathInfo* PathInfo(ITrafficNetworkMapOccupant*) override { return pathInfo; }
    float TerrainAltitude(float, float) override { return terrain; }
};

void AddPath(cSC4PathInfo& info, std::uint32_t key, std::vector<cS3DVector3> points) {
    tPath* path = info.NewPath(key);
    path->points = std::move(points);
}

}  // namespace

int main() {
    Host host;
    SetTrainJunctionViewHost(&host);
    auto* view = new SC4TrainJunctionView();
    view->AddRef();
    CHECK(view->Init() && view->Initialized());
    // the cell vectors: edge 0 (west, z 8) to edge 2 (east) is +x; to edge 1 (north, x 8, z 0) it is (1, 0, -1) / sqrt 2;
    // the diagonal is the zero vector
    CHECK_NEAR(SC4TrainJunctionView::CellVector(0, 2).x, 1.0f);
    CHECK_NEAR(SC4TrainJunctionView::CellVector(0, 1).z, -0.70710677f);
    CHECK_NEAR(SC4TrainJunctionView::CellVector(3, 3).x, 0.0f);
    CHECK(host.bindings.size() == 6 && host.bindings[0]->id == 0x6be09921 && host.bindings[5]->id == 0x6be09926);
    CHECK(view->LeverModel(false) == &host.lever && view->LeverModel(true) == &host.leverClicked);

    // one junction at cell (1, 2): rail (type 1) from the west edge, straight on to the east (switched to) or turning
    // to the north edge
    auto* info = new cSC4PathInfo();
    info->AddRef();
    const std::uint32_t straight = MakePathKey(1, 0, 2), turn = MakePathKey(1, 0, 1);
    AddPath(*info, straight, {{16, 0, 40}, {24, 0, 40}, {32, 0, 40}});
    AddPath(*info, turn, {{16, 0, 40}, {20, 0, 39}, {23, 0, 37}, {24, 0, 34}, {24, 0, 32}});
    host.pathInfo = info;
    host.cells[{1, 2}] = reinterpret_cast<ITrafficNetworkMapOccupant*>(&host);   // an opaque handle
    host.keys = {straight, turn};
    host.current = 0;
    host.terrain = -5.0f;

    SC4DrawContext context;
    view->DrawViewObject(context);
    // the turn's arrow first at 0.4 (not the pointer's cell), then the switched path's at 0.5, two arrows in all
    CHECK(Count(DrawCall::Draw) == 2 && Count(DrawCall::State) == 1);
    std::vector<float> alphas;
    std::vector<int> textures;
    const DrawCall* firstQuad = nullptr;
    for (const DrawCall& call : g_calls) {
        if (call.kind == DrawCall::Transparency) alphas.push_back(call.alpha);
        if (call.kind == DrawCall::Texture) textures.push_back(call.value);
        if (call.kind == DrawCall::Draw && !firstQuad) firstQuad = &call;
    }
    CHECK(alphas.size() == 2 && alphas[0] == 0.4f && alphas[1] == 0.5f);
    // the turn bends toward -z against the cell vector: cross(v, dir).y = v.z * dir.x - v.x * dir.z > 0.25 -> +0x2c
    // (unselected); the straight path is the straight arrow, selected: +0x40
    CHECK(textures.size() == 2 && textures[0] == 0x6be09921 && textures[1] == 0x6be09926);
    if (firstQuad) {
        CHECK(firstQuad->value == 6 * 100 + 2 * 10 + 4);
        // from the straight path's midpoint (24, 0, 40) along the turn's cell vector (1, 0, -1) / sqrt 2: 12 long, 12 wide
        // (half to each side), above the terrain (-5) at the midpoint's height plus the first lift (0.1)
        const float s = 12.0f * 0.70710677f;
        CHECK_NEAR(firstQuad->vertices[0].x, 24.0f + 0.5f * s);
        CHECK_NEAR(firstQuad->vertices[0].z, 40.0f + 0.5f * s);
        CHECK_NEAR(firstQuad->vertices[0].y, 0.1f);
        CHECK_NEAR(firstQuad->vertices[1].x, 24.0f + 0.5f * s + s);
        CHECK_NEAR(firstQuad->vertices[1].z, 40.0f + 0.5f * s - s);
        CHECK(firstQuad->vertices[1].v == 1.0f && firstQuad->vertices[2].u == 1.0f && firstQuad->vertices[3].v == 0.0f);
    }
    CHECK_NEAR(view->Lift(), 0.12f);   // 0.1 + two arrows
    // the lever: above the midpoint by 0.2 + the lift, facing along the first key's cell vector, at 0.5, drawn once
    CHECK(host.lever.draws == 1 && host.leverClicked.draws == 0 && host.lever.opacity == 0.5f && host.lever.translucent);
    CHECK_NEAR(host.lever.transform.translation[0], 24.0f);
    CHECK_NEAR(host.lever.transform.translation[1], 0.2f + 0.12f);
    CHECK_NEAR(host.lever.transform.translation[2], 40.0f);
    CHECK((host.lever.transform.flags & 5) == 5);

    // the pointer over the cell: the arrows at 0.7, the lever at 0.8; a click there: the other lever
    g_calls.clear();
    view->SetHighlightCell(1, 2);
    view->DrawViewObject(context);
    alphas.clear();
    for (const DrawCall& call : g_calls) if (call.kind == DrawCall::Transparency) alphas.push_back(call.alpha);
    CHECK(alphas.size() == 2 && alphas[0] == 0.7f && alphas[1] == 0.8f);
    CHECK_NEAR(view->Lift(), 0.12f);   // reset every Update
    view->SetMouseDownCell(1, 2);
    view->DrawViewObject(context);
    CHECK(host.leverClicked.draws == 1 && host.leverClicked.opacity == 0.8f);

    // a monorail junction (type 7) lifts arrows and lever by 3
    g_calls.clear();
    auto* mono = new cSC4PathInfo();
    mono->AddRef();
    const std::uint32_t monoKey = MakePathKey(7, 0, 2);
    AddPath(*mono, monoKey, {{16, 10, 40}, {32, 10, 40}});
    host.pathInfo = mono;
    host.keys = {monoKey};
    view->SetHighlightCell(-1, -1);
    view->SetMouseDownCell(-1, -1);
    view->DrawViewObject(context);
    for (const DrawCall& call : g_calls)
        if (call.kind == DrawCall::Draw) CHECK_NEAR(call.vertices[0].y, 10.0f + 0.1f + 3.0f);
    CHECK_NEAR(host.lever.transform.translation[1], 10.0f + 0.2f + 0.11f + 3.0f);

    // Shutdown gives back the textures and models
    view->Shutdown();
    CHECK(!view->Initialized() && host.bindings[0]->released == 1 && host.lever.refs == 1 && host.leverClicked.refs == 1);
    view->Release();
    info->Release();
    mono->Release();
    std::puts(failures ? "sc4_trainjunctionview: FAILED" : "sc4_trainjunctionview: ok");
    return failures != 0;
}
