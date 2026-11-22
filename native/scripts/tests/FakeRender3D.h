// A cISC43DRender that does nothing except what the tests watch: the viewport size it reports and the viewport rectangles
// drawn through it (the screen flash). Generated from the interface in src/sc4/Render3D.h; regenerate when it changes.
#pragma once
#include <vector>
#include "sc4/Render3D.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

class FakeRender3D final : public cISC43DRender {
public:
    struct Rectangle { cRZRect rect; float color[4]; bool depthMask, colorMask; };
    std::uint32_t width = 640, height = 480;
    std::vector<Rectangle> rectangles;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void GetViewportSize(std::uint32_t& w, std::uint32_t& h) const override { w = width; h = height; }
    void DrawViewportRectangle(const cRZRect& rect, const float* color, bool a, bool b) override {
        rectangles.push_back({rect, {color[0], color[1], color[2], color[3]}, a, b});
    }
    bool Init() override { return {}; }
    bool Shutdown() override { return {}; }
    void SetMapScaleFactor(int factor) override { }
    void SetGraphicSystem(IRender3DGraphicSystem* system) override { }
    void SetGDriver(gd::cIGZGDriver* driver) override { }
    SC4ViewUtilities* GetViewUtilities() const override { return {}; }
    IS4LightingManager* GetLightingManager() const override { return {}; }
    SC4DrawContext* GetDrawContext() const override { return {}; }
    void SetViewportSize(std::uint32_t width, std::uint32_t height) override { }
    void SetSubpixelImageBias(float x, float y) override { }
    s3d::cS3DCamera* GetCamera() override { return {}; }
    void SetCamera(s3d::cS3DCamera* camera) override { }
    sc4::cSC4CameraControl* GetCameraControl() override { return {}; }
    void SetCameraControl(sc4::cSC4CameraControl* control) override { }
    const float* GetProjectionMatrixEntries() const override { return {}; }
    const float* GetViewMatrixEntries() const override { return {}; }
    void ForceFullRedraw() override { }
    void Draw() override { }
    bool TakeSnapshot(cIGZBuffer** out, bool a) override { return {}; }
    bool TakeSnapshot(cIGZString& path, bool a) override { return {}; }
    void Set3DCameraView() override { }
    void Set2DCameraView() override { }
    void SetUICameraView() override { }
    bool IsModelInserted(s3d::cIS3DModelInstance* model) override { return {}; }
    bool InsertModel(s3d::cIS3DModelInstance* model) override { return {}; }
    bool RemoveModel(s3d::cIS3DModelInstance* model) override { return {}; }
    bool MoveModel(s3d::cIS3DModelInstance* model) override { return {}; }
    bool ChangeModel(s3d::cIS3DModelInstance* model, int change) override { return {}; }
    bool AddViewObject(cISC4ViewObject3D* object, int list, std::uint32_t priority) override { return {}; }
    bool RemoveViewObject(cISC4ViewObject3D* object) override { return {}; }
    bool FindViewObject(cISC4ViewObject3D* object) const override { return {}; }
    void SetZoom(int zoom) override { }
    void ZoomIn() override { }
    void ZoomOut() override { }
    void SetRotation(int rotation) override { }
    void RotateLeft() override { }
    void RotateRight() override { }
    void SetZoomAndRotation(int zoom, int rotation) override { }
    std::uint32_t StartScreenShake(float amplitude, float duration) override { return {}; }
    std::uint32_t StartScreenShake(const cSC4ShakeDescription& description, const cS3DVector2* position) override { return {}; }
    void StopScreenShake(std::uint32_t serial) override { }
    void GetShakeOffsets(float& x, float& y) override { }
    void SetViewportClearColor(const float* color) override { }
    void ClearViewport(bool clearDepth) override { }
    void ClearViewportWithSolidColor(const float* color, bool a, bool b) override { }
    void DrawTexturedViewportRectangle(const cRZRect& rect, std::uint32_t texture, float alpha, bool a, bool b) override { }
    void EnableBackgroundPicture(bool enable) override { }
    cIGZBuffer* GetBackgroundPicture(bool* visible) const override { return {}; }
    bool SetBackgroundPicture(cIGZBuffer* buffer, bool visible) override { return {}; }
    bool SetBackgroundPicture(std::uint32_t texture, int width, int height, bool visible) override { return {}; }
    void EnableTransparentBackground(bool enable) override { }
    bool GetBackingStoreEnabled() override { return {}; }
    void SetBackingStoreEnabled(bool enable) override { }
    bool BorrowRenderSystem() override { return {}; }
    bool ReturnRenderSystem() override { return {}; }
    bool PickTerrain(const float* origin, const float* direction, float* out, bool a, bool b) override { return {}; }
    bool PickTerrain(int x, int y, float* out, bool a, bool b) override { return {}; }
    bool PickTerrainAlways(const float* origin, const float* direction, float* out, bool a) override { return {}; }
    bool PickTerrainAlways(int x, int y, float* out, bool a) override { return {}; }
    bool Pick(int x, int y, sc4::cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& out) override { return {}; }
    bool Pick(int x, int y, const float* origin, const float* direction, sc4::cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& out) override { return {}; }
    bool GetModelsInVolume(const cS3DBoundingBox& box, s3d::cIS3DModelInstance** out, std::uint32_t& capacity) override { return {}; }
    bool IsModelVisible(s3d::cIS3DModelInstance* model, FrustumType frustum) override { return {}; }
    bool IsModelVisible(SC43DGridElement* element, FrustumType frustum) override { return {}; }
    bool GetViewFogEnabled() const override { return {}; }
    bool SetViewFogEnabled(bool enable) override { return {}; }
    bool GetLightingEnabled() const override { return {}; }
    bool SetLightingEnabled(bool enable) override { return {}; }
    void GetStatsString1(cIGZString& out) override { }
    void GetStatsString2(cIGZString& out) override { }
};
#pragma GCC diagnostic pop
