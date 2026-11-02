// cSC43DRender — see src/sc4/Render3D.h for the layout and the quirks. This file holds the object's lifecycle
// (0x10075f3a8..0x100760dbc), the render-property read (0x100760546), the view-object lists (0x10075f3e6..0x10075f4ce,
// 0x1007693d0..0x100769508), the camera / clip / driver / backing-store wiring, the screen-shake list, the two
// stats strings, the per-frame draw pass (0x100763ac6..0x1007663a8) and — since RENDER3D-2 — the model change queue
// and the quad-grid rebuild (0x10076113a..0x1007657c6). What is left in the class is the camera / city message
// handling (DoMessage 0x100762b36, UpdateVisibleTerrainMetrics 0x1007630ca) and picking; the pending ledger file
// says why.
#include "sc4/Render3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gz/Buffer.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZVariant.h"
#include "render/gd/VideoModes.h"
#include "s3d/ModelInstance.h"
#include "rz/cRZRandom.h"

#include "sc4/CameraControl.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/DBSegment.h"
#include "sc4/QuadGrid.h"
#include "sc4/RenderProperties.h"

namespace sc4 {

namespace {
// Defined with the draw pass further down; declared here so the small services above can read the same seam.
IRender3DSettings& RendererSettings();

// The binary's file-scope tables (0x100a2a170 / 0x100a2a180), indexed by camera rotation.
constexpr float kFVToCXBase[4] = {3000.0f, 1400.0f, 5200.0f, 6800.0f};
constexpr float kFVToCYBase[4] = {4500.0f, 6400.0f, 7200.0f, 5300.0f};
// City size Init stores into the view utilities (+0x28 / +0x2c of SC4ViewUtilities, 0x45800000).
constexpr float kDefaultCitySize = 4096.0f;

// Driver extension ids StaticSetGDriver queries (0x1007614c6): the buffer-region extension (the backing store in this
// port) and the colour-buffer extension (snapshots, unused here).
constexpr std::uint32_t kIIDBufferRegionExtension = 0x669565fe;
constexpr std::uint32_t kIIDSnapshotExtension = 0xe69bfe2a;
// cSC4CameraControl's class id, used by Read for the serialized sub-object (0x1007612a2).
constexpr std::uint32_t kCLSIDCameraControl = 0xc9c628ec;
// The message types Init subscribes to and Shutdown unsubscribes (0x1007602xx / 0x100760dbc).
constexpr std::uint32_t kRender3DMessages[] = {
    0xa6b79601, 0xa6b79602, 0xa6b79612, 0x870bc918, 0xa6b79621, 0x26d31ec1,
    0xc9f775bb, 0xc53d10aa, 0x4a9f2ffe, 0x8acaa95f, 0x6acb92a9,
};
// The highlight-colour properties ReadProperties reads: 0xea639fb1..0xea639fbb -> draw-context slots 0..10.
constexpr std::uint32_t kHighlightPropertyBase = 0xea639fb1;
constexpr int kHighlightPropertyCount = 11;

// SC4DrawContext wants the view utilities through its one-field seam; SC4ViewUtilities keeps +0x15c private, so the
// adapter inverts ViewportUnitsToWorldUnits(1). UpdateCameraPosition writes +0x15c = PixelPerWorld.
class ViewUtilitiesSeam final : public IS4ViewUtilities {
public:
    explicit ViewUtilitiesSeam(SC4ViewUtilities* utilities) : utilities_(utilities) {}
    float ViewportUnitsPerWorld() const override {
        const float worldPerUnit = utilities_ ? utilities_->ViewportUnitsToWorldUnits(1.0f) : 0.0f;
        return worldPerUnit != 0.0f ? 1.0f / worldPerUnit : 0.0f;
    }

private:
    SC4ViewUtilities* utilities_;
};

// The settings seam over the port's cSC4RenderProperties. The original reads fields of the inner cProperties object
// (SL::spRenderProperties +0x18); the port has the generated default table, so the values below are the ones the
// decompilation compares against, and the tests supply their own IRender3DSettings fake.
class DefaultSettings final : public IRender3DSettings {
public:
    bool DisablePartialRegionWrites() const override { return false; }   // +0x7b8
    bool ClipRectSupported() const override { return true; }             // +0x638
    int BackingStoreMaxZoom() const override { return 3; }               // +0x278
    int StaticViewMaxZoomA() const override { return 3; }                // +0x308
    int StaticViewMaxZoomB() const override { return 3; }                // +0x2c0
    int StaticViewMaxZoomC() const override { return 3; }                // +0x350
    bool StaticViewRedrawOnViewportChange() const override { return true; }   // +0xb8
    bool DrawDynamicViewEnabled() const override { return true; }        // +0x78
    bool ViewCleanupEnabled() const override { return true; }            // +0x938
    bool ClearViewportPreservesLighting() const override { return false; }   // +0x738
    bool TranslatedUpdateRebuildsDrawFrustum() const override { return true; }   // +0x438
    bool ScreenShakeEnabled() const override { return true; }            // +0x5b8
    int ScreenShakeOffsetX() const override { return 0; }                // +0x30 block, +0x80
    int ScreenShakeOffsetY() const override { return 0; }                // +0x30 block, +0x38
    int DirtyRectangleMaxAgeFrames() const override { return 10000; }    // +0x30 block, +0x548
};

struct StreamReader {
    cIGZIStream* stream;
    bool ok = true;
    std::int32_t Int() {
        std::int32_t value = 0;
        ok = stream->GetSint32(value) && ok;
        return value;
    }
    std::uint32_t Uint() {
        std::uint32_t value = 0;
        ok = stream->GetUint32(value) && ok;
        return value;
    }
};

struct StreamWriter {
    cIGZOStream* stream;
    bool ok = true;
    void Int(std::int32_t value) { ok = stream->SetSint32(value) && ok; }
    void Uint(std::uint32_t value) { ok = stream->SetUint32(value) && ok; }
};

}  // namespace

IRender3DHost* Render3DHost() { return cSC43DRender::Host(); }
void SetRender3DHost(IRender3DHost* host) { cSC43DRender::SetHost(host); }

// ---------------------------------------------------------------------------------------------
// cSC43DRender: construction / destruction / COM
// ---------------------------------------------------------------------------------------------
cSC43DRender::cSC43DRender() {
    // The constructor always allocates the view utilities and the draw context (0x1007603a2 / 0x1007603b6); the quad
    // grid comes from Init.
    viewUtilities_ = new SC4ViewUtilities();
    viewUtilitiesCameraHost_.owner = this;
    drawContext_ = new SC4DrawContext();
    frameTimer_.SetUnits(cRZTimer::kMilliseconds);   // cRZTimer::ConstructWithUnits(this + 0x340, 4)
    shakeTimer_.SetUnits(cRZTimer::kSeconds);        // cRZTimer::ConstructWithUnits(this + 0x380, 5)
    lightLevels_[3] = 10000.0f;
    lightLevels_[4] = 15000.0f;
    fogEnd_ = 15000.0f;
    unknown358_ = 10000;
    spInstance = this;
}

cSC43DRender::~cSC43DRender() {
    Shutdown();
    delete viewUtilities_;
    viewUtilities_ = nullptr;
    delete drawContext_;
    drawContext_ = nullptr;
    // The shake list holds plain records; the dirty-rectangle list and the model queue hold referenced models.
    for (DirtyRectangleQueueItem& item : dirtyRectangles_) {
        if (item.model) item.model->Release();
    }
    dirtyRectangles_.clear();
    for (ModelChangeQueueItem& item : modelChanges_) {
        if (item.model) item.model->Release();
    }
    modelChanges_.clear();
    shakes_.clear();
    for (std::list<ViewObjectInfo>& list : viewObjectLists_) ClearViewObjectList(list);
    if (backgroundPicture_) backgroundPicture_->Release();
    backgroundPicture_ = nullptr;
    if (terrainView_) terrainView_->Release();
    terrainView_ = nullptr;
    if (backgroundViewObject_) backgroundViewObject_->Release();
    backgroundViewObject_ = nullptr;
    if (cameraControl_) cameraControl_->Release();
    cameraControl_ = nullptr;
    if (camera_ && camera_->Release() == 0) delete camera_;
    camera_ = nullptr;
    if (uiCamera_ && uiCamera_->Release() == 0) delete uiCamera_;
    uiCamera_ = nullptr;
    if (lightingManager_) delete lightingManager_;
    lightingManager_ = nullptr;
    if (renderProperties_) renderProperties_->Release();
    renderProperties_ = nullptr;
    if (graphicSystem_) graphicSystem_->Release();
    graphicSystem_ = nullptr;
    if (spInstance == this) spInstance = nullptr;
}

bool cSC43DRender::QueryInterface(std::uint32_t iid, void** out) {
    // 0x10075feee: an unknown id returns false and writes nothing; a hit is AddRef'd.
    if (out == nullptr) return false;
    if (iid == 1 || iid == kCLSID) {
        *out = static_cast<cISC43DRender*>(this);
    } else if (iid == 0x681bd529 || iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == 0xe4fda3d4) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC43DRender::AddRef() { return ++refCountPrimary_; }

std::uint32_t cSC43DRender::Release() {
    // 0x10075fe36: below 2 the original calls Shutdown (vslot 0x20) and then the deleting destructor (vslot 0x260),
    // so the last release frees the object. Heap-allocate a renderer before releasing it to zero (AGENT_RULES: COM
    // objects must not be stack-allocated when a test releases them for real).
    if (refCountPrimary_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCountPrimary_;
}

// ---------------------------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::StaticInit(void* context) {
    (void)context;
    if (!mbStaticInitialized) {
        mbStaticInitialized = true;
        if (host_) host_->RegisterStaticShutdown(&cSC43DRender::StaticShutdown, nullptr);
    }
    return true;
}

bool cSC43DRender::StaticShutdown(void* context) {
    (void)context;
    if (mbStaticInitialized) {
        mbStaticInitialized = false;
        if (mpGDriver) {
            if (mpBackingStoreExtension) {
                mpBackingStoreExtension->Destroy();
                mpBackingStoreExtension = nullptr;
            }
            mpGDriver->Release();
            mpGDriver = nullptr;
        }
        mpDriverSnapshotExtension = nullptr;
    }
    return true;
}

void cSC43DRender::StaticSetGDriver(gd::cIGZGDriver* driver) {
    // 0x1007614c6
    if (mpGDriver == driver) return;
    if (mpGDriver && mpBackingStoreExtension) {
        mpBackingStoreExtension->Destroy();   // _glDeleteFramebuffersEXT
        mpBackingStoreExtension = nullptr;
    }
    if (driver) driver->AddRef();
    if (mpGDriver) mpGDriver->Release();
    mpGDriver = driver;
    mpDriverSnapshotExtension = nullptr;
    if (driver) {
        void* extension = nullptr;
        if (driver->Punt(kIIDBufferRegionExtension, &extension))
            mpBackingStoreExtension = static_cast<gd::IBackingStore*>(extension);
        void* snapshot = nullptr;
        if (driver->Punt(kIIDSnapshotExtension, &snapshot)) mpDriverSnapshotExtension = snapshot;
        if (mpBackingStoreExtension) CreateBackingStore();
    }
}

bool cSC43DRender::CreateBackingStore() {
    // 0x1007699ee: the size is the driver viewport (GetViewport returns x, y, x + w, y + h). The GL renderbuffers are
    // the driver's IBackingStore service in this port (the recovered logic lives in cSC43DBackingStore).
    if (!mpBackingStoreExtension || !mpGDriver) return false;
    std::uint32_t viewport[4] = {0, 0, 0, 0};
    mpGDriver->GetViewport(viewport);
    const bool ok = mpBackingStoreExtension->Create(viewport[2] - viewport[0], viewport[3] - viewport[1]);
    if (spInstance) spInstance->backingStoreValid_ = ok;
    return ok;
}

bool cSC43DRender::HasBackingStore() { return mpBackingStoreExtension != nullptr; }

// ---------------------------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::Init() {
    // 0x10076002a
    if (initialized_) return true;
    if (!mbStaticInitialized) StaticInit(nullptr);

    quadGrid_ = CreateQuadGrid();
    for (int i = 0; i < 4; ++i) {
        fixedViewToContainerX_[i] = static_cast<float>(mapScaleFactor_) * kFVToCXBase[i];
        fixedViewToContainerY_[i] = static_cast<float>(mapScaleFactor_) * kFVToCYBase[i];
    }

    // SL::spRenderProperties: the port host supplies the set; Init takes its own reference (0x1007601e2).
    if (host_) {
        cSC4RenderProperties* properties = host_->RenderProperties();
        if (properties != renderProperties_) {
            if (renderProperties_) renderProperties_->Release();
            renderProperties_ = properties;
            if (renderProperties_) renderProperties_->AddRef();
        }
    }

    IRender3DSettings* settings = host_ ? host_->Settings() : nullptr;
    DefaultSettings defaults;
    if (!settings) settings = &defaults;
    // +0xaf / +0xb0: the buffer-region extension answers "can do partial region writes" at vslot 0x48.
    partialWritesSupported_ = mpBackingStoreExtension != nullptr;
    partialWritesAllowed_ = partialWritesSupported_ && !settings->DisablePartialRegionWrites();

    // The camera / city / property notifications (0x10076025a).
    if (host_) {
        if (cIGZMessageServer2* server = host_->MessageServer()) {
            for (std::uint32_t type : kRender3DMessages) server->AddNotification(this, type);
        }
    }

    // Lighting manager (+0x178, 0x240 bytes in the original, 0x1007602f6).
    if (!lightingManager_ && host_) lightingManager_ = host_->CreateLightingManager();

    // The orthographic camera is +0x130; the 3D camera +0xd8 comes from SetCamera (the camera control owns it), so
    // Init only creates the ortho one (0x1007602f6).
    if (!uiCamera_) {
        uiCamera_ = new s3d::cS3DCamera();
        uiCamera_->AddRef();
        uiCamera_->Init();
        uiCamera_->SetViewport(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_));
        uiCamera_->SetOrtho(0.0f, static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), 0.0f,
                            0.0f, 32767.0f);
        uiCamera_->MakeIdentity();
        std::memcpy(projectionMatrix_, uiCamera_->ProjectionMatrix(), sizeof(projectionMatrix_));
    }

    viewUtilities_->SetHost(&viewUtilitiesCameraHost_);   // the original stores the renderer itself at +8
    viewUtilities_->SetCitySize(kDefaultCitySize, kDefaultCitySize);
    if (cameraControl_) viewUtilities_->UpdateCameraPosition(cameraControl_);
    viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);

    viewContainerRectValid_ = false;
    drawContainerRectValid_ = false;
    drawContext_->SetRenderPass(0);
    drawContext_->SetDriver(mpGDriver);
    drawContext_->SetRenderProperties(renderProperties_);
    drawContext_->SetCameraControl(cameraControl_);
    drawContext_->SetLightingManager(lightingManager_);
    drawContext_->SetViewportSize(viewportWidth_, viewportHeight_);
    viewUtilitiesSeam_ = std::make_unique<ViewUtilitiesSeam>(viewUtilities_);
    drawContext_->SetViewUtilities(viewUtilitiesSeam_.get());
    drawContext_->SetZoomAndRotation(viewUtilities_->Zoom(), viewUtilities_->Rotation());
    drawContext_->Init();

    ReadProperties();
    dirtyMaxArea_ = 0x7fffffff;   // +0x304
    unknown320_ = 40000;          // +0x320
    initialized_ = true;
    return true;
}

SC4QuadGrid* cSC43DRender::CreateQuadGrid() {
    // 0x1007604c2
    const float size = static_cast<float>(mapScaleFactor_) * 8192.0f;
    unknown190_ = size;   // +0x190, the same value the original caches
    SC4QuadGrid* grid = new SC4QuadGrid(1.0f, 1.0f);
    grid->SetSize(size, static_cast<float>(mapScaleFactor_) * 32.0f);
    return grid;
}

bool cSC43DRender::Shutdown() {
    // 0x100760dbc
    if (!initialized_) return true;
    ClearModelChangeQueue();
    if (quadGrid_) {
        // Every element gives up its model (the reference InsertModel took for the grid) before the grid lets go of the
        // elements. The port also clears the model's link to its element: an occupant's placeable object that is hidden
        // after this renderer is gone (an old city's queued removal reaching the next city's model maker) then finds no
        // element instead of a freed one.
        int count = quadGrid_->ElementCount();
        std::vector<SC4GridElement*> elements(static_cast<std::size_t>(count));
        if (count > 0) quadGrid_->GetAllElements(elements.data(), count);
        for (int i = 0; i < count; ++i) {
            auto* element = static_cast<SC43DGridElement*>(elements[static_cast<std::size_t>(i)]);
            if (s3d::cIS3DModelInstance* model = element->model) {
                if (model->GetCustomData() == element) model->SetCustomData(nullptr);
                model->Release();
            }
            element->model = nullptr;
        }
        quadGrid_->RemoveAllElements();
        delete quadGrid_;
        quadGrid_ = nullptr;
    }
    for (std::list<ViewObjectInfo>& list : viewObjectLists_) ClearViewObjectList(list);
    if (drawContext_) drawContext_->Shutdown();
    if (host_) {
        if (cIGZMessageServer2* server = host_->MessageServer()) {
            for (std::uint32_t type : kRender3DMessages) server->RemoveNotification(this, type);
        }
    }
    if (lightingManager_) {
        delete lightingManager_;
        lightingManager_ = nullptr;
    }
    if (backgroundPicture_) {
        backgroundPicture_->Release();
        backgroundPicture_ = nullptr;
    }
    if (backgroundTexture_ != 0 && mpGDriver) {
        mpGDriver->DeleteTextures(1, &backgroundTexture_);
        backgroundTexture_ = 0;
    }
    if (terrainView_) {
        terrainView_->Release();
        terrainView_ = nullptr;
    }
    if (backgroundViewObject_) {
        backgroundViewObject_->Release();
        backgroundViewObject_ = nullptr;
    }
    if (cameraControl_) {
        cameraControl_->Release();
        cameraControl_ = nullptr;
    }
    if (camera_) {
        camera_->Shutdown();
        if (camera_->Release() == 0) delete camera_;
        camera_ = nullptr;
    }
    if (uiCamera_) {
        if (uiCamera_->Release() == 0) delete uiCamera_;
        uiCamera_ = nullptr;
    }
    if (renderProperties_) {
        renderProperties_->Release();
        renderProperties_ = nullptr;
    }
    viewUtilitiesSeam_.reset();
    initialized_ = false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------------------------
void cSC43DRender::ReadProperties() {
    // 0x100760546: the render-property group 0x4a639ef2 holds one float[4] per highlight colour.
    if (!host_ || !drawContext_) return;
    cISCPropertyHolder* holder = host_->RenderPropertyHolder();
    if (!holder) return;
    for (int index = 0; index < kHighlightPropertyCount; ++index) {
        cISCProperty* property = holder->GetProperty(kHighlightPropertyBase + index);
        if (!property) continue;
        const cIGZVariant* value = property->GetPropertyValue();
        if (!value) continue;
        const std::uint16_t type = value->GetType();
        if (type != (cIGZVariant::kTypeFloat32 | cIGZVariant::kTypeArrayFlag)) continue;
        if (value->GetCount() != 4) continue;   // the original requires exactly four floats
        const float* data = value->RefFloat32();
        if (!data) continue;
        drawContext_->SetHighlightColor(index, cS3DVector4{data[0], data[1], data[2], data[3]});
    }
}

// ---------------------------------------------------------------------------------------------
// Graphic system / driver
// ---------------------------------------------------------------------------------------------
void cSC43DRender::SetGraphicSystem(IRender3DGraphicSystem* system) {
    // 0x100761392 (PARTIAL: the original also wires the two embedded graphic-port sub-objects and the two name strings
    // the GZ graphic system fills; the port keeps the reference protocol and asks the seam for the two ports).
    if (system) system->AddRef();
    if (graphicSystem_) graphicSystem_->Release();
    graphicSystem_ = system;
    if (system) {
        void* primary = nullptr;
        void* secondary = nullptr;
        system->GetGraphicPorts(&primary, &secondary);
        graphicPortA_ = GraphicPort{system, 0};
        graphicPortB_ = GraphicPort{system, 1};
    } else {
        graphicSystemNameA_.clear();
        graphicSystemNameB_.clear();
        graphicPortA_ = GraphicPort{};
        graphicPortB_ = GraphicPort{};
    }
}

void cSC43DRender::SetGDriver(gd::cIGZGDriver* driver) {
    // 0x10076143c
    StaticSetGDriver(driver);
    DefaultSettings defaults;
    IRender3DSettings* settings = host_ ? host_->Settings() : nullptr;
    if (!settings) settings = &defaults;
    partialWritesSupported_ = mpBackingStoreExtension != nullptr;
    partialWritesAllowed_ = partialWritesSupported_ && !settings->DisablePartialRegionWrites();
    if (camera_) {
        gd::sGDMode mode{};
        if (mpGDriver) mpGDriver->GetVideoModeInfo(mode);   // vslot 0x208, the depth size source
        camera_->SetDepthSize(65536.0f);
    }
    if (drawContext_) drawContext_->SetDriver(mpGDriver);
}

// ---------------------------------------------------------------------------------------------
// Camera / viewport / clip
// ---------------------------------------------------------------------------------------------
SC4ViewUtilities* cSC43DRender::GetViewUtilities() const { return viewUtilities_; }
IS4LightingManager* cSC43DRender::GetLightingManager() const { return lightingManager_; }
SC4DrawContext* cSC43DRender::GetDrawContext() const { return drawContext_; }

void cSC43DRender::GetViewportSize(std::uint32_t& width, std::uint32_t& height) const {
    width = static_cast<std::uint32_t>(viewportWidth_);
    height = static_cast<std::uint32_t>(viewportHeight_);
}

void cSC43DRender::SetViewportSize(std::uint32_t width, std::uint32_t height) {
    viewportWidth_ = static_cast<int>(width);
    viewportHeight_ = static_cast<int>(height);
    drawFrustumRect_[2] = viewportWidth_;
    drawFrustumRect_[3] = viewportHeight_;
}

void cSC43DRender::SetSubpixelImageBias(float x, float y) {
    subpixelImageBiasX_ = x;
    subpixelImageBiasY_ = y;
}

s3d::cS3DCamera* cSC43DRender::GetCamera() { return camera_; }

void cSC43DRender::SetCamera(s3d::cS3DCamera* camera) {
    // 0x100761720
    if (camera_ == camera) return;
    if (camera) camera->AddRef();
    if (camera_ && camera_->Release() == 0) delete camera_;
    camera_ = camera;
    if (camera_) {
        gd::sGDMode mode{};
        if (mpGDriver) mpGDriver->GetVideoModeInfo(mode);
        camera_->SetDepthSize(65536.0f);
        projectionState_ = static_cast<int>(camera_->GetProjectionState()) - 1;
    }
}

cSC4CameraControl* cSC43DRender::GetCameraControl() { return cameraControl_; }

void cSC43DRender::SetCameraControl(cSC4CameraControl* control) {
    // 0x1007617b0
    if (cameraControl_ == control) return;
    if (control) control->AddRef();
    if (cameraControl_) cameraControl_->Release();
    cameraControl_ = control;
    if (cameraControl_ && viewUtilities_) viewUtilities_->UpdateCameraPosition(cameraControl_);
}

const float* cSC43DRender::GetProjectionMatrixEntries() const { return nullptr; }   // 0x10076a1ba returns 0
const float* cSC43DRender::GetViewMatrixEntries() const { return nullptr; }        // 0x10076a1c2 returns 0

void cSC43DRender::SetClipRect(const cRZRect& rect) {
    // 0x10076160e
    DefaultSettings defaults;
    IRender3DSettings* settings = host_ ? host_->Settings() : nullptr;
    if (!settings) settings = &defaults;
    if (!settings->ClipRectSupported()) return;
    const int left = rect.left;
    const int right = rect.right;
    const int x = left;
    const int y = viewportHeight_ - rect.bottom;
    const int width = right - left;
    const int height = rect.bottom - rect.top;
    if (mpGDriver) mpGDriver->SetViewport(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    if (camera_) camera_->SetSubview(fx, fy, fw, fh);
    if (uiCamera_) uiCamera_->SetSubview(fx, fy, fw, fh);
    cameraView_ = 0;
}

void cSC43DRender::UnsetClipRect() {
    // 0x1007616de
    if (mpGDriver) mpGDriver->SetViewport();   // vslot 0x250
    if (camera_) camera_->ClearSubview();
    if (uiCamera_) uiCamera_->ClearSubview();
    cameraView_ = 0;
}

void cSC43DRender::Set3DCameraView() {
    // 0x100766a22
    if (cameraView_ == 2) return;
    if (drawContext_) drawContext_->SetCamera(camera_);
    cameraView_ = 2;
    if (camera_) viewState_ = camera_->GetViewState();
}

void cSC43DRender::Set2DCameraView() {
    // 0x100766a6a
    if (cameraView_ == 1) return;
    if (drawContext_) drawContext_->SetCamera(uiCamera_);
    cameraView_ = 1;
}

void cSC43DRender::SetUICameraView() {
    // 0x100766aa0
    if (cameraView_ == 3) return;
    if (mpGDriver) {
        mpGDriver->MatrixMode(1);
        mpGDriver->LoadMatrix(projectionMatrix_);
        mpGDriver->MatrixMode(0);
        mpGDriver->LoadIdentity();
    }
    if (drawContext_) drawContext_->SetModelViewTransformChanged();
    cameraView_ = 3;
}

// ---------------------------------------------------------------------------------------------
// Zoom / rotation (all forwarded to the camera control; 0x100768e2e..0x100768e9e)
// ---------------------------------------------------------------------------------------------
void cSC43DRender::SetZoom(int zoom) {
    if (cameraControl_) cameraControl_->SetZoom(static_cast<float>(zoom));
}
void cSC43DRender::ZoomIn() {
    if (cameraControl_) cameraControl_->ZoomIn();
}
void cSC43DRender::ZoomOut() {
    if (cameraControl_) cameraControl_->ZoomOut();
}
void cSC43DRender::SetRotation(int rotation) {
    if (cameraControl_) cameraControl_->SetRotation(rotation);
}
void cSC43DRender::RotateLeft() {
    if (cameraControl_) cameraControl_->RotateLeft();
}
void cSC43DRender::RotateRight() {
    if (cameraControl_) cameraControl_->RotateRight();
}
void cSC43DRender::SetZoomAndRotation(int zoom, int rotation) {
    if (cameraControl_) cameraControl_->SetZoomAndRotation(zoom, rotation);
}

// ---------------------------------------------------------------------------------------------
// View-object lists
// ---------------------------------------------------------------------------------------------
void cSC43DRender::InsertIntoViewObjectList(std::list<ViewObjectInfo>& list, cISC4ViewObject3D* object,
                                           std::uint32_t priority) {
    // 0x10075f3e6: the list stays sorted by priority, so the lowest priority is the first node; equal priorities keep
    // insertion order (the new node goes before the first entry with a strictly higher priority).
    object->AddRef();
    auto position = list.begin();
    while (position != list.end() && position->priority <= priority) ++position;
    list.insert(position, ViewObjectInfo{object, priority});
}

bool cSC43DRender::RemoveFromViewObjectList(std::list<ViewObjectInfo>& list, cISC4ViewObject3D* object) {
    // 0x10075f474
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->object == object) {
            list.erase(it);
            object->Release();
            return true;
        }
    }
    return false;
}

void cSC43DRender::ClearViewObjectList(std::list<ViewObjectInfo>& list) {
    // 0x10075f4ce: Release every object, then clear.
    for (ViewObjectInfo& info : list) {
        if (info.object) info.object->Release();
    }
    list.clear();
}

bool cSC43DRender::AddViewObject(cISC4ViewObject3D* object, int list, std::uint32_t priority) {
    // 0x1007693d0: 0 = foreground (+0x258), 2 = overlays (+0x270), 3 = dynamic (+0x228), 5 = static (+0x240), any
    // other value adds nothing and returns false. The two "static" lists also invalidate the backing store.
    switch (list) {
        case 0:
            InsertIntoViewObjectList(viewObjectLists_[2], object, priority);
            backingStoreValid_ = false;
            return true;
        case 2:
            InsertIntoViewObjectList(viewObjectLists_[3], object, priority);
            backingStoreValid_ = false;
            return true;
        case 3:
            InsertIntoViewObjectList(viewObjectLists_[0], object, priority);
            return true;
        case 5:
            InsertIntoViewObjectList(viewObjectLists_[1], object, priority);
            return true;
        default:
            return false;
    }
}

bool cSC43DRender::RemoveViewObject(cISC4ViewObject3D* object) {
    // 0x100769450
    bool found = false;
    if (backgroundViewObject_ == object) {
        if (object) {
            object->Release();
            backgroundViewObject_ = nullptr;
        }
        found = true;
    }
    if (RemoveFromViewObjectList(viewObjectLists_[1], object)) found = true;
    if (RemoveFromViewObjectList(viewObjectLists_[0], object)) found = true;
    if (RemoveFromViewObjectList(viewObjectLists_[3], object)) {
        backingStoreValid_ = false;
        found = true;
    }
    if (RemoveFromViewObjectList(viewObjectLists_[2], object)) {
        backingStoreValid_ = false;
        found = true;
    }
    return found;
}

bool cSC43DRender::FindViewObject(cISC4ViewObject3D* object) const {
    // 0x100769508
    if (backgroundViewObject_ == object) return true;
    for (const std::list<ViewObjectInfo>* list :
         {&viewObjectLists_[1], &viewObjectLists_[0], &viewObjectLists_[3], &viewObjectLists_[2]}) {
        for (const ViewObjectInfo& info : *list) {
            if (info.object == object) return true;
        }
    }
    return false;
}

cSC43DRender::ViewObjectInfo* cSC43DRender::FindViewObjectInfo(cISC4ViewObject3D* object) {
    for (std::list<ViewObjectInfo>& list : viewObjectLists_) {
        for (ViewObjectInfo& info : list) {
            if (info.object == object) return &info;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// View updates / dirty rectangles
// ---------------------------------------------------------------------------------------------
void cSC43DRender::DoFullViewUpdate(ViewUpdateReason reason) {
    // 0x1007625c0. The original also stores the reason in draw-context +0x20, a field nothing in the port reads yet.
    (void)reason;
    if (captureDirtyRectangles_) return;
    captureDirtyRectangles_ = true;
    backingStoreValid_ = false;
    if (viewUtilities_) viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);
    viewContainerRectValid_ = false;
    drawContainerRectValid_ = false;
    ClearDirtyRectangleQueue();
}

void cSC43DRender::ForceFullRedraw() { DoFullViewUpdate(kViewUpdateForceFullRedraw); }   // 0x1007658ae

void cSC43DRender::ClearDirtyRectangleQueue() {
    // 0x100763a56
    if (dirtyRectangles_.empty() && dirtyCount_ == 0 && dirtyAccumulatedArea_ == 0) return;
    for (DirtyRectangleQueueItem& item : dirtyRectangles_) {
        if (item.model) item.model->Release();
    }
    dirtyRectangles_.clear();
    dirtyScratch_.clear();
    dirtyScratch_.shrink_to_fit();
    dirtyCount_ = 0;
    dirtyAccumulatedArea_ = 0;
}

void cSC43DRender::AddDirtyRectangle(int kind, const cRZRect& rect, s3d::cIS3DModelInstance* model, int extra) {
    // 0x100769c44
    if (captureDirtyRectangles_) return;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (rect.left >= viewportWidth_ || rect.top >= viewportHeight_) return;
    cRZRect clipped = rect;
    if (clipped.left < 0) clipped.left = 0;
    if (clipped.top < 0) clipped.top = 0;
    if (clipped.right > viewportWidth_) clipped.right = viewportWidth_;
    if (clipped.bottom > viewportHeight_) clipped.bottom = viewportHeight_;
    AddDirtyRectangleInternal(kind, clipped, model, extra);
}

void cSC43DRender::AddDirtyRectangleInternal(int kind, const cRZRect& rect, s3d::cIS3DModelInstance* model,
                                             int extra) {
    // 0x100769cca. The merge heuristic is kept as decompiled: among the queued rectangles pick the one with the best
    // (negative) cost of merging, merge when the cost is above -2501 or when the queue is already 16 long, and fall
    // back to a full update once the accumulated area passes +0x304.
    if (kind != 0) model = nullptr;
    const int area = (rect.bottom - rect.top) * (rect.right - rect.left);
    const int maxArea = dirtyMaxArea_;

    if (dirtyRectangles_.empty()) {
        unknown324_ = unknown1f8_ - extra;   // +0x324: the stamp of the first rectangle after the last full update
    } else {
        int best = -maxArea;
        DirtyRectangleQueueItem* mergeItem = nullptr;
        int bestSaving = 0;
        unknown324_ -= extra;
        for (DirtyRectangleQueueItem& item : dirtyRectangles_) {
            cRZRect merged;
            merged.left = std::min(rect.left, item.rect.left);
            merged.top = std::min(rect.top, item.rect.top);
            merged.right = std::max(rect.right, item.rect.right);
            merged.bottom = std::max(rect.bottom, item.rect.bottom);
            const int itemArea = (item.rect.bottom - item.rect.top) * (item.rect.right - item.rect.left);
            const int intersection = (merged.bottom - merged.top) * (merged.right - merged.left);
            int cost = (itemArea + area) - intersection;
            if (kind == 0 || item.kind == 0) cost -= std::min(area, itemArea);
            if (best < cost) {
                bestSaving = intersection - itemArea;
                mergeItem = &item;
            }
            if (best <= cost) best = cost;
        }
        if (-2501 < best || dirtyCount_ > 15) {
            const int accumulated = dirtyAccumulatedArea_;
            dirtyAccumulatedArea_ = accumulated + bestSaving;
            if (accumulated + bestSaving < maxArea) {
                cRZRect merged;
                merged.left = std::min(rect.left, mergeItem->rect.left);
                merged.top = std::min(rect.top, mergeItem->rect.top);
                merged.right = std::max(rect.right, mergeItem->rect.right);
                merged.bottom = std::max(rect.bottom, mergeItem->rect.bottom);
                if (mergeItem->model) mergeItem->model->Release();
                mergeItem->model = nullptr;
                if (bestSaving > 0 && dirtyCount_ > 1) {
                    // The merge frees a slot: drop this item and requeue the union as a merged rectangle.
                    const cRZRect mergedCopy = merged;
                    dirtyRectangles_.remove_if([mergeItem](const DirtyRectangleQueueItem& item) {
                        return &item == mergeItem;
                    });
                    --dirtyCount_;
                    AddDirtyRectangleInternal(kViewUpdateDirtyRectMerged, mergedCopy, nullptr, 0);
                    return;
                }
                mergeItem->kind = kViewUpdateDirtyRectMerged;
                mergeItem->rect = merged;
                return;
            }
            DoFullViewUpdate(kViewUpdateDirtyRectOverflow);
            return;
        }
    }

    const int accumulated = dirtyAccumulatedArea_;
    dirtyAccumulatedArea_ = area + accumulated;
    if (area + accumulated < maxArea) {
        if (model) model->AddRef();
        DirtyRectangleQueueItem item;
        item.kind = kind;
        item.rect = rect;
        item.model = model;
        item.stamp = static_cast<std::uint32_t>(changeCounter_);
        dirtyRectangles_.push_front(item);
        ++dirtyCount_;
        return;
    }
    DoFullViewUpdate(kViewUpdateDirtyRectOverflow);
}

bool cSC43DRender::AttemptTranslatedViewUpdate(int dx, int dy) {
    // 0x1007658be: a translation is cheap enough to blit when both offsets stay below 14/16 of the viewport and the
    // backing store can be written partially; the queued dirty rectangles move with the view, then the store is
    // restored shifted and only the two uncovered strips are redrawn. Otherwise the whole view is redrawn.
    const int limitX = (viewportWidth_ * 14) / 16;
    const int limitY = (viewportHeight_ * 14) / 16;
    const int distanceX = dx < 0 ? -dx : dx;
    const int distanceY = dy < 0 ? -dy : dy;
    // The original tests sMacBackingStoreFBO (the GL frame buffer object) here; the port's equivalent is the store.
    if (distanceX < limitX && distanceY < limitY && partialWritesAllowed_ && mbBackingStoreEnabled &&
        backingStoreValid_ && mpBackingStoreExtension) {
        for (DirtyRectangleQueueItem& item : dirtyRectangles_) {
            // The original blends the shifted and the unshifted rectangle (pblendw 0x33): x moves by -dx, y by +dy.
            item.rect.left -= dx;
            item.rect.right -= dx;
            item.rect.top += dy;
            item.rect.bottom += dy;
        }
        DoTranslatedViewUpdate(dx, dy);
        return true;
    }
    drawContext_->SetViewUpdateReason(kViewUpdateTranslated);
    captureDirtyRectangles_ = true;
    backingStoreValid_ = false;
    viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);
    viewContainerRectValid_ = false;
    drawContainerRectValid_ = false;
    ClearDirtyRectangleQueue();
    return false;
}

void cSC43DRender::UpdateViewportRect(const cRZRect& rect) {
    // 0x100764096: rebuild the draw frustum for a sub-rectangle, clip to it, clear it and redraw the static view
    // plus the two view-object lists around it.
    IRender3DSettings& settings = RendererSettings();
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;
    viewUtilities_->BuildDrawFrustumDataFromViewportRect(&rect.left, &rect.right);
    viewContainerRectValid_ = false;
    drawContainerRectValid_ = false;
    SetClipRect(rect);
    ClearViewport(false);
    Set3DCameraView();
    if (settings.StaticViewRedrawOnViewportChange()) {
        drawContext_->SetViewPass(0);
        for (const ViewObjectInfo& info : viewObjectLists_[3]) {
            if (info.object) info.object->DrawViewObject(*drawContext_);
        }
        DrawStaticView();
        drawContext_->SetViewPass(2);
        for (const ViewObjectInfo& info : viewObjectLists_[2]) {
            if (info.object) info.object->DrawViewObject(*drawContext_);
        }
    }
}

void cSC43DRender::TurnOnLighting(bool enable) {
    // 0x100764062
    if (drawContext_) drawContext_->SetLighting(enable && lightingEnabled_);
}

// ---------------------------------------------------------------------------------------------
// Screen shake
// ---------------------------------------------------------------------------------------------
std::uint32_t cSC43DRender::StartScreenShake(float amplitude, float frequency) {
    // 0x100768eb0 builds a description {amplitude, frequency, curveA = {0, amplitude, 0}, curveB = {0, 60, 0},
    // falloff = 1, flags = 0} and forwards it. PARTIAL: the decompiler leaves the frequency stack slot uninitialised,
    // so the port passes its two arguments through (see the pending ledger).
    cSC4ShakeDescription description;
    description.amplitude = amplitude;
    description.frequency = frequency;
    description.curveA = {0.0f, amplitude, 0.0f};
    description.curveB = {0.0f, 60.0f, 0.0f};
    description.falloff = 1.0f;
    cSC43DRender::StartScreenShake(description, nullptr);
    return static_cast<std::uint32_t>(shakeSerial_ - 1);
}

void cSC43DRender::StartScreenShake(const cSC4ShakeDescription& description, const cS3DVector2* position) {
    // 0x100769064
    if (shakes_.empty()) {
        shakeTimer_.Restart();
        unknown3a0_ = 0;
    }
    cSC4Shake shake;
    shake.amplitude = description.amplitude;
    shake.frequency = description.frequency;
    shake.curveA = description.curveA;
    shake.curveB = description.curveB;
    shake.falloff = description.falloff;
    shake.duration = description.falloff;   // the node's +0x10 takes the description's +0x38 (falloff)
    if (position) {
        shake.positionX = position->x;
        shake.positionY = position->y;
    }
    shake.positioned = position != nullptr;
    shake.elapsed = 0;
    shake.serial = static_cast<std::uint32_t>(shakeSerial_++);
    shakes_.push_back(shake);
}

void cSC43DRender::StopScreenShake(std::uint32_t serial) {
    // 0x10076929a: a zero falloff removes the shake, otherwise its duration is shortened to now + the remaining
    // falloff window (the original adds the elapsed clock to the description's two time fields).
    for (auto it = shakes_.begin(); it != shakes_.end(); ++it) {
        if (it->serial != serial) continue;
        if (it->falloff == 0.0f) {
            shakes_.erase(it);
            return;
        }
        float now = 0.0f;
        shakeTimer_.GetElapsedTimeFloat(now);
        now = (it->falloff - it->amplitude) + now;
        if (it->duration <= now) return;
        it->duration = now;
        return;
    }
}

void cSC43DRender::GetShakeOffsets(float& x, float& y) {
    x = static_cast<float>(shakeX_);
    y = static_cast<float>(shakeY_);
}

// ---------------------------------------------------------------------------------------------
// Background picture / backing store
// ---------------------------------------------------------------------------------------------
void cSC43DRender::SetViewportClearColor(const float* color) {
    // 0x1007693a0
    if (!color) return;
    viewportClearColorValid_ = false;
    viewportClearColor_[0] = color[0];
    viewportClearColor_[1] = color[1];
    viewportClearColor_[2] = color[2];
    viewportClearColor_[3] = color[3];
}

void cSC43DRender::EnableBackgroundPicture(bool enable) { backgroundPictureEnabled_ = enable; }   // 0x1007697f6

cIGZBuffer* cSC43DRender::GetBackgroundPicture(bool* visible) const {
    // 0x100769804
    if (visible) *visible = backgroundPictureVisible_;
    return backgroundPicture_;
}

bool cSC43DRender::SetBackgroundPicture(cIGZBuffer* buffer, bool visible) {
    // 0x10076981e (PARTIAL: the texture upload through nsTexUtil::SetupTexture is not ported; the port swaps the
    // buffer and forgets the texture id, so the draw path will re-upload it when it is ported).
    backgroundPictureVisible_ = visible;
    if (backgroundPicture_ == buffer) return true;
    if (backgroundPicture_) backgroundPicture_->Release();
    backgroundPicture_ = nullptr;
    if (buffer) buffer->AddRef();
    if (backgroundTexture_ != 0 && mpGDriver) {
        mpGDriver->DeleteTextures(1, &backgroundTexture_);
        backgroundTexture_ = 0;
    }
    if (!buffer) return false;
    if (!mpGDriver) return false;
    backgroundPicture_ = buffer;
    return true;
}

bool cSC43DRender::SetBackgroundPicture(std::uint32_t texture, int width, int height, bool visible) {
    // 0x10076990a
    backgroundPictureVisible_ = visible;
    if (backgroundTexture_ == texture) return true;
    if (backgroundPicture_) {
        backgroundPicture_->Release();
        backgroundPicture_ = nullptr;
    }
    if (backgroundTexture_ != 0 && mpGDriver) {
        mpGDriver->DeleteTextures(1, &backgroundTexture_);
        backgroundTexture_ = 0;
    }
    backgroundTexture_ = texture;
    backgroundTextureWidth_ = width;
    backgroundTextureHeight_ = height;
    return true;
}

void cSC43DRender::EnableTransparentBackground(bool enable) { transparentBackground_ = enable; }   // 0x100769996

bool cSC43DRender::GetBackingStoreEnabled() { return mbBackingStoreEnabled; }   // 0x1007699a4

void cSC43DRender::SetBackingStoreEnabled(bool enable) {
    // 0x1007699b0: only meaningful when the driver supplied the buffer-region extension.
    const bool wanted = HasBackingStore() && enable;
    mbBackingStoreEnabled = wanted;
}

// ---------------------------------------------------------------------------------------------
// Render system borrow / return
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::BorrowRenderSystem() {
    renderSystemBorrowed_ = true;   // 0x100769ff6
    return true;
}

bool cSC43DRender::ReturnRenderSystem() {
    // 0x10076a006
    renderSystemBorrowed_ = false;
    // The original dereferences the control without a check; the port skips the update when none is attached.
    if (cameraControl_) cameraControl_->UpdateCameraPosition(5);
    const int view = cameraView_;
    cameraView_ = 0;
    if (view == 3)
        SetUICameraView();
    else if (view == 2)
        Set3DCameraView();
    else if (view == 1)
        Set2DCameraView();
    if (drawContext_) drawContext_->SetDefaultRenderStateUnilaterally();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Visibility / query
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::GetModelsInVolume(const cS3DBoundingBox&, s3d::cIS3DModelInstance**, std::uint32_t&) {
    return false;   // 0x100768d5c: the original returns 0 as well
}

bool cSC43DRender::IsModelVisible(s3d::cIS3DModelInstance* model, FrustumType frustum) {
    // 0x100768d64: GetWorldBounds then the matching frustum test (the third argument is the frustum type; 2 = view).
    if (!model || !viewUtilities_) return false;
    cS3DBoundingBox box;
    model->GetWorldBounds(box);
    if (frustum == kFrustumView) return viewUtilities_->IsBoundingBoxInViewFrustum(box.min, box.max);
    return viewUtilities_->IsBoundingBoxInDrawFrustum(box.min, box.max);
}

bool cSC43DRender::IsModelVisible(SC43DGridElement* element, FrustumType frustum) {
    // 0x100768dca: the element wraps the model at +0x20.
    if (!element || !element->model) return false;
    return IsModelVisible(element->model, frustum);
}

// ---------------------------------------------------------------------------------------------
// Fog / lighting switches
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::GetViewFogEnabled() const { return fogEnabled_; }   // 0x100769bce
bool cSC43DRender::SetViewFogEnabled(bool enable) {                   // 0x100769bda
    if (fogEnabled_ != enable) {
        fogEnabled_ = enable;
        ForceFullRedraw();   // vslot +0xa0
    }
    return true;
}
bool cSC43DRender::GetLightingEnabled() const { return lightingEnabled_; }   // 0x100769c02
bool cSC43DRender::SetLightingEnabled(bool enable) {                         // 0x100769c0e
    if (lightingEnabled_ != enable) {
        ForceFullRedraw();   // vslot +0xa0
        lightingEnabled_ = enable;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Stats / map scale / serialization
// ---------------------------------------------------------------------------------------------
void cSC43DRender::GetStatsString1(cIGZString& out) {
    // 0x10076a084; the counters are +0x3c8 / +0x3cc / +0x3d0 / +0x3d4 / +0x3ec / +0x3d8 / +0x3fc of the original.
    out.Sprintf("SPS:%.1f BST:%u BRT:%u VCT:%u PrVDT:%u TeDT:%u PoVDT:%u ToDT:%u", static_cast<double>(statsSps_),
                stats_[0], stats_[1], stats_[2], stats_[3], stats_[9], stats_[4], stats_[13]);
}

void cSC43DRender::GetStatsString2(cIGZString& out) {
    // 0x10076a0f2.
    out.Sprintf("QGT:%u MST:%u MTC:%u, MC1:%u MDC:%u AIT:%u AMT:%u ART:%u TPT%u MPT:%u", stats_[7], stats_[8],
                stats_[10], stats_[11], stats_[12], stats_[14], stats_[15], stats2_[0], stats2_[1], stats2_[2]);
}

void cSC43DRender::SetMapScaleFactor(int factor) { mapScaleFactor_ = factor; }   // 0x10076a176

std::uint32_t cSC43DRender::GetGZCLSID() const { return kCLSID; }   // 0x10076a1ca

bool cSC43DRender::Write(cIGZOStream& stream) const {
    // 0x1007611e2
    StreamWriter out{&stream};
    out.ok = stream.SetSint16(2) && out.ok;
    out.Int(viewportWidth_);
    out.Int(viewportHeight_);
    for (int i = 0; i < 4; ++i) out.Int(drawFrustumRect_[i]);
    SC4WriteGZSerializable(stream, cameraControl_);
    out.Uint(static_cast<std::uint32_t>(projectionState_));
    out.Uint(static_cast<std::uint32_t>(viewState_));
    out.Int(cameraView_);
    return !stream.GetError() && out.ok;
}

bool cSC43DRender::Read(cIGZIStream& stream) {
    // 0x1007612a2
    std::int16_t version = 0;
    if (!stream.GetSint16(version) || version != 2) return false;
    StreamReader in{&stream};
    viewportWidth_ = in.Int();
    viewportHeight_ = in.Int();
    for (int i = 0; i < 4; ++i) drawFrustumRect_[i] = in.Int();
    // The camera control is a nested serializable (class id 0xc9c628ec); a non-DB stream skips it, as in the original.
    void* object = nullptr;
    if (SC4ReadGZSerializable(stream, kCLSIDCameraControl, &object, false) && object) {
        auto* control = static_cast<cSC4CameraControl*>(static_cast<cIGZSerializable*>(object));
        SetCameraControl(control);
        static_cast<cIGZSerializable*>(object)->Release();
    }
    projectionState_ = static_cast<int>(in.Uint());
    viewState_ = static_cast<int>(in.Uint());
    cameraView_ = in.Int();
    return !stream.GetError() && in.ok;
}

// ---------------------------------------------------------------------------------------------
// The model change queue (RENDER3D-2)
//
// The queue is what puts models into the recovered draw pass: InsertModel builds the grid element that DrawStaticView
// and DrawDynamicView collect, and every static (backing-store) change is deferred into +0x2c0 until Draw drains it.
// The four entry points take their own reference on the model twice — once for the duration of the call and once on
// behalf of the grid or the queue — and release the call's reference before returning.
// ---------------------------------------------------------------------------------------------
namespace {
// 0x2bc0 in the original: the queue's byte size, i.e. 200 items of 0x38. Reaching it forces a full redraw.
constexpr std::size_t kMaxQueuedModelChanges = 0x2bc0 / sizeof(cSC43DRender::ModelChangeQueueItem);
// The tolerance ProcessModelChangeQueue's box intersection uses (the original compares |a - b| against 0.0001).
constexpr float kBoxEpsilon = 0.0001f;
// The isometric corner offsets WorldToContainerRect adds per rotation (0x1007620c4).
constexpr float kBoxDiagonal = 0.35355339f;   // 1 / (2 * sqrt(2))
constexpr float kBoxEdge = 0.14644661f;       // (1 - 1 / sqrt(2)) / 2

// The box the queue item stores (its +0x1c / +0x28 words).
cS3DBoundingBox ItemBox(const cSC43DRender::ModelChangeQueueItem& item) {
    cS3DBoundingBox box;
    std::memcpy(box.min, item.boundsMin, sizeof(box.min));
    std::memcpy(box.max, item.boundsMax, sizeof(box.max));
    return box;
}

// The drain's "do the old and the new box meet" test: every axis pair is compared with a 0.0001 tolerance so that
// boxes that merely touch still count as intersecting.
bool BoxesIntersect(const cS3DBoundingBox& a, const cS3DBoundingBox& b) {
    for (int axis = 0; axis < 3; ++axis) {
        if (!(a.min[axis] < b.max[axis] || std::fabs(a.min[axis] - b.max[axis]) < kBoxEpsilon)) return false;
        if (!(a.max[axis] > b.min[axis] || std::fabs(a.max[axis] - b.min[axis]) < kBoxEpsilon)) return false;
    }
    return true;
}

// The original's inlined box union (0x100762e6a..0x1007631ce): an empty first box leaves the second alone, an empty
// second box takes the first, otherwise the six min/max merges.
cS3DBoundingBox UnionBox(const cS3DBoundingBox& oldBox, const cS3DBoundingBox& newBox) {
    if (!(oldBox.min[0] <= oldBox.max[0])) return newBox;
    if (!(newBox.min[0] <= newBox.max[0])) return oldBox;
    cS3DBoundingBox merged = newBox;
    for (int axis = 0; axis < 3; ++axis) {
        merged.min[axis] = std::min(newBox.min[axis], oldBox.min[axis]);
        merged.max[axis] = std::max(newBox.max[axis], oldBox.max[axis]);
    }
    return merged;
}
}  // namespace

SC43DGridElement* cSC43DRender::CreateGridElement() {
    // 0x100761edf..0x100761f08 (and the identical block in RebuildQuadGrid, 0x100765700): the element is 0x40 bytes,
    // its bounds start as the empty box 1,1,1,0,0,0 and its reference count at 1.
    auto* element = new SC43DGridElement();
    element->refCount = 1;
    return element;
}

cS3DBoundingBox cSC43DRender::ElementBox(const SC43DGridElement* element) {
    cS3DBoundingBox box;
    std::memcpy(box.min, &element->boundsMinX, sizeof(box.min));
    std::memcpy(box.max, &element->boundsMaxX, sizeof(box.max));
    return box;
}

void cSC43DRender::SetElementBox(SC43DGridElement* element, const cS3DBoundingBox& box) {
    std::memcpy(&element->boundsMinX, box.min, sizeof(box.min));
    std::memcpy(&element->boundsMaxX, box.max, sizeof(box.max));
}

void cSC43DRender::QueueDirtyRectangleForBox(int kind, const float* boxMin, const float* boxMax,
                                             s3d::cIS3DModelInstance* model, int extra) {
    // 0x100761a4e: ProjectBoundingBoxToViewportRectangle writes the rectangle's top-left and bottom-right corners into
    // two adjacent pairs, which is exactly a cRZRect.
    int rectMin[2] = {0, 0};
    int rectMax[2] = {0, 0};
    viewUtilities_->ProjectBoundingBoxToViewportRectangle(boxMin, boxMax, rectMin, rectMax);
    AddDirtyRectangle(kind, cRZRect{rectMin[0], rectMin[1], rectMax[0], rectMax[1]}, model, extra);
}

void cSC43DRender::QueueModelChange(SC43DGridElement* element, s3d::cIS3DModelInstance* model, std::uint8_t change,
                                    const float* boxMin, const float* boxMax) {
    // The tail of InsertModel / RemoveModel / MoveModel / ChangeModel: reference the model for the queue, mark the
    // element queued and append the item (with the caller's box, or the empty box for the changes that carry none).
    model->AddRef();
    element->flags = static_cast<std::uint16_t>(element->flags | 0x10);
    ModelChangeQueueItem item;
    item.model = model;
    item.change = change;
    if (boxMin) {
        std::memcpy(item.boundsMin, boxMin, sizeof(item.boundsMin));
        std::memcpy(item.boundsMax, boxMax, sizeof(item.boundsMax));
    }
    modelChanges_.push_back(item);
    // The original compares the vector's byte size against 0x2bc0 (200 items).
    if (modelChanges_.size() == kMaxQueuedModelChanges) DoFullViewUpdate(kViewUpdateDirtyRectOverflow);
}

void cSC43DRender::ClearModelChangeQueue() {
    // 0x10076113a: for each queued model clear the queued (0x10) and static (0x8) flags, then let RemoveModel do the
    // grid work while the queue slot's model pointer is temporarily cleared, and release the queue's reference.
    if (walkingModelChanges_) return;
    walkingModelChanges_ = true;
    for (ModelChangeQueueItem& item : modelChanges_) {
        if (!item.model) continue;
        auto* element = static_cast<SC43DGridElement*>(item.model->GetCustomData());
        s3d::cIS3DModelInstance* model = item.model;
        if (element) {
            element->flags = static_cast<std::uint16_t>(element->flags & ~0x18);
            item.model = nullptr;
            RemoveModel(model);
            item.model = model;
        }
        model->Release();
    }
    modelChanges_.clear();
    walkingModelChanges_ = false;
}

bool cSC43DRender::IsModelInserted(s3d::cIS3DModelInstance* model) {
    // 0x10076180e: the model's grid element (model custom data, vslot 0x28) carries the flags.
    if (!model) return false;
    auto* element = static_cast<SC43DGridElement*>(model->GetCustomData());
    if (!element) return false;
    if ((element->flags & 0x10) != 0) {
        for (const ModelChangeQueueItem& item : modelChanges_) {
            if (item.model == model && item.change == 1) return false;
        }
        // An element that has been queued for more than eight changes is drained right here.
        if (modelChanges_.size() > 8) ProcessModelChangeQueue();
    }
    return true;
}

bool cSC43DRender::InsertModel(s3d::cIS3DModelInstance* model) {
    // 0x100761e58
    if (!model) return false;
    // The original dereferences +0x188 without a check; the port's grid is created by Init, so guard it.
    if (!quadGrid_) return false;
    model->AddRef();
    bool result = false;
    bool create = false;
    auto* element = static_cast<SC43DGridElement*>(model->GetCustomData());
    if (!element) {
        create = true;
    } else if ((element->flags & 0x10) != 0) {
        // The element has changes waiting: drain them, then build the element anyway (the original's jump to the
        // create path, see the header quirk).
        for (const ModelChangeQueueItem& item : modelChanges_) {
            if (item.model == model) {
                ProcessModelChangeQueue();
                create = true;
                break;
            }
        }
    }
    if (create) {
        element = CreateGridElement();
        model->SetCustomData(element);
        cS3DBoundingBox box;
        model->GetWorldBounds(box);
        SetElementBox(element, box);
        SC4RectFloat cellRect;
        WorldToContainerRect(&element->boundsMinX, &element->boundsMaxX, cellRect, viewUtilities_->Rotation());
        quadGrid_->SetGridRectForElement(cellRect, element);
        element->model = model;
        element->flags = 0;
        UpdateGridElementObjectFlags(model, element);
        result = quadGrid_->AddElement(element, 1);
        if (!result) {
            model->SetCustomData(nullptr);
        } else {
            model->AddRef();   // the reference the grid holds while the element is in it
            element->flags = static_cast<std::uint16_t>(element->flags | 0x4);
            if (ModelShouldGoIntoBackingStore(element)) {
                // Insert: change 0, empty box (the item's defaults).
                QueueModelChange(element, model, 0, nullptr, nullptr);
            }
        }
        // Drop the local reference taken by CreateGridElement; the quadrants hold the element from here on.
        if (--element->refCount == 0) delete element;
    }
    model->Release();
    return result;
}

bool cSC43DRender::RemoveModel(s3d::cIS3DModelInstance* model) {
    // 0x100762628
    if (!model) return false;
    model->AddRef();
    bool result = false;
    auto* element = static_cast<SC43DGridElement*>(model->GetCustomData());
    if (!element) {
        result = false;
    } else if ((element->flags & 0x8) == 0) {
        // A dynamic element leaves the grid immediately; RemoveElement deletes it when the last quadrant lets go.
        if ((element->flags & 0x10) != 0) ProcessModelChangeQueue();
        if (quadGrid_ && quadGrid_->RemoveElement(element)) {
            model->Release();   // the reference InsertModel took for the grid
            result = true;
        }
        model->SetCustomData(nullptr);
    } else {
        // A static element has to leave the backing store through the queue (change 1, its current box so the drain
        // can repaint where it was).
        if ((element->flags & 0x10) != 0) {
            ProcessModelChangeQueue();
            element = static_cast<SC43DGridElement*>(model->GetCustomData());
            if (!element) {
                model->Release();
                return false;
            }
        }
        const cS3DBoundingBox box = ElementBox(element);
        QueueModelChange(element, model, 1, box.min, box.max);
        result = true;
    }
    model->Release();
    return result;
}

bool cSC43DRender::MoveModel(s3d::cIS3DModelInstance* model) {
    // 0x10076279c: a model without an element is inserted; a dynamic one moves in place; a static one is queued with
    // the box it is leaving.
    if (!model) return false;
    model->AddRef();
    auto* element = static_cast<SC43DGridElement*>(model->GetCustomData());
    if (!element) {
        InsertModel(model);
    } else if ((element->flags & 0x8) == 0) {
        MoveGridElement(element, model);
    } else {
        const cS3DBoundingBox box = ElementBox(element);
        QueueModelChange(element, model, 2, box.min, box.max);
    }
    model->Release();
    return true;
}

bool cSC43DRender::ChangeModel(s3d::cIS3DModelInstance* model, int change) {
    // 0x100762968. The change code is the queue item's byte: 1 toggles the 0x80 draw-attribute bit, 2 / 3 / 4 / 5 map
    // to the queue's 3 / 4 / 5 / 5 (4 never reaches the lookup: it forwards to MoveModel).
    if (!model) return false;
    if (change == 4) return MoveModel(model);
    model->AddRef();
    bool result = false;
    auto* element = static_cast<SC43DGridElement*>(model->GetCustomData());
    if (element) {
        result = true;
        if (change == 1) {
            const s3d::SC4DrawAttributes attributes = model->GetDrawAttributes();
            // The original tests the byte after the flags word (local_7f at +1 of the returned struct).
            if (((attributes.flags >> 8) & 0xff) == 0) {
                element->flags = static_cast<std::uint16_t>(element->flags & ~0x80);
            } else {
                element->flags = static_cast<std::uint16_t>(element->flags | 0x80);
                if (quadGrid_) quadGrid_->SetQuadrantFlagsForElement(element, 1);
            }
        } else if (change == 5 || (element->flags & 0x8) != 0) {
            // 0x05050403 >> (8 * ((change - 2) & 3)), and 5 for anything past the table.
            static constexpr std::uint8_t kChangeCodes[4] = {3, 4, 5, 5};
            const std::uint32_t index = static_cast<std::uint32_t>(change - 2) & 3u;
            const std::uint8_t queued = (change - 2u) > 3u ? 5 : kChangeCodes[index];
            QueueModelChange(element, model, queued, nullptr, nullptr);
        }
    }
    model->Release();
    return result;
}

// ---------------------------------------------------------------------------------------------
// The drain Draw calls before the static view pass
// ---------------------------------------------------------------------------------------------
void cSC43DRender::ProcessModelChangeQueue() {
    // 0x100761896. For every queued item: clear the queued flag, apply the change to the element's flags, and (when
    // the backing store is live) queue the dirty rectangles the redraw needs. The queue is emptied at the end and the
    // walking guard keeps the entry points from re-entering it through RemoveModel.
    if (walkingModelChanges_) return;
    walkingModelChanges_ = true;
    const bool addDirty = mbBackingStoreEnabled && !captureDirtyRectangles_;
    for (ModelChangeQueueItem& item : modelChanges_) {
        s3d::cIS3DModelInstance* model = item.model;
        if (!model) continue;
        auto* element = static_cast<SC43DGridElement*>(model->GetCustomData());
        if (element) {
            const std::uint16_t flags = element->flags;
            element->flags = static_cast<std::uint16_t>(flags & ~0x10);
            switch (item.change) {
                case 0: {   // insert: from here on the element belongs in the static view
                    element->flags = static_cast<std::uint16_t>((flags & ~0x10) | 0x8);
                    if (addDirty) {
                        const int kind = (flags >> 9) & 1;   // the 0x200 property picks a partial update
                        cS3DBoundingBox box;
                        model->GetWorldBounds(box);
                        QueueDirtyRectangleForBox(kind, box.min, box.max, model, 0);
                    }
                    break;
                }
                case 1: {   // remove: repaint where it was, then take it out of the grid
                    element->flags = static_cast<std::uint16_t>(flags & ~0x18);
                    if (addDirty) {
                        const cS3DBoundingBox oldBox = ItemBox(item);
                        QueueDirtyRectangleForBox(2, oldBox.min, oldBox.max, model, 0);
                    }
                    s3d::cIS3DModelInstance* saved = item.model;
                    item.model = nullptr;
                    RemoveModel(saved);
                    item.model = saved;
                    break;
                }
                case 2: {   // move
                    if (!MoveGridElement(element, model) || !addDirty) break;
                    const int kind = (element->flags >> 9) & 1;
                    const cS3DBoundingBox oldBox = ItemBox(item);
                    cS3DBoundingBox box;
                    model->GetWorldBounds(box);
                    // The original repaints the union of the old and the new box when they meet, and both boxes
                    // separately when the move jumped over a gap.
                    if (BoxesIntersect(oldBox, box)) {
                        box = UnionBox(oldBox, box);
                        QueueDirtyRectangleForBox(kind, box.min, box.max, model, 0);
                    } else {
                        QueueDirtyRectangleForBox(kind, oldBox.min, oldBox.max, model, 0);
                        QueueDirtyRectangleForBox(kind, box.min, box.max, model, 0);
                    }
                    break;
                }
                case 3:
                case 4: {   // redraw a changed model (3 is an ordinary change, 4 a partial-update one)
                    if (!addDirty) break;
                    const int kind = item.change != 3 ? 3 : 0;
                    cS3DBoundingBox box;
                    model->GetWorldBounds(box);
                    QueueDirtyRectangleForBox(kind, box.min, box.max, model, 0);
                    break;
                }
                case 5: {   // the model's draw properties changed
                    UpdateGridElementObjectFlags(model, element);
                    const std::uint16_t updated = element->flags;
                    const bool staticNow = ModelShouldGoIntoBackingStore(element);
                    const std::uint16_t after = static_cast<std::uint16_t>(staticNow ? (updated | 0x8)
                                                                                    : (updated & ~0x8u));
                    element->flags = after;
                    if (!addDirty) break;
                    if ((updated & 0x8) == 0 && (after & 0x8) != 0) {
                        // It became static: draw it into the backing store (the 8 marks the model draw).
                        const int kind = (after >> 9) & 1;
                        cS3DBoundingBox box;
                        model->GetWorldBounds(box);
                        QueueDirtyRectangleForBox(kind, box.min, box.max, model, 8);
                    } else if ((after & 0x8) == 0 && (updated & 0x8) != 0) {
                        // It stopped being static: flag the quadrants and repaint the area.
                        if (quadGrid_) quadGrid_->SetQuadrantFlagsForElement(element, 1);
                        cS3DBoundingBox box;
                        model->GetWorldBounds(box);
                        QueueDirtyRectangleForBox(2, box.min, box.max, model, 0);
                    }
                    break;
                }
                default: break;
            }
        }
        model->Release();   // the queue's reference
    }
    modelChanges_.clear();
    walkingModelChanges_ = false;
}

// ---------------------------------------------------------------------------------------------
// The world -> container mapping and the object flags (the queue's two helpers)
// ---------------------------------------------------------------------------------------------
void cSC43DRender::WorldToContainerRect(const float* boxMin, const float* boxMax, SC4RectFloat& out,
                                        int rotation) {
    // 0x1007620c4: two corners of the world box, chosen by the camera rotation, projected into fixed-view space and
    // shifted into container space; then widened by 2 units, clamped into the grid and repaired if degenerate. The
    // four rotations only differ in which two corners they pick and in the isometric offsets they apply to them.
    float first[3] = {0.0f, 0.0f, 0.0f};
    float second[3] = {0.0f, 0.0f, 0.0f};
    switch (rotation) {
        case 1: {
            const float size = boxMax[0] - boxMin[0];
            const float diagonal = size * kBoxDiagonal;
            const float edge = size * kBoxEdge;
            first[0] = boxMax[0] - edge;
            first[1] = boxMax[1];
            first[2] = boxMin[2] - diagonal;
            second[0] = boxMin[0] + edge;
            second[1] = boxMin[1];
            second[2] = boxMax[2] + diagonal;
            break;
        }
        case 2: {
            const float size = boxMax[2] - boxMin[2];
            const float diagonal = size * kBoxDiagonal;
            const float edge = size * kBoxEdge;
            first[0] = boxMax[0] + diagonal;
            first[1] = boxMax[1];
            first[2] = boxMax[2] - edge;
            second[0] = boxMin[0] - diagonal;
            second[1] = boxMin[1];
            second[2] = boxMin[2] + edge;
            break;
        }
        case 3: {
            const float size = boxMax[0] - boxMin[0];
            const float diagonal = size * kBoxDiagonal;
            const float edge = size * kBoxEdge;
            first[0] = boxMin[0] + edge;
            first[1] = boxMax[1];
            first[2] = boxMax[2] + diagonal;
            second[0] = boxMax[0] - edge;
            second[1] = boxMin[1];
            second[2] = boxMin[2] - diagonal;
            break;
        }
        default: {   // rotation 0
            const float size = boxMax[2] - boxMin[2];
            const float diagonal = size * kBoxDiagonal;
            const float edge = size * kBoxEdge;
            first[0] = boxMin[0] - diagonal;
            first[1] = boxMax[1];
            first[2] = boxMin[2] + edge;
            second[0] = boxMax[0] + diagonal;
            second[1] = boxMin[1];
            second[2] = boxMax[2] - edge;
            break;
        }
    }
    // The two fixed-view -> container offset tables (+0x3a8 / +0x3b8). The original indexes them with the rotation
    // unchecked; the port masks it so a caller cannot read past the arrays (every original caller passes 0..3).
    const int index = rotation & 3;
    viewUtilities_->ProjectPointToFixedViewCoordinates(first);
    first[0] = fixedViewToContainerX_[index] + first[0];
    first[1] = fixedViewToContainerY_[index] - first[1];
    viewUtilities_->ProjectPointToFixedViewCoordinates(second);
    second[0] = fixedViewToContainerX_[index] + second[0];
    second[1] = fixedViewToContainerY_[index] - second[1];
    float minX = first[0] - 2.0f;
    float minY = first[1] - 2.0f;
    float maxX = second[0] + 2.0f;
    float maxY = second[1] + 2.0f;
    out.x0 = minX;
    out.z0 = minY;
    out.x1 = maxX;
    out.z1 = maxY;
    if (minX <= 0.5f) {
        minX = 0.5f;
        out.x0 = minX;
    }
    if (minY <= 0.5f) {
        minY = 0.5f;
        out.z0 = minY;
    }
    const float gridSize = unknown190_;
    if (gridSize - 0.5f <= maxX) {
        maxX = gridSize - 0.5f;
        out.x1 = maxX;
    }
    if (gridSize - 0.5f <= maxY) {
        maxY = gridSize - 0.5f;
        out.z1 = maxY;
    }
    if (maxX <= minX) {
        minX = maxX - 0.1f;
        out.x0 = minX;
    }
    if (maxY <= minY) {
        minY = maxY - 0.1f;
        out.z0 = minY;
    }
    if (gridSize * 0.5f < maxX - minX) out.x1 = minX + 1.0f;
    if (gridSize * 0.5f < maxY - minY) out.z1 = minY + 1.0f;
}

void cSC43DRender::UpdateGridElementObjectFlags(s3d::cIS3DModelInstance* model, SC43DGridElement* element) {
    // 0x100762458: fold the model's properties and draw attributes into the element's flag word. 0xc01f is cleared
    // first, so every flag here is recomputed.
    const int properties = model->GetProperties();
    element->flags = static_cast<std::uint16_t>(((properties & 1) << 5) | (element->flags & 0xc01f));
    std::uint16_t bits = 0x101;
    const s3d::SC4DrawAttributes attributes = model->GetDrawAttributes();
    if ((attributes.flags & 4) == 0) {
        const s3d::SC4DrawAttributes other = model->GetDrawAttributes();   // the original asks twice
        bits = 0x100;
        if ((other.flags & 2) == 0) {
            if ((properties & 2) != 0) element->flags = static_cast<std::uint16_t>(element->flags | 0x40);
            if ((properties & 4) == 0) goto propertyBits;
        }
    }
    element->flags = static_cast<std::uint16_t>(element->flags | bits);
propertyBits:
    if ((properties & 8) != 0) element->flags = static_cast<std::uint16_t>(element->flags | 0x200);
    if ((properties & 0xf0) == 0) {
        element->flags = static_cast<std::uint16_t>(element->flags | 0x800);
        return;
    }
    if ((properties & 0x10) != 0) element->flags = static_cast<std::uint16_t>(element->flags | 0x400);
    if ((properties & 0x20) != 0) element->flags = static_cast<std::uint16_t>(element->flags | 0x800);
    if ((properties & 0x40) != 0) element->flags = static_cast<std::uint16_t>(element->flags | 0x1000);
    if ((properties & 0x80) != 0) element->flags = static_cast<std::uint16_t>(element->flags | 0x2000);
}

bool cSC43DRender::ModelShouldGoIntoBackingStore(const SC43DGridElement* element) {
    // 0x100762538: an always-dynamic (0x100) or not-plainly-static element stays out, and each zoom-threshold group
    // (0x400 / 0x1800 / 0x2000) drops out once the zoom reaches its threshold. The zoom is the draw context's +0x14.
    const std::uint16_t flags = element->flags;
    const int zoom = drawContext_ ? drawContext_->Zoom() : 0;
    IRender3DSettings& settings = RendererSettings();
    if ((flags & 0x100) != 0) return false;
    if ((flags & 0x60) != 0x40) return false;
    if ((flags & 0x400) != 0 && zoom >= settings.StaticViewMaxZoomB()) return false;   // +0x308
    if ((flags & 0x1800) != 0 && zoom >= settings.StaticViewMaxZoomA()) return false;  // +0x2c0
    if ((flags & 0x2000) != 0 && zoom >= settings.StaticViewMaxZoomC()) return false;  // +0x350
    return true;
}

bool cSC43DRender::MoveGridElement(SC43DGridElement* element, s3d::cIS3DModelInstance* model) {
    // 0x1007628d6: refresh the element's box, remap it to cells and move it; a rejected move drops the element.
    if (!element || !model || !quadGrid_) return false;
    cS3DBoundingBox box;
    model->GetWorldBounds(box);
    SetElementBox(element, box);
    SC4RectFloat cellRect;
    WorldToContainerRect(&element->boundsMinX, &element->boundsMaxX, cellRect, viewUtilities_->Rotation());
    // The quadrant flag is 1 unless the element is both not-highlighted and static:
    // ((flags >> 7) | ~(flags >> 3)) & 1.
    const std::uint16_t flags = element->flags;
    const std::uint32_t gridFlags = ((flags >> 7) | ~(flags >> 3)) & 1u;
    const bool moved = quadGrid_->MoveElement(element, cellRect, gridFlags);
    if (!moved) model->SetCustomData(nullptr);
    return moved;
}

// ---------------------------------------------------------------------------------------------
// The quad grid rebuild (zoom / rotation changes)
// ---------------------------------------------------------------------------------------------
void cSC43DRender::RebuildQuadGrid(int oldZoom, int oldRotation, int newZoom, int newRotation) {
    // 0x100765592: only a zoom or rotation change matters; every element is re-created and re-inserted into a fresh
    // grid (the old grid's cell metrics are still used for the cell rectangle, see the header quirk).
    if (newZoom == oldZoom && newRotation == oldRotation) return;
    if (!quadGrid_) return;
    int count = quadGrid_->ElementCount();
    if (count <= 0) return;
    std::vector<SC4GridElement*> elements(static_cast<std::size_t>(count));
    quadGrid_->GetAllElements(elements.data(), count);
    SC4QuadGrid* newGrid = CreateQuadGrid();
    for (int i = 0; i < count; ++i) {
        auto* old = static_cast<SC43DGridElement*>(elements[static_cast<std::size_t>(i)]);
        s3d::cIS3DModelInstance* model = old->model;
        if (!model) continue;
        auto* element = CreateGridElement();
        element->flags = old->flags;
        element->model = model;
        const cS3DBoundingBox box = ElementBox(old);
        SetElementBox(element, box);
        cS3DBoundingBox worldBox;
        model->GetWorldBounds(worldBox);
        SC4RectFloat cellRect;
        WorldToContainerRect(worldBox.min, worldBox.max, cellRect, newRotation);
        quadGrid_->SetGridRectForElement(cellRect, element);   // quirk: the old grid's cell size
        model->SetCustomData(element);
        const std::uint32_t gridFlags = ((element->flags >> 7) | ~(element->flags >> 3)) & 1u;
        if (!newGrid->AddElement(element, gridFlags)) {
            // The original retries with the cell rectangle (0, 0)-(1, 1) and gives up on the model's element.
            element->x0 = 0;
            element->z0 = 0;
            element->x1 = 1;
            element->z1 = 1;
            if (!newGrid->AddElement(element, gridFlags)) model->SetCustomData(nullptr);
        }
        if (--element->refCount == 0) delete element;
    }
    delete quadGrid_;
    quadGrid_ = newGrid;
}

void cSC43DRender::DoViewRotationChange2(int oldZoom, int oldRotation, int newZoom, int newRotation) {
    // 0x10076385e: the camera rotation changed (the caller already compared the two rotations). Everything the old
    // rotation placed in the grid is rebuilt, the backing store goes stale and the dirty rectangles are dropped.
    if (oldRotation == newRotation) return;
    if (!modelChanges_.empty()) ProcessModelChangeQueue();
    captureDirtyRectangles_ = true;
    backingStoreValid_ = false;
    drawContext_->SetZoomAndRotation(newZoom, newRotation);
    RebuildQuadGrid(oldZoom, oldRotation, newZoom, newRotation);
    viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);
    viewContainerRectValid_ = false;
    drawContainerRectValid_ = false;
    ++changeCounter_;
    ClearDirtyRectangleQueue();
}

void cSC43DRender::DoViewZoomChange3(int oldZoom, int oldRotation, int newZoom, int newRotation) {
    // 0x100763936: the zoom changed. The backing store is disabled past the property threshold (+0x278), the grid is
    // rebuilt and every element's static flag re-evaluated.
    if (oldZoom == newZoom) return;
    if (!modelChanges_.empty()) ProcessModelChangeQueue();
    SetBackingStoreEnabled(newZoom < RendererSettings().BackingStoreMaxZoom());
    captureDirtyRectangles_ = true;
    backingStoreValid_ = false;
    drawContext_->SetZoomAndRotation(newZoom, newRotation);
    RebuildQuadGrid(oldZoom, oldRotation, newZoom, newRotation);
    viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);
    ReevaluateBackingStoreObjectFlags();
    viewContainerRectValid_ = false;
    drawContainerRectValid_ = false;
    ++changeCounter_;
    ClearDirtyRectangleQueue();
}

void cSC43DRender::ReevaluateBackingStoreObjectFlags() {
    // 0x1007657c6: after a zoom change every element's static flag is recomputed, and every quadrant is marked as
    // filled so the dynamic pass walks it.
    if (!quadGrid_) return;
    int count = quadGrid_->ElementCount();
    if (count > 0) {
        std::vector<SC4GridElement*> elements(static_cast<std::size_t>(count));
        quadGrid_->GetAllElements(elements.data(), count);
        for (int i = 0; i < count; ++i) {
            auto* element = static_cast<SC43DGridElement*>(elements[static_cast<std::size_t>(i)]);
            const std::uint16_t flags = element->flags;
            element->flags = static_cast<std::uint16_t>(flags | 0x8);
            if (!ModelShouldGoIntoBackingStore(element))
                element->flags = static_cast<std::uint16_t>(flags & ~0x8u);
        }
    }
    // The original walks the grid's own quadrant vectors; the port walks the axes.
    const int dimension = quadGrid_->Dimension();
    for (int x = 0; x < dimension; ++x)
        for (int z = 0; z < dimension; ++z)
            quadGrid_->Quadrant(x, z).flags = (quadGrid_->Quadrant(x, z).flags & 0xffff0000u) | 1u;
}


// ---------------------------------------------------------------------------------------------
// The per-frame draw pass (RENDER3D-1)
//
// The decompiler drops the driver arguments of every draw call here (they go through cIGZGDriver's vtable), so the
// slot of each one was read from the disassembly: `call qword ptr [rax + 0x58]` is Clear (slot 11 = 0x58 / 8), +0x60
// is ClearColor (12), +0x68 ClearDepth (13), +0x28 InterleavedArrays (5), and the renderer's own vtable calls are
// +0xc0 Set3DCameraView (24), +0xc8 Set2DCameraView (25), +0xd0 SetUICameraView (26), +0x178 ClearViewport (47) and
// +0x188 DrawViewportRectangle (49). Model draws are cIS3DModelInstance::Draw (vslot 0x60) and its sort value
// (vslot 0x88).
// ---------------------------------------------------------------------------------------------
namespace {

constexpr float kMagicFloat = 12582912.0f;     // 1.5 * 2^23, the float -> int conversion constant
constexpr std::int32_t kMagicBits = 0x4b400000;
constexpr float kColourMagic = 49152.0f;       // 1.5 * 2^15: one ulp is 1/256, so the bit delta is value * 256
constexpr std::int32_t kColourMagicBits = 0x47400000;
constexpr std::uint32_t kColourBlack = 0xff000000u;   // nS3D::kColorARGB8Black
// (0x100a2a190) the block / background texture size per zoom level. The original indexes it with the draw context's
// zoom unchecked; the port clamps the index (zoom 7+ would read unrelated data in the original).
constexpr int kTextureSizeTable[7] = {1024, 512, 256, 128, 64, 32, 16};
// ClearViewport draws at a fixed depth in front of the whole scene.
constexpr float kViewportClearZ = -32500.0f;
// The driver's vertex formats the viewport rectangles use (eGDVertexFormat).
constexpr std::uint32_t kVertexFormatV3fC4ub = 1;   // 16 bytes: x, y, z, packed r/g/b/a
constexpr std::uint32_t kVertexFormatV3fT2f = 2;    // 20 bytes: x, y, z, u, v

// The original's unit-float -> byte conversion (see gd::QuantizeUnitFloat): the bits of (value + 49152.0f) minus the
// bits of 49152.0f give round(value * 256), and folding the high byte into the low one turns 256 into 255.
std::uint8_t PackViewportColourByte(float value) {
    const float added = value + kColourMagic;
    std::int32_t bits = 0;
    std::memcpy(&bits, &added, sizeof(bits));
    const std::int32_t v = bits - kColourMagicBits;
    return static_cast<std::uint8_t>((v & 0xff) - ((v >> 8) & 0xff));
}

// The four bytes of one V3F_C4UB vertex colour, in memory order r, g, b, a (0x1007677da).
std::uint32_t PackViewportColour(const float* colour) {
    return static_cast<std::uint32_t>(PackViewportColourByte(colour[0])) |
           (static_cast<std::uint32_t>(PackViewportColourByte(colour[1])) << 8) |
           (static_cast<std::uint32_t>(PackViewportColourByte(colour[2])) << 16) |
           (static_cast<std::uint32_t>(PackViewportColourByte(colour[3])) << 24);
}

// SC4ViewUtilities::viewportRect_ (+0x234) and the other int rectangles as a cRZRect.
cRZRect AsRect(const std::int32_t* rect) { return cRZRect{rect[0], rect[1], rect[2], rect[3]}; }

// (int)v for 0 <= v < 2^22 the way the original computes it: add 1.5 * 2^23, reinterpret the float's bits and
// subtract the magic's bits.
inline std::int32_t RoundToInt(float value) {
    const float added = value + kMagicFloat;
    std::int32_t bits = 0;
    std::memcpy(&bits, &added, sizeof(bits));
    return bits - kMagicBits;
}

// (anonymous namespace)::SC4ComputeViewportZProjection @ 0x10076529e: the gradient of the viewport Z with respect to
// world x / y / z (out[0..2]) and the Z of the world origin plus one (out[3]). The passes use it to turn an element's
// world bounds into a depth key.
void ComputeViewportZProjection(float* out, const SC4ViewUtilities& utilities) {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float alongX[3] = {1.0f, 0.0f, 0.0f};
    const float alongY[3] = {0.0f, 1.0f, 0.0f};
    const float alongZ[3] = {0.0f, 0.0f, 1.0f};
    const float z0 = utilities.ProjectPointToViewportZ(origin);
    const float zx = utilities.ProjectPointToViewportZ(alongX);
    const float zy = utilities.ProjectPointToViewportZ(alongY);
    const float zz = utilities.ProjectPointToViewportZ(alongZ);
    out[0] = zx - z0;
    out[1] = zy - z0;
    out[2] = zz - z0;
    out[3] = z0 + 1.0f;
}

// cModelSort<cSortModelsFrontToBack>::sort @ 0x100765366 and cModelSort<cSortModelsBackToFront>::sort @ 0x10076547c.
// The original's own loop: move the middle element to the front, bubble it into place against its successor, put it at
// the position it reached, recurse on the left part and continue with the rest. The two copies differ only in the key
// comparison. Hand-written from the disassembly so the permutation (including the order of equal keys) is the same.
void SortModelList(cSC4ModelSortElement* list, int count, bool ascending) {
    while (count > 1) {
        std::swap(list[0], list[(count - 1) / 2]);
        const std::uint32_t key = list[0].key;
        int position = 0;
        for (int left = count - 1; left != 0; --left) {
            const bool movesForward = ascending ? key < list[position + 1].key : list[position + 1].key < key;
            if (movesForward) {
                std::swap(list[position], list[position + 1]);
                ++position;
            }
        }
        std::swap(list[0], list[position]);
        SortModelList(list, position, ascending);
        list += position + 1;
        count = (count - 1) - position;
    }
}

// The depth key the two view draws build for one element (0x100764240 / 0x100764c54): the element's projected half
// extent plus its projected centre, rounded to 1/65535, mixed with the model's own sort value (vslot 0x88):
// key = round(value * 65535) + (sortValue & 0xffff0000) + sortValue * 0x10000.
std::uint32_t ModelSortKey(const SC43DGridElement& element, const float* projection) {
    const float extentX = std::fabs(element.boundsMaxX - element.boundsMinX) * projection[0];
    const float extentY = std::fabs(element.boundsMaxY - element.boundsMinY) * projection[1];
    const float extentZ = std::fabs(element.boundsMaxZ - element.boundsMinZ) * projection[2];
    const float centreX = (element.boundsMinX + element.boundsMaxX) * 0.5f * projection[0];
    const float centreY = (element.boundsMinY + element.boundsMaxY) * 0.5f * projection[1];
    const float centreZ = (element.boundsMinZ + element.boundsMaxZ) * 0.5f * projection[2];
    const float value = (extentX + extentY + extentZ) * 0.5f + (centreX + centreY) + centreZ + projection[3];
    std::int32_t key = RoundToInt(value * 65535.0f);
    std::uint32_t sortValue = 0;
    if (element.model) element.model->GetSortValue(sortValue);
    key += static_cast<std::int32_t>((sortValue & 0xffff0000u) + (sortValue << 16));
    return static_cast<std::uint32_t>(key);
}

// The two shake tables the original keeps at 0x100b73820 / 0x100b73840 (built once, seed 0x58) and its generator.
std::vector<cS3DVector2> sShakeTableRandom;
std::vector<cS3DVector2> sShakeTableSine;
std::unique_ptr<cRZRandom> sShakeRandom;

IRender3DSettings& RendererSettings() {
    static DefaultSettings defaults;
    IRender3DHost* host = cSC43DRender::Host();
    IRender3DSettings* settings = host ? host->Settings() : nullptr;
    return settings ? *settings : defaults;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Backing store copy (the original's raw glBlitFramebufferEXT pairs, now the driver's IBackingStore service)
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::SaveBackingStore() {
    // 0x1007641b4: when the Mac build has just zoomed, the shake offsets are dropped and the store is refilled once
    // before the normal save.
    if (mbMacJustZoomed) {
        const int shakeX = shakeX_;
        const int shakeY = shakeY_;
        if (shakeX != 0 || shakeY != 0) {
            shakeX_ = 0;
            shakeY_ = 0;
            SaveBackingStore(0, 0, viewportWidth_, viewportHeight_);
            shakeX_ = shakeX;
            shakeY_ = shakeY;
            mbMacJustZoomed = false;
        }
    }
    return SaveBackingStore(0, 0, viewportWidth_, viewportHeight_);
}

bool cSC43DRender::SaveBackingStore(int x, int y, int width, int height) {
    // 0x100764aec: frame -> store. The shake offsets shift the copied block and the block is clipped to the viewport.
    int sourceX = x;
    int sourceY = y;
    int destinationX = x;
    int destinationY = y;
    int copyWidth = width;
    int copyHeight = height;
    const int shakeX = shakeX_;
    const int shakeY = shakeY_;
    if (shakeX != 0 || shakeY != 0) {
        const int shiftedX = x - shakeX;
        if (shiftedX < 0) {
            if (width - shakeX == 0 || width < shakeX) return true;
            sourceX = x + shakeX;
            copyWidth = width - shakeX;
            destinationX = 0;
        } else {
            if (viewportWidth_ < shiftedX + width) {
                copyWidth = viewportWidth_ - shiftedX;
                if (copyWidth == 0 || viewportWidth_ < shiftedX) return true;
            }
            destinationX = shiftedX;
        }
        const int shiftedY = y - shakeY;
        if (shiftedY < 0) {
            if (height - shakeY == 0 || height < shakeY) return true;
            sourceY = shakeY + y;
            copyHeight = height - shakeY;
            destinationY = 0;
        } else {
            if (viewportHeight_ < shiftedY + height) {
                copyHeight = viewportHeight_ - shiftedY;
                if (copyHeight == 0 || viewportHeight_ < shiftedY) return true;
            }
            destinationY = shiftedY;
        }
    }
    if (copyHeight < 1) return false;
    if (copyWidth < 1) return false;
    // A full-frame save always works; a partial one needs the buffer-region extension's partial writes.
    if (!partialWritesAllowed_ && (copyWidth != viewportWidth_ || copyHeight != viewportHeight_)) return false;
    if (!mpBackingStoreExtension) return false;
    mpBackingStoreExtension->SaveFromFrame(sourceX, sourceY, copyWidth, copyHeight, destinationX, destinationY);
    return true;
}

bool cSC43DRender::RestoreBackingStore() {
    // 0x10076605c: store -> frame. The shaken image is restored from the store with the block shortened by the shake
    // distance and pushed (source 0 / destination |shake|) to the side the shake moved away from.
    const int shakeX = shakeX_;
    const int shakeY = shakeY_;
    int width = viewportWidth_;
    int height = viewportHeight_;
    int destinationX = 0;
    int destinationY = 0;
    int sourceX = 0;
    int sourceY = 0;
    if (shakeX != 0 || shakeY != 0) {
        const int distanceX = shakeX < 0 ? -shakeX : shakeX;
        const int distanceY = shakeY < 0 ? -shakeY : shakeY;
        destinationX = shakeX < 0 ? distanceX : 0;
        sourceX = shakeX < 0 ? 0 : shakeX;
        destinationY = shakeY < 0 ? distanceY : 0;
        sourceY = shakeY < 0 ? 0 : shakeY;
        width = viewportWidth_ - distanceX;
        height = viewportHeight_ - distanceY;
    }
    RestoreBackingStore(destinationX, destinationY, width, height, sourceX, sourceY);
    return true;
}

bool cSC43DRender::RestoreBackingStore(int dstX, int dstY, int width, int height, int srcX, int srcY) {
    // 0x100769b1c: the original's glBlitFramebufferEXT from the backing-store FBO into the frame.
    if (!mpBackingStoreExtension) return false;
    mpBackingStoreExtension->RestoreToFrame(srcX, srcY, width, height, dstX, dstY);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Container rectangles
// ---------------------------------------------------------------------------------------------
void cSC43DRender::GetVisibleContainerRect(SC4RectFloat& out, FrustumType frustum) {
    // 0x100765194: the cached container-space rectangle of the requested frustum. frustum < 2 uses the draw frustum
    // (+0x214 / +0x218), anything else the view frustum (+0x200 / +0x204). The clamping runs on the cached copy, so
    // it only happens once per invalidation.
    float* cached = nullptr;
    SC4RectFloat computed;
    if (frustum < kFrustumView) {
        if (!drawContainerRectValid_) {
            GetDrawFrustumContainerRect(computed);
            drawContainerRect_[0] = computed.x0;
            drawContainerRect_[1] = computed.z0;
            drawContainerRect_[2] = computed.x1;
            drawContainerRect_[3] = computed.z1;
            drawContainerRectValid_ = true;
        }
        cached = drawContainerRect_;
    } else {
        if (!viewContainerRectValid_) {
            GetViewFrustumContainerRect(computed);
            viewContainerRect_[0] = computed.x0;
            viewContainerRect_[1] = computed.z0;
            viewContainerRect_[2] = computed.x1;
            viewContainerRect_[3] = computed.z1;
            viewContainerRectValid_ = true;
        }
        cached = viewContainerRect_;
    }
    if (cached[0] <= 0.0f && cached[0] != 0.0f) cached[0] = 0.0f;
    if (cached[1] <= 0.0f && cached[1] != 0.0f) cached[1] = 0.0f;
    const float limit = unknown190_ - 1.0f;
    if (limit < cached[2]) cached[2] = limit;
    if (limit < cached[3]) cached[3] = limit;
    out.x0 = cached[0];
    out.z0 = cached[1];
    out.x1 = cached[2];
    out.z1 = cached[3];
}

void cSC43DRender::GetViewFrustumContainerRect(SC4RectFloat& out) {
    // 0x100766b22: the frustum's viewport rectangle (0, 0) - (viewport width, height) unprojected into fixed-view
    // coordinates and shifted by the rotation's fixed-view -> container offset.
    const std::int32_t rotation = viewUtilities_->Rotation();
    float origin[3] = {0.0f, 0.0f, 0.0f};
    viewUtilities_->ProjectPointFromViewportCoordinatesToFixedViewCoordinates(origin);
    origin[1] = fixedViewToContainerY_[rotation] - origin[1];
    origin[0] = fixedViewToContainerX_[rotation] + origin[0];
    float farCorner[3] = {static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), 0.0f};
    viewUtilities_->ProjectPointFromViewportCoordinatesToFixedViewCoordinates(farCorner);
    farCorner[0] = fixedViewToContainerX_[viewUtilities_->Rotation()] + farCorner[0];
    const float offsetY = fixedViewToContainerY_[viewUtilities_->Rotation()];
    out.x0 = origin[0];
    out.z0 = origin[1];
    out.x1 = farCorner[0];
    out.z1 = offsetY - farCorner[1];
}

void cSC43DRender::GetDrawFrustumContainerRect(SC4RectFloat& out) {
    // 0x100766c30: the same for the draw frustum, starting from its viewport rectangle (+0x234 of the view
    // utilities) and ending at the rectangle's bottom-right corner.
    const std::int32_t* rect = viewUtilities_->DrawFrustumViewportRect();
    float origin[3] = {static_cast<float>(rect[0]), static_cast<float>(rect[1]), 0.0f};
    float farCorner[3] = {static_cast<float>(rect[2]), static_cast<float>(rect[3]), 0.0f};
    viewUtilities_->ProjectPointFromViewportCoordinatesToFixedViewCoordinates(origin);
    viewUtilities_->ProjectPointFromViewportCoordinatesToFixedViewCoordinates(farCorner);
    const std::int32_t rotation = viewUtilities_->Rotation();
    out.x0 = fixedViewToContainerX_[rotation] + origin[0];
    out.z0 = fixedViewToContainerY_[rotation] - origin[1];
    out.x1 = fixedViewToContainerX_[viewUtilities_->Rotation()] + farCorner[0];
    out.z1 = fixedViewToContainerY_[viewUtilities_->Rotation()] - farCorner[1];
}

void cSC43DRender::GetViewportRayContainerRect(int x, int y, SC4RectFloat& out) {
    // 0x1007679b4: the container rectangle the picking path scans the grid with for one viewport point. The point is
    // unprojected to the world, mapped to fixed-view / container space, widened to a 2 x 2 window and clamped into
    // [0, gridSize - 2]; only a window that would end up degenerate is pulled forward by one unit.
    float point[3] = {static_cast<float>(x), static_cast<float>(y), 0.0f};
    viewUtilities_->UnProjectPointFromViewportCoordinatesToWorldCoordinates(point);
    // The original indexes the offset tables with the rotation unclamped; the port masks it (as WorldToContainerRect).
    const int rotation = viewUtilities_->Rotation() & 3;
    viewUtilities_->ProjectPointToFixedViewCoordinates(point);
    const float containerX = fixedViewToContainerX_[rotation] + point[0];
    const float containerY = fixedViewToContainerY_[rotation] - point[1];
    out.x0 = containerX;
    out.z0 = containerY;
    out.x1 = containerX + 2.0f;
    out.z1 = containerY + 2.0f;
    const float limit = unknown190_ - 2.0f;
    float startX = containerX;
    if (containerX < 0.0f) {
        out.x0 = 0.0f;
        startX = 0.0f;
    }
    if (limit < startX) {
        out.x0 = limit;
        startX = limit;
    }
    if (containerX + 2.0f < startX + 1.0f) out.x1 = startX + 1.0f;
    float startY = containerY;
    if (containerY < 0.0f) {
        out.z0 = 0.0f;
        startY = 0.0f;
    }
    if (limit < startY) {
        out.z0 = limit;
        startY = limit;
    }
    if (containerY + 2.0f < startY + 1.0f) out.z1 = startY + 1.0f;
}

// ---------------------------------------------------------------------------------------------
// Viewport rectangles
// ---------------------------------------------------------------------------------------------
void cSC43DRender::DrawViewportRectangle(const cRZRect& rect, const float* color, bool depthMask, bool colorMask) {
    // 0x1007677da: a screen-space quad at z = -32500 with a solid vertex colour (V3F_C4UB). The colour is only used
    // when colorMask is set; otherwise the vertices are black.
    std::uint32_t packed = kColourBlack;
    std::uint8_t red = 0xff, green = 0x00, blue = 0x00, alpha = 0x00;   // kColorARGB8Black
    if (colorMask) {
        packed = PackViewportColour(color);
        red = static_cast<std::uint8_t>(packed & 0xff);
        green = static_cast<std::uint8_t>((packed >> 8) & 0xff);
        blue = static_cast<std::uint8_t>((packed >> 16) & 0xff);
        alpha = static_cast<std::uint8_t>((packed >> 24) & 0xff);
    }
    struct Vertex {
        float x, y, z;
        std::uint8_t r, g, b, a;
    };
    const Vertex vertices[4] = {
        {static_cast<float>(rect.left), static_cast<float>(rect.top), kViewportClearZ, red, green, blue, alpha},
        {static_cast<float>(rect.left), static_cast<float>(rect.bottom), kViewportClearZ, red, green, blue, alpha},
        {static_cast<float>(rect.right), static_cast<float>(rect.top), kViewportClearZ, red, green, blue, alpha},
        {static_cast<float>(rect.right), static_cast<float>(rect.bottom), kViewportClearZ, red, green, blue, alpha},
    };
    static_assert(sizeof(Vertex) == 16, "V3F_C4UB");
    drawContext_->EnableTextureStateFlag(false, 0);
    drawContext_->EnableDepthTestFlag(true);
    drawContext_->SetDepthFunc(7);
    drawContext_->EnableDepthMaskFlag(depthMask);
    drawContext_->EnableColorMaskFlag(colorMask);
    Set2DCameraView();
    drawContext_->DrawPrims(1, static_cast<int>(kVertexFormatV3fC4ub), 4, vertices);
}

void cSC43DRender::DrawTexturedViewportRectangle(const cRZRect& rect, std::uint32_t texture, float alpha, bool depthMask,
                                                 bool colorMask) {
    // 0x1007695a2: the same quad with texture coordinates (V3F_T2F); the depth is -alpha, so a smaller alpha draws
    // the rectangle nearer (the original quirk).
    struct Vertex {
        float x, y, z;
        float u, v;
    };
    const Vertex vertices[4] = {
        {static_cast<float>(rect.left), static_cast<float>(rect.top), -alpha, 0.0f, 0.0f},
        {static_cast<float>(rect.left), static_cast<float>(rect.bottom), -alpha, 0.0f, 1.0f},
        {static_cast<float>(rect.right), static_cast<float>(rect.top), -alpha, 1.0f, 0.0f},
        {static_cast<float>(rect.right), static_cast<float>(rect.bottom), -alpha, 1.0f, 1.0f},
    };
    static_assert(sizeof(Vertex) == 20, "V3F_T2F");
    drawContext_->EnableTextureStateFlag(colorMask, 0);
    drawContext_->EnableColorMaskFlag(colorMask);
    drawContext_->EnableDepthTestFlag(true);
    drawContext_->SetDepthFunc(3);
    drawContext_->EnableDepthMaskFlag(depthMask);
    drawContext_->SetTexture(static_cast<int>(texture), 0);
    Set2DCameraView();
    drawContext_->DrawPrims(1, static_cast<int>(kVertexFormatV3fT2f), 4, vertices);
}

void cSC43DRender::ClearViewportWithSolidColor(const float* color, bool a, bool b) {
    // 0x100767716: clear the frame with a one-off colour, then restore the viewport clear colour. `a` is unused by
    // the recovered body (the original only reaches it through the stale register the decompiler saw); `b` selects
    // colour + depth instead of colour only. The trailing ClearColor re-reads the viewport clear colour, which the
    // disassembly shows as a reload of +0x2a0.
    mpGDriver->ClearColor(color[0], color[1], color[2], color[3]);
    drawContext_->EnableColorMaskFlag(true);
    std::uint32_t mask = 0x4000;
    if (b) {
        drawContext_->EnableDepthMaskFlag(true);
        mpGDriver->ClearDepth(static_cast<double>(unknown2b0_));
        mask = 0x5000;
    }
    mpGDriver->Clear(mask);
    mpGDriver->ClearColor(viewportClearColor_[0], viewportClearColor_[1], viewportClearColor_[2],
                          viewportClearColor_[3]);
    (void)a;
}

void cSC43DRender::ClearViewport(bool clearDepth) {
    // 0x100766d5e: the viewport clear the frame starts with. With no usable background picture it is a plain
    // driver clear (colour + depth only when the visible ground area is inside the city, depth otherwise); with one
    // it tiles the picture over the screen, or — when the picture is "visible" — lays it on the ground plane in
    // block-sized decals and fills the two screen strips the ground plane does not cover.
    IRender3DSettings& settings = RendererSettings();
    float ground[8] = {};
    float boxMin[2] = {};
    float boxMax[2] = {};
    if (!backgroundPictureEnabled_ || backgroundTexture_ == 0 || !viewUtilities_->IsReady()) {
        viewUtilities_->GetDrawFrustumGroundPlanePositions(ground, ground + 2, ground + 4, ground + 6);
        SC4GeometryUtilities::GetBoundingBoxOfVertices2D(reinterpret_cast<const float(*)[2]>(ground), 4, boxMin,
                                                        boxMax);
        const bool covers = boxMin[0] < 0.0f || clearDepth || citySizeX_ < boxMax[0] || boxMin[1] < 0.0f ||
                            citySizeZ_ < boxMax[1];
        if (!viewUtilities_->DrawFrustumReduced()) {
            if (!covers || transparentBackground_) {
                drawContext_->EnableDepthMaskFlag(true);
                mpGDriver->Clear(0x1000);   // depth
            } else {
                drawContext_->EnableColorMaskFlag(true);
                drawContext_->EnableDepthMaskFlag(true);
                mpGDriver->Clear(0x5000);   // colour + depth
            }
        } else {
            DrawViewportRectangle(AsRect(viewUtilities_->DrawFrustumViewportRect()), viewportClearColor_, true,
                                  covers);
        }
        return;
    }

    const bool lightingWasOn = drawContext_->GetLighting();
    if (settings.ClearViewportPreservesLighting()) drawContext_->SetLighting(false);
    drawContext_->SetDepthOffset(0);
    if (!backgroundPictureVisible_) {
        if (!viewUtilities_->DrawFrustumReduced()) {
            // The whole screen, tiled with the background texture: countX x countY repeats.
            const int textureWidth = backgroundTextureWidth_;
            const int textureHeight = backgroundTextureHeight_;
            int countX = viewportWidth_ / textureWidth;
            if (textureWidth * countX < viewportWidth_) ++countX;
            int countY = viewportHeight_ / textureHeight;
            if (textureHeight * countY < viewportHeight_) ++countY;
            const float width = static_cast<float>(textureWidth * countX);
            const float height = static_cast<float>(textureHeight * countY);
            struct Vertex {
                float x, y, z;
                float u, v;
            };
            const Vertex vertices[4] = {
                {0.0f, 0.0f, kViewportClearZ, 0.0f, 0.0f},
                {0.0f, height, kViewportClearZ, 0.0f, static_cast<float>(countY)},
                {width, 0.0f, kViewportClearZ, static_cast<float>(countX), 0.0f},
                {width, height, kViewportClearZ, static_cast<float>(countX), static_cast<float>(countY)},
            };
            drawContext_->EnableColorMaskFlag(true);
            drawContext_->EnableDepthMaskFlag(true);
            drawContext_->EnableDepthTestFlag(true);
            drawContext_->SetDepthFunc(7);
            drawContext_->EnableTextureStateFlag(true, 0);
            drawContext_->EnableBlendStateFlag(false);
            drawContext_->EnableAlphaTestFlag(false);
            drawContext_->SetTexEnvMode(1, 0);
            drawContext_->SetTexture(static_cast<int>(backgroundTexture_), 0);
            drawContext_->SetTexFiltering(1, 1, 0);
            drawContext_->SetTexWrapModes(3, 3, 0);
            Set2DCameraView();
            mpGDriver->InterleavedArrays(kVertexFormatV3fT2f, 0x14, vertices);
            drawContext_->DrawPrims(1, 0, 4);
        }
        if (settings.ClearViewportPreservesLighting()) drawContext_->SetLighting(lightingWasOn);
        return;
    }

    // The picture is visible: draw it on the ground plane.
    const int zoom = drawContext_->Zoom();
    const int block = kTextureSizeTable[zoom < 0 ? 0 : (zoom > 6 ? 6 : zoom)];
    drawContext_->EnableAlphaTestFlag(false);
    if (!viewUtilities_->DrawFrustumReduced()) {
        drawContext_->EnableDepthMaskFlag(true);
        mpGDriver->Clear(0x1000);
        drawContext_->EnableColorMaskFlag(true);
        drawContext_->EnableDepthTestFlag(false);
    } else {
        DrawViewportRectangle(AsRect(viewUtilities_->DrawFrustumViewportRect()), viewportClearColor_, true, false);
        drawContext_->EnableColorMaskFlag(true);
        drawContext_->EnableDepthTestFlag(true);
        drawContext_->SetDepthFunc(3);
    }
    Set2DCameraView();
    viewUtilities_->GetDrawFrustumGroundPlanePositions(ground, ground + 2, ground + 4, ground + 6);
    SC4GeometryUtilities::GetBoundingBoxOfVertices2D(reinterpret_cast<const float(*)[2]>(ground), 4, boxMin, boxMax);
    if (!(boxMin[0] < 0.0f || clearDepth || citySizeX_ < boxMax[0]) &&
        !(boxMin[1] < 0.0f || citySizeZ_ < boxMax[1])) {
        if (settings.ClearViewportPreservesLighting()) drawContext_->SetLighting(lightingWasOn);
        return;
    }
    drawContext_->EnableDepthMaskFlag(false);
    drawContext_->EnableTextureStateFlag(true, 0);
    drawContext_->EnableBlendStateFlag(false);
    drawContext_->SetTexEnvMode(1, 0);
    drawContext_->SetTexture(static_cast<int>(backgroundTexture_), 0);
    drawContext_->SetTexFiltering(1, 1, 0);
    drawContext_->SetTexWrapModes(3, 3, 0);
    struct Vertex {
        float x, y, z;
        float u, v;
    };
    Vertex vertices[4] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
    };
    mpGDriver->InterleavedArrays(kVertexFormatV3fT2f, 0x14, vertices);
    float blockMin[2] = {};
    float blockMax[2] = {};
    viewUtilities_->GetGroundPlaneVisibleAreaRectangle(nullptr, nullptr, blockMin, blockMax);
    blockMin[0] = SC4ViewUtilities::RoundDownToNearestBlockBoundary(blockMin[0], block);
    blockMin[1] = SC4ViewUtilities::RoundDownToNearestBlockBoundary(blockMin[1], block);
    blockMax[0] = SC4ViewUtilities::RoundUpToNearestBlockBoundary(blockMax[0], block);
    blockMax[1] = SC4ViewUtilities::RoundUpToNearestBlockBoundary(blockMax[1], block);
    const int beginX = static_cast<int>(blockMin[0]);
    const int endX = static_cast<int>(blockMax[0]);
    const int beginZ = static_cast<int>(blockMin[1]);
    const int endZ = static_cast<int>(blockMax[1]);
    for (int x = beginX; x < endX; x += block) {
        for (int z = beginZ; z < endZ; z += block) {
            // Only the border blocks are repainted unless the caller forced a depth clear; the interior is already
            // in the backing store (0x1007674d4: the toggled clearDepth flag of the original).
            const bool interior = x > -5 && z >= -4 && static_cast<float>(x) < citySizeX_ - 4.0f &&
                                  static_cast<float>(z) < citySizeZ_ - 4.0f;
            if (!clearDepth && interior) continue;
            vertices[0] = {static_cast<float>(x), 0.0f, static_cast<float>(z), 0.0f, 0.0f};
            vertices[1] = {static_cast<float>(x), 0.0f, static_cast<float>(z + block), 0.0f, 1.0f};
            vertices[2] = {static_cast<float>(x + block), 0.0f, static_cast<float>(z), 1.0f, 0.0f};
            vertices[3] = {static_cast<float>(x + block), 0.0f, static_cast<float>(z + block), 1.0f, 1.0f};
            drawContext_->DrawPrims(1, 0, 4);
        }
    }
    // The two screen strips above / below the ground plane: the original casts the near edge's ray twice, the second
    // time from corner 7 with the first ray's direction (0x100767592).
    const FrustumBounds& frustum = viewUtilities_->ViewFrustum();
    float direction[3] = {frustum.corner[2][0] - frustum.corner[3][0], frustum.corner[2][1] - frustum.corner[3][1],
                          frustum.corner[2][2] - frustum.corner[3][2]};
    float point[3] = {};
    SC4ViewUtilities::GetRayIntersectionWithGroundPlane(frustum.corner[3], direction, point);
    viewUtilities_->ProjectPointToViewportCoordinates2D(point);
    if (point[1] < static_cast<float>(viewportHeight_)) {
        DrawViewportRectangle(cRZRect{0, static_cast<int>(point[1]), viewportWidth_, viewportHeight_},
                              viewportClearColor_, false, true);
    }
    SC4ViewUtilities::GetRayIntersectionWithGroundPlane(frustum.corner[7], direction, point);
    viewUtilities_->ProjectPointToViewportCoordinates2D(point);
    if (point[1] > 0.0f) {
        DrawViewportRectangle(cRZRect{0, 0, viewportWidth_, static_cast<int>(point[1])}, viewportClearColor_, false,
                              true);
    }
    if (settings.ClearViewportPreservesLighting()) drawContext_->SetLighting(lightingWasOn);
}

// ---------------------------------------------------------------------------------------------
// The two quad-grid view passes
// ---------------------------------------------------------------------------------------------
void cSC43DRender::DrawStaticView() {
    // 0x100764c54: collect every element in the visible container rectangle whose 0x8 flag is set, sorted
    // front-to-back, then draw them. Models that return 2 ask for another pass; the pass counter (+0x4 of the draw
    // context) drives the model's trench / piece choice, and the survivors bounce between the two lists.
    drawContext_->SetViewPass(1);
    SC4RectFloat visible;
    GetVisibleContainerRect(visible, static_cast<FrustumType>(1) /* the original passes 1: < 2 selects the draw frustum */);
    float projection[4] = {};
    ComputeViewportZProjection(projection, *viewUtilities_);
    modelSortFront_.clear();
    modelSortBack_.clear();
    if (quadGrid_) {
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        quadGrid_->GetQuadrantsOfScreenRect(visible, x0, z0, x1, z1);
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                SC4QuadGridQuadrant& quadrant = quadGrid_->Quadrant(x, z);
                for (SC4GridElement* base : quadrant.elements) {
                    auto* element = static_cast<SC43DGridElement*>(base);
                    if ((element->flags & 0x8) == 0) continue;
                    // The cell-overlap test the original runs against the element's minimum cell and the query's
                    // minimum cell (0x100764dd5): an element is drawn at its own leading edge or at the query's.
                    const bool overlaps =
                        ((x - 1 - element->x0) | (x - x0 - 1)) < 0 && ((z - 1 - element->z0) | (z - z0 - 1)) < 0;
                    if (!overlaps) continue;
                    modelSortFront_.push_back(cSC4ModelSortElement{element->model, ModelSortKey(*element, projection)});
                }
            }
        }
        if (!modelSortFront_.empty())
            SortModelList(modelSortFront_.data(), static_cast<int>(modelSortFront_.size()), true);
    }
    drawContext_->SetRenderPass(0);
    int source = 0;
    std::vector<cSC4ModelSortElement>* lists[2] = {&modelSortFront_, &modelSortBack_};
    while (true) {
        std::vector<cSC4ModelSortElement>& current = *lists[source];
        std::vector<cSC4ModelSortElement>& next = *lists[1 - source];
        for (const cSC4ModelSortElement& entry : current) {
            if (!entry.model) continue;
            if (entry.model->Draw(drawContext_, false) == 2) next.push_back(entry);
        }
        const std::size_t remaining = next.size();
        current.clear();
        source = 1 - source;
        drawContext_->SetRenderPass(drawContext_->RenderPass() + 1);
        if (remaining == 0 || drawContext_->RenderPass() >= 12) break;
    }
}

void cSC43DRender::DrawDynamicView() {
    // 0x100764240: three sorted lists (front-to-back for the plain dynamic models, back-to-front for the models that
    // go behind them) plus the highlight list. The third list (+0x1e0) is collected but never drawn — quirk kept.
    drawContext_->SetViewPass(4);
    modelSortFront_.clear();
    modelSortBack_.clear();
    modelSortUnused_.clear();
    highlightElements_.clear();
    SC4RectFloat visible;
    GetVisibleContainerRect(visible, kFrustumView);
    float projection[4] = {};
    ComputeViewportZProjection(projection, *viewUtilities_);
    if (quadGrid_) {
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        quadGrid_->GetQuadrantsOfScreenRect(visible, x0, z0, x1, z1);
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                SC4QuadGridQuadrant& quadrant = quadGrid_->Quadrant(x, z);
                if ((quadrant.flags & 1) == 0) continue;   // the "not yet walked" bit of the quadrant
                for (SC4GridElement* base : quadrant.elements) {
                    auto* element = static_cast<SC43DGridElement*>(base);
                    const std::uint16_t flags = element->flags;
                    if ((flags & 0x8) != 0 && static_cast<std::int8_t>(flags & 0xff) >= 0) continue;
                    const bool overlaps =
                        ((x - 1 - element->x0) | (x - x0 - 1)) < 0 && ((z - 1 - element->z0) | (z - z0 - 1)) < 0;
                    if (!overlaps) continue;
                    if ((flags & 0x8) == 0) {
                        cSC4ModelSortElement entry{element->model, ModelSortKey(*element, projection)};
                        if ((flags & 0x1) == 0) {
                            if ((flags & 0x100) == 0)
                                modelSortFront_.push_back(entry);
                            else
                                modelSortBack_.push_back(entry);
                        } else {
                            modelSortUnused_.push_back(entry);
                        }
                    }
                    if ((flags & 0x80) != 0) highlightElements_.push_back(element);
                }
            }
        }
        if (!modelSortUnused_.empty())
            SortModelList(modelSortUnused_.data(), static_cast<int>(modelSortUnused_.size()), false);
        if (!modelSortBack_.empty())
            SortModelList(modelSortBack_.data(), static_cast<int>(modelSortBack_.size()), false);
        if (!modelSortFront_.empty())
            SortModelList(modelSortFront_.data(), static_cast<int>(modelSortFront_.size()), true);
    }
    drawContext_->SetRenderPass(0);
    std::vector<cSC4ModelSortElement>* lists[2] = {&modelSortFront_, &modelSortBack_};
    bool passLimitReached = false;
    for (int list = 0; list < 2 && !passLimitReached; ++list) {
        std::vector<cSC4ModelSortElement>& entries = *lists[list];
        if (entries.empty()) continue;
        // The survivors are compacted to the front of the same list and drawn again until none want another pass.
        std::size_t end = entries.size();
        while (end > 0) {
            std::size_t kept = 0;
            for (std::size_t i = 0; i < end; ++i) {
                if (!entries[i].model) continue;
                if (entries[i].model->Draw(drawContext_, false) == 2) {
                    entries[kept] = entries[i];
                    ++kept;
                }
            }
            end = kept;
            drawContext_->SetRenderPass(drawContext_->RenderPass() + 1);
            if (drawContext_->RenderPass() > 11) {
                passLimitReached = true;
                break;
            }
        }
        if (!passLimitReached) entries.clear();
    }
    if (!highlightElements_.empty()) {
        drawContext_->SetRenderPass(0);
        drawContext_->SetDepthOffset(4);
        for (SC43DGridElement* element : highlightElements_) {
            if (element->model) element->model->Draw(drawContext_, true);
        }
        drawContext_->SetDepthOffset(0);
    }
}

void cSC43DRender::DoSingleModelStaticDrawAddition(s3d::cIS3DModelInstance* model, const cRZRect& rect) {
    // 0x1007650d0: re-render one model into a dirty rectangle of the static view (reason 8), then refresh that part
    // of the backing store.
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;
    drawContext_->SetViewUpdateReason(kViewUpdateDirtyRectModel);
    Set3DCameraView();
    drawContext_->SetRenderPass(0);
    int result = 2;
    do {
        if (drawContext_->RenderPass() > 11) break;
        result = model->Draw(drawContext_, false);
        drawContext_->SetRenderPass(drawContext_->RenderPass() + 1);
    } while (result == 2);
    if (backingStoreValid_) {
        backingStoreValid_ = SaveBackingStore(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    drawContext_->SetViewUpdateReason(kViewUpdateNone);
}

// ---------------------------------------------------------------------------------------------
// Partial / translated updates and the dirty-rectangle queue
// ---------------------------------------------------------------------------------------------
void cSC43DRender::DoPartialViewUpdate(const cRZRect& rect, ViewUpdateReason reason) {
    // 0x100764a6c: redraw one viewport rectangle and fold it back into the backing store.
    if (!mbBackingStoreEnabled || rect.left >= rect.right || rect.top >= rect.bottom) return;
    drawContext_->SetViewUpdateReason(reason);
    UpdateViewportRect(rect);
    UnsetClipRect();
    if (backingStoreValid_) {
        backingStoreValid_ = SaveBackingStore(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    drawContext_->SetViewUpdateReason(kViewUpdateNone);
}

void cSC43DRender::DoTranslatedViewUpdate(int dx, int dy) {
    // 0x100763ac6: restore the stored frame shifted by (dx, dy), redraw the two strips the shift uncovers, then save
    // the shifted region again. The eight cases are the sign combination of the two offsets.
    if (inDraw_) return;
    inDraw_ = true;
    // The blit: source rectangle in the store, destination in the frame, plus the two uncovered strips and the
    // region the store now describes.
    int sourceX = 0, sourceY = 0, destinationX = 0, destinationY = 0, width = 0, height = 0;
    cRZRect first{0, 0, 0, 0};
    cRZRect second{0, 0, 0, 0};
    int savedLeft = 0, savedTop = 0, savedRight = viewportWidth_, savedBottom = viewportHeight_;
    const bool right = dx > 0;
    const bool left = dx < 0;
    const bool down = dy > 0;
    const bool up = dy < 0;
    drawContext_->SetViewUpdateReason(kViewUpdateTranslated);
    shakeX_ = 0;
    shakeY_ = 0;
    if (cameraControl_) cameraControl_->SetCameraPositionTweakAdd(subpixelImageBiasX_, subpixelImageBiasY_);
    drawContext_->SetDefaultRenderStateUnilaterally();
    Set3DCameraView();
    if (fogEnabled_) drawContext_->SetFog(true, lightLevels_, fogStart_, fogEnd_);
    TurnOnLighting(true);
    if (left) {
        sourceX = 0;
        width = viewportWidth_ + dx;
        destinationX = -dx;
        if (down) {
            sourceY = 0;
            height = viewportHeight_ - dy;
            destinationY = dy;
            first = cRZRect{0, 0, -dx, viewportHeight_};
            second = cRZRect{-dx, 0, viewportWidth_, dy};
            savedLeft = -dx;
            savedTop = dy;
            savedRight = viewportWidth_;
            savedBottom = viewportHeight_;
        } else if (up) {
            sourceY = -dy;
            height = viewportHeight_ + dy;
            destinationY = 0;
            first = cRZRect{0, 0, -dx, viewportHeight_};
            second = cRZRect{-dx, viewportHeight_ + dy, viewportWidth_, viewportHeight_};
            savedLeft = -dx;
            savedTop = 0;
            savedRight = viewportWidth_;
            savedBottom = viewportHeight_ + dy;
        } else {
            sourceY = 0;
            height = viewportHeight_;
            destinationY = 0;
            first = cRZRect{0, 0, -dx, viewportHeight_};
            second = cRZRect{0, 0, 0, 0};
            savedLeft = -dx;
            savedTop = 0;
            savedRight = viewportWidth_;
            savedBottom = viewportHeight_;
        }
    } else if (right) {
        sourceX = dx;
        width = viewportWidth_ - dx;
        destinationX = 0;
        if (down) {
            sourceY = 0;
            height = viewportHeight_ - dy;
            destinationY = dy;
            first = cRZRect{viewportWidth_ - dx, 0, viewportWidth_, viewportHeight_};
            second = cRZRect{0, 0, viewportWidth_ - dx, dy};
            savedLeft = 0;
            savedTop = dy;
            savedRight = viewportWidth_ - dx;
            savedBottom = viewportHeight_;
        } else if (up) {
            sourceY = -dy;
            height = viewportHeight_ + dy;
            destinationY = 0;
            first = cRZRect{viewportWidth_ - dx, 0, viewportWidth_, viewportHeight_};
            second = cRZRect{0, viewportHeight_ + dy, viewportWidth_ - dx, viewportHeight_};
            savedLeft = 0;
            savedTop = 0;
            savedRight = viewportWidth_ - dx;
            savedBottom = viewportHeight_ + dy;
        } else {
            sourceY = 0;
            height = viewportHeight_;
            destinationY = 0;
            first = cRZRect{viewportWidth_ - dx, 0, viewportWidth_, viewportHeight_};
            second = cRZRect{0, 0, 0, 0};
            savedLeft = 0;
            savedTop = 0;
            savedRight = viewportWidth_ - dx;
            savedBottom = viewportHeight_;
        }
    } else {
        sourceX = 0;
        width = viewportWidth_;
        destinationX = 0;
        if (down) {
            sourceY = 0;
            height = viewportHeight_ - dy;
            destinationY = dy;
            first = cRZRect{0, 0, viewportWidth_, dy};
            second = cRZRect{0, 0, 0, 0};
            savedLeft = 0;
            savedTop = dy;
            savedRight = viewportWidth_;
            savedBottom = viewportHeight_;
        } else if (up) {
            sourceY = -dy;
            height = viewportHeight_ + dy;
            destinationY = 0;
            first = cRZRect{0, viewportHeight_ + dy, viewportWidth_, viewportHeight_};
            second = cRZRect{0, 0, 0, 0};
            savedLeft = 0;
            savedTop = 0;
            savedRight = viewportWidth_;
            savedBottom = viewportHeight_ + dy;
        } else {
            sourceY = 0;
            height = viewportHeight_;
            destinationY = 0;
            first = cRZRect{0, 0, 0, 0};
            second = cRZRect{0, 0, 0, 0};
        }
    }
    if (width > 0 && height > 0) RestoreBackingStore(destinationX, destinationY, width, height, sourceX, sourceY);
    UpdateViewportRect(first);
    UpdateViewportRect(second);
    UnsetClipRect();
    viewCleanupPending_ = true;
    cleanupRect_[0] = savedLeft;
    cleanupRect_[1] = savedTop;
    cleanupRect_[2] = savedRight;
    cleanupRect_[3] = savedBottom;
    frameTimer_.Reset();
    backingStoreValid_ = SaveBackingStore();
    if (!RendererSettings().TranslatedUpdateRebuildsDrawFrustum()) {
        viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);
        viewContainerRectValid_ = false;
        drawContainerRectValid_ = false;
    }
    Set3DCameraView();
    IRender3DSettings& settings = RendererSettings();
    if (settings.DrawDynamicViewEnabled()) {
        drawContext_->SetViewPass(3);
        for (const ViewObjectInfo& info : viewObjectLists_[0]) {
            if (info.object) info.object->DrawViewObject(*drawContext_);
        }
        DrawDynamicView();
        drawContext_->SetViewPass(5);
        for (const ViewObjectInfo& info : viewObjectLists_[1]) {
            if (info.object) info.object->DrawViewObject(*drawContext_);
        }
    }
    drawContext_->SetLighting(false);
    drawContext_->SetFog(false, nullptr, 3.4028235e38f, 3.4028235e38f);
    SetUICameraView();
    drawContext_->SetViewUpdateReason(kViewUpdateNone);
    inDraw_ = false;
}

void cSC43DRender::DoViewCleanup() {
    // 0x1007660ee: a translated update leaves a band of the store stale; every +0x358 ms one cleanupStep_-wide slice
    // of it is redrawn (reason 9) until the saved rectangle is used up.
    if (!viewCleanupPending_ || !unknown328_) return;
    if (!frameTimer_.IsTimerRunning()) {
        frameTimer_.Restart();
        return;
    }
    if (frameTimer_.GetElapsedTimeU32() <= static_cast<std::uint32_t>(unknown358_)) return;
    const std::int32_t right = cleanupRect_[2];
    std::int32_t left = cleanupStep_ + cleanupRect_[0];
    if (left > right) left = right;
    const cRZRect band{cleanupRect_[0], cleanupRect_[1], left, cleanupRect_[3]};
    DoPartialViewUpdate(band, kViewUpdateViewCleanup);
    cleanupRect_[0] = left;
    if (right <= left) {
        viewCleanupPending_ = false;
        frameTimer_.Reset();
    }
}

void cSC43DRender::ProcessDirtyRectangleQueue() {
    // 0x1007661ba: drain the dirty-rectangle queue. Kind 0 re-renders one model into its rectangle, every other kind
    // redraws the rectangle (kind 1 with reason 8, the rest with 7), then the store is refreshed if it went stale.
    processingDirtyRectangles_ = true;
    if (!dirtyRectangles_.empty()) {
        for (DirtyRectangleQueueItem& item : dirtyRectangles_) {
            cRZRect rect = item.rect;
            const bool visible = rect.right > 0 && rect.left < viewportWidth_ && rect.bottom > 0 &&
                                 rect.top < viewportHeight_;
            if (visible) {
                if (rect.left < 0) rect.left = 0;
                if (rect.right > viewportWidth_) rect.right = viewportWidth_;
                if (rect.top < 0) rect.top = 0;
                if (rect.bottom > viewportHeight_) rect.bottom = viewportHeight_;
                if (item.kind == 0) {
                    if (item.model) DoSingleModelStaticDrawAddition(item.model, rect);
                } else {
                    DoPartialViewUpdate(rect, item.kind == 1 ? kViewUpdateDirtyRectModel
                                                             : kViewUpdateDirtyRectPartial);
                }
            }
            if (item.model) item.model->Release();
        }
        dirtyRectangles_.clear();
        dirtyScratch_.clear();
        dirtyCount_ = 0;
        dirtyAccumulatedArea_ = 0;
    }
    if (!backingStoreValid_) backingStoreValid_ = SaveBackingStore();
    processingDirtyRectangles_ = false;
}

// ---------------------------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------------------------
void cSC43DRender::Draw() {
    // 0x1007659f6: one frame. A pending translated update is tried first; the static view is restored from (or saved
    // into) the backing store, the dynamic view and the two view-object lists follow, then the UI camera is restored.
    IRender3DSettings& settings = RendererSettings();
    if (viewOffsetX_ != 0 || viewOffsetY_ != 0) {
        const bool translated = AttemptTranslatedViewUpdate(viewOffsetX_, viewOffsetY_);
        viewOffsetX_ = 0;
        viewOffsetY_ = 0;
        if (translated) return;
    }
    if (inDraw_) return;
    inDraw_ = true;
    if (cameraControl_) {
        cameraControl_->SetCameraPositionTweakAdd(static_cast<float>(-shakeX_) + subpixelImageBiasX_,
                                                  static_cast<float>(shakeY_) + subpixelImageBiasY_);
    }
    ++changeCounter_;
    // 0x100765a93: the model changes queued since the last frame are applied before the static view pass.
    if (!modelChanges_.empty()) ProcessModelChangeQueue();
    drawContext_->SetDefaultRenderStateUnilaterally();
    drawContext_->SetLighting(false);
    bool restored = false;
    bool savedByRestore = false;
    if (!mbBackingStoreEnabled || !backingStoreValid_) {
        viewUtilities_->BuildDrawFrustumDataFromViewportRect(drawFrustumRect_, drawFrustumRect_ + 2);
        viewContainerRectValid_ = false;
        drawContainerRectValid_ = false;
        if (!viewportClearColorValid_) {
            viewportClearColorValid_ = true;
            mpGDriver->ClearColor(viewportClearColor_[0], viewportClearColor_[1], viewportClearColor_[2],
                                  viewportClearColor_[3]);
            mpGDriver->ClearDepth(static_cast<double>(unknown2b0_));
        }
        if (!backgroundPictureVisible_) {
            const bool clearDepth = mbBackingStoreEnabled && !backingStoreValid_;
            ClearViewport(clearDepth);
        }
    } else {
        RestoreBackingStore();
        restored = true;
        savedByRestore = true;
    }
    Set3DCameraView();
    if (fogEnabled_) drawContext_->SetFog(true, lightLevels_, fogStart_, fogEnd_);
    TurnOnLighting(true);
    if (!restored && backgroundPictureVisible_) {
        if (unusedAd_) {
            mpGDriver->ClearColor(0.6f, 0.6f, 0.7f, 1.0f);
            mpGDriver->Clear(0x4000);
        }
        // The original leaves ClearViewport's argument in a stale register here; the port passes false.
        ClearViewport(false);
        Set3DCameraView();
    }
    bool staticViewDrawn = false;
    if (!mbBackingStoreEnabled || backingStoreValid_) {
        if (!savedByRestore) {
            if (!dirtyRectangles_.empty()) ClearDirtyRectangleQueue();
            captureDirtyRectangles_ = false;
            Set3DCameraView();
            if (settings.StaticViewRedrawOnViewportChange()) {
                drawContext_->SetViewPass(0);
                for (const ViewObjectInfo& info : viewObjectLists_[3]) {
                    if (info.object) info.object->DrawViewObject(*drawContext_);
                }
                DrawStaticView();
                drawContext_->SetViewPass(2);
                for (const ViewObjectInfo& info : viewObjectLists_[2]) {
                    if (info.object) info.object->DrawViewObject(*drawContext_);
                }
            }
            staticViewDrawn = true;
        }
    } else {
        if (settings.StaticViewRedrawOnViewportChange()) {
            drawContext_->SetViewPass(0);
            for (const ViewObjectInfo& info : viewObjectLists_[3]) {
                if (info.object) info.object->DrawViewObject(*drawContext_);
            }
            DrawStaticView();
            drawContext_->SetViewPass(2);
            for (const ViewObjectInfo& info : viewObjectLists_[2]) {
                if (info.object) info.object->DrawViewObject(*drawContext_);
            }
        }
        backingStoreValid_ = SaveBackingStore();
    }
    if (!staticViewDrawn) {
        if (viewCleanupPending_ && settings.ViewCleanupEnabled()) DoViewCleanup();
        if (!dirtyRectangles_.empty()) {
            if (changeCounter_ < unknown324_ || unknown324_ + settings.DirtyRectangleMaxAgeFrames() < changeCounter_) {
                ProcessDirtyRectangleQueue();
            }
        }
        captureDirtyRectangles_ = false;
        Set3DCameraView();
    }
    if (settings.DrawDynamicViewEnabled()) {
        drawContext_->SetViewPass(3);
        for (const ViewObjectInfo& info : viewObjectLists_[0]) {
            if (info.object) info.object->DrawViewObject(*drawContext_);
        }
        DrawDynamicView();
        drawContext_->SetViewPass(5);
        for (const ViewObjectInfo& info : viewObjectLists_[1]) {
            if (info.object) info.object->DrawViewObject(*drawContext_);
        }
    }
    drawContext_->SetLighting(false);
    drawContext_->SetFog(false, nullptr, 3.4028235e38f, 3.4028235e38f);
    SetUICameraView();
    int offsetY = 0;
    float offsetX = 0.0f;
    if (!settings.ScreenShakeEnabled()) {
        shakeX_ = 0;   // the X and Y offsets are dropped together
    } else {
        SetShakeOffsets();
        shakeX_ += settings.ScreenShakeOffsetX();
        offsetY = shakeY_ + settings.ScreenShakeOffsetY();
        offsetX = static_cast<float>(-shakeX_);
    }
    shakeY_ = offsetY;
    if (cameraControl_) {
        cameraControl_->SetCameraPositionTweakAdd(offsetX + subpixelImageBiasX_,
                                                  static_cast<float>(shakeY_) + subpixelImageBiasY_);
    }
    drawContext_->SetViewUpdateReason(kViewUpdateNone);
    inDraw_ = false;
}

// ---------------------------------------------------------------------------------------------
// Screen-shake offsets
// ---------------------------------------------------------------------------------------------
void cSC43DRender::SetShakeOffsets() {
    // 0x1007663a8. PARTIAL: the per-shake curve / table sampling at the end of the original is not ported (see the
    // pending ledger). What is ported: the one-time random / sine tables, the reset of both offsets and dropping the
    // shakes whose fade window has elapsed. With no live shake — the common case, and the one the draw pass needs —
    // the result is identical to the original: both offsets are 0.
    if (sShakeTableRandom.empty()) {
        sShakeRandom = std::make_unique<cRZRandom>(0x58);   // cRZRandom::Seed(0x58)
        for (int index = 0; index < 0x40; ++index) {
            const float x = static_cast<float>(sShakeRandom->RandomDoubleRangeUniform(-10.0, 10.0));
            const float y = static_cast<float>(sShakeRandom->RandomDoubleRangeUniform(-10.0, 10.0));
            sShakeTableRandom.push_back(cS3DVector2{x, y});
        }
        // 64 entries around the circle: sin(index * 0.09817477) * 10 (2 * pi / 64 = 0.09817477).
        for (int index = 0; index < 0x40; ++index) {
            const float value = std::sin(static_cast<float>(index) * 0.09817477f) * 10.0f;
            sShakeTableSine.push_back(cS3DVector2{0.0f, value});
        }
    }
    shakeX_ = 0;
    shakeY_ = 0;
    if (shakes_.empty()) return;
    float now = 0.0f;
    shakeTimer_.GetElapsedTimeFloat(now);
    unknown3a0_ = now;
    for (auto it = shakes_.begin(); it != shakes_.end();) {
        // A shake's +0x10 field is the timestamp its fade ends; it lives on for `amplitude` seconds after that.
        const float elapsed = now - it->duration;
        if (elapsed > it->amplitude) {
            it = shakes_.erase(it);
            continue;
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------------------------
// Snapshots / picking (not ported; see the pending ledger file)
// ---------------------------------------------------------------------------------------------
bool cSC43DRender::TakeSnapshot(cIGZBuffer**, bool) { return false; }
bool cSC43DRender::TakeSnapshot(cIGZString&, bool) { return false; }
bool cSC43DRender::PickTerrain(const float*, const float*, float*, bool, bool) { return false; }
bool cSC43DRender::PickTerrain(int, int, float*, bool, bool) { return false; }
bool cSC43DRender::PickTerrainAlways(const float*, const float*, float*, bool) { return false; }
bool cSC43DRender::PickTerrainAlways(int, int, float*, bool) { return false; }
bool cSC43DRender::Pick(int, int, cISC4OccupantFilter*, s3d::cIS3DModelInstance*&) { return false; }
bool cSC43DRender::Pick(int, int, const float*, const float*, cISC4OccupantFilter*, s3d::cIS3DModelInstance*&) {
    return false;
}

bool cSC43DRender::DoMessage(cIGZMessage2* message) {
    // 0x100762b36 (PARTIAL: the camera zoom / rotation messages and the property reload are ported; the view
    // translation (0xa6b79612, the translated backing-store update), the terrain dirty rectangles (0x870bc918), the
    // full redraws (0xa6b79621 / 0x6acb92a9), the backing-store switch (0xc9f775bb), the lighting-manager swap
    // (0x8acaa95f) and the city / terrain set-up (0x26d31ec1 / 0x4a9f2ffe) are not. See
    // notes/ledger_pending/cSC43DRender.txt.)
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = standard->GetType();
    if (type == 0xa6b79601 || type == 0xa6b79602) {
        const auto* oldView = static_cast<const int*>(standard->GetVoid1());   // {zoom, rotation}
        const auto* newView = static_cast<const int*>(standard->GetVoid2());
        if (!oldView || !newView) return true;
        if (type == 0xa6b79601) {   // the rotation changed
            if (oldView[1] != newView[1]) DoViewRotationChange2(oldView[0], oldView[1], newView[0], newView[1]);
            return true;
        }
        viewOffsetX_ = 0;           // the zoom / rotation is done: a pending translated update is dropped
        viewOffsetY_ = 0;
        if (oldView[0] != newView[0]) DoViewZoomChange3(oldView[0], oldView[1], newView[0], newView[1]);
        if (oldView[1] != newView[1] && !modelChanges_.empty()) ProcessModelChangeQueue();
    } else if (type == 0xc53d10aa) {
        ReadProperties();
    }
    return true;
}

}  // namespace sc4
