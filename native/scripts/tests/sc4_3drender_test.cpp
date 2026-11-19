// cSC43DRender (src/sc4/Render3D.*): lifecycle, reference counting, interface queries, static driver plumbing, Init /
// Shutdown, render properties, clip / camera wiring, view-object lists, dirty rectangles, backing-store flags, screen
// shake and the stream round trip, against a recording driver and host.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_gdriver.h"
#include "sc4/Render3D.h"
#include "sc4/SCProperty.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"

using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int gLiveViewObjects = 0;

struct FakeStore final : gd::IBackingStore {
    bool exists = false;
    std::uint32_t width = 0, height = 0;
    int creates = 0, destroys = 0;
    int saves = 0, restores = 0;
    int lastSaveSrcX = 0, lastSaveSrcY = 0, lastSaveWidth = 0, lastSaveHeight = 0, lastSaveDstX = 0, lastSaveDstY = 0;
    int lastRestoreSrcX = 0, lastRestoreSrcY = 0, lastRestoreWidth = 0, lastRestoreHeight = 0;
    int lastRestoreDstX = 0, lastRestoreDstY = 0;
    bool Create(std::uint32_t w, std::uint32_t h) override {
        width = w;
        height = h;
        exists = true;
        ++creates;
        return true;
    }
    void Destroy() override {
        exists = false;
        ++destroys;
    }
    bool Exists() const override { return exists; }
    void SaveFromFrame(int srcX, int srcY, int w, int h, int dstX, int dstY) override {
        ++saves;
        lastSaveSrcX = srcX;
        lastSaveSrcY = srcY;
        lastSaveWidth = w;
        lastSaveHeight = h;
        lastSaveDstX = dstX;
        lastSaveDstY = dstY;
    }
    void RestoreToFrame(int srcX, int srcY, int w, int h, int dstX, int dstY) override {
        ++restores;
        lastRestoreSrcX = srcX;
        lastRestoreSrcY = srcY;
        lastRestoreWidth = w;
        lastRestoreHeight = h;
        lastRestoreDstX = dstX;
        lastRestoreDstY = dstY;
    }
};

struct FakeViewObject final : cISC4ViewObject3D {
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        --gLiveViewObjects;
        refs = 0;
        return 0;
    }
    void DrawViewObject(SC4DrawContext&) override {}
    static FakeViewObject* Make() {
        ++gLiveViewObjects;
        return new FakeViewObject();
    }
};

struct FakeModel final : s3d::cIS3DModelInstance {
    std::uint32_t refs = 1;
    std::int32_t properties = 0;
    cS3DBoundingBox worldBox;
    void* customData = nullptr;
    s3d::SC4DrawAttributes attributes;
    int draws = 0;
    int highlightDraws = 0;
    int drawResult = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs > 1 ? --refs : 0; }
    cIGZUnknown* GetOwner() override { return nullptr; }
    void SetOwner(cIGZUnknown*) override {}
    void* GetCustomData() override { return customData; }
    void SetCustomData(void* data) override { customData = data; }
    void GetBounds(cS3DBoundingBox& out) override { out = worldBox; }
    void GetWorldBounds(cS3DBoundingBox& out) override { out = worldBox; }
    void GetTransform(cS3DTransform&) override {}
    void SetTransform(const cS3DTransform&) override {}
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool highlight) override {
        ++draws;
        if (highlight) ++highlightDraws;
        return drawResult;
    }
    std::int32_t GetProperties() override { return properties; }
    void SetProperties(std::int32_t value) override { properties = value; }
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return attributes; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes&) override {}
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
};

struct FakeLighting final : IS4LightingManager {
    int globals = 0;
    int* deleted = nullptr;
    ~FakeLighting() override {
        if (deleted) ++*deleted;
    }
    void GetGlobalLight(cS3DVector3& out) override {
        out = cS3DVector3{1, 1, 1};
        ++globals;
    }
    bool GetModelLight(s3d::cIS3DModelInstance*, cS3DVector3& out) override {
        out = cS3DVector3{1, 1, 1};
        return true;
    }
};

struct FakeSettings final : IRender3DSettings {
    bool disablePartialWrites = false;
    bool clipSupported = true;
    bool staticRedraw = true;
    bool dynamicView = true;
    bool viewCleanup = true;
    bool clearPreservesLighting = false;
    bool translatedRebuildsFrustum = true;
    bool shakeEnabled = true;
    int shakeOffsetX = 0;
    int shakeOffsetY = 0;
    int dirtyMaxAge = 10000;
    bool DisablePartialRegionWrites() const override { return disablePartialWrites; }
    bool ClipRectSupported() const override { return clipSupported; }
    int BackingStoreMaxZoom() const override { return 3; }
    int StaticViewMaxZoomA() const override { return 3; }
    int StaticViewMaxZoomB() const override { return 3; }
    int StaticViewMaxZoomC() const override { return 3; }
    bool StaticViewRedrawOnViewportChange() const override { return staticRedraw; }
    bool DrawDynamicViewEnabled() const override { return dynamicView; }
    bool ViewCleanupEnabled() const override { return viewCleanup; }
    bool ClearViewportPreservesLighting() const override { return clearPreservesLighting; }
    bool TranslatedUpdateRebuildsDrawFrustum() const override { return translatedRebuildsFrustum; }
    bool ScreenShakeEnabled() const override { return shakeEnabled; }
    int ScreenShakeOffsetX() const override { return shakeOffsetX; }
    int ScreenShakeOffsetY() const override { return shakeOffsetY; }
    int DirtyRectangleMaxAgeFrames() const override { return dirtyMaxAge; }
};

struct FakeHost final : IRender3DHost {
    sc4::cSCPropertyHolder properties;
    FakeSettings settings;
    FakeLighting* lighting = nullptr;   // created on demand, owned by the renderer
    int lightingDeleted = 0;
    bool (*shutdownHook)(void*) = nullptr;
    int shutdownRegistrations = 0;
    int lightingCreated = 0;
    void RegisterStaticShutdown(bool (*function)(void*), void*) override {
        shutdownHook = function;
        ++shutdownRegistrations;
    }
    cIGZMessageServer2* MessageServer() override { return nullptr; }
    cISCPropertyHolder* RenderPropertyHolder() override { return &properties; }
    IS4LightingManager* CreateLightingManager() override {
        ++lightingCreated;
        lighting = new FakeLighting();
        lighting->deleted = &lightingDeleted;
        return lighting;
    }
    cIGZUnknown* TerrainView() override { return nullptr; }
    cIGZUnknown* City() override { return nullptr; }
    IRender3DSettings* Settings() override { return &settings; }
    cSC4RenderProperties* RenderProperties() override { return nullptr; }
};

// A recording cIGZOStream: enough for cSC43DRender::Write.
struct FakeOStream final : cIGZOStream {
    std::vector<std::int64_t> ints;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int sint16s = 0;
    std::int16_t lastSint16 = 0;
    bool error = false;
    void Flush() override {}
    bool SetSint8(std::int8_t) override { return true; }
    bool SetUint8(std::uint8_t) override { return true; }
    bool SetSint16(std::int16_t value) override {
        ++sint16s;
        lastSint16 = value;
        return true;
    }
    bool SetUint16(std::uint16_t) override { return true; }
    bool SetSint32(std::int32_t value) override {
        ints.push_back(value);
        return true;
    }
    bool SetUint32(std::uint32_t value) override {
        ints.push_back(value);
        return true;
    }
    bool SetSint64(std::int64_t) override { return true; }
    bool SetUint64(std::uint64_t) override { return true; }
    bool SetFloat32(float) override { return true; }
    bool SetFloat64(double) override { return true; }
    bool SetRZCharStr(const char*) override { return true; }
    bool SetGZStr(const cIGZString&) override { return true; }
    bool SetGZSerializable(const cIGZSerializable& object) override { return object.Write(*this); }
    bool SetVoid(const void*, std::uint32_t) override { return true; }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

// A cIGZIStream that replays the fixed record cSC43DRender::Read expects.
struct FakeIStream final : cIGZIStream {
    std::vector<std::int32_t> ints;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::size_t next = 0;
    std::int16_t version = 2;
    bool error = false;
    bool Skip(std::uint32_t) override { return true; }
    bool GetSint8(std::int8_t&) override { return false; }
    bool GetUint8(std::uint8_t&) override { return false; }
    bool GetSint16(std::int16_t& value) override {
        value = version;
        return true;
    }
    bool GetUint16(std::uint16_t& value) override {
        value = static_cast<std::uint16_t>(version);
        return true;
    }
    bool GetSint32(std::int32_t& value) override {
        if (next >= ints.size()) return false;
        value = ints[next++];
        return true;
    }
    bool GetUint32(std::uint32_t& value) override {
        std::int32_t raw = 0;
        if (!GetSint32(raw)) return false;
        value = static_cast<std::uint32_t>(raw);
        return true;
    }
    bool GetSint64(std::int64_t&) override { return false; }
    bool GetUint64(std::uint64_t&) override { return false; }
    bool GetFloat32(float&) override { return false; }
    bool GetFloat64(double&) override { return false; }
    bool GetRZCharStr(char*, std::uint32_t) override { return false; }
    bool GetGZStr(cIGZString&) override { return false; }
    bool GetGZSerializable(cIGZSerializable&) override { return false; }
    bool GetVoid(void*, std::uint32_t) override { return false; }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

void AddColour(sc4::cSCPropertyHolder& holder, std::uint32_t id, float r, float g, float b, float a) {
    cRZVariant value;
    const float data[4] = {r, g, b, a};
    value.RefFloat32(data, 4);
    holder.AddProperty(id, &value, false);
}

}  // namespace

int main() {
    // ---- construction defaults -----------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        CHECK(!render->IsInitialized());
        CHECK(render->GetViewUtilities() != nullptr);
        CHECK(render->GetDrawContext() != nullptr);
        CHECK(render->GetCamera() == nullptr);
        CHECK(render->GetCameraControl() == nullptr);
        CHECK(render->GetLightingManager() == nullptr);
        CHECK(render->GetProjectionMatrixEntries() == nullptr);   // quirk: the binary returns 0
        CHECK(render->GetViewMatrixEntries() == nullptr);
        CHECK(render->GetGZCLSID() == 0xe9c622d8);
        CHECK(render->MapScaleFactor() == 1);
        CHECK(render->GetViewFogEnabled() == false);
        CHECK(render->GetLightingEnabled() == true);
        CHECK(render->CameraView() == 0);
        CHECK(render->GetViewUtilities()->CitySizeX() == 0.0f);   // Init has not run yet
        CHECK(render->Shutdown());                                // no-op before Init
        CHECK(render->Release() == 0);
    }

    // ---- reference counting / QueryInterface ---------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        CHECK(render->Release() == 0);   // count 0 -> Shutdown + delete this
        auto* object = new cSC43DRender();
        object->AddRef();   // 1
        CHECK(object->AddRef() == 2);
        void* primary = nullptr;
        CHECK(object->QueryInterface(1, &primary) && primary == static_cast<cISC43DRender*>(object));   // 2 -> 3
        CHECK(object->Release() == 2);                                     // back to 2
        void* target = nullptr;
        CHECK(object->QueryInterface(0x452294aa, &target) && target != nullptr);
        CHECK(object->Release() == 2);
        CHECK(object->QueryInterface(0x681bd529, &primary));
        CHECK(object->Release() == 2);
        void* serializable = nullptr;
        CHECK(object->QueryInterface(0xe4fda3d4, &serializable) && serializable != nullptr);
        CHECK(object->Release() == 2);
        void* untouched = reinterpret_cast<void*>(0x1234);
        CHECK(!object->QueryInterface(0xdeadbeef, &untouched));
        CHECK(untouched == reinterpret_cast<void*>(0x1234));
        CHECK(object->Release() == 1);
        // The last Release shuts the object down and frees it.
        CHECK(object->Release() == 0);
    }

    FakeStore store;
    sc4test::FakeGDriver driver;
    driver.viewport[0] = 0;
    driver.viewport[1] = 0;
    driver.viewport[2] = 640;
    driver.viewport[3] = 480;
    driver.store = &store;

    // ---- statics -------------------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        CHECK(!cSC43DRender::StaticInitialized());
        CHECK(cSC43DRender::StaticInit(nullptr));
        CHECK(cSC43DRender::StaticInitialized());
        CHECK(host.shutdownRegistrations == 1);
        CHECK(host.shutdownHook != nullptr);
        CHECK(host.shutdownHook(nullptr) == true);
        CHECK(!cSC43DRender::StaticInitialized());
        // A second shutdown is a no-op.
        CHECK(host.shutdownHook(nullptr) == true);

        cSC43DRender::StaticSetGDriver(&driver);
        CHECK(cSC43DRender::Driver() == &driver);
        CHECK(driver.addRefs == 1);
        CHECK(cSC43DRender::BackingStoreService() == &store);
        CHECK(store.creates == 1 && store.exists);
        CHECK(store.width == 640 && store.height == 480);
        // Setting the same driver changes nothing.
        cSC43DRender::StaticSetGDriver(&driver);
        CHECK(driver.addRefs == 1);
        // A different (null) driver releases the old one and destroys the store.
        cSC43DRender::StaticSetGDriver(nullptr);
        CHECK(cSC43DRender::Driver() == nullptr);
        CHECK(driver.releases == 1);
        CHECK(store.destroys == 1 && !store.exists);
        CHECK(!cSC43DRender::HasBackingStore());
        // Restore for the following blocks.
        cSC43DRender::StaticSetGDriver(&driver);
        CHECK(cSC43DRender::BackingStoreService() == &store);
    }

    // ---- Init / Shutdown -----------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        AddColour(host.properties, 0xea639fb1, 0.1f, 0.2f, 0.3f, 0.4f);
        AddColour(host.properties, 0xea639fb5, 1.0f, 0.0f, 0.0f, 1.0f);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(800, 600);
        CHECK(render->Init());
        CHECK(render->IsInitialized());
        CHECK(render->GetCamera() == nullptr);          // +0xd8 comes from SetCamera, not Init
        CHECK(render->GetLightingManager() == host.lighting);
        CHECK(host.lighting != nullptr);
        CHECK(host.lightingCreated == 1);
        CHECK(render->GetDrawContext()->IsInitialized());
        CHECK(render->GetDrawContext()->Driver() == &driver);
        CHECK(render->GetViewUtilities()->CitySizeX() == 4096.0f);
        CHECK(render->GetViewUtilities()->CitySizeZ() == 4096.0f);
        CHECK(render->PartialWritesAllowed());
        // The 11 highlight colours: only the two supplied ones differ from the defaults.
        const auto& colours = render->GetDrawContext()->HighlightColors();
        CHECK(colours.size() == 11);
        CHECK(colours[0].x == 0.1f && colours[0].y == 0.2f && colours[0].z == 0.3f && colours[0].w == 0.4f);
        CHECK(colours[4].x == 1.0f && colours[4].y == 0.0f);
        CHECK(colours[1].x == 0.0f && colours[1].y == 1.0f);   // untouched default
        // Init is idempotent: a second call creates nothing and keeps the lighting manager.
        const int created = host.lightingCreated;
        CHECK(render->Init());
        CHECK(render->GetLightingManager() == host.lighting);
        CHECK(host.lightingCreated == created);
        // Shutdown releases the owned subsystems and drops the camera.
        CHECK(render->Shutdown());
        CHECK(!render->IsInitialized());
        CHECK(render->GetCamera() == nullptr);
        CHECK(render->GetLightingManager() == nullptr);
        CHECK(render->Release() == 0);
    }

    // ---- viewport / clip rect ------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(800, 600);
        CHECK(render->Init());
        std::uint32_t width = 0, height = 0;
        render->GetViewportSize(width, height);
        CHECK(width == 800 && height == 600);

        cRZRect rect{10, 20, 210, 140};
        render->SetClipRect(rect);
        // SetViewport(x, viewportHeight - bottom, right - left, bottom - top)
        CHECK(driver.setViewportCalls == 1);
        CHECK(driver.setViewport[0] == 10);
        CHECK(driver.setViewport[1] == 460);
        CHECK(driver.setViewport[2] == 200);
        CHECK(driver.setViewport[3] == 120);
        CHECK(render->CameraView() == 0);
        render->UnsetClipRect();
        CHECK(driver.resetViewportCalls == 1);

        // The settings seam can disable clipping entirely.
        host.settings.clipSupported = false;
        render->SetClipRect(rect);
        CHECK(driver.setViewportCalls == 1);
        host.settings.clipSupported = true;
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- camera views --------------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        CHECK(render->GetCamera() == nullptr);
        render->Set3DCameraView();
        CHECK(render->CameraView() == 2);
        CHECK(render->GetDrawContext()->Camera() == nullptr);
        render->Set2DCameraView();
        CHECK(render->CameraView() == 1);
        CHECK(render->GetDrawContext()->Camera() != nullptr);   // the ortho camera +0x130
        render->SetUICameraView();
        CHECK(render->CameraView() == 3);
        CHECK(render->BorrowRenderSystem());
        CHECK(render->RenderSystemBorrowed());
        CHECK(render->ReturnRenderSystem());
        CHECK(!render->RenderSystemBorrowed());
        CHECK(render->CameraView() == 3);   // Returning restores the borrowed view
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- view-object lists ---------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        FakeViewObject* a = FakeViewObject::Make();
        FakeViewObject* b = FakeViewObject::Make();
        FakeViewObject* c = FakeViewObject::Make();
        CHECK(!render->AddViewObject(a, 1, 0));   // unknown list
        CHECK(render->AddViewObject(a, 3, 10));
        CHECK(render->AddViewObject(b, 3, 5));
        CHECK(render->AddViewObject(c, 5, 0));
        CHECK(render->FindViewObject(a));
        CHECK(render->FindViewObject(c));
        // The dynamic list is sorted ascending by priority: b (5) before a (10).
        const auto& dynamic = render->ViewObjects(0);
        CHECK(dynamic.size() == 2);
        CHECK(dynamic.front().object == b);
        CHECK(dynamic.back().object == a);
        CHECK(render->ViewObjects(1).size() == 1);
        CHECK(a->refs == 2 && b->refs == 2 && c->refs == 2);
        CHECK(render->RemoveViewObject(a));
        CHECK(!render->FindViewObject(a));
        CHECK(a->refs == 1);
        CHECK(!render->RemoveViewObject(a));
        CHECK(render->Shutdown());   // clears the remaining lists
        CHECK(b->refs == 1 && c->refs == 1);
        CHECK(gLiveViewObjects == 3);
        a->Release();
        b->Release();
        c->Release();
        CHECK(gLiveViewObjects == 0);
        delete a; delete b; delete c;   // Release only counts down
        CHECK(render->Release() == 0);
    }

    // ---- dirty rectangles / full-update invalidation -------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(400, 300);
        CHECK(render->Init());
        render->AddDirtyRectangle(0, cRZRect{10, 10, 60, 40}, nullptr, 0);
        CHECK(render->DirtyRectangleCount() == 1);
        CHECK(render->DirtyRectangleAccumulatedArea() == 50 * 30);
        // Fully off-screen rectangles are dropped, partially visible ones are clipped.
        render->AddDirtyRectangle(0, cRZRect{500, 500, 600, 600}, nullptr, 0);
        CHECK(render->DirtyRectangleCount() == 1);
        render->AddDirtyRectangle(0, cRZRect{-10, -10, 10, 10}, nullptr, -100);
        // The merge heuristic (0x100769cca) absorbs the clipped rectangle into the queued one instead of
        // appending a second item while the cost stays above -2501.
        CHECK(render->DirtyRectangleCount() == 1);
        CHECK(render->DirtyRectangles().front().rect.left == 0);
        CHECK(render->DirtyRectangles().front().rect.top == 0);
        CHECK(render->DirtyRectangles().front().rect.right == 60);
        CHECK(render->DirtyRectangles().front().rect.bottom == 40);
        // A lighting change forces a full update, which clears the queue.
        CHECK(render->SetLightingEnabled(false));
        CHECK(render->DirtyRectangleCount() == 0);
        CHECK(render->DirtyRectangleAccumulatedArea() == 0);
        CHECK(!render->GetLightingEnabled());
        // While a full update is pending, rectangle capture is suppressed (quirk of +0x165).
        render->AddDirtyRectangle(0, cRZRect{0, 0, 10, 10}, nullptr, 0);
        CHECK(render->DirtyRectangleCount() == 0);
        CHECK(render->SetViewFogEnabled(true));
        CHECK(render->GetViewFogEnabled());
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- screen shake --------------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        CHECK(render->StartScreenShake(2.0f, 30.0f) == 0);
        CHECK(render->ShakeCount() == 1);
        const std::uint32_t serial = render->StartScreenShake(1.0f, 10.0f);
        CHECK(serial == 1);
        CHECK(render->ShakeCount() == 2);
        float x = 5, y = 5;
        render->GetShakeOffsets(x, y);
        CHECK(x == 0.0f && y == 0.0f);
        // The synthetic description StartScreenShake(f, f) builds has falloff 1, so stopping it shortens the
        // duration (0x10076929a) instead of removing it.
        render->StopScreenShake(0);
        CHECK(render->ShakeCount() == 2);
        render->StopScreenShake(1234);   // unknown serial does nothing
        CHECK(render->ShakeCount() == 2);
        // A description with a zero falloff is removed outright.
        cSC4ShakeDescription description;
        description.falloff = 0.0f;
        render->StartScreenShake(description, nullptr);
        CHECK(render->ShakeCount() == 3);
        render->StopScreenShake(2);
        CHECK(render->ShakeCount() == 2);
        CHECK(render->Release() == 0);
    }

    // ---- backing-store flags -------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        CHECK(render->GetBackingStoreEnabled() == true);
        CHECK(render->HasBackingStore());
        render->SetBackingStoreEnabled(false);
        CHECK(!render->GetBackingStoreEnabled());
        render->SetBackingStoreEnabled(true);
        CHECK(render->GetBackingStoreEnabled());
        CHECK(render->Release() == 0);
    }

    // ---- background picture --------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        CHECK(render->GetBackgroundPicture(nullptr) == nullptr);
        CHECK(render->SetBackgroundPicture(0x1234, 64, 32, true));
        CHECK(render->BackgroundTexture() == 0x1234);
        CHECK(render->BackgroundTextureWidth() == 64);
        CHECK(render->BackgroundTextureHeight() == 32);
        CHECK(render->BackgroundPictureVisible());
        bool visible = false;
        CHECK(render->GetBackgroundPicture(&visible) == nullptr);
        CHECK(visible);
        render->EnableBackgroundPicture(false);
        CHECK(!render->BackgroundPictureEnabled());
        render->EnableTransparentBackground(true);
        CHECK(render->TransparentBackground());
        render->SetViewportClearColor(nullptr);   // null is ignored
        CHECK(!render->ViewportClearColorValid());
        const float clear[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        render->SetViewportClearColor(clear);
        CHECK(render->ViewportClearColor()[0] == 0.1f && render->ViewportClearColor()[3] == 0.4f);
        CHECK(!render->ViewportClearColorValid());   // SetViewportClearColor clears the valid byte
        CHECK(render->Release() == 0);
    }

    // ---- map scale / stats / queries -----------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetMapScaleFactor(2);
        CHECK(render->MapScaleFactor() == 2);
        cRZString stats;
        render->GetStatsString1(stats);
        CHECK(std::strncmp(stats.ToChar(), "SPS:", 4) == 0);
        render->GetStatsString2(stats);
        CHECK(std::strncmp(stats.ToChar(), "QGT:", 4) == 0);
        std::uint32_t capacity = 0;
        CHECK(!render->GetModelsInVolume(cS3DBoundingBox{}, nullptr, capacity));
        CHECK(render->DoMessage(nullptr));   // partial: accepts and ignores
        CHECK(render->Release() == 0);
    }

    // ---- stream round trip ---------------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(320, 200);
        FakeOStream out;
        CHECK(render->Write(out));
        CHECK(out.sint16s == 1 && out.lastSint16 == 2);
        CHECK(out.ints.size() == 9);   // width, height, 4 x draw frustum, projection state, view state, camera view
        CHECK(out.ints[0] == 320 && out.ints[1] == 200);
        CHECK(out.ints[2] == 0 && out.ints[3] == 0 && out.ints[4] == 320 && out.ints[5] == 200);

        FakeIStream in;
        in.ints = {320, 200, 0, 0, 320, 200, 4, 5, 1};
        CHECK(render->Read(in));
        CHECK(render->CameraView() == 1);
        // A version other than 2 is rejected and leaves the object alone.
        FakeIStream bad;
        bad.version = 1;
        CHECK(!render->Read(bad));
        CHECK(render->Release() == 0);
    }

    // ---- the draw pass: frame start, viewport rectangles, clear ------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        render->SetBackingStoreEnabled(true);
        // A fresh renderer has no valid backing store and no clear colour yet: Draw clears the driver colour/depth
        // once, then ClearViewport issues the depth-only clear (the background picture is disabled by its 0 texture).
        const int clearColourBefore = driver.clearColourCalls;
        const int clearDepthBefore = driver.clearDepthCalls;
        const int clearBefore = driver.clearCalls;
        render->Draw();
        CHECK(driver.clearColourCalls == clearColourBefore + 1);
        CHECK(driver.lastClearColour[0] == 0.0f && driver.lastClearColour[3] == 0.0f);
        CHECK(driver.clearDepthCalls == clearDepthBefore + 1);
        CHECK(driver.lastClearDepth == 1.0);        // +0x2b0
        CHECK(driver.clearCalls == clearBefore + 1);
        CHECK(driver.lastClearMask == 0x1000);      // depth only: the ground area is not fully inside the city
        CHECK(render->ViewportClearColorValid());
        // The second frame does not repeat the colour / depth clear.
        render->Draw();
        CHECK(driver.clearColourCalls == clearColourBefore + 1);
        CHECK(driver.clearDepthCalls == clearDepthBefore + 1);
        CHECK(render->GetDrawContext()->ViewUpdateReason() == 0);   // Draw clears the reason
        CHECK(render->GetDrawContext()->ViewPass() == 5);           // the last list of the dynamic view

        // ClearViewportWithSolidColor: the colour clear restores the viewport clear colour afterwards.
        const float solid[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        const int clearBefore2 = driver.clearCalls;
        render->ClearViewportWithSolidColor(solid, true, true);
        CHECK(driver.clearColourCalls == clearColourBefore + 3);   // the solid colour, then the restore
        CHECK(driver.lastClearColour[0] == 0.0f);
        CHECK(driver.clearDepthCalls == clearDepthBefore + 2);
        CHECK(driver.clearCalls == clearBefore2 + 1);
        CHECK(driver.lastClearMask == 0x5000);
        render->ClearViewportWithSolidColor(solid, false, false);
        CHECK(driver.lastClearMask == 0x4000);                     // colour only

        // DrawViewportRectangle: one V3F_C4UB quad (format 1, 4 vertices, triangle strip).
        const int interleavedBefore = driver.interleavedCalls;
        const int drawBefore = driver.drawArrayCalls;
        render->DrawViewportRectangle(cRZRect{10, 20, 110, 70}, solid, true, true);
        CHECK(driver.interleavedCalls == interleavedBefore + 1);
        CHECK(driver.lastInterleavedFormat == 1);
        CHECK(driver.drawArrayCalls == drawBefore + 1);
        CHECK(driver.lastPrimitive == 1);
        CHECK(driver.lastDrawCount == 4);
        CHECK(render->CameraView() == 1);                          // Set2DCameraView

        // DrawTexturedViewportRectangle: one V3F_T2F quad (format 2) with the texture bound.
        const int textureBefore = driver.textureCalls;
        render->DrawTexturedViewportRectangle(cRZRect{0, 0, 32, 32}, 0xabc, 0.5f, false, true);
        CHECK(driver.interleavedCalls == interleavedBefore + 2);
        CHECK(driver.lastInterleavedFormat == 2);
        CHECK(driver.drawArrayCalls == drawBefore + 2);
        CHECK(driver.textureCalls > textureBefore);
        CHECK(driver.lastTexture == 0xabc);
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- the draw pass: dirty queue, partial / translated updates, view cleanup ---------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(400, 300);
        CHECK(render->Init());
        render->SetBackingStoreEnabled(true);
        render->SetViewportClearColor(nullptr);
        const int savesBeforeFrame = store.saves;
        render->Draw();                                   // establishes the clear colour
        // Draw's fallback branch redraws the static view and then saves the frame into the store.
        CHECK(render->BackingStoreValid());
        CHECK(store.saves == savesBeforeFrame + 1);

        // One dirty rectangle with a model: ProcessDirtyRectangleQueue draws that model once and saves its rectangle.
        FakeModel model;
        model.drawResult = 1;
        render->AddDirtyRectangle(0, cRZRect{10, 10, 60, 40}, &model, 0);
        CHECK(render->DirtyRectangleCount() == 1);
        const int savesBefore = store.saves;
        render->ProcessDirtyRectangleQueue();
        CHECK(model.draws == 1);
        CHECK(render->DirtyRectangleCount() == 0);
        CHECK(store.saves == savesBefore + 1);
        CHECK(store.lastSaveWidth == 50 && store.lastSaveHeight == 30);
        CHECK(store.lastSaveSrcX == 10 && store.lastSaveSrcY == 10);

        // A kind 2 rectangle takes the partial-update path instead: clip + clear + static redraw + save.
        render->AddDirtyRectangle(2, cRZRect{0, 0, 20, 20}, nullptr, 0);
        const int viewportBefore = driver.setViewportCalls;
        const int savesBefore2 = store.saves;
        render->ProcessDirtyRectangleQueue();
        CHECK(driver.setViewportCalls == viewportBefore + 1);   // SetClipRect
        CHECK(store.saves == savesBefore2 + 1);
        CHECK(store.lastSaveWidth == 20 && store.lastSaveHeight == 20);

        // DoPartialViewUpdate on its own.
        const int savesBefore3 = store.saves;
        render->DoPartialViewUpdate(cRZRect{5, 5, 25, 35}, cSC43DRender::kViewUpdateDirtyRectPartial);
        CHECK(store.saves == savesBefore3 + 1);
        CHECK(store.lastSaveWidth == 20 && store.lastSaveHeight == 30);

        // DoTranslatedViewUpdate: the store is restored shifted, the two uncovered strips redrawn and the shifted
        // region saved again; the cleanup band is queued for the following frames.
        render->ClearViewport(true);                       // make the store valid
        render->ForceFullRedraw();
        render->Draw();
        CHECK(render->BackingStoreValid());
        const int restoresBefore = store.restores;
        const int viewportBefore2 = driver.setViewportCalls;
        render->DoTranslatedViewUpdate(8, -4);
        CHECK(store.restores == restoresBefore + 1);
        CHECK(store.lastRestoreSrcX == 8 && store.lastRestoreSrcY == 4);
        CHECK(store.lastRestoreDstX == 0 && store.lastRestoreDstY == 0);
        CHECK(store.lastRestoreWidth == 392 && store.lastRestoreHeight == 296);
        CHECK(driver.setViewportCalls == viewportBefore2 + 2);   // the two strip clip rectangles
        CHECK(render->ViewCleanupPending());
        CHECK(render->CleanupRect()[0] == 0 && render->CleanupRect()[1] == 0);
        CHECK(render->CleanupRect()[2] == 392 && render->CleanupRect()[3] == 296);
        // The first DoViewCleanup only starts the trickle clock, so nothing is redrawn yet.
        const int savesBefore4 = store.saves;
        render->DoViewCleanup();
        CHECK(store.saves == savesBefore4);
        CHECK(render->ViewCleanupPending());

        // AttemptTranslatedViewUpdate: below 14/16 of the viewport it translates, and moves the queued rectangles.
        render->AddDirtyRectangle(2, cRZRect{100, 100, 140, 140}, nullptr, 0);
        const int restoresBefore2 = store.restores;
        CHECK(render->AttemptTranslatedViewUpdate(4, 6));
        CHECK(store.restores == restoresBefore2 + 1);
        CHECK(render->DirtyRectangleCount() == 1);
        CHECK(render->DirtyRectangles().front().rect.left == 96);
        CHECK(render->DirtyRectangles().front().rect.top == 106);
        // A translation of more than 14/16 of the viewport gives up and forces a full redraw.
        CHECK(!render->AttemptTranslatedViewUpdate(400, 0));
        CHECK(render->DirtyRectangleCount() == 0);
        CHECK(!render->BackingStoreValid());
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- the draw pass: the two quad-grid views ---------------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        render->SetBackingStoreEnabled(true);
        CHECK(render->QuadGrid() != nullptr);
        // The visible container rectangle the passes scan (the zeroed view utilities project the viewport corners
        // onto the fixed-view origin, so it is the rotation-0 fixed-view offset). The elements go into the one
        // quadrant it covers.
        SC4RectFloat visible{};
        render->GetVisibleContainerRect(visible, cISC43DRender::kFrustumDraw);
        CHECK(visible.x0 == 3000.0f && visible.z0 == 4500.0f);
        CHECK(visible.x1 == visible.x0 && visible.z1 == visible.z0);

        // A static element (flag 8).
        FakeModel staticModel;
        staticModel.drawResult = 1;
        auto* staticElement = new SC43DGridElement();
        staticElement->model = &staticModel;
        staticElement->flags = 8;
        staticElement->boundsMinX = 0.0f;
        staticElement->boundsMinY = 0.0f;
        staticElement->boundsMinZ = 0.0f;
        staticElement->boundsMaxX = 10.0f;
        staticElement->boundsMaxY = 10.0f;
        staticElement->boundsMaxZ = 10.0f;
        render->QuadGrid()->SetGridRectForElement(visible, staticElement);
        CHECK(render->QuadGrid()->AddElement(staticElement, 1));
        render->DrawStaticView();
        CHECK(staticModel.draws == 1);
        CHECK(render->ModelSortCount(0) == 0);   // the lists are cleared after the pass
        CHECK(render->GetDrawContext()->ViewPass() == 1);
        CHECK(render->CameraView() == 0);        // the pass itself does not switch the camera

        // A dynamic element (no static flag) takes the dynamic pass; flag 0x80 also collects it for the highlight
        // pass, which is the only place a model is drawn with highlight = true.
        FakeModel dynamicModel;
        dynamicModel.drawResult = 1;
        auto* dynamicElement = new SC43DGridElement();
        dynamicElement->model = &dynamicModel;
        dynamicElement->flags = 0x80;
        render->QuadGrid()->SetGridRectForElement(visible, dynamicElement);
        CHECK(render->QuadGrid()->AddElement(dynamicElement, 1));
        render->DrawDynamicView();
        CHECK(dynamicModel.draws == 2);          // once in the sorted list, once in the highlight pass
        CHECK(dynamicModel.highlightDraws == 1);
        CHECK(render->ModelSortCount(0) == 0);
        CHECK(render->ModelSortCount(1) == 0);
        CHECK(render->ModelSortCount(2) == 0);
        // The highlight list keeps the last pass's elements until the next DrawDynamicView clears it (quirk of
        // +0x198: only the pass that fills it empties it).
        CHECK(render->HighlightElements().size() == 1);
        CHECK(render->HighlightElements().front() == dynamicElement);
        CHECK(render->GetDrawContext()->ViewPass() == 4);   // the dynamic view's own pass
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- the world -> container rectangle (0x1007620c4) -------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        // Before Init the cached grid size (+0x190) is 0 and the fixed-view offsets are 0, so both projected corners
        // are (0, 0): the clamp path runs and the degenerate size is repaired. The two projected corners are 2 units
        // outside the box, get clamped to 0.5 and to gridSize - 0.5 = -0.5, then the 0.1 pull-back and the "wider than
        // half the grid" cap give a 1.0 x 1.0 rectangle.
        auto* render = new cSC43DRender();
        render->AddRef();
        const float boxMin[3] = {0.0f, 0.0f, 0.0f};
        const float boxMax[3] = {10.0f, 10.0f, 10.0f};
        for (int rotation = 0; rotation < 4; ++rotation) {
            SC4RectFloat rect;
            render->WorldToContainerRect(boxMin, boxMax, rect, rotation);
            // The zeroed view utilities flatten every 3D point to (0, 0), so all four rotations land on the same
            // clamped rectangle; what is asserted here is the clamp / repair, which the rotations do not change.
            CHECK(Near(rect.x0, -0.6f) && Near(rect.z0, -0.6f));
            CHECK(Near(rect.x1, 0.4f) && Near(rect.z1, 0.4f));
        }
        // GetViewportRayContainerRect: the 2 x 2 window around the (0, 0) container point, pulled to gridSize - 2
        // = -2 here because the grid has not been created yet, then repaired to the one-unit minimum.
        SC4RectFloat rayRect;
        render->GetViewportRayContainerRect(10, 20, rayRect);
        CHECK(Near(rayRect.x0, -2.0f) && Near(rayRect.z0, -2.0f));
        CHECK(Near(rayRect.x1, 2.0f) && Near(rayRect.z1, 2.0f));
        CHECK(render->Release() == 0);
    }

    // ---- the model change queue: an inserted model reaches the static draw pass --------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        render->SetBackingStoreEnabled(true);
        CHECK(render->QuadGrid() != nullptr);

        FakeModel model;
        model.properties = 2;   // cS3DModelInstanceBasic's default: bit 2 is "static model"
        model.worldBox.min[0] = model.worldBox.min[1] = model.worldBox.min[2] = 0.0f;
        model.worldBox.max[0] = model.worldBox.max[1] = model.worldBox.max[2] = 10.0f;
        CHECK(!render->IsModelInserted(&model));
        CHECK(render->InsertModel(&model));

        // InsertModel built the grid element, registered it at the model's custom data and put it in the grid.
        auto* element = static_cast<SC43DGridElement*>(model.customData);
        CHECK(element != nullptr);
        CHECK(element->model == &model);
        CHECK(render->QuadGrid()->ElementCount() == 1);
        // The zeroed view utilities map the world box onto the single visible container cell (the same (3000, 4500)
        // corner GetVisibleContainerRect reports), so the element covers the quadrant (93, 140) only.
        CHECK(element->x0 == 93 && element->z0 == 140 && element->x1 == 93 && element->z1 == 140);
        CHECK(render->QuadGrid()->Quadrant(93, 140).flags & 1);
        // Flag 4 is "in the grid"; the element is static-capable (0x40 draw bit + the 0x800 zoom group) but only the
        // drain turns it into a static (0x8) element.
        CHECK((element->flags & 0x4) != 0);
        CHECK((element->flags & 0x40) != 0);
        CHECK((element->flags & 0x800) != 0);
        CHECK((element->flags & 0x8) == 0);
        CHECK((element->flags & 0x10) != 0);   // queued
        CHECK(render->ModelChanges().size() == 1);
        CHECK(render->ModelChanges().front().model == &model);
        CHECK(render->ModelChanges().front().change == 0);
        CHECK(render->IsModelInserted(&model));

        // Draw drains the queue before the static pass: the element becomes static and the model is drawn by
        // DrawStaticView. This is the whole point of the queue (RENDER3D-2).
        render->Draw();
        CHECK(render->ModelChanges().empty());
        CHECK((element->flags & 0x10) == 0);
        CHECK((element->flags & 0x8) != 0);
        CHECK(model.draws == 1);   // once, by the front-to-back static list
        CHECK(render->ModelSortCount(0) == 0);
        CHECK(render->BackingStoreValid());   // the pass was saved into the store

        // The next frame restores the store instead of redrawing the static view, so the model is not drawn again
        // (the insert's dirty rectangle projects to nothing here: the view utilities are zeroed).
        render->Draw();
        CHECK(render->ModelChanges().empty());
        CHECK(model.draws == 1);

        // ---- MoveModel: a static element is queued with the box it is leaving ---------------------------------
        model.worldBox.max[0] = 20.0f;
        CHECK(render->MoveModel(&model));
        CHECK(render->ModelChanges().size() == 1);
        CHECK(render->ModelChanges().front().change == 2);
        CHECK(Near(render->ModelChanges().front().boundsMin[0], 1.0f) == false);   // an element box, not the empty one
        CHECK(Near(render->ModelChanges().front().boundsMax[0], 10.0f));           // the box before the move
        CHECK(render->IsModelInserted(&model));
        render->ProcessModelChangeQueue();
        CHECK(render->ModelChanges().empty());
        CHECK(Near(element->boundsMaxX, 20.0f));   // MoveGridElement refreshed the element's box
        render->DrawStaticView();
        CHECK(model.draws == 2);

        // ---- ChangeModel: 1 toggles the 0x80 draw-attribute bit, 2/3/5 queue a changed element -----------------
        model.attributes.flags = 0x0500;               // a non-zero byte at +1 of the draw attributes
        CHECK(render->ChangeModel(&model, 1));
        CHECK(render->ModelChanges().empty());        // no dirty rectangle for this one
        CHECK((element->flags & 0x80) != 0);          // set: the dynamic pass draws it too
        CHECK(render->QuadGrid()->Quadrant(93, 140).flags & 1);
        model.attributes.flags = 0;
        CHECK(render->ChangeModel(&model, 1));
        CHECK((element->flags & 0x80) == 0);          // cleared again
        CHECK(render->ChangeModel(&model, 3));
        CHECK(render->ModelChanges().size() == 1);
        CHECK(render->ModelChanges().front().change == 4);   // the 0x05050403 table maps 3 -> 4
        render->ProcessModelChangeQueue();
        CHECK(render->ModelChanges().empty());
        render->DrawStaticView();
        CHECK(model.draws == 3);
        CHECK(render->ChangeModel(&model, 5));
        CHECK(render->ModelChanges().size() == 1);
        CHECK(render->ModelChanges().front().change == 5);
        render->ProcessModelChangeQueue();
        CHECK(render->ModelChanges().empty());
        CHECK((element->flags & 0x8) != 0);           // still static after the property refresh
        render->DrawStaticView();
        CHECK(model.draws == 4);

        // ---- IsModelInserted drains a queue longer than eight items ------------------------------------------
        for (int i = 0; i < 9; ++i) CHECK(render->MoveModel(&model));
        CHECK(render->ModelChanges().size() == 9);
        CHECK(render->IsModelInserted(&model));
        CHECK(render->ModelChanges().empty());
        CHECK((element->flags & 0x10) == 0);

        // ---- RemoveModel: a static element leaves through the queue -------------------------------------------
        CHECK(render->RemoveModel(&model));
        CHECK(render->ModelChanges().size() == 1);
        CHECK(render->ModelChanges().front().change == 1);
        CHECK(model.customData == element);           // still in the grid, flagged 0x10
        render->ProcessModelChangeQueue();             // the drain removes it
        CHECK(render->ModelChanges().empty());
        CHECK(model.customData == nullptr);
        CHECK(render->QuadGrid()->ElementCount() == 0);
        CHECK(!render->IsModelInserted(&model));
        render->DrawStaticView();                      // nothing left to collect
        CHECK(model.draws == 4);

        // ---- the InsertModel quirk: an already-queued model gets a second element -----------------------------
        CHECK(render->InsertModel(&model));
        CHECK(render->ModelChanges().size() == 1);
        auto* firstElement = static_cast<SC43DGridElement*>(model.customData);
        CHECK(firstElement != nullptr);
        CHECK(render->InsertModel(&model));             // drains the queue, then builds another element
        CHECK((firstElement->flags & 0x8) != 0);         // the drain turned the first element static
        CHECK((firstElement->flags & 0x10) == 0);
        CHECK(render->ModelChanges().size() == 1);       // and the second insert queued itself
        CHECK(render->ModelChanges().front().change == 0);
        CHECK(model.customData != static_cast<void*>(firstElement));
        CHECK(render->QuadGrid()->ElementCount() == 2);  // the drained element is still in the grid (leak kept)
        CHECK(render->Shutdown());                       // ...which the grid teardown releases
        CHECK(render->Release() == 0);
    }

    // ---- the model change queue: dynamic models, the zoom groups and the grid rebuild -------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        render->SetBackingStoreEnabled(true);

        // properties 0 leaves the 0x40 bit clear, so ModelShouldGoIntoBackingStore is false and nothing is queued.
        FakeModel dynamicModel;
        dynamicModel.worldBox.max[0] = dynamicModel.worldBox.max[1] = dynamicModel.worldBox.max[2] = 4.0f;
        CHECK(render->InsertModel(&dynamicModel));
        auto* dynamicElement = static_cast<SC43DGridElement*>(dynamicModel.customData);
        CHECK(dynamicElement != nullptr);
        CHECK((dynamicElement->flags & 0x40) == 0);
        CHECK((dynamicElement->flags & 0x800) != 0);   // no property zoom bit -> the 0x800 group
        CHECK((dynamicElement->flags & 0x8) == 0);
        CHECK(render->ModelChanges().empty());          // no backing-store work for a dynamic element
        CHECK(render->MoveModel(&dynamicModel));        // moves in place, no queue item
        CHECK(render->ModelChanges().empty());
        CHECK(render->ChangeModel(&dynamicModel, 3));   // dynamic: only change 5 is queued
        CHECK(render->ModelChanges().empty());

        FakeModel staticModel;
        staticModel.properties = 2;
        staticModel.worldBox.max[0] = staticModel.worldBox.max[1] = staticModel.worldBox.max[2] = 8.0f;
        CHECK(render->InsertModel(&staticModel));
        auto* staticElement = static_cast<SC43DGridElement*>(staticModel.customData);
        CHECK(staticElement != nullptr);
        CHECK(render->QuadGrid()->ElementCount() == 2);
        render->Draw();
        CHECK(render->ModelChanges().empty());
        CHECK((staticElement->flags & 0x8) != 0);
        // The dynamic element is drawn by the dynamic pass, the static one by the static pass.
        CHECK(dynamicModel.draws == 1);
        CHECK(staticModel.draws == 1);

        // ReevaluateBackingStoreObjectFlags: at zoom 3 (the fake's threshold for the 0x1800 group) the static
        // element no longer belongs in the store; every quadrant is marked filled either way.
        const int dimension = render->QuadGrid()->Dimension();
        render->GetDrawContext()->SetZoomAndRotation(3, 0);
        render->ReevaluateBackingStoreObjectFlags();
        CHECK((staticElement->flags & 0x8) == 0);
        CHECK((dynamicElement->flags & 0x8) == 0);
        CHECK(render->QuadGrid()->Quadrant(0, 0).flags & 1);
        CHECK(render->QuadGrid()->Quadrant(dimension - 1, dimension - 1).flags & 1);
        render->GetDrawContext()->SetZoomAndRotation(0, 0);
        render->ReevaluateBackingStoreObjectFlags();
        CHECK((staticElement->flags & 0x8) != 0);
        CHECK((dynamicElement->flags & 0x8) == 0);

        // RebuildQuadGrid: no zoom / rotation change does nothing; a zoom change re-creates the grid and every
        // element (the models' custom data moves to the new elements, the old ones die with the old grid).
        SC4QuadGrid* const oldGrid = render->QuadGrid();
        render->RebuildQuadGrid(0, 0, 0, 0);
        CHECK(render->QuadGrid() == oldGrid);
        render->RebuildQuadGrid(0, 0, 2, 0);
        CHECK(render->QuadGrid() != oldGrid);
        CHECK(render->QuadGrid()->ElementCount() == 2);
        CHECK(staticModel.customData != static_cast<void*>(staticElement));
        CHECK(dynamicModel.customData != static_cast<void*>(dynamicElement));
        auto* rebuiltStatic = static_cast<SC43DGridElement*>(staticModel.customData);
        CHECK(rebuiltStatic != nullptr && rebuiltStatic->model == &staticModel);
        CHECK((rebuiltStatic->flags & 0x8) != 0);
        CHECK(Near(rebuiltStatic->boundsMaxX, 8.0f));
        // The rebuilt elements are drawn by the next static pass.
        const int staticDraws = staticModel.draws;
        render->DrawStaticView();
        CHECK(staticModel.draws == staticDraws + 1);

        // Removing a dynamic model takes the immediate path: no queue item, the element goes at once.
        CHECK(render->RemoveModel(&dynamicModel));
        CHECK(render->ModelChanges().empty());
        CHECK(dynamicModel.customData == nullptr);
        CHECK(render->QuadGrid()->ElementCount() == 1);
        CHECK(render->RemoveModel(&staticModel));
        CHECK(render->ModelChanges().size() == 1);
        render->Draw();
        CHECK(render->QuadGrid()->ElementCount() == 0);
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    // ---- the camera notifications that rebuild the grid -------------------------------------------------------
    {
        FakeHost host;
        SetRender3DHost(&host);
        auto* render = new cSC43DRender();
        render->AddRef();
        render->SetViewportSize(640, 480);
        CHECK(render->Init());
        render->SetBackingStoreEnabled(true);
        FakeModel model;
        model.properties = 2;
        model.worldBox.max[0] = model.worldBox.max[1] = model.worldBox.max[2] = 6.0f;
        CHECK(render->InsertModel(&model));
        render->Draw();
        CHECK(render->BackingStoreValid());
        auto* element = static_cast<SC43DGridElement*>(model.customData);
        CHECK(element != nullptr);

        // DoViewRotationChange2: the grid is rebuilt for the new rotation, the store and the dirty rectangles go.
        render->AddDirtyRectangle(0, cRZRect{0, 0, 10, 10}, nullptr, 0);
        CHECK(render->DirtyRectangleCount() == 1);
        render->DoViewRotationChange2(0, 0, 0, 1);
        CHECK(render->GetDrawContext()->ViewRotation() == 1);
        CHECK(!render->BackingStoreValid());
        CHECK(render->DirtyRectangleCount() == 0);
        CHECK(render->QuadGrid()->ElementCount() == 1);
        CHECK(model.customData != static_cast<void*>(element));   // the element was re-created
        // Rotation 1 moves the container origin to (1400, 6400): (1400 / 32 - 0.5) = 43.25 -> cell 43 and
        // (6400 / 32 - 0.5) = 199.5 -> cell 199 (the cell size stays 32 at map scale 1).
        auto* rotated = static_cast<SC43DGridElement*>(model.customData);
        CHECK(rotated != nullptr);
        CHECK(rotated->x0 == 43 && rotated->z0 == 199);
        CHECK((rotated->flags & 0x8) != 0);

        // The same rotation again is a no-op (the handler compares the two rotations).
        render->DoViewRotationChange2(0, 1, 0, 1);
        CHECK(model.customData == static_cast<void*>(rotated));

        // DoViewZoomChange3: below the +0x278 threshold (the fake answers 3) the backing store stays enabled, and
        // every element's static flag is re-evaluated at the new zoom.
        render->DoViewZoomChange3(0, 1, 2, 1);
        CHECK(render->GetDrawContext()->Zoom() == 2);
        CHECK(render->GetBackingStoreEnabled());
        CHECK(render->QuadGrid()->ElementCount() == 1);
        CHECK((static_cast<SC43DGridElement*>(model.customData)->flags & 0x8) != 0);
        // At zoom 3 the 0x1800 group reaches its threshold (the fake answers 3 for +0x2c0): the store is disabled
        // and the element stops being static.
        render->DoViewZoomChange3(2, 1, 3, 1);
        CHECK(!render->GetBackingStoreEnabled());
        CHECK((static_cast<SC43DGridElement*>(model.customData)->flags & 0x8) == 0);
        // Back to zoom 0: the store is enabled again and the element is static again.
        render->DoViewZoomChange3(3, 1, 0, 1);
        CHECK(render->GetBackingStoreEnabled());
        CHECK((static_cast<SC43DGridElement*>(model.customData)->flags & 0x8) != 0);
        CHECK(render->Shutdown());
        CHECK(render->Release() == 0);
    }

    (void)driver;
    if (failures == 0) std::printf("sc4_3drender: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
