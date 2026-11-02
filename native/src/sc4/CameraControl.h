// cSC4CameraControl (43 functions, 0x10076afe8..0x10076ce2c): the city view's camera controller. It owns the zoom
// level (0..4 real, up to 5 "virtual" through a custom magnification), the rotation (0..3 quarter turns), the view
// target and its velocity, and drives a cS3DCamera that is set up as an orthographic camera looking at the target
// from a fixed offset. It also keeps a polygon of scroll bounds in camera-plane coordinates and clamps scrolling to it.
// Class id 0xc9c628ec (also answers 0xe4fda3d4 and 1); it is a cIGZSerializable (Write / Read the target and the
// zoom / rotation pairs).
//
// Original layout (0x178 bytes, the size GZCOM_CreatecSC4CameraControl allocates): +8 initialised, +9 loaded from a
// stream, +0xc refcount, +0x10 camera (referenced), +0x18 anchor point (base + offset), +0x24 base target for the
// rotation, +0x30..+0x3f four floats the recovered methods never read (zeroed by the constructor), +0x40 camera
// position, +0x4c view target, +0x58 target velocity, +0x64 view direction, +0x70 camera up row, +0x7c camera
// right row, +0x88 ground-plane equation through the camera position, +0x98 / +0xa0 camera-plane point and its
// screen offset (+0xa8 / +0xb0 the previous ones), +0xb8 / +0xbc position tweak, +0xc0 world units per pixel per
// zoom level, +0xd4..+0xdc three floats and +0xe0..+0xf7 six FLT_MAX floats (never read by the recovered methods,
// zeroed / set by the constructor), +0xf8 custom magnification, +0xfc / +0x100 / +0x104 min / max real / max
// virtual zoom, +0x108 / +0x10c previous zoom / rotation, +0x110 / +0x114 zoom / rotation, +0x118 / +0x11c copy
// sent with the messages, +0x120 yaw, +0x124 pitch, +0x128 camera distance, +0x12c / +0x130 clip planes,
// +0x134 / +0x138 viewport size, +0x13c / +0x140 world units per pixel and back, +0x144 camera offset from the
// base target, +0x150 bounds enabled, +0x158 scroll bounds (std::vector), +0x170 / +0x174 city size in world
// units.
//
// Every SetZoom sets the Mac port's shared _g_MacJustZoomed flag, cSC43DBackingStore::justZoomed (the backing
// store reads it and clears it in SaveBackingStore(), 0x1007641b4).
//
// Quirks kept: SetRotation returns true when the rotation is out of range or unchanged (0x10076bd7a); the "virtual
// zoom" is zoom + 1 while the magnification is above 1.5; ClampScrollToBounds returns the
// start point when its slide loop runs out of iterations (0x10076b71a); SendPreViewChangeMessage /
// SendPostViewChangeMessage dereference the message server without a null check in the original (the port skips
// the send when the host supplies none); AttachToCamera dereferences the new camera without a null check
// (0x10076b2a6, the port keeps a null camera detached).
// Compile without FMA contraction.
#pragma once
#include <cfloat>
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "s3d/Camera.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4CameraControl = 0xc9c628ec;
constexpr std::uint32_t kGZIID_cSC4CameraControlSerializable = 0xe4fda3d4;   // cIGZSerializable's IID

// Message types the controller sends (0x10076c1b0, 0x10076cb0c).
constexpr std::uint32_t kMsgCameraViewAboutToChange = 0xa6b79600;   // pre change, zoom / rotation
constexpr std::uint32_t kMsgCameraZoomRotationChanged = 0xa6b79601;
constexpr std::uint32_t kMsgCameraZoomRotationDone = 0xa6b79602;
constexpr std::uint32_t kMsgCameraTargetChanged = 0xa6b79611;
constexpr std::uint32_t kMsgCameraTargetDone = 0xa6b79612;
constexpr std::uint32_t kMsgCameraViewChanged = 0xa6b79620;
constexpr std::uint32_t kMsgCameraMagnificationChanged = 0xa6b79621;
constexpr std::uint32_t kMsgCameraChangedBy = 0x4a9f2ffe;           // carries the controller itself

// What UpdateCameraPosition was asked to do (the argument of the Send*ViewChangeMessage functions).
enum CameraUpdateKind : std::uint32_t {
    kCameraUpdateGeneral = 0,
    kCameraUpdateCitySize = 1,
    kCameraUpdateZoomRotation = 2,
    kCameraUpdateTarget = 3,
    kCameraUpdateMagnification = 4,
    kCameraUpdateTweak = 5,
};

class cSC4CameraControl;

// Everything the controller reaches outside itself.
class ICameraControlHost {
public:
    virtual ~ICameraControlHost() = default;
    // RZGetFramework()->GetSystemService(0x4fa845b, 0x652294c7); null when the framework is not up.
    virtual cIGZMessageServer2* MessageServer() = 0;
    // new cRZMessage2Standard, AddRef'd once; the controller Releases it after sending.
    virtual cIGZMessage2Standard* CreateMessage() = 0;
    // SL::spViewUtilities != null.
    virtual bool HasViewUtilities() = 0;
    // SC4ViewUtilities::UpdateCameraPosition(spViewUtilities, control) @ 0x1001745da
    virtual void UpdateViewUtilities(cSC4CameraControl* control) = 0;
    // SC4ViewUtilities::GetCityCornerPosition(spViewUtilities, corner, out) @ 0x10017708a
    virtual void GetCityCornerPosition(int corner, cS3DVector3& out) = 0;
    // SL::TerrainView() vslot 0x18 (origin, direction, 0, hitPoint, 1): terrain ray cast. hit is written on success.
    virtual bool PickTerrain(const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& hit) = 0;
};
ICameraControlHost* CameraControlHost();
void SetCameraControlHost(ICameraControlHost* host);

// 0x10076baeb: intersects the ray (origin, direction) with the plane y = 0. Fails for a (nearly) horizontal ray or
// when the plane lies behind the origin.
bool IntersectWithGroundPlane(const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& out);
// SC4ViewUtilities::GetCityWidthFromOrthoView @ 0x100177278: cos(a) * w - sin(a) * w with a = kSC4YViewAngle[zoom].
// Kept here as a local mirror (all five table entries are the same angle) so the camera control links without
// sc4/ViewUtilities.cpp; it is behaviourally identical to the recovered static method.
float CityWidthFromOrthoView(float width, int zoom);

// kSC4ViewportCellPixelWidth (0x100a2a2c0) and kSC4PitchViewAngle (0x100a2a2a0), indexed by zoom level 0..4. The
// pitch table is a local copy of SC4ViewUtilities' kSC4PitchViewAngle (same address, same five values).
extern const int kViewportCellPixelWidth[5];
extern const float kPitchViewAngle[5];

class cSC4CameraControl : public cIGZSerializable {
public:
    cSC4CameraControl();                      // 0x10076affe (0x10076afe8 forwards to it)
    virtual ~cSC4CameraControl();             // 0x10076b194 / 0x10076b1c0

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x10076b224
    std::uint32_t AddRef() override { return ++refCount_; }        // 0x10076b1fa
    std::uint32_t Release() override;                              // 0x10076b208
    bool Write(cIGZOStream& stream) const override;                // 0x10076b3e6
    bool Read(cIGZIStream& stream) override;                       // 0x10076b480
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4CameraControl; }   // 0x10076ce2c

    bool Init(const cS3DVector3& target, s3d::cS3DCamera* camera);   // 0x10076b24e
    void AttachToCamera(s3d::cS3DCamera* camera);                    // 0x10076b2a6
    bool UpdateCameraPosition(std::uint32_t kind);                   // 0x10076b30e
    void Shutdown();                                                 // 0x10076b3be

    bool ScrollByViewportAngle(float angle, float distance);         // 0x10076b540 (returns UpdateCameraPosition's result)
    void ConvertWorldPointToCameraPlanePoint(const cS3DVector3& world, cS3DVector2& out);   // 0x10076b670
    void ClampScrollToBounds(const cS3DVector2& from, const cS3DVector2& to, cS3DVector2& out);   // 0x10076b71a
    void ConvertCameraPlanePointToWorldPoint(const cS3DVector2& plane, cS3DVector3& out);   // 0x10076ba0c

    bool ZoomIn();                                                   // 0x10076bb7c
    int GetVirtualZoom() const;                                      // 0x10076bbc2
    bool SetZoom(float zoom);                                        // 0x10076bbe2
    bool ZoomOut();                                                  // 0x10076bcfc
    float GetCustomMagnification() const { return magnification_; }   // 0x10076bd30
    void SetCustomMagnification(float magnification);                // 0x10076bd3e
    bool SetRotation(int rotation);                                  // 0x10076bd7a
    void RotateLeft();                                               // 0x10076bdbc
    void RotateRight();                                              // 0x10076bde2
    bool SetZoomAndRotation(int zoom, int rotation);                 // 0x10076be08
    bool SetViewTargetPosition(const cS3DVector3& position);         // 0x10076bf2a
    bool SetViewTargetVelocity(const cS3DVector3& velocity);         // 0x10076bffc
    void GetViewportSize(int& width, int& height) const { width = viewportW_; height = viewportH_; }   // 0x10076c07a
    void SetViewportSize(int width, int height);                     // 0x10076c090
    // Window pixels per logical (game-resolution) pixel. The original draws the city texel for pixel; with a UI scale the
    // viewport is logical while the scene rasterises at full resolution, so the projection works in real pixels: the city
    // keeps the original's pixel size (more of it shows) and its pre-rendered building textures stay sharp. 1 = the original.
    static void SetDevicePixelScale(float scale);
    static float DevicePixelScale();
    void GetDistancesToClipPlanes(float& nearPlane, float& farPlane) const { nearPlane = near_; farPlane = far_; }   // 0x10076c0ba
    void SetCameraPositionTweakAdd(float x, float y);                // 0x10076c0d6
    void SetCitySize(float x, float z);                              // 0x10076c114
    void GetBaseTargetPositionForRotation(float* out, int rotation) const;   // 0x10076c148
    void SendPreViewChangeMessage(std::uint32_t kind);               // 0x10076c1b0
    void UpdateCameraZoomAndRotationParams();                        // 0x10076c2a6
    void UpdateCameraPositionParams();                               // 0x10076c498
    void UpdateScrollBounds();                                       // 0x10076c7a4
    void SendPostViewChangeMessage(std::uint32_t kind);              // 0x10076cb0c
    void ClampTargetToScrollBounds();                                // 0x10076ccca
    void SendViewChangeMessage() { SendPostViewChangeMessage(kCameraUpdateGeneral); }   // 0x10076ce08

    // state the other view classes read
    s3d::cS3DCamera* Camera() const { return camera_; }
    const cS3DVector3& ViewTarget() const { return target_; }
    const cS3DVector3& ViewDirection() const { return viewDir_; }
    const cS3DVector3& CameraPosition() const { return cameraPos_; }
    int Zoom() const { return zoom_; }
    int Rotation() const { return rotation_; }
    bool Initialised() const { return initialised_; }
    bool BoundsEnabled() const { return boundsEnabled_; }
    void SetBoundsEnabled(bool enabled) { boundsEnabled_ = enabled; }
    const std::vector<cS3DVector2>& ScrollBounds() const { return bounds_; }
    const float* GroundPlane() const { return plane_; }
    cS3DVector2 CameraPlanePoint() const { return planePoint_; }
    cS3DVector2 ScreenOffset() const { return screenOffset_; }
    float WorldPerPixel() const { return worldPerPixel_; }
    float PixelPerWorld() const { return pixelPerWorld_; }   // +0x140 (read by SC4ViewUtilities::UpdateCameraPosition)
    float YawAngle() const { return yaw_; }
    float PitchAngle() const { return pitch_; }

private:
    bool initialised_ = false;                 // +8
    bool loadedFromStream_ = false;            // +9
    std::uint32_t refCount_ = 0;               // +0xc
    s3d::cS3DCamera* camera_ = nullptr;        // +0x10
    cS3DVector3 anchor_;                       // +0x18
    cS3DVector3 baseTarget_;                   // +0x24
    [[maybe_unused]] float reserved30_[4] = {0, 0, 0, 0};       // +0x30: never read, the constructor zeroes it
    cS3DVector3 cameraPos_;                    // +0x40
    cS3DVector3 target_;                       // +0x4c
    cS3DVector3 velocity_;                     // +0x58
    cS3DVector3 viewDir_;                      // +0x64
    float up_[3] = {0, 0, 0};                  // +0x70 (the constructor leaves +0x74 at 1.0)
    float right_[3] = {0, 0, 0};               // +0x7c
    float plane_[4] = {0, 0, 0, 0};            // +0x88
    cS3DVector2 planePoint_;                   // +0x98
    cS3DVector2 screenOffset_;                 // +0xa0
    cS3DVector2 prevPlanePoint_;               // +0xa8
    cS3DVector2 prevScreenOffset_;             // +0xb0
    float tweakX_ = 0, tweakY_ = 0;            // +0xb8 / +0xbc
    float unitsPerPixel_[5] = {};              // +0xc0: 100 / (1 << zoom)
    [[maybe_unused]] float reservedD4_[3] = {0, 0, 0};          // +0xd4: never read, the constructor zeroes it
    [[maybe_unused]] float reservedE0_[6] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};   // +0xe0: never read
    float magnification_ = 1.0f;               // +0xf8
    float minZoom_ = 0.0f;                     // +0xfc
    float maxRealZoom_ = 4.0f;                 // +0x100
    float maxVirtualZoom_ = 5.0f;              // +0x104
    struct ZoomRotation { int zoom = 0, rotation = 0; };   // the messages carry pointers to these pairs
    ZoomRotation prev_;                        // +0x108 / +0x10c
    int zoom_ = 0, rotation_ = 0;              // +0x110 / +0x114
    ZoomRotation sent_;                        // +0x118 / +0x11c
    float yaw_ = -0.39269909f;                 // +0x120 (-pi / 8)
    float pitch_ = 0.5235988f;                 // +0x124 (pi / 6)
    float distance_ = 8192.0f;                 // +0x128
    float near_ = 0.0f, far_ = 0.0f;           // +0x12c / +0x130
    int viewportW_ = 800, viewportH_ = 600;    // +0x134 / +0x138
    float worldPerPixel_ = 1.0f;               // +0x13c
    float pixelPerWorld_ = 1.0f;               // +0x140
    cS3DVector3 offset_;                       // +0x144
    bool boundsEnabled_ = true;                // +0x150
    std::vector<cS3DVector2> bounds_;          // +0x158
    float citySizeX_ = 4096.0f, citySizeZ_ = 4096.0f;   // +0x170 / +0x174
};

}  // namespace sc4
