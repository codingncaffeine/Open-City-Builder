// cSC4CameraControl: see CameraControl.h. Single-precision arithmetic is written in the order the original did.
#pragma STDC FP_CONTRACT OFF
#include "sc4/CameraControl.h"

#include <cmath>
#include <cstring>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/BackingStore.h"
#include "sc4/GeometryUtilities.h"

namespace sc4 {

const int kViewportCellPixelWidth[5] = {8, 16, 32, 73, 146};
const float kPitchViewAngle[5] = {0.5235988f, 0.61086524f, 0.6981317f, 0.7853982f, 0.7853982f};   // 0x100a2a2a0
static const float kYViewAngle = -0.3926991f;    // kSC4YViewAngle[0..4] (0x100a19640), all -pi / 8
static const float kHalfPi = 1.5707964f;

// The original allocates 0x178 bytes for this class (GZCOM_CreatecSC4CameraControl); the layout is asserted here
// so the unread tail fields cannot drift.
static_assert(sizeof(cSC4CameraControl) == 0x178,
              "cSC4CameraControl must match the original 0x178-byte object");

static ICameraControlHost* g_cameraHost = nullptr;
ICameraControlHost* CameraControlHost() { return g_cameraHost; }
void SetCameraControlHost(ICameraControlHost* host) { g_cameraHost = host; }

static float Abs(float v) { return v < 0.0f ? -v : v; }
static bool IsNaN(float v) { return v != v; }

bool IntersectWithGroundPlane(const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& out) {
    const float dy = direction.y;
    if (0.0001f <= Abs(dy)) {
        const float oy = origin.y;
        const float t = -oy / dy;
        if (0.0f <= t) {
            out.x = direction.x * t + origin.x;
            out.y = oy + dy * t;
            out.z = t * direction.z + origin.z;
            return true;
        }
    }
    return false;
}

// Mirrors SC4ViewUtilities::GetCityWidthFromOrthoView (0x100177278) so this class links without ViewUtilities.cpp.
float CityWidthFromOrthoView(float width, int zoom) {
    (void)zoom;   // every table entry is the same angle
    const float s = std::sin(kYViewAngle), c = std::cos(kYViewAngle);
    return c * width - s * width;
}

cSC4CameraControl::cSC4CameraControl() {
    up_[1] = 1.0f;     // +0x74
    for (int i = 0; i < 5; ++i) unitsPerPixel_[i] = 100.0f / float(1 << i);
}

cSC4CameraControl::~cSC4CameraControl() = default;

bool cSC4CameraControl::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4CameraControl || iid == kGZIID_cSC4CameraControlSerializable || iid == 1) {
        *out = this;
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4CameraControl::Release() {
    if (1 < refCount_) return --refCount_;
    delete this;   // vslot 0x38: deleting destructor
    return 0;
}

bool cSC4CameraControl::Init(const cS3DVector3& target, s3d::cS3DCamera* camera) {
    if (!initialised_) {
        if (!loadedFromStream_) {
            target_ = target;
        } else {
            sent_.zoom = zoom_;          // +0x118 <- +0x110 (zoom and rotation together)
            sent_.rotation = rotation_;
        }
        AttachToCamera(camera);
        UpdateCameraPosition(kCameraUpdateGeneral);
        initialised_ = true;
    }
    return true;
}

void cSC4CameraControl::AttachToCamera(s3d::cS3DCamera* camera) {
    if (camera_ == camera) return;
    if (camera) camera->AddRef();
    if (camera_ && camera_->Release() == 0) delete camera_;
    camera_ = camera;
    // The original calls SetViewport on the new camera without a null check (0x10076b2a6); the port leaves a null
    // camera detached instead of dereferencing it.
    if (camera_) camera_->SetViewport(float(viewportW_), float(viewportH_));
    if (initialised_) UpdateCameraPosition(kCameraUpdateGeneral);
}

bool cSC4CameraControl::UpdateCameraPosition(std::uint32_t kind) {
    if (!camera_) return false;
    SendPreViewChangeMessage(kind);
    if (kind != kCameraUpdateTarget) UpdateCameraZoomAndRotationParams();
    UpdateCameraPositionParams();
    ICameraControlHost* host = CameraControlHost();
    if (host && host->HasViewUtilities()) {
        host->UpdateViewUtilities(this);
        // kinds 1 and 2 rebuild the scroll bounds; the others only when none exist yet
        if (1 < kind - 1 && !bounds_.empty()) {
            SendPostViewChangeMessage(kind);
            return true;
        }
        UpdateScrollBounds();
    }
    SendPostViewChangeMessage(kind);
    if (kind == kCameraUpdateZoomRotation) ClampTargetToScrollBounds();
    return true;
}

void cSC4CameraControl::Shutdown() {
    if (camera_) {
        if (camera_->Release() == 0) delete camera_;
        camera_ = nullptr;
    }
}

bool cSC4CameraControl::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    stream << target_;
    stream.SetSint32(prev_.zoom);
    stream.SetSint32(prev_.rotation);
    stream.SetSint32(zoom_);
    stream.SetSint32(rotation_);
    stream.SetSint32(sent_.zoom);
    stream.SetSint32(sent_.rotation);
    return !stream.GetError();
}

bool cSC4CameraControl::Read(cIGZIStream& stream) {
    loadedFromStream_ = true;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    stream >> target_;
    stream.GetSint32(prev_.zoom);
    stream.GetSint32(prev_.rotation);
    stream.GetSint32(zoom_);
    stream.GetSint32(rotation_);
    stream.GetSint32(sent_.zoom);
    stream.GetSint32(sent_.rotation);
    return !stream.GetError();
}

bool cSC4CameraControl::ScrollByViewportAngle(float angle, float distance) {
    const float s = std::sin(angle), c = std::cos(angle);
    const float scale = unitsPerPixel_[zoom_];
    const float dx = c * distance * scale;
    const float dy = distance * scale * s;
    cS3DVector2 here;
    ConvertWorldPointToCameraPlanePoint(target_, here);
    cS3DVector2 wanted{dx + here.x, here.y - dy};
    cS3DVector2 clamped;
    ClampScrollToBounds(here, wanted, clamped);
    ConvertCameraPlanePointToWorldPoint(clamped, target_);
    cS3DVector3 hit;
    bool found = false;
    ICameraControlHost* host = CameraControlHost();
    if (host) found = host->PickTerrain(target_, viewDir_, hit);   // SL::TerrainView() vslot 0x18
    if (!found) found = IntersectWithGroundPlane(target_, viewDir_, hit);
    if (found) target_ = hit;
    return UpdateCameraPosition(kCameraUpdateTarget);
}

void cSC4CameraControl::ConvertWorldPointToCameraPlanePoint(const cS3DVector3& world, cS3DVector2& out) {
    const float x = world.x, y = world.y, z = world.z;
    const cS3DTransform& v = camera_->ViewXform();
    const float s = v.scale;
    out.x = (v.rotation[0][2] * (z - anchor_.z) + v.rotation[0][1] * (y - anchor_.y) +
             v.rotation[0][0] * (x - anchor_.x)) * s;
    out.y = ((z - anchor_.z) * v.rotation[1][2] + (y - anchor_.y) * v.rotation[1][1] +
             (x - anchor_.x) * v.rotation[1][0]) * s;
}

void cSC4CameraControl::ClampScrollToBounds(const cS3DVector2& from, const cS3DVector2& to, cS3DVector2& out) {
    if (!boundsEnabled_ || bounds_.empty()) {
        out = to;
        return;
    }
    const std::size_t n = bounds_.size();
    const float dx0 = to.x - from.x, dy0 = to.y - from.y;     // the whole requested move
    cS3DVector2 start = from, target = to;
    bool prevFlag = false, anyFlag = false;
    for (std::size_t iteration = 0; iteration < n; ++iteration) {
        float best = 1.0f;
        const cS3DVector2* bestPrev = nullptr;
        const cS3DVector2* bestCur = nullptr;
        const cS3DVector2* prev = &bounds_[n - 1];
        for (std::size_t j = 0; j < n; ++j) {
            const cS3DVector2* cur = &bounds_[j];
            const float ex = prev->x - cur->x, ey = prev->y - cur->y;
            const float len = std::sqrt(ey * ey + ex * ex);
            if (!(1e-8 > double(len))) {
                const float nx = ex / -len;        // unit edge direction x
                const float ny = ey / len;         // minus the unit edge direction y
                const float sTarget = target.y * nx + target.x * ny;
                const float sPrev = prev->y * nx + prev->x * ny;
                const float distTarget = sTarget - sPrev;      // signed distance of the target to the edge line
                if (!(distTarget > 0.0f)) {
                    const float distStart = (ny * start.x + nx * start.y) - sPrev;
                    if (!(distTarget >= distStart)) {
                        const float t = distStart / (distStart - distTarget);
                        if (best > t) {
                            bestPrev = prev;
                            bestCur = cur;
                        }
                        best = t < best ? t : best;
                    }
                }
            }
            prev = cur;
        }
        if (!bestPrev) {
            out = target;
            return;
        }
        const float k = best < 0.001f ? 1.0f : 1.0f - (best + -0.0001f);
        const float backX = k * (target.x - start.x), backY = k * (target.y - start.y);
        const float ex = bestCur->x - bestPrev->x, ey = bestCur->y - bestPrev->y;
        const float along = ex * dx0 + ey * dy0;
        const bool flag = (0.0f > along) ? true : prevFlag;
        const cS3DVector2 back{target.x - backX, target.y - backY};     // the point just before the wall
        if (!(0.0f > along)) anyFlag = true;
        if (flag && anyFlag) {
            out = back;
            return;
        }
        const float edgeLen = std::sqrt(ey * ey + ex * ex);
        const float inv = 1.0f / edgeLen;
        const float ux = inv * ex, uy = inv * ey;
        const float slide = uy * backY + backX * ux;
        target = cS3DVector2{ux * slide + back.x, uy * slide + back.y};   // slide the remaining move along the wall
        prevFlag = flag;
        start = back;
    }
    out = from;   // ran out of iterations
}

void cSC4CameraControl::ConvertCameraPlanePointToWorldPoint(const cS3DVector2& plane, cS3DVector3& out) {
    const float u = plane.x, v = plane.y;
    const cS3DTransform& t = camera_->ViewXform();
    const float inv = 1.0f / t.scale;
    out.x = (t.rotation[2][0] * 0.0f + t.rotation[1][0] * v + t.rotation[0][0] * u) * inv + anchor_.x;
    out.y = (t.rotation[2][1] * 0.0f + t.rotation[1][1] * v + t.rotation[0][1] * u) * inv + anchor_.y;
    out.z = inv * (t.rotation[2][2] * 0.0f + v * t.rotation[1][2] + u * t.rotation[0][2]) + anchor_.z;
}

bool cSC4CameraControl::ZoomIn() {
    const int virtualZoom = int(1.5f < magnification_) + zoom_;
    if (float(virtualZoom) < maxVirtualZoom_) return SetZoom(float(virtualZoom + 1));
    return false;
}

int cSC4CameraControl::GetVirtualZoom() const { return int(1.5f < magnification_) + zoom_; }

bool cSC4CameraControl::SetZoom(float zoom) {
    cSC43DBackingStore::justZoomed = true;   // _g_MacJustZoomed, read by cSC43DBackingStore::SaveBackingStore
    float magnification = 1.0f;
    if (minZoom_ <= zoom && zoom <= maxVirtualZoom_ && maxRealZoom_ < zoom) {
        // beyond the real zoom range the rest becomes a magnification
        magnification = std::exp2(zoom - maxRealZoom_);
        zoom = maxRealZoom_;
    }
    if (minZoom_ <= zoom && zoom <= maxRealZoom_) {
        const float current = magnification_;
        if (!(zoom == float(zoom_) && magnification == current)) {
            if (current < 1000.0f) {
                if (!(current == magnification) && 0.0f < current) {
                    magnification_ = magnification;
                    UpdateCameraPosition(kCameraUpdateMagnification);
                }
            }
            prev_.zoom = zoom_;
            prev_.rotation = rotation_;
            zoom_ = int(zoom);
            sent_.zoom = zoom_;
            sent_.rotation = rotation_;
            return UpdateCameraPosition(kCameraUpdateZoomRotation);
        }
    }
    return false;
}

bool cSC4CameraControl::ZoomOut() {
    const int virtualZoom = int(1.5f < magnification_) + zoom_;
    if (virtualZoom > 0) return SetZoom(float(virtualZoom - 1));
    return false;
}

void cSC4CameraControl::SetCustomMagnification(float magnification) {
    const float current = magnification_;
    if (current < 1000.0f) {
        if (!(current == magnification) && 0.0f < current) {
            magnification_ = magnification;
            UpdateCameraPosition(kCameraUpdateMagnification);
        }
    }
}

bool cSC4CameraControl::SetRotation(int rotation) {
    if (std::uint32_t(rotation) < 4 && rotation_ != rotation) {
        prev_.zoom = zoom_;
        prev_.rotation = rotation_;
        rotation_ = rotation;
        sent_.zoom = zoom_;
        sent_.rotation = rotation_;
        return UpdateCameraPosition(kCameraUpdateZoomRotation);
    }
    return true;   // quirk: an out-of-range or unchanged rotation reports success
}

void cSC4CameraControl::RotateLeft() { SetRotation((rotation_ + 1) % 4); }
void cSC4CameraControl::RotateRight() { SetRotation((rotation_ + 3) % 4); }

bool cSC4CameraControl::SetZoomAndRotation(int zoom, int rotation) {
    const float fz = float(zoom);
    float magnification = 1.0f;
    if (minZoom_ <= fz && fz <= maxVirtualZoom_ && maxRealZoom_ < fz) {
        magnification = std::exp2(fz - maxRealZoom_);
        zoom = int(maxRealZoom_);
    }
    if (minZoom_ <= float(zoom) && std::uint32_t(rotation) < 4 && float(zoom) <= maxRealZoom_) {
        const float current = magnification_;
        if (current < 1000.0f) {
            if (!(current == magnification) && 0.0f < current) {
                magnification_ = magnification;
                UpdateCameraPosition(kCameraUpdateMagnification);
            }
        }
        if (zoom != zoom_ || rotation_ != rotation) {
            prev_.zoom = zoom_;
            prev_.rotation = rotation_;
            zoom_ = zoom;
            rotation_ = rotation;
            sent_.zoom = zoom_;
            sent_.rotation = rotation_;
            return UpdateCameraPosition(kCameraUpdateZoomRotation);
        }
    }
    return false;
}

bool cSC4CameraControl::SetViewTargetPosition(const cS3DVector3& position) {
    if (IsNaN(position.x) || IsNaN(position.y) || IsNaN(position.z)) return false;
    if (!(-100000.0f <= position.x && position.x <= 100000.0f)) return false;
    if (!(-100000.0f <= position.y && position.y <= 100000.0f)) return false;
    if (!(-100000.0f <= position.z && position.z <= 100000.0f)) return false;
    target_ = position;
    velocity_ = cS3DVector3{0, 0, 0};
    UpdateCameraPosition(kCameraUpdateTarget);
    return true;
}

bool cSC4CameraControl::SetViewTargetVelocity(const cS3DVector3& velocity) {
    if (IsNaN(velocity.x) || IsNaN(velocity.y) || IsNaN(velocity.z)) return false;
    velocity_ = velocity;
    return true;
}

void cSC4CameraControl::SetViewportSize(int width, int height) {
    viewportW_ = width;
    viewportH_ = height;
    if (camera_) camera_->SetViewport(float(width), float(height));
}

void cSC4CameraControl::SetCameraPositionTweakAdd(float x, float y) {
    if (tweakX_ == x && tweakY_ == y) return;   // NaN never compares equal, so it always updates
    tweakX_ = x;
    tweakY_ = y;
    UpdateCameraPosition(kCameraUpdateTweak);
}

void cSC4CameraControl::SetCitySize(float x, float z) {
    citySizeX_ = x;
    citySizeZ_ = z;
    if (initialised_) {
        bounds_.clear();
        UpdateCameraPosition(kCameraUpdateCitySize);
    }
}

void cSC4CameraControl::GetBaseTargetPositionForRotation(float* out, int rotation) const {
    out[1] = 0.0f;
    switch (rotation) {
        case 0: out[0] = 0.0f; out[2] = 0.0f; break;
        case 1: out[0] = citySizeX_; out[2] = 0.0f; break;
        case 2: out[0] = citySizeX_; out[2] = citySizeZ_; break;
        case 3: out[0] = 0.0f; out[2] = citySizeZ_; break;
        default: break;
    }
}

void cSC4CameraControl::SendPreViewChangeMessage(std::uint32_t kind) {
    if (kind != kCameraUpdateZoomRotation) return;
    ICameraControlHost* host = CameraControlHost();
    if (!host) return;
    cIGZMessageServer2* server = host->MessageServer();
    cIGZMessage2Standard* message = host->CreateMessage();
    if (!message) return;
    message->SetType(kMsgCameraViewAboutToChange);                           // vslot 0x28
    message->SetVoid1(&prev_);                                           // vslot 0x60: +0x108
    message->SetVoid2(&sent_);                                           // vslot 0x88: +0x118
    if (server) server->MessageSend(message);                                // server vslot 0x18
    message->Release();
}

namespace {
float g_devicePixelScale = 1.0f;
}  // namespace

void cSC4CameraControl::SetDevicePixelScale(float scale) { g_devicePixelScale = scale > 0.0f ? scale : 1.0f; }
float cSC4CameraControl::DevicePixelScale() { return g_devicePixelScale; }

void cSC4CameraControl::UpdateCameraZoomAndRotationParams() {
    const int zoom = zoom_;
    const int pixels = kViewportCellPixelWidth[zoom];
    const float mag = magnification_;
    const float cityWidth = CityWidthFromOrthoView(16.0f, zoom);
    // logical pixels per cell: a logical pixel spans `scale` window pixels, so a cell keeps the original's window pixel width
    const float pixelMag = float(pixels) * mag / g_devicePixelScale;
    worldPerPixel_ = cityWidth / pixelMag;
    pixelPerWorld_ = pixelMag / cityWidth;
    const float yaw = yaw_;
    const int rotation = rotation_;
    const float pitch = kPitchViewAngle[zoom];
    const float a = citySizeX_ > 4096.0f ? citySizeX_ : 4096.0f;
    const float b = citySizeZ_ > 4096.0f ? citySizeZ_ : 4096.0f;
    const float diagonal = std::sqrt(b * b + a * a);
    const float sinPitch = std::sin(pitch), cosPitch = std::cos(pitch);
    const float reach = distance_ / cosPitch;
    pitch_ = pitch;
    near_ = float(double(reach - diagonal / cosPitch) * 0.95);
    far_ = 1.5f * reach;
    const float halfW = float(double(viewportW_) * 0.5 * double(worldPerPixel_));
    const float halfH = float(double(viewportH_) * 0.5 * double(worldPerPixel_));
    camera_->SetOrtho(-halfW, halfW, -halfH, halfH, near_, far_);
    const float angle = float(rotation) * kHalfPi + (yaw + kHalfPi);
    const float s = std::sin(angle), c = std::cos(angle);
    offset_.x = c * distance_;
    offset_.y = reach * sinPitch;
    offset_.z = distance_ * s;
}

void cSC4CameraControl::UpdateCameraPositionParams() {
    float base[3];
    GetBaseTargetPositionForRotation(base, rotation_);
    baseTarget_ = cS3DVector3{base[0], base[1], base[2]};     // +0x28 is zeroed by the call as well
    const float ox = offset_.x, oy = offset_.y, oz = offset_.z;
    anchor_ = cS3DVector3{baseTarget_.x + ox, baseTarget_.y + oy, oz + baseTarget_.z};
    // the original squares x and y together and adds z last (packed mulps at 0x10076c52e)
    const float lenSq = ox * ox + oy * oy + oz * oz;
    float factor = 0.0f;
    if (0.0f < lenSq) {
        factor = 1.0f - ((target_.y - baseTarget_.y) * oy + (target_.x - baseTarget_.x) * ox +
                         (target_.z - baseTarget_.z) * oz) / lenSq;
    }
    cameraPos_ = cS3DVector3{target_.x + ox * factor, target_.y + oy * factor, target_.z + factor * oz};
    camera_->SetPosition(cameraPos_);
    camera_->LookAt(target_, cS3DVector3{0.0f, 1.0f, 0.0f});
    camera_->SetViewportOffset(cS3DVector2{0.0f, 0.0f});
    cS3DVector3 screen{0, 0, 0};
    camera_->Project(baseTarget_, screen);
    // round to the nearest whole pixel (the original adds the 1.5 * 2^23 magic constant)
    auto roundDown = [](float v) {
        const float magic = v + -0.5f + 12582912.0f;
        std::int32_t bits;
        std::memcpy(&bits, &magic, 4);
        const std::int32_t lower = bits - 0x4b400000;
        std::int32_t upper = bits - 0x4b3fffff;
        if (float(upper) != v || IsNaN(float(upper)) || IsNaN(v)) upper = lower;
        return upper;
    };
    // snapped to whole window pixels: logical coordinates times the device pixel scale (identical at scale 1)
    const float s = g_devicePixelScale;
    const float sx = float(roundDown(screen.x * s)) / s, sy = float(roundDown(screen.y * s)) / s;
    const cS3DVector2 offset{(screen.x - sx) + tweakX_, tweakY_ - (screen.y - sy)};
    camera_->SetViewportOffset(offset);
    const cS3DTransform& v = camera_->ViewXform();
    viewDir_ = cS3DVector3{-v.rotation[2][0], -v.rotation[2][1], -v.rotation[2][2]};
    right_[0] = v.rotation[0][0]; right_[1] = v.rotation[0][1]; right_[2] = v.rotation[0][2];
    up_[0] = v.rotation[1][0]; up_[1] = v.rotation[1][1]; up_[2] = v.rotation[1][2];
    const float point[3] = {cameraPos_.x, cameraPos_.y, cameraPos_.z};
    const float normal[3] = {viewDir_.x, viewDir_.y, viewDir_.z};
    SC4GeometryUtilities::PointAndVectorToPlaneEquation(point, normal, plane_);   // 0x1000d6ad6
    prevPlanePoint_ = planePoint_;
    prevScreenOffset_ = screenOffset_;
    ConvertWorldPointToCameraPlanePoint(cameraPos_, planePoint_);
    screenOffset_.x = planePoint_.x * pixelPerWorld_ + offset.x;
    screenOffset_.y = pixelPerWorld_ * planePoint_.y + offset.y;
}

void cSC4CameraControl::UpdateScrollBounds() {
    if (!boundsEnabled_) return;
    ICameraControlHost* host = CameraControlHost();
    bounds_.clear();
    cS3DVector3 cornerA, cornerB, cornerC, cornerD;
    host->GetCityCornerPosition(1, cornerA);
    host->GetCityCornerPosition(0, cornerB);
    host->GetCityCornerPosition(2, cornerC);
    host->GetCityCornerPosition(3, cornerD);
    cS3DVector2 a, b, c, d;
    ConvertWorldPointToCameraPlanePoint(cornerA, a);
    ConvertWorldPointToCameraPlanePoint(cornerB, b);
    ConvertWorldPointToCameraPlanePoint(cornerC, c);
    ConvertWorldPointToCameraPlanePoint(cornerD, d);
    // how far the view may overshoot the city to the south; the original computes this one in double precision
    const float lift = float(double(float((double(offset_.y) * 0.48) / double(up_[1]))) +
                             double(viewportH_) * (double(worldPerPixel_) * -0.5));
    c.x = c.x + 500.0f;
    c.y = c.y + 0.0f;
    b.x = b.x + -500.0f;
    bounds_.push_back(cS3DVector2{a.x + 0.0f, a.y + lift});
    bounds_.push_back(cS3DVector2{b.x + 0.0f, b.y + lift});
    bounds_.push_back(b);
    bounds_.push_back(d);
    bounds_.push_back(c);
    bounds_.push_back(cS3DVector2{c.x + 0.0f, lift + c.y});
}

void cSC4CameraControl::SendPostViewChangeMessage(std::uint32_t kind) {
    if (!(kind < 5 && kind != kCameraUpdateCitySize)) return;
    ICameraControlHost* host = CameraControlHost();
    if (!host) return;
    cIGZMessageServer2* server = host->MessageServer();
    cIGZMessage2Standard* message = host->CreateMessage();
    if (!message) return;
    const bool zoomOrTarget = (kind | 1) == 3;
    if (zoomOrTarget) {
        message->SetType(kMsgCameraChangedBy);
        message->SetVoid1(this);
        if (server) server->MessageSend(message);
    }
    if (kind == kCameraUpdateGeneral) message->SetType(kMsgCameraViewChanged);
    else if (kind == kCameraUpdateZoomRotation) message->SetType(kMsgCameraZoomRotationChanged);
    else if (kind == kCameraUpdateMagnification) message->SetType(kMsgCameraMagnificationChanged);
    else if (kind == kCameraUpdateTarget) message->SetType(kMsgCameraTargetChanged);
    message->SetVoid1(&prev_);
    message->SetVoid2(&sent_);
    if (server) server->MessageSend(message);
    if (zoomOrTarget) {
        message->SetType(kind == kCameraUpdateZoomRotation ? kMsgCameraZoomRotationDone : kMsgCameraTargetDone);
        if (server) server->MessageSend(message);
    }
    message->Release();
}

void cSC4CameraControl::ClampTargetToScrollBounds() {
    cS3DVector2 anchorPlane{0, 0};
    ConvertWorldPointToCameraPlanePoint(baseTarget_, anchorPlane);
    cS3DVector2 targetPlane{0, 0};
    ConvertWorldPointToCameraPlanePoint(target_, targetPlane);
    cS3DVector2 clamped{0, 0};
    ClampScrollToBounds(anchorPlane, targetPlane, clamped);
    const float dx = clamped.x - targetPlane.x;
    if (0.0001f <= Abs(dx) || 0.0001f <= Abs(clamped.y - targetPlane.y)) {
        ConvertCameraPlanePointToWorldPoint(clamped, target_);
        cS3DVector3 hit{0, 0, 0};
        if (IntersectWithGroundPlane(target_, viewDir_, hit)) {
            target_ = hit;
            velocity_ = cS3DVector3{0, 0, 0};
        }
        UpdateCameraPosition(kCameraUpdateTarget);
    }
}

}  // namespace sc4
