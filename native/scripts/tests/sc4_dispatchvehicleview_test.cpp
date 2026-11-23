// cSC4DispatchVehicleView: Init (icon bindings, maxima, render states, the rebuild message), tracking and untracking (the
// per-type counts and maxima, the shared handle counter, the automata flag, the occupant references), the top-priority flag
// 0x800 and the hidden types, the call number / picture quads, fading in and out (removal by Update), flashing, locking,
// the selectable CSI indicator (SetActiveIndicator / FindIndicatorHandle / the button state columns), the billboard and the
// 2-D arrow draws, the clustered indicators round a building, the window clip list and the nudge off an overlapping window,
// the player-drive 3-D arrow and Shutdown — driven through IDispatchVehicleViewHost fakes and a recording draw context
// (DrawContext.cpp is not linked: the calls the view makes are stubbed below).
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "gz/Buffer.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/DispatchVehicleView.h"
#include "sc4/DrawContext.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { const float va_ = (a), vb_ = (b); if (!(std::fabs(va_ - vb_) <= 1e-3f)) { \
    std::printf("FAIL line %d: %s = %g, expected %g\n", __LINE__, #a, double(va_), double(vb_)); ++failures; } } while (0)

using namespace sc4;

// ---- the recording draw context -------------------------------------------------------------------------------------------
namespace {
struct DrawCall {
    enum Kind { Texture, Draw, State, Transparency, ResetTransparency, ModelTransform, ResetModelView, ClearTex } kind;
    int value = 0;
    float alpha = 0.0f;
    std::uint32_t state = 0;
    DispatchViewVertex vertices[4];
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
    std::memcpy(g_calls.back().vertices, vertices, sizeof(DispatchViewVertex) * 4);
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

// ---- fakes ----------------------------------------------------------------------------------------------------------------
struct FakeBinding final : IPylonTextureBinding {
    std::uint32_t id = 0;
    int releases = 0;
    std::uint32_t TextureId() const override { return id; }
    void Release() override { ++releases; }
};

struct FakeFont final : IPylonFont {
    int addRefs = 0, releases = 0, size = 7, drawTexts = 0;
    std::vector<int> sizes;
    int textWidth = 20, textHeight = 10;
    void AddRef() override { ++addRefs; }
    void Release() override { ++releases; }
    int GetFontSize() override { return size; }
    void SetFontSize(int value) override {
        size = value;
        sizes.push_back(value);
    }
    int TextHeight(const char*, std::uint32_t) override { return textHeight; }
    int TextWidth(const char*, std::uint32_t) override { return textWidth; }
    void DrawText(cIGZBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const void*, int) override { ++drawTexts; }
};

struct FakeMessages final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeMessage final : cIGZMessage2 {
    std::uint32_t type = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
};

class FakeOccupant final : public cISC4Occupant {
public:
    FakeOccupant(std::uint32_t occupantType, float x, float y, float z) : type(occupantType), position{x, y, z} {}
    std::uint32_t type;
    float position[3];
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
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

struct FakeAutomaton final : IDispatchViewAutomaton {
    int queries = 0, releases = 0;
    std::vector<std::pair<std::uint32_t, bool>> flagSets;
    bool removed = false, wrecked = false, hasState = false, hasParams = false;
    std::uint32_t stateValue = 0;
    float limit = 0.0f;
    void Release() override { ++releases; }
    void SetAutomataFlag(std::uint32_t flag, bool on) override { flagSets.emplace_back(flag, on); }
    bool CheckAutomataFlag(std::uint32_t flag) override { return flag == 1 ? removed : flag == 0x8000000 ? wrecked : false; }
    bool StateAndGroup(std::uint32_t& value) override {
        value = stateValue;
        return hasState;
    }
    bool PlayerDriveParam5c(float& value) override {
        value = limit;
        return hasParams;
    }
};

// the player-drive arrow (the same shape as sc4_advisoruiimagewinproc_test's head model)
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

cIGZBuffer* NewBuffer(std::uint32_t width, std::uint32_t height) {
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    buffer->Init(width, height, cGZBufferColorType{kBufferColorARGB8888, 0x20});
    return buffer;
}

struct Host final : IDispatchVehicleViewHost {
    FakeMessages messages;
    FakeFont font;
    std::vector<std::unique_ptr<FakeBinding>> bindings;
    std::vector<std::uint32_t> bindingInstances;
    std::vector<std::uint32_t> released;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> uploads;   // buffer width, height
    std::uint32_t nextTexture = 500;
    std::vector<std::string> bitmapFiles;
    std::map<const cISC4Occupant*, FakeAutomaton*> automata;
    FakeModel* model = nullptr;
    int modelRequests = 0;
    cGZPersistResourceKey modelKey;
    int zoom = 1;
    float elapsed = 0.0f;
    int restarts = 0, camera2D = 0, camera3D = 0;
    bool snapshot = false, emergency = false;
    std::uint32_t viewLevel = 1;
    float seaLevel = 5.0f;
    std::vector<DispatchViewChildWindow> children;

    cIGZMessageServer2* Messages() override { return &messages; }
    bool RendererPresent() override { return true; }
    void RendererGetDrawContext() override {}
    IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) override {
        bindings.push_back(std::make_unique<FakeBinding>());
        bindings.back()->id = 100 + static_cast<std::uint32_t>(bindings.size());
        bindingInstances.push_back(instance);
        return bindings.back().get();
    }
    void ReleaseTexture(std::uint32_t texture) override { released.push_back(texture); }
    bool UploadTexture(cIGZBuffer* buffer, std::uint32_t& texture) override {
        uploads.emplace_back(buffer->Width(), buffer->Height());
        texture = ++nextTexture;
        return true;
    }
    bool CreateBuffer(cIGZBuffer** out) override {
        auto* buffer = new cGZBuffer();   // the view initialises it
        buffer->AddRef();
        *out = buffer;
        return true;
    }
    IPylonFont* CallNumberFont() override { return &font; }
    bool LoadBitmapFromFile(const cIGZString& path, cIGZBuffer**) override {
        bitmapFiles.emplace_back(path.ToChar(), path.Strlen());
        return false;
    }
    bool LoadPictureBitmap(std::uint32_t id, cIGZBuffer** out) override {
        if (id != 77) return false;
        *out = NewBuffer(128, 32);
        return true;
    }
    void GetZoomAndRotation(int& z, int& r) override {
        z = zoom;
        r = 2;
    }
    float ViewportUnitsToWorldUnits(float units) override { return units * 0.5f; }
    float ViewPitchAngle() override { return 1.0471976f; }   // 60 degrees: cos = 0.5
    void ViewDirection(float& x, float& z) override {
        x = 0.0f;
        z = -1.0f;   // atan2(-0, 1): the billboard keeps its axes
    }
    // screen x = world x, screen y = world z - world y
    void ProjectPointToViewportCoordinates2D(float* point) override {
        const float x = point[0], y = point[1], z = point[2];
        point[0] = x;
        point[1] = z - y;
        point[2] = 0.0f;
    }
    float StopTickTimer() override { return elapsed; }
    void RestartTickTimer() override { ++restarts; }
    bool RenderSnapshot(SC4DrawContext&) override { return snapshot; }
    std::uint32_t CurrentViewLevel() override { return viewLevel; }
    bool SimulatorEmergencyPaused() override { return emergency; }
    bool View3DWinArea(cRZRect& out) override {
        out = cRZRect{0, 0, 800, 600};
        return true;
    }
    void View3DChildWindows(std::vector<DispatchViewChildWindow>& out) override { out = children; }
    bool TerrainLocationInBounds(float, float) override { return false; }
    float TerrainAltitude(float, float) override { return 1000.0f; }
    float TerrainSeaLevel() override { return seaLevel; }
    void Set2DCameraView(SC4DrawContext&) override { ++camera2D; }
    void Set3DCameraView(SC4DrawContext&) override { ++camera3D; }
    IDispatchViewAutomaton* QueryAutomaton(cISC4Occupant* occupant) override {
        auto it = automata.find(occupant);
        if (it == automata.end()) return nullptr;
        ++it->second->queries;
        return it->second;
    }
    bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key, s3d::cIS3DModelInstanceAnimated** out) override {
        ++modelRequests;
        modelKey = key;
        *out = model;
        if (model) model->AddRef();
        return model != nullptr;
    }
    cISC4LightingManager* LightingManager() override { return nullptr; }
};

const cSC4DispatchVehicleView::TrackInfo& Info(cSC4DispatchVehicleView* view, int handle) {
    static const cSC4DispatchVehicleView::TrackInfo none;
    const cSC4DispatchVehicleView::TrackInfo* info = view->FindTrackInfo(static_cast<std::uint32_t>(handle));
    return info ? *info : none;
}

void ExpectVertex(const DispatchViewVertex& v, float x, float y, float z, float u, float tv, int line) {
    if (std::fabs(v.x - x) > 1e-3f || std::fabs(v.y - y) > 1e-3f || std::fabs(v.z - z) > 1e-3f || std::fabs(v.u - u) > 1e-3f ||
        std::fabs(v.v - tv) > 1e-3f) {
        std::printf("FAIL line %d: vertex (%g %g %g %g %g), expected (%g %g %g %g %g)\n", line, double(v.x), double(v.y), double(v.z),
                    double(v.u), double(v.v), double(x), double(y), double(z), double(u), double(tv));
        ++failures;
    }
}

}  // namespace

int main() {
    Host host;
    SetDispatchVehicleViewHost(&host);
    SC4DrawContext context;

    // ---- construction, interfaces, Init -----------------------------------------------------------------------------------
    auto* view = new cSC4DispatchVehicleView();
    view->AddRef();
    CHECK(!view->IsInitialized());
    CHECK(view->IndicatorMax(0) == 0 && view->TrackedCount(6) == 0 && view->ActiveHandle() == 0);
    void* out = nullptr;
    CHECK(view->QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(view));
    view->Release();
    CHECK(!view->QueryInterface(0x12345678, &out) && out == nullptr);

    CHECK(view->Init());
    CHECK(view->IsInitialized());
    CHECK(host.bindings.size() == 7);
    for (int slot = 0; slot < 7 && slot < static_cast<int>(host.bindingInstances.size()); ++slot) {
        CHECK(host.bindingInstances[slot] == kDispatchViewIconInstance[slot]);
        CHECK(view->IconBinding(slot) == host.bindings[slot].get());
    }
    for (int type = 0; type < kDispatchViewTrackerTypes; ++type) CHECK(view->IndicatorMax(type) == 0xffffffffu);
    CHECK(host.messages.added.size() == 1 && host.messages.added[0].second == kDispatchViewMessageRebuild &&
          host.messages.added[0].first == static_cast<cIGZMessageTarget2*>(view));
    {
        cSC4PackedRenderState full, faint, third;
        full.PackRenderState(true, true, true, true, true, 1, true, 3, 4, 5, 4, 0.0f, 1, -1);
        faint.PackRenderState(true, false, false, true, true, 1, true, 3, 4, 5, 4, 0.0f, 1, -1);
        third.PackRenderState(true, false, false, false, false, 1, true, -1, -1, -1, -1, -1.0f, 1, -1);
        CHECK(view->RenderState(0).value == full.value && view->RenderState(0).mask == full.mask);
        CHECK(view->RenderState(1).value == faint.value && view->RenderState(1).mask == faint.mask);
        CHECK(view->RenderState(2).value == third.value && view->RenderState(2).mask == third.mask);
        CHECK(full.value != faint.value);
    }
    CHECK(view->Init());   // a second Init does nothing
    CHECK(host.bindings.size() == 7);

    // ---- a fire vehicle: the call number quad --------------------------------------------------------------------------
    FakeOccupant truck(kDispatchViewAutomatonTypeA, 100.0f, 0.0f, 300.0f);
    FakeAutomaton truckAutomaton;
    host.automata[&truck] = &truckAutomaton;
    CHECK(view->TrackOccupant(nullptr, 0, 1, nullptr) == 0);
    const int fire = view->TrackOccupant(&truck, kTrackerFire, 42, nullptr);
    CHECK(fire > 0);
    CHECK(view->TrackedCount(kTrackerFire) == 1);
    CHECK(truck.refs == 1);
    CHECK(truckAutomaton.flagSets.size() == 1 && truckAutomaton.flagSets[0] == std::make_pair(kDispatchViewTrackedAutomataFlag, true));
    CHECK(truckAutomaton.releases == truckAutomaton.queries);
    {
        const auto& info = Info(view, fire);
        CHECK(info.handle == static_cast<std::uint32_t>(fire) && info.type == kTrackerFire && info.occupant == &truck);
        CHECK(info.flags == (0x2au | 0x800u));
        CHECK(info.callNumber == 42 && info.pictureId == 0 && info.binding == view->IconBinding(0));
        CHECK(info.alpha == 0.0f && info.flashPeriod == 1.0f && info.buttonState == 0);
        CHECK(info.texture == host.nextTexture);
        CHECK(host.uploads.size() == 1 && host.uploads[0] == std::make_pair(32u, 32u));   // 20 x 10 digits fit 32
        CHECK(host.font.drawTexts == 1 && host.font.addRefs == 1 && host.font.releases == 1);
        CHECK(host.font.sizes.size() == 2 && host.font.sizes[0] == 3 && host.font.sizes[1] == 7);   // size 3, then restored
        CHECK(info.width == 20.0f && info.height == 14.0f);
        // zoom 1: kCallNumberYOffset 30 viewport units = 15 world units / cos 60 = 30; the icon is 32 wide and 64 high, the
        // digits 5 x 5 world units centred on the icon (62), u up to 20 / 32 and v up to 10 / 32
        ExpectVertex(info.numberQuad[0], -5.0f, 67.0f, 0.3f, 0.0f, 0.0f, __LINE__);
        ExpectVertex(info.numberQuad[1], -5.0f, 57.0f, 0.3f, 0.0f, 0.3125f, __LINE__);
        ExpectVertex(info.numberQuad[2], 5.0f, 57.0f, 0.3f, 0.625f, 0.3125f, __LINE__);
        ExpectVertex(info.numberQuad[3], 5.0f, 67.0f, 0.3f, 0.625f, 0.0f, __LINE__);
        ExpectVertex(info.iconQuad[0], -16.0f, 94.0f, 0.0f, 0.0f, 0.0f, __LINE__);
        ExpectVertex(info.iconQuad[1], -16.0f, 30.0f, 0.0f, 0.0f, 1.0f, __LINE__);
        ExpectVertex(info.iconQuad[2], 16.0f, 30.0f, 0.0f, 1.0f, 1.0f, __LINE__);
        ExpectVertex(info.iconQuad[3], 16.0f, 94.0f, 0.0f, 1.0f, 0.0f, __LINE__);
    }
    // a wider number needs a bigger texture
    host.font.textWidth = 40;
    view->HandleViewChanged();
    CHECK(host.uploads.size() == 2 && host.uploads[1] == std::make_pair(64u, 64u));
    CHECK(host.released.size() == 1 && host.released[0] == host.nextTexture - 1);   // the previous texture
    host.font.textWidth = 20;
    // another zoom moves the quad: zoom 3 = 50 viewport units
    host.zoom = 3;
    FakeMessage rebuild;
    rebuild.type = kDispatchViewMessageRebuild;
    CHECK(view->DoMessage(&rebuild));
    CHECK(host.uploads.size() == 3);
    CHECK_NEAR(Info(view, fire).iconQuad[1].y, 50.0f);
    rebuild.type = 0x1234;
    view->DoMessage(&rebuild);
    CHECK(host.uploads.size() == 3);
    host.zoom = 1;
    view->HandleViewChanged();

    // ---- the per-type maximum and the handle counter -----------------------------------------------------------------------
    FakeOccupant other(0x12345678, 50.0f, 0.0f, 50.0f);
    view->SetIndicatorMax(kTrackerFire, 1);
    CHECK(view->TrackOccupant(&other, kTrackerFire, 7, nullptr) == 0);
    CHECK(other.refs == 0 && view->TrackedCount(kTrackerFire) == 1);
    view->SetIndicatorMax(kTrackerFire, 0);   // 0 = no limit for tracking
    const int second = view->TrackOccupant(&other, kTrackerFire, 7, nullptr);
    CHECK(second == fire + 1);
    CHECK(view->TrackedCount(kTrackerFire) == 2 && other.refs == 1);
    CHECK((Info(view, second).flags & 0x1) == 0);
    CHECK(view->Untrack(static_cast<std::uint32_t>(second), true));
    CHECK(view->TrackedCount(kTrackerFire) == 1 && other.refs == 0 && view->FindTrackInfo(second) == nullptr);
    CHECK(!view->Untrack(static_cast<std::uint32_t>(second), true));
    CHECK(!view->Untrack(0, true));
    view->SetIndicatorMax(kTrackerFire, 0xffffffffu);

    // ---- the top-priority flag and the hidden types --------------------------------------------------------------------
    const int target = view->TrackOccupant(&truck, kTrackerSituationTarget, 0, nullptr);
    CHECK(truck.refs == 2);
    CHECK((Info(view, target).flags & 0x800) != 0 && (Info(view, fire).flags & 0x800) == 0);
    CHECK(Info(view, target).flags == (0x28u | 0x800u) && Info(view, target).binding == view->IconBinding(5));
    view->ShowAllIndicators(kTrackerSituationTarget, false);
    CHECK(view->HiddenTypes().count(kTrackerSituationTarget) == 1);
    CHECK((Info(view, target).flags & 0x800) == 0 && (Info(view, fire).flags & 0x800) != 0);
    view->ShowAllIndicators(kTrackerSituationTarget, true);
    CHECK(view->HiddenTypes().empty());
    CHECK((Info(view, target).flags & 0x800) != 0 && (Info(view, fire).flags & 0x800) == 0);
    truckAutomaton.flagSets.clear();
    CHECK(view->Untrack(static_cast<std::uint32_t>(target), true));
    CHECK((Info(view, fire).flags & 0x800) != 0);
    CHECK(truck.refs == 1);
    // the removal clears the automata flag although another indicator still tracks the automaton
    CHECK(truckAutomaton.flagSets.size() == 1 && truckAutomaton.flagSets[0] == std::make_pair(kDispatchViewTrackedAutomataFlag, false));

    // ---- the MySim: a picture, no fading -------------------------------------------------------------------------------
    FakeOccupant sim(0x12345678, 10.0f, 0.0f, 10.0f);
    const int mysim = view->TrackOccupant(&sim, kTrackerMySim, 77, nullptr);
    {
        const auto& info = Info(view, mysim);
        CHECK(info.flags == (0x22u | 0x800u) && info.alpha == 1.0f && info.pictureId == 77);
        CHECK(info.binding == view->IconBinding(2));
        CHECK(info.width == 32.0f && info.height == 32.0f);
        CHECK(host.uploads.back() == std::make_pair(128u, 32u));
        CHECK(info.texture == host.nextTexture);
        // the 128 x 32 picture in a 64-texel texture: u runs to 2, v to 0.5; the icon sits in front (z 0.3)
        CHECK_NEAR(info.numberQuad[3].u, 2.0f);
        CHECK_NEAR(info.numberQuad[1].v, 0.5f);
        CHECK_NEAR(info.numberQuad[0].z, 0.0f);
        CHECK_NEAR(info.iconQuad[0].z, 0.3f);
    }
    const std::uint32_t simTexture = Info(view, mysim).texture;
    CHECK(view->Untrack(static_cast<std::uint32_t>(mysim), false));   // no fade flag: gone at once
    CHECK(view->FindTrackInfo(mysim) == nullptr && view->TrackedCount(kTrackerMySim) == 0 && sim.refs == 0);
    CHECK(host.released.back() == simTexture);
    {
        const cRZString label("portrait.bmp");
        const int labelled = view->TrackOccupant(&sim, kTrackerMySim, 99, &label);
        CHECK(labelled > 0);
        CHECK(host.bitmapFiles.size() == 1 && host.bitmapFiles[0] == "portrait.bmp");
        CHECK(Info(view, labelled).pictureId == 0 && Info(view, labelled).texture == 0);
        CHECK(std::strcmp(Info(view, labelled).label.ToChar(), "portrait.bmp") == 0);
        view->Untrack(static_cast<std::uint32_t>(labelled), true);
    }

    // ---- fading in, fading out, removal by Update ------------------------------------------------------------------------
    host.elapsed = 250.0f;   // 0.25 s at two alpha units per second
    g_calls.clear();
    CHECK(view->Update(&context));
    CHECK(host.restarts == 1);
    CHECK_NEAR(Info(view, fire).alpha, 0.5f);
    CHECK(view->Untrack(static_cast<std::uint32_t>(fire), false));
    CHECK(view->FindTrackInfo(fire) != nullptr && (Info(view, fire).flags & 0x10) != 0);
    host.elapsed = 100.0f;
    view->Update(&context);
    CHECK_NEAR(Info(view, fire).alpha, 0.3f);
    truckAutomaton.flagSets.clear();
    host.elapsed = 200.0f;
    view->Update(&context);
    CHECK(view->FindTrackInfo(fire) == nullptr);
    CHECK(view->TrackedCount(kTrackerFire) == 0 && truck.refs == 0);
    CHECK(truckAutomaton.flagSets.size() == 1 && truckAutomaton.flagSets[0].second == false);
    // the automaton's "removed" flag starts the fade-out by itself
    const int fire2 = view->TrackOccupant(&truck, kTrackerFire, 5, nullptr);
    truckAutomaton.removed = true;
    host.elapsed = 10.0f;
    view->Update(&context);
    CHECK((Info(view, fire2).flags & 0x10) != 0);
    truckAutomaton.removed = false;
    view->Untrack(static_cast<std::uint32_t>(fire2), true);
    // nothing is drawn or faded while snapshotting or away from view level 1, and the timer is not restarted
    {
        const int restarts = host.restarts;
        host.snapshot = true;
        view->Update(&context);
        host.snapshot = false;
        host.viewLevel = 2;
        view->Update(&context);
        host.viewLevel = 1;
        CHECK(host.restarts == restarts);
    }

    // ---- the mission target: a billboard, flashing ---------------------------------------------------------------------
    FakeOccupant goal(0x12345678, 200.0f, 0.0f, 300.0f);
    const int flash = view->TrackOccupant(&goal, kTrackerSituationTarget, 0, nullptr);
    view->FlashIndicator(static_cast<std::uint32_t>(flash), true, 1.0f);
    CHECK((Info(view, flash).flags & 0x100) != 0 && Info(view, flash).flashPeriod == 1.0f);
    host.elapsed = 500.0f;
    g_calls.clear();
    const int camera2DBeforeFlash = host.camera2D;
    view->Update(&context);
    CHECK_NEAR(Info(view, flash).flashTime, 0.5f);
    CHECK_NEAR(Info(view, flash).alpha, 0.70710677f);   // cos(0.5 * pi / 2)
    {
        // on screen and without a number: the icon as a billboard over the occupant (lifted to the sea level 5), a faint pass at
        // half the alpha then the full one
        CHECK(Count(DrawCall::Draw) == 2 && host.camera2D == camera2DBeforeFlash);
        CHECK(Count(DrawCall::ModelTransform) == 1);
        int transparencies = 0, states = 0;
        for (const DrawCall& call : g_calls) {
            if (call.kind == DrawCall::ModelTransform) {
                CHECK(call.transform.translation[0] == 200.0f && call.transform.translation[1] == 5.0f &&
                      call.transform.translation[2] == 300.0f && (call.transform.flags & 1) != 0);
            } else if (call.kind == DrawCall::Transparency) {
                CHECK_NEAR(call.alpha, transparencies == 0 ? 0.35355339f : 0.70710677f);
                ++transparencies;
            } else if (call.kind == DrawCall::State) {
                CHECK(call.state == view->RenderState(states == 0 ? 1 : 0).value);
                ++states;
            } else if (call.kind == DrawCall::Texture) {
                CHECK(call.value == static_cast<int>(view->IconBinding(5)->TextureId()));
            } else if (call.kind == DrawCall::Draw) {
                CHECK(call.value == 6 * 100 + 2 * 10 + 4);
                CHECK(std::memcmp(call.vertices, Info(view, flash).iconQuad, sizeof call.vertices) == 0);
            }
        }
        CHECK(transparencies == 2 && states == 2 && Count(DrawCall::ResetTransparency) == 1 && Count(DrawCall::ResetModelView) == 1);
        const DispatchViewRect& rect = Info(view, flash).lastRect;
        CHECK(rect.left == 184 && rect.top == 201 && rect.right == 216 && rect.bottom == 265);
        CHECK(Info(view, flash).lastCenter.y == 5.0f);
        const DispatchViewRect* last = view->GetLastScreenIconRect(static_cast<std::uint32_t>(flash));
        CHECK(last == &rect);
    }
    host.elapsed = 600.0f;   // past the period: the flash restarts
    view->Update(&context);
    CHECK(Info(view, flash).flashTime == 0.0f);
    CHECK_NEAR(Info(view, flash).alpha, 1.0f);
    host.elapsed = 300.0f;
    view->Update(&context);
    view->FlashIndicator(static_cast<std::uint32_t>(flash), true, 0.2f);   // a shorter period pulls the running time back
    CHECK_NEAR(Info(view, flash).flashTime, 0.2f);
    CHECK_NEAR(Info(view, flash).flashPeriod, 0.2f);
    view->FlashIndicator(static_cast<std::uint32_t>(flash), false, 0.0f);
    CHECK((Info(view, flash).flags & 0x100) == 0);
    view->LockIndicator(static_cast<std::uint32_t>(flash), true);
    CHECK((Info(view, flash).flags & 0x80) != 0);
    goal.position[0] = 5000.0f;   // locked: the last rect stays although the occupant left the screen
    view->Update(&context);
    CHECK(Info(view, flash).lastRect.left == 184 && Info(view, flash).lastRect.top == 201);
    view->LockIndicator(static_cast<std::uint32_t>(flash), false);
    goal.position[0] = 200.0f;

    // ---- the CSI button: selectable, the 2-D arrow, the window nudge ----------------------------------------------------
    FakeOccupant csiOccupant(0x12345678, 400.0f, 0.0f, 300.0f);
    const int csi = view->TrackOccupant(&csiOccupant, kTrackerSituation, 77, nullptr);
    {
        const auto& info = Info(view, csi);
        CHECK(info.flags == (0x2eu | 0x800u) && info.buttonState == 1 && info.binding == view->IconBinding(4));
        CHECK(info.width == 35.0f && info.height == 35.0f);
        // four 32-texel button states side by side in a 128-texel texture; state 1 is the second column
        CHECK_NEAR(info.numberQuad[0].u, 0.25f);
        CHECK_NEAR(info.numberQuad[3].u, 0.5f);
        CHECK_NEAR(info.numberQuad[1].v, 0.25f);
    }
    view->SetActiveIndicator(static_cast<std::uint32_t>(csi), 2);   // still transparent: not selectable
    CHECK(view->ActiveHandle() == 0);
    host.elapsed = 500.0f;
    g_calls.clear();
    const int camera2D = host.camera2D;
    view->Update(&context);
    CHECK(host.camera2D == camera2D + 1 && host.camera3D == camera2D + 1);
    {
        const auto& info = Info(view, csi);
        CHECK_NEAR(info.alpha, 1.0f);
        // the CSI always takes the 2-D path: a 42-unit square at the projected icon corner (384, 201)
        CHECK(info.lastRect.left == 384 && info.lastRect.top == 201 && info.lastRect.right == 426 && info.lastRect.bottom == 243);
        CHECK(info.lastCenter.x == 405.0f && info.lastCenter.y == 222.0f && info.lastCenter.z == 0.0f);
        // the arrow quad turns towards the occupant's viewport point (400, 295) round the centre; the picture quad is
        // centred on it, 35 units square
        const float dx = 400.0f - 405.0f, dy = 295.0f - 222.0f;
        const float length = std::sqrt(dx * dx + dy * dy);
        const float angle = std::atan2(-(dx / length), dy / length);
        const float c = std::cos(angle), s = std::sin(angle);
        std::vector<const DrawCall*> draws;
        for (const DrawCall& call : g_calls)
            if (call.kind == DrawCall::Draw) draws.push_back(&call);
        // the map runs by tracker type: the CSI's arrow and picture first, then the target's two billboard passes
        CHECK(draws.size() == 4);
        if (draws.size() == 4) {
            ExpectVertex(draws[0]->vertices[0], c * -32.0f - s * -32.0f + 405.0f, s * -32.0f + c * -32.0f + 222.0f, 0.0f, 0.0f, 0.0f, __LINE__);
            ExpectVertex(draws[0]->vertices[2], c * 32.0f - s * 32.0f + 405.0f, s * 32.0f + c * 32.0f + 222.0f, 0.0f, 1.0f, 1.0f, __LINE__);
            ExpectVertex(draws[1]->vertices[0], 405.0f - 17.5f, 222.0f - 17.5f, 0.0f, info.numberQuad[0].u, info.numberQuad[0].v, __LINE__);
            ExpectVertex(draws[1]->vertices[2], 405.0f + 17.5f, 222.0f + 17.5f, 0.0f, info.numberQuad[2].u, info.numberQuad[2].v, __LINE__);
        }
        // the arrow first, then the picture: the binding is not the MySim's
        std::vector<int> textures;
        for (const DrawCall& call : g_calls)
            if (call.kind == DrawCall::Texture) textures.push_back(call.value);
        CHECK(textures.size() == 4 && textures[0] == static_cast<int>(view->IconBinding(4)->TextureId()) &&
              textures[1] == static_cast<int>(info.texture));
    }
    CHECK(view->FindIndicatorHandle(405, 222) == static_cast<std::uint32_t>(csi));
    CHECK(view->FindIndicatorHandle(384, 243) == static_cast<std::uint32_t>(csi));   // the rect is inclusive
    CHECK(view->FindIndicatorHandle(10, 10) == 0);
    CHECK(view->FindIndicatorHandle(200, 230) == 0);   // the target is not a selectable kind
    view->SetActiveIndicator(static_cast<std::uint32_t>(csi), 2);
    CHECK(view->ActiveHandle() == static_cast<std::uint32_t>(csi));
    CHECK(Info(view, csi).buttonState == 2);
    CHECK_NEAR(Info(view, csi).numberQuad[0].u, 0.5f);   // the third column
    CHECK(view->GetActiveOccupant() == &csiOccupant);
    view->SetActiveIndicator(0, 0);
    CHECK(view->ActiveHandle() == 0 && Info(view, csi).buttonState == 1);
    CHECK_NEAR(Info(view, csi).numberQuad[0].u, 0.25f);
    CHECK(view->GetActiveOccupant() == nullptr);
    // a hidden type is neither drawn nor selectable
    view->ShowAllIndicators(kTrackerSituation, false);
    CHECK(view->FindIndicatorHandle(405, 222) == 0);
    view->ShowAllIndicators(kTrackerSituation, true);
    // the emergency pause hides the situation indicators; a maximum of 0 draws none of the type
    {
        const int before = host.camera2D;
        host.emergency = true;
        view->Update(&context);
        host.emergency = false;
        view->SetIndicatorMax(kTrackerSituation, 0);
        view->Update(&context);
        view->SetIndicatorMax(kTrackerSituation, 0xffffffffu);
        CHECK(host.camera2D == before);
        view->Update(&context);
        CHECK(host.camera2D == before + 1);
    }

    // the window clip list: a visible window smaller than the view; not the three unclipped ids, an invisible one, a fading
    // one or one as large as the view
    host.children = {
        DispatchViewChildWindow{0x1111, true, cRZRect{380, 190, 460, 250}, false},
        DispatchViewChildWindow{0xaa32bce6, true, cRZRect{10, 10, 50, 50}, false},
        DispatchViewChildWindow{0xea8cad14, true, cRZRect{10, 10, 50, 50}, false},
        DispatchViewChildWindow{0x4c30e4fa, true, cRZRect{10, 10, 50, 50}, false},
        DispatchViewChildWindow{0x2222, false, cRZRect{10, 10, 50, 50}, false},
        DispatchViewChildWindow{0x3333, true, cRZRect{10, 10, 50, 50}, true},
        DispatchViewChildWindow{0x4444, true, cRZRect{0, 0, 800, 600}, false},
    };
    host.elapsed = 16.0f;
    view->Update(&context);
    CHECK(view->WindowClipList().size() == 2);
    if (view->WindowClipList().size() == 2) {
        const DispatchViewRectInfo& edge = view->WindowClipList().front();
        const DispatchViewRectInfo& window = view->WindowClipList().back();
        CHECK(edge.left == 0 && edge.right == 800 && edge.bottom == 600 && edge.priority == 0xff);
        CHECK(window.left == 380 && window.top == 190 && window.right == 460 && window.bottom == 250 && window.priority == 0xff);
    }
    {
        // the arrow overlapped the window: it walked towards the view centre (400, 300) in two-unit steps until it cleared it
        const DispatchViewRect& rect = Info(view, csi).lastRect;
        CHECK(rect.top > 250 && rect.top <= 253);
        CHECK(rect.right - rect.left == 42 && rect.bottom - rect.top == 42);
        CHECK(rect.left >= 380 && rect.left <= 384);
    }
    host.children.clear();
    view->Update(&context);
    CHECK(Info(view, csi).lastRect.top == 201);   // back where it was without the window

    // ---- clustered indicators round a building --------------------------------------------------------------------------
    FakeOccupant building(kDispatchViewClusteredOccupantType, 600.0f, 0.0f, 300.0f);
    const int callA = view->TrackOccupant(&building, kTrackerFire, 1, nullptr);
    const int callB = view->TrackOccupant(&building, kTrackerFire, 2, nullptr);
    CHECK((Info(view, callA).flags & 0x1) != 0 && (Info(view, callB).flags & 0x801) == 0x801);
    view->Update(&context);
    {
        // the building's viewport point is (600, 295); the first indicator sits at -18 degrees, the second at 54 degrees on a
        // 50-unit circle, each a 42-unit square
        const DispatchViewRect& a = Info(view, callA).lastRect;
        const DispatchViewRect& b = Info(view, callB).lastRect;
        CHECK(a.left == 626 && a.top == 258 && a.right == 668 && a.bottom == 300);
        CHECK(b.left == 608 && b.top == 314 && b.right == 650 && b.bottom == 356);
    }
    view->Untrack(static_cast<std::uint32_t>(callA), true);
    view->Untrack(static_cast<std::uint32_t>(callB), true);
    CHECK(building.refs == 0);

    // ---- UseAlternateModel looks the map up by the handle's low byte ------------------------------------------------------
    CHECK(view->UseAlternateModel(0x104, true));
    CHECK((Info(view, csi).flags & 0x400) != 0);
    CHECK(!view->UseAlternateModel(0x103, true));

    // ---- the player-drive arrow --------------------------------------------------------------------------------------------
    FakeModel arrow;
    FakeOccupant car(kDispatchViewAutomatonTypeB, 300.0f, 20.0f, 100.0f);
    FakeAutomaton carAutomaton;
    host.automata[&car] = &carAutomaton;
    host.model = &arrow;
    const int drive = view->TrackOccupant(&car, kTrackerPlayerDrive, 0, nullptr);
    CHECK(drive > 0);
    CHECK(host.modelRequests == 1 && host.modelKey.type == 0x5ad0e817 && host.modelKey.group == 0xbadb57f1 &&
          host.modelKey.instance == 0x8bb90000);
    {
        const auto& info = Info(view, drive);
        CHECK(info.flags == (0x268u | 0x800u));
        CHECK(info.model == &arrow && info.processor != nullptr);
        CHECK(info.height == 6.0f);
        CHECK(info.texture == 0);   // BuildCallNumberDisplay leaves a 3-D indicator alone
    }
    CHECK(arrow.refs == 2);   // the test's and the view's
    CHECK(arrow.owner == static_cast<cIGZUnknown*>(&car) && arrow.properties == 1);
    CHECK(arrow.zoom == 1 && arrow.rotation == 2);
    CHECK((arrow.attributes.flags & 0x10) != 0);
    CHECK(arrow.rate == std::bit_cast<float>(0x3c449ba6u));
    CHECK(carAutomaton.flagSets.size() == 1 && carAutomaton.flagSets[0] == std::make_pair(kDispatchViewTrackedAutomataFlag, true));
    carAutomaton.hasState = true;
    carAutomaton.hasParams = true;
    carAutomaton.stateValue = 50;
    carAutomaton.limit = 200.0f;
    host.elapsed = 500.0f;
    g_calls.clear();
    view->Update(&context);
    CHECK(arrow.draws == 1 && arrow.selfLit && !arrow.translucent && arrow.opacity == 1.0f && arrow.lastUpdate == 500.0f);
    {
        // lifted 10 units plus half its height over the occupant (no terrain clamp here), turned 7 degrees about y
        CHECK(arrow.transform.translation[0] == 300.0f && arrow.transform.translation[1] == 33.0f && arrow.transform.translation[2] == 100.0f);
        const float turn = std::bit_cast<float>(0x3c8efa35u) * 7.0f;
        CHECK_NEAR(arrow.transform.rotation[0][0], std::cos(turn));
        CHECK_NEAR(arrow.transform.rotation[0][2], std::sin(turn));
        CHECK_NEAR(arrow.transform.rotation[2][0], -std::sin(turn));
        CHECK((arrow.transform.flags & 5) == 5);
        // a quarter of the damage limit: a quarter of the way from green to red
        CHECK_NEAR(arrow.attributes.shade[0], 0.25f * 0.921875f + 0.75f * 0.12890625f);
        CHECK_NEAR(arrow.attributes.shade[1], 0.25f * 0.0546875f + 0.75f * 0.89453125f);
        CHECK_NEAR(arrow.attributes.shade[2], 0.25f * 0.05859375f + 0.75f * 0.09765625f);
        CHECK(arrow.attributes.shade[3] == 1.0f);
    }
    carAutomaton.wrecked = true;
    view->Update(&context);
    CHECK(arrow.draws == 2);
    CHECK_NEAR(arrow.transform.rotation[0][0], std::cos(std::bit_cast<float>(0x3c8efa35u) * 14.0f));   // another 7 degrees
    CHECK_NEAR(arrow.attributes.shade[0], 0.921875f);
    CHECK_NEAR(arrow.attributes.shade[1], 0.0546875f);

    // ---- Shutdown --------------------------------------------------------------------------------------------------------
    const std::uint32_t csiCount = view->TrackedCount(kTrackerSituation);
    CHECK(view->Shutdown());
    CHECK(!view->IsInitialized());
    CHECK(view->Tracks().empty());
    CHECK(host.messages.removed.size() == 1 && host.messages.removed[0].second == kDispatchViewMessageRebuild);
    CHECK(csiOccupant.refs == 0 && goal.refs == 0 && car.refs == 0 && truck.refs == 0);
    CHECK(arrow.refs == 1);
    for (const auto& binding : host.bindings) CHECK(binding->releases == 1);
    for (int slot = 0; slot < 7; ++slot) CHECK(view->IconBinding(slot) == nullptr);
    CHECK(view->TrackedCount(kTrackerSituation) == csiCount && csiCount == 1);   // Shutdown keeps the counts
    CHECK(view->Shutdown());   // a second Shutdown does nothing
    CHECK(host.messages.removed.size() == 1);

    CHECK(view->Release() == 0);
    if (failures == 0) std::printf("PASS sc4_dispatchvehicleview\n");
    return failures == 0 ? 0 : 1;
}
