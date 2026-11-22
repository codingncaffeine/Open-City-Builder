// SC4ViewUtilities — implementation. See ViewUtilities.h for the layout, the seams and the list of quirks kept.
// Every function is a hand port of the exported decompile, cross-checked against the disassembly where the
// decompiler lost the SIMD lane structure (IsBoundingBoxIn*Frustum, IsPointInViewFrustum, the frustum corner and
// plane blocks, the rounding helpers). Compile without FMA contraction: results follow the original operation order.
#pragma STDC FP_CONTRACT OFF

#include "sc4/ViewUtilities.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "sc4/CameraControl.h"
#include "sc4/GeometryUtilities.h"
#include "s3d/Camera.h"
#include "s3d/S3DMath.h"

namespace {

constexpr float kPiOver2 = 1.5707964f;             // 0x100a18194
constexpr float kRoundMagic = 12582912.0f;         // 1.5 * 2^23, the float->int rounding bias
constexpr int kRoundMagicInt = 12582912;   // 0xC00000 (the float bias as an integer)
constexpr int kBlockSize = 64;                     // the city block size used by the visible-area rectangle
constexpr float kGroundPlane[4] = {0.0f, 1.0f, 0.0f, 0.0f};   // RayIntersectsGroundPlane::fPlaneEquation (0x100a19680)

// (int)(v +- 0.5 + 12582912) - 12582912: the original's floor(v +- 0.5) for the viewport rectangle.
inline int RoundDownToInt(float v) { return static_cast<int>(v - 0.5f + kRoundMagic) - kRoundMagicInt; }
inline int RoundUpToInt(float v) { return static_cast<int>(v + 0.5f + kRoundMagic) - kRoundMagicInt; }

// The wrap the original uses in GetVisibleEdges / RotateVector / GetVectorForEdge: v - ((v + bias) & ~3) with bias 3
// for negative v. It behaves like v mod 4 for v >= 0 and yields a negative result for negative v.
inline int BlockModulo(int v) {
    const unsigned bias = static_cast<unsigned>(v >> 31) >> 30;
    return v - ((v + static_cast<int>(bias)) & -4);
}

inline float Dot3(const cS3DVector3& a, const float* b) { return a.x * b[0] + a.y * b[1] + a.z * b[2]; }

inline void Copy3(float* dst, const float* src) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}
inline void Copy3(float* dst, const cS3DVector3& src) {
    dst[0] = src.x;
    dst[1] = src.y;
    dst[2] = src.z;
}
inline void Scale3(float* v, float s) {
    v[0] *= s;
    v[1] *= s;
    v[2] *= s;
}

}  // namespace

const float kSC4YViewAngle[5] = {-0.3926991f, -0.3926991f, -0.3926991f, -0.3926991f, -0.3926991f};   // 0x100a19640
const float kSC4PitchViewAngle[5] = {0.5235988f, 0.61086524f, 0.6981317f, 0.7853982f, 0.7853982f};  // 0x100a2a2a0

SC4ViewUtilities::SC4ViewUtilities() {   // 0x100173480: all fields zero, except the visible terrain rectangle's max = 1, 1
    // Layout of the original object (asserted here because offsetof needs a member context).
    static_assert(offsetof(SC4ViewUtilities, host_) == 0x08, "layout");
    static_assert(offsetof(SC4ViewUtilities, eye_) == 0x44, "layout");
    static_assert(offsetof(SC4ViewUtilities, view_) == 0x80, "layout");
    static_assert(offsetof(SC4ViewUtilities, viewPlanes_) == 0x168, "layout");
    static_assert(offsetof(SC4ViewUtilities, viewEdges_) == 0x210, "layout");
    static_assert(offsetof(SC4ViewUtilities, draw_) == 0x244, "layout");
    static_assert(offsetof(SC4ViewUtilities, drawPlanes_) == 0x31c, "layout");
    static_assert(offsetof(SC4ViewUtilities, terrainCenter_) == 0x3e4, "layout");
    static_assert(offsetof(SC4ViewUtilities, visibleTerrainRect_) == 0x3f4, "layout");
    static_assert(offsetof(SC4ViewUtilities, visibleAreaBlockMin_) == 0x434, "layout");
    static_assert(offsetof(SC4ViewUtilities, sinYaw_) == 0x450, "layout");
    static_assert(sizeof(SC4ViewUtilities) == 0x478, "layout");
}

// ---------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------
void SC4ViewUtilities::ClipWorldPositionToCityPosition(float* position) const {   // 0x1001736e2
    const float x = position[0];
    const float z = position[2];
    float newX = 0.0f;
    if (0.0f <= x) {
        newX = x;
        if (citySizeX_ <= x) newX = citySizeX_ - 1.0f;
    }
    float newZ = 0.0f;
    if (0.0f <= z) {
        newZ = z;
        if (citySizeZ_ <= z) newZ = citySizeZ_ - 1.0f;
    }
    position[0] = newX;
    position[2] = newZ;
}

float SC4ViewUtilities::ViewportUnitsToWorldUnits(float units) const {   // 0x10017374c
    return units * worldPerViewportUnit_;
}

void SC4ViewUtilities::GetZoomAndRotation(int& zoom, int& rotation) const {   // 0x10017375a
    zoom = zoom_;
    rotation = rotation_;
}

void SC4ViewUtilities::GetVisibleEdges(int& edgeA, int& edgeB) const {   // 0x10017376a
    edgeA = BlockModulo(rotation_ + 1);
    edgeB = BlockModulo(edgeA + 1);
}

void SC4ViewUtilities::GetCameraViewAngles(float& yaw, float& pitch, float& screenPitch) const {   // 0x1001737ae
    GetCameraViewAnglesForRotation(rotation_, zoom_, yaw, pitch, screenPitch);
}

void SC4ViewUtilities::GetCameraViewAnglesForRotation(int rotation, int zoom, float& yaw, float& pitch,
                                                      float& screenPitch) {   // 0x1001737ca
    const float tableYaw = kSC4YViewAngle[zoom];
    yaw = static_cast<float>(rotation) * kPiOver2 + tableYaw;
    pitch = kSC4PitchViewAngle[zoom];
    // The original divides tan(pitch) by cos(kSC4YViewAngle[zoom]) (no bounds check on the zoom).
    screenPitch = std::atan(std::tan(pitch) / std::cos(tableYaw));
}

// ---------------------------------------------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------------------------------------------
void SC4ViewUtilities::ProjectPointToViewCoordinates(float* point) const {   // 0x10017383c
    const float dx = point[0] - eye_.x;
    const float dy = point[1] - eye_.y;
    const float dz = point[2] - eye_.z;
    point[0] = cosYaw_ * dx - sinYaw_ * dz;
    const float z1 = dz * cosYaw_ + dx * sinYaw_;
    point[1] = cosPitch_ * dy - sinPitch_ * z1;
    point[2] = z1 * cosPitch_ + dy * sinPitch_;
}

void SC4ViewUtilities::ProjectPointFromViewCoordinatesToViewportCoordinates(float* point) const {   // 0x1001738e0
    point[0] = (point[0] - view_.subLeft) * scaleX_;
    point[1] = (view_.subHigh - point[1]) * scaleY_;
    point[2] = (point[2] + view_.nearPlane) * scaleZ_;
}

void SC4ViewUtilities::ProjectPointToViewportCoordinates(float* point) const {   // 0x100173940
    ProjectPointToViewCoordinates(point);
    ProjectPointFromViewCoordinatesToViewportCoordinates(point);
}

float SC4ViewUtilities::ProjectPointToViewportZ(const float* point) const {   // 0x1001739b8
    const float dx = point[0] - eye_.x;
    const float dy = point[1] - eye_.y;
    const float dz = point[2] - eye_.z;
    const float zView = dz * cosYaw_ + dx * sinYaw_;
    const float z = zView * cosPitch_ + dy * sinPitch_;
    return (z + view_.nearPlane) * scaleZ_;
}

void SC4ViewUtilities::ProjectPointToViewportCoordinates2D(float* point) const {   // 0x100173a1a
    ProjectPointToViewCoordinates(point);
    point[0] = (point[0] - view_.subLeft) * scaleX_;
    point[1] = (view_.subHigh - point[1]) * scaleY_;
}

void SC4ViewUtilities::ProjectPointToFixedViewCoordinates(float* point) const {   // 0x100173a70
    const float dx = point[0];
    const float dy = point[1];
    point[0] = cosYaw_ * dx - sinYaw_ * point[2];
    const float z1 = point[2] * cosYaw_ + dx * sinYaw_;
    point[2] = z1;
    point[1] = cosPitch_ * dy - sinPitch_ * z1;
    point[2] = z1 * cosPitch_ + dy * sinPitch_;
}

void SC4ViewUtilities::ProjectPointFromViewportCoordinatesToFixedViewCoordinates(float* point) const {   // 0x100173af8
    UnProjectPointFromViewportCoordinatesToWorldCoordinates(point);
    ProjectPointToFixedViewCoordinates(point);
}

void SC4ViewUtilities::UnProjectPointFromViewportCoordinatesToWorldCoordinates(float* point) const {   // 0x100173b1a
    point[2] = invScaleZ_ * point[2] - view_.nearPlane;
    point[1] = view_.subHigh - invScaleY_ * point[1];
    point[0] = invScaleX_ * point[0] + view_.subLeft;
    UnProjectPointFromViewCoordinatesToWorldCoordinates(point);
}

void SC4ViewUtilities::UnProjectPointFromViewCoordinatesToWorldCoordinates(float* point) const {   // 0x100173b8c
    const float x = point[0];
    const float y = point[1];
    const float z = point[2];
    const float worldY = sinPitch_ * z + cosPitch_ * y;
    const float viewZ = z * cosPitch_ - y * sinPitch_;
    const float worldX = sinYaw_ * viewZ + cosYaw_ * x;
    const float worldZ = viewZ * cosYaw_ - x * sinYaw_;
    point[0] = worldX + eye_.x;
    point[1] = worldY + eye_.y;
    point[2] = worldZ + eye_.z;
}

// ---------------------------------------------------------------------------------------------------------------
// Culling
// ---------------------------------------------------------------------------------------------------------------
void SC4ViewUtilities::GetViewFrustumBoundingBox(float* boxMin, float* boxMax) const {   // 0x100173c3c
    Copy3(boxMin, view_.boundsMin);
    Copy3(boxMax, view_.boundsMax);
}

// The shared body of IsBoundingBoxInViewFrustum (0x100173c76) and IsBoundingBoxInDrawFrustum (0x100173dfe); the two
// only differ in the data block they read (view at +0x188/+0x210, draw at +0x33c/+0x3c4).
bool SC4ViewUtilities::BoxInFrustum(const ViewPlaneBlock& planes, const ViewEdgeBlock& edges, const float* boxMin,
                                    const float* boxMax) {
    // 2D side edges, in (x, z). The corner of the box that is extreme along each edge's normal is selected by the
    // signs of the edge direction (0x218 / 0x21c for the left edge, 0x228 / 0x22c for the right one).
    const float leftDirX = edges.leftDirX, leftDirZ = edges.leftDirZ;
    const float leftX = (leftDirZ < 0.0f) ? boxMax[0] : boxMin[0];
    const float leftZ = (leftDirX > 0.0f) ? boxMax[2] : boxMin[2];
    if (!(leftDirZ * (leftX - edges.leftOriginX) - leftDirX * (leftZ - edges.leftOriginZ) < 0.0f)) return false;

    const float rightX = (leftDirZ < 0.0f) ? boxMin[0] : boxMax[0];
    const float rightZ = (leftDirX > 0.0f) ? boxMin[2] : boxMax[2];
    if (!(edges.rightDirZ * (rightX - edges.rightOriginX) - edges.rightDirX * (rightZ - edges.rightOriginZ) > 0.0f))
        return false;

    // The two horizontal planes through near corners 2 and 3: the box must straddle them. "along" is the box corner
    // farthest along +up, "against" the one farthest along -up.
    float along[3];
    float against[3];
    for (int i = 0; i < 3; ++i) {
        if (planes.up[i] > 0.0f) {
            along[i] = boxMax[i];
            against[i] = boxMin[i];
        } else {
            along[i] = boxMin[i];
            against[i] = boxMax[i];
        }
    }
    const float testAgainst = planes.negUp[0] * against[0] + planes.negUp[1] * against[1] +
                              planes.negUp[2] * against[2] + planes.negUpD;
    if (!(testAgainst > 0.0f)) return false;
    const float testAlong =
        planes.up[0] * along[0] + planes.up[1] * along[1] + planes.up[2] * along[2] + planes.upD;
    return testAlong > 0.0f;
}

bool SC4ViewUtilities::IsBoundingBoxInViewFrustum(const float* boxMin, const float* boxMax) const {   // 0x100173c76
    return BoxInFrustum(viewPlanes_, viewEdges_, boxMin, boxMax);
}

bool SC4ViewUtilities::IsBoundingBoxInDrawFrustum(const float* boxMin, const float* boxMax) const {   // 0x100173dfe
    return BoxInFrustum(drawPlanes_, drawEdges_, boxMin, boxMax);
}

bool SC4ViewUtilities::IsPointInViewFrustum(const float* point) const {   // 0x100173f8a
    const float dx = point[0] - eye_.x;
    const float dy = point[1] - eye_.y;
    const float dz = point[2] - eye_.z;
    const float xView = cosYaw_ * dx - sinYaw_ * dz;
    if (xView < view_.subLeft) return false;
    if (xView > view_.subRight) return false;
    const float zView = dz * cosYaw_ + dx * sinYaw_;
    const float yView = cosPitch_ * dy - sinPitch_ * zView;
    if (yView < view_.subLow) return false;
    if (yView > view_.subHigh) return false;
    const float z = -(zView * cosPitch_ + dy * sinPitch_);   // -view z (the depth the volume stores)
    return view_.nearPlane < z && z < view_.farPlane;
}

void SC4ViewUtilities::ProjectBoundingBoxToViewportRectangle(const float* boxMin, const float* boxMax, int* rectMin,
                                                             int* rectMax) const {   // 0x10017405e
    // The four corners the original projects per rotation, as (axis selection, 0 = min, 1 = max) in x, y, z order.
    static const int kCorners[4][4][3] = {
        {{0, 1, 1}, {0, 1, 0}, {1, 1, 0}, {1, 0, 1}},   // rotation 0
        {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 0, 1}},   // rotation 1
        {{1, 1, 0}, {1, 1, 1}, {0, 1, 1}, {0, 0, 0}},   // rotation 2
        {{1, 1, 1}, {0, 1, 1}, {0, 1, 0}, {1, 0, 0}},   // rotation 3
    };
    if (rotation_ < 0 || rotation_ > 3) return;   // the original switch's default writes nothing
    for (int i = 0; i < 4; ++i) {
        const int* sel = kCorners[rotation_][i];
        float point[3] = {
            sel[0] ? boxMax[0] : boxMin[0],
            sel[1] ? boxMax[1] : boxMin[1],
            sel[2] ? boxMax[2] : boxMin[2],
        };
        ProjectPointToViewCoordinates(point);
        switch (i) {
            case 0: rectMin[0] = RoundDownToInt((point[0] - view_.subLeft) * scaleX_); break;
            case 1: rectMin[1] = RoundDownToInt((view_.subHigh - point[1]) * scaleY_); break;
            case 2: rectMax[0] = RoundUpToInt((point[0] - view_.subLeft) * scaleX_); break;
            case 3: rectMax[1] = RoundUpToInt((view_.subHigh - point[1]) * scaleY_); break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Camera / frustum updates
// ---------------------------------------------------------------------------------------------------------------
void SC4ViewUtilities::FillPlaneBlock(ViewPlaneBlock& block, const cS3DVector3& right, const cS3DVector3& up,
                                      const cS3DVector3& dir, const float* corner0, const float* corner2,
                                      const float* corner3, const float* dirCorner3, const float* dirCorner7) {
    block.right[0] = right.x;
    block.right[1] = right.y;
    block.right[2] = right.z;
    block.rightD = -Dot3(right, corner3);
    block.negRight[0] = -right.x;
    block.negRight[1] = -right.y;
    block.negRight[2] = -right.z;
    block.negRightD = Dot3(right, corner0);
    block.up[0] = up.x;
    block.up[1] = up.y;
    block.up[2] = up.z;
    block.upD = -Dot3(up, corner2);
    block.negUp[0] = -up.x;
    block.negUp[1] = -up.y;
    block.negUp[2] = -up.z;
    block.negUpD = Dot3(up, corner3);
    block.dir[0] = dir.x;
    block.dir[1] = dir.y;
    block.dir[2] = dir.z;
    block.dirD = -Dot3(dir, dirCorner3);
    block.negDir[0] = -dir.x;
    block.negDir[1] = -dir.y;
    block.negDir[2] = -dir.z;
    block.negDirFarD = -Dot3(dir, dirCorner7);
    // Quirk kept: the copy drops -right.y (0x100175102 reads 0x178, 0x180, 0x184 rather than 0x178/0x17c/0x180).
    block.rightXZ[0] = right.x;
    block.rightXZ[1] = right.z;
    block.rightPlaneCopy[0] = block.rightD;
    block.rightPlaneCopy[1] = -right.x;
    block.rightPlaneCopy[2] = -right.z;
    block.rightPlaneCopy[3] = block.negRightD;
    for (float& value : block.reserved) value = 0.0f;
}

void SC4ViewUtilities::FillEdgeBlock(ViewEdgeBlock& edges, const FrustumBounds& frustum) {
    edges.leftOriginX = frustum.corner[3][0];
    edges.leftOriginZ = frustum.corner[3][2];
    edges.leftDirX = frustum.corner[7][0] - frustum.corner[3][0];
    edges.leftDirZ = frustum.corner[7][2] - frustum.corner[3][2];
    edges.rightOriginX = frustum.corner[0][0];
    edges.rightOriginZ = frustum.corner[0][2];
    edges.rightDirX = frustum.corner[4][0] - frustum.corner[0][0];
    edges.rightDirZ = frustum.corner[4][2] - frustum.corner[0][2];
}

void SC4ViewUtilities::UpdateCameraPosition(sc4::cSC4CameraControl* control) {   // 0x1001745da
    visibleAreaValid_ = false;      // +0x44c
    viewportLimitsValid_ = false;   // +0x44d
    ready_ = false;
    if (!host_) return;
    s3d::cS3DCamera* camera = host_->GetCamera();
    if (!camera) return;

    camera->GetPosition(eye_);
    camera->GetLookAt(lookAt_);
    viewDir_ = lookAt_ - eye_;

    // Zoom: the camera may be at a fractional zoom (custom magnification). floor(zoom - 0.5), then +1 when that
    // lands exactly on the zoom value (so integral zooms stay exact).
    zoomUnits_ = static_cast<float>(control->Zoom());
    const int lower = RoundDownToInt(zoomUnits_);
    int zoom = lower + 1;
    if (static_cast<float>(zoom) != zoomUnits_) zoom = lower;
    zoom_ = zoom;
    rotation_ = control->Rotation();

    int width = 0, height = 0;
    control->GetViewportSize(width, height);
    viewportW_ = width;
    viewportH_ = height;
    viewportWidth_ = static_cast<float>(width);
    viewportHeight_ = static_cast<float>(height);
    depthScale_ = 1.0f;

    control->GetDistancesToClipPlanes(clipNear_, clipFar_);

    float left = 0, right = 0, top = 0, bottom = 0, nearPlane = 0, farPlane = 0;
    camera->GetViewVolume(left, right, top, bottom, nearPlane, farPlane);
    view_.subLeft = left;
    view_.subRight = right;
    view_.subLow = top;
    view_.subHigh = bottom;
    view_.nearPlane = nearPlane;
    view_.farPlane = farPlane;

    scaleX_ = viewportWidth_ / (view_.subRight - view_.subLeft);
    scaleY_ = viewportHeight_ / (view_.subHigh - view_.subLow);
    scaleZ_ = 1.0f / (view_.farPlane - view_.nearPlane);
    invScaleX_ = 1.0f / scaleX_;
    invScaleY_ = 1.0f / scaleY_;
    invScaleZ_ = 1.0f / scaleZ_;

    worldPerViewportUnit_ = control->WorldPerPixel();          // +0x13c
    viewportUnitsPerWorldUnit_ = control->PixelPerWorld();     // +0x140

    const float yaw = kSC4YViewAngle[zoom_] + static_cast<float>(rotation_) * kPiOver2;
    yaw_ = yaw;
    pitch_ = kSC4PitchViewAngle[zoom_];
    rotationAngle_ = static_cast<float>(rotation_) * kPiOver2;
    sinYaw_ = std::sin(-yaw);     // the original negates the angle before sinf (0x10017488d)
    cosYaw_ = std::cos(yaw);
    sinPitch_ = std::sin(pitch_);
    cosPitch_ = std::cos(pitch_);

    {   // view direction
        const float length = std::sqrt(viewDir_.x * viewDir_.x + viewDir_.y * viewDir_.y + viewDir_.z * viewDir_.z);
        const float inv = 1.0f / length;
        viewDir_.x *= inv;
        viewDir_.y *= inv;
        viewDir_.z *= inv;
    }
    {   // right = dir x world up, up = right x dir
        const float worldUp[3] = {0.0f, 1.0f, 0.0f};
        sc4::SC4GeometryUtilities::CrossProduct(&viewDir_.x, worldUp, &right_.x);
        sc4::SC4GeometryUtilities::CrossProduct(&right_.x, &viewDir_.x, &up_.x);
    }
    {   // normalize right
        const float length = std::sqrt(right_.x * right_.x + right_.y * right_.y + right_.z * right_.z);
        const float inv = 1.0f / length;
        right_.x *= inv;
        right_.y *= inv;
        right_.z *= inv;
    }
    {   // normalize up
        const float length = std::sqrt(up_.x * up_.x + up_.y * up_.y + up_.z * up_.z);
        const float inv = 1.0f / length;
        up_.x *= inv;
        up_.y *= inv;
        up_.z *= inv;
    }

    // near / far centres along the view direction
    float nearDir[3] = {viewDir_.x, viewDir_.y, viewDir_.z};
    float farDir[3] = {viewDir_.x, viewDir_.y, viewDir_.z};
    {
        const float lengthSq = viewDir_.x * viewDir_.x + viewDir_.y * viewDir_.y + viewDir_.z * viewDir_.z;
        if (0.0f < lengthSq) {
            const float length = std::sqrt(lengthSq);
            const float nearScale = view_.nearPlane / length;
            nearDir[0] = viewDir_.x * nearScale;
            nearDir[1] = viewDir_.y * nearScale;
            nearDir[2] = viewDir_.z * nearScale;
            const float farScale = view_.farPlane / length;
            farDir[0] = viewDir_.x * farScale;
            farDir[1] = viewDir_.y * farScale;
            farDir[2] = viewDir_.z * farScale;
        }
    }

    // half extents of the frustum's near / far rectangles
    float rightHalf[3] = {right_.x, right_.y, right_.z};
    {
        const float lengthSq = right_.x * right_.x + right_.y * right_.y + right_.z * right_.z;
        if (0.0f < lengthSq) {
            const float scale = ((view_.subRight - view_.subLeft) * 0.5f) / std::sqrt(lengthSq);
            Scale3(rightHalf, scale);
        }
    }
    float upHalf[3] = {up_.x, up_.y, up_.z};
    {
        const float lengthSq = up_.x * up_.x + up_.y * up_.y + up_.z * up_.z;
        if (0.0f < lengthSq) {
            const float scale = ((view_.subHigh - view_.subLow) * 0.5f) / std::sqrt(lengthSq);
            Scale3(upHalf, scale);
        }
    }

    const float nearCenter[3] = {eye_.x + nearDir[0], eye_.y + nearDir[1], eye_.z + nearDir[2]};
    const float farCenter[3] = {eye_.x + farDir[0], eye_.y + farDir[1], eye_.z + farDir[2]};

    // The near / far quad corners in the original's order: +R+U, +R-U, -R-U, -R+U.
    for (int side = 0; side < 4; ++side) {
        const float sx = (side == 0 || side == 1) ? 1.0f : -1.0f;   // +R, +R, -R, -R
        const float sy = (side == 0 || side == 3) ? 1.0f : -1.0f;   // +U, -U, -U, +U
        view_.corner[side][0] = nearCenter[0] + sx * rightHalf[0] + sy * upHalf[0];
        view_.corner[side][1] = nearCenter[1] + sx * rightHalf[1] + sy * upHalf[1];
        view_.corner[side][2] = nearCenter[2] + sx * rightHalf[2] + sy * upHalf[2];
        view_.corner[side + 4][0] = farCenter[0] + sx * rightHalf[0] + sy * upHalf[0];
        view_.corner[side + 4][1] = farCenter[1] + sx * rightHalf[1] + sy * upHalf[1];
        view_.corner[side + 4][2] = farCenter[2] + sx * rightHalf[2] + sy * upHalf[2];
    }

    // Perspective projection type: pull the far corners toward the eye by far / near. Quirk kept: the original builds
    // the far corners in the orthographic branch only, so this path scales the corners left over from the previous
    // call (0x100174daa reads +0xe0..+0x10c before writing them).
    if (camera->GetProjectionType() != s3d::kProjectionOrtho) {
        const float ratio = view_.farPlane / view_.nearPlane;
        for (int i = 4; i < 8; ++i) {
            view_.corner[i][0] = (view_.corner[i][0] - eye_.x) * ratio + eye_.x;
            view_.corner[i][1] = (view_.corner[i][1] - eye_.y) * ratio + eye_.y;
            view_.corner[i][2] = (view_.corner[i][2] - eye_.z) * ratio + eye_.z;
        }
    }

    sc4::SC4GeometryUtilities::GetBoundingBoxOfVertices(view_.corner, 8, view_.boundsMin, view_.boundsMax);
    cS3DVector3 clippedMin;
    cS3DVector3 clippedMax;
    GetFrustumAABBCityClipped(true, clippedMin, clippedMax);
    Copy3(view_.clippedMin, clippedMin);
    Copy3(view_.clippedMax, clippedMax);

    FillPlaneBlock(viewPlanes_, right_, up_, viewDir_, view_.corner[0], view_.corner[2], view_.corner[3],
                   view_.corner[3], view_.corner[7]);
    FillEdgeBlock(viewEdges_, view_);
    ready_ = true;
}

void SC4ViewUtilities::GetFrustumAABBCityClipped(bool useView, cS3DVector3& outMin, cS3DVector3& outMax) {   // 0x1001751f4
    const FrustumBounds& frustum = useView ? view_ : draw_;
    float hit0[3] = {0, 0, 0};
    float hit1[3] = {0, 0, 0};
    {
        const float origin[3] = {frustum.corner[0][0], frustum.corner[0][1], frustum.corner[0][2]};
        const float direction[3] = {
            frustum.corner[4][0] - frustum.corner[0][0],
            frustum.corner[4][1] - frustum.corner[0][1],
            frustum.corner[4][2] - frustum.corner[0][2],
        };
        RayIntersectsGroundPlane(origin, direction, hit0);
    }
    {
        const float origin[3] = {frustum.corner[3][0], frustum.corner[3][1], frustum.corner[3][2]};
        const float direction[3] = {
            frustum.corner[7][0] - frustum.corner[3][0],
            frustum.corner[7][1] - frustum.corner[3][1],
            frustum.corner[7][2] - frustum.corner[3][2],
        };
        RayIntersectsGroundPlane(origin, direction, hit1);
    }

    const float subject[4][2] = {
        {frustum.corner[3][0], frustum.corner[3][2]},
        {frustum.corner[0][0], frustum.corner[0][2]},
        {hit0[0], hit0[2]},
        {hit1[0], hit1[2]},
    };
    const float clip[4][2] = {{0.0f, 0.0f}, {0.0f, citySizeZ_}, {citySizeX_, citySizeZ_}, {citySizeX_, 0.0f}};
    float clipped[10][2] = {};
    int count = 0;
    sc4::SC4GeometryUtilities::ClipPolygonToPolygon2D(subject, 4, clip, 4, clipped, 10, count);

    if (count < 1) {
        outMin.x = 0.0f;
        outMin.y = 0.0f;
        outMin.z = 0.0f;
        outMax.x = 0.0f;
        outMax.y = frustum.corner[3][1];
        outMax.z = 0.0f;
        return;
    }
    float boxMin[2] = {0, 0};
    float boxMax[2] = {0, 0};
    sc4::SC4GeometryUtilities::GetBoundingBoxOfVertices2D(clipped, count, boxMin, boxMax);
    outMin.x = boxMin[0];
    outMin.y = 0.0f;
    outMin.z = boxMin[1];
    outMax.x = boxMax[0];
    outMax.y = frustum.corner[3][1];
    outMax.z = boxMax[1];
}

void SC4ViewUtilities::RayIntersectsGroundPlane(const float* origin, const float* direction, float* out) {   // 0x10017542a
    sc4::SC4GeometryUtilities::RayIntersectsPlane(origin, direction, kGroundPlane, out);
}

void SC4ViewUtilities::BuildDrawFrustumDataFromViewportRect(const int* rectMin, const int* rectMax) {   // 0x100175444
    visibleAreaValid_ = false;
    viewportLimitsValid_ = false;
    viewportRect_[0] = rectMin[0];
    viewportRect_[1] = rectMin[1];
    viewportRect_[2] = rectMax[0];
    viewportRect_[3] = rectMax[1];

    if (rectMin[0] == 0 && rectMin[1] == 0 && rectMax[0] == viewportW_ && rectMax[1] == viewportH_) {
        drawReduced_ = false;
        // Make sure the viewport ground points exist, then copy the whole view block (+0x80..+0x157 -> +0x244).
        GetViewportLimitsGroundPlanePositions(nullptr, nullptr, nullptr, nullptr);
        draw_ = view_;
        drawPlanes_ = viewPlanes_;
        drawEdges_ = viewEdges_;
        return;
    }

    s3d::cS3DCamera* camera = host_ ? host_->GetCamera() : nullptr;
    if (!camera) return;
    drawReduced_ = true;

    if (rectMin[0] == 0 && rectMax[0] == viewportW_) {
        draw_.subLeft = view_.subLeft;
        draw_.subRight = view_.subRight;
    } else {
        const int centerX = (rectMax[0] + rectMin[0]) / 2;
        draw_.subLeft = static_cast<float>(rectMin[0] - centerX) * worldPerViewportUnit_;
        draw_.subRight = static_cast<float>(rectMax[0] - centerX) * worldPerViewportUnit_;
    }
    if (rectMin[1] == 0 && rectMax[1] == viewportH_) {
        draw_.subHigh = view_.subHigh;
        draw_.subLow = view_.subLow;
    } else {
        const int centerY = (rectMax[1] + rectMin[1]) / 2;
        draw_.subHigh = static_cast<float>(centerY - rectMin[1]) * worldPerViewportUnit_;
        draw_.subLow = static_cast<float>(centerY - rectMax[1]) * worldPerViewportUnit_;
    }
    draw_.nearPlane = view_.nearPlane;
    draw_.farPlane = view_.farPlane;

    // The sub-rectangle's centre in viewport space, unprojected to the world (the point lies on the near plane).
    float center[3] = {
        static_cast<float>((rectMax[0] + rectMin[0]) / 2),
        static_cast<float>((rectMax[1] + rectMin[1]) / 2),
        0.0f,
    };
    UnProjectPointFromViewportCoordinatesToWorldCoordinates(center);

    float nearDir[3] = {viewDir_.x, viewDir_.y, viewDir_.z};
    float farDir[3] = {viewDir_.x, viewDir_.y, viewDir_.z};
    {
        const float lengthSq = viewDir_.x * viewDir_.x + viewDir_.y * viewDir_.y + viewDir_.z * viewDir_.z;
        if (0.0f < lengthSq) {
            const float length = std::sqrt(lengthSq);
            const float nearScale = draw_.nearPlane / length;
            nearDir[0] = viewDir_.x * nearScale;
            nearDir[1] = viewDir_.y * nearScale;
            nearDir[2] = viewDir_.z * nearScale;
            const float farScale = draw_.farPlane / length;
            farDir[0] = viewDir_.x * farScale;
            farDir[1] = viewDir_.y * farScale;
            farDir[2] = viewDir_.z * farScale;
        }
    }
    float rightHalf[3] = {right_.x, right_.y, right_.z};
    {
        const float lengthSq = right_.x * right_.x + right_.y * right_.y + right_.z * right_.z;
        if (0.0f < lengthSq) {
            const float scale = ((draw_.subRight - draw_.subLeft) * 0.5f) / std::sqrt(lengthSq);
            Scale3(rightHalf, scale);
        }
    }
    float upHalf[3] = {up_.x, up_.y, up_.z};
    {
        const float lengthSq = up_.x * up_.x + up_.y * up_.y + up_.z * up_.z;
        if (0.0f < lengthSq) {
            const float scale = ((draw_.subHigh - draw_.subLow) * 0.5f) / std::sqrt(lengthSq);
            Scale3(upHalf, scale);
        }
    }

    for (int side = 0; side < 4; ++side) {
        const float sx = (side == 0 || side == 1) ? 1.0f : -1.0f;
        const float sy = (side == 0 || side == 3) ? 1.0f : -1.0f;
        draw_.corner[side][0] = center[0] + sx * rightHalf[0] + sy * upHalf[0];
        draw_.corner[side][1] = center[1] + sx * rightHalf[1] + sy * upHalf[1];
        draw_.corner[side][2] = center[2] + sx * rightHalf[2] + sy * upHalf[2];
    }
    if (camera->GetProjectionType() == s3d::kProjectionOrtho) {
        const float farCenter[3] = {center[0] + farDir[0] - nearDir[0], center[1] + farDir[1] - nearDir[1],
                                    center[2] + farDir[2] - nearDir[2]};
        for (int side = 0; side < 4; ++side) {
            const float sx = (side == 0 || side == 1) ? 1.0f : -1.0f;
            const float sy = (side == 0 || side == 3) ? 1.0f : -1.0f;
            draw_.corner[side + 4][0] = farCenter[0] + sx * rightHalf[0] + sy * upHalf[0];
            draw_.corner[side + 4][1] = farCenter[1] + sx * rightHalf[1] + sy * upHalf[1];
            draw_.corner[side + 4][2] = farCenter[2] + sx * rightHalf[2] + sy * upHalf[2];
        }
    } else {
        // Quirk kept: the perspective far corners are derived from the values already stored (the orthographic
        // computation is in the other branch), pulled toward the sub-rectangle's near-plane centre.
        const float base[3] = {center[0] - nearDir[0], center[1] - nearDir[1], center[2] - nearDir[2]};
        const float ratio = draw_.farPlane / draw_.nearPlane;
        for (int i = 4; i < 8; ++i) {
            for (int k = 0; k < 3; ++k) draw_.corner[i][k] = (draw_.corner[i][k] - base[k]) * ratio + base[k];
        }
    }

    sc4::SC4GeometryUtilities::GetBoundingBoxOfVertices(draw_.corner, 8, draw_.boundsMin, draw_.boundsMax);

    FillPlaneBlock(drawPlanes_, right_, up_, viewDir_, draw_.corner[0], draw_.corner[2], draw_.corner[3],
                   view_.corner[3], view_.corner[7]);   // quirk: the direction group uses the view corners
    FillEdgeBlock(drawEdges_, draw_);

    float ground[4][2] = {};
    CalculateViewportGroundPlanePositions(ground[0], ground[1], ground[2], ground[3], rectMin, rectMax);
    for (int i = 0; i < 4; ++i) {
        draw_.ground[i][0] = ground[i][0];
        draw_.ground[i][1] = 0.0f;
        draw_.ground[i][2] = ground[i][1];
    }

    cS3DVector3 clippedMin;
    cS3DVector3 clippedMax;
    GetFrustumAABBCityClipped(false, clippedMin, clippedMax);
    Copy3(draw_.clippedMin, clippedMin);
    Copy3(draw_.clippedMax, clippedMax);
}

void SC4ViewUtilities::GetViewportLimitsGroundPlanePositions(float* bottomLeft, float* bottomRight, float* topLeft,
                                                             float* topRight) {   // 0x10017611c
    if (!viewportLimitsValid_) {
        GetViewportPointIntersectionWithGroundPlane(0, 0, view_.ground[0]);
        GetViewportPointIntersectionWithGroundPlane(viewportW_, 0, view_.ground[1]);
        GetViewportPointIntersectionWithGroundPlane(0, viewportH_, view_.ground[2]);
        GetViewportPointIntersectionWithGroundPlane(viewportW_, viewportH_, view_.ground[3]);
        viewportLimitsValid_ = true;
    }
    if (bottomLeft) {
        bottomLeft[0] = view_.ground[0][0];
        bottomLeft[1] = view_.ground[0][2];
    }
    if (bottomRight) {
        bottomRight[0] = view_.ground[1][0];
        bottomRight[1] = view_.ground[1][2];
    }
    if (topLeft) {
        topLeft[0] = view_.ground[2][0];
        topLeft[1] = view_.ground[2][2];
    }
    if (topRight) {
        topRight[0] = view_.ground[3][0];
        topRight[1] = view_.ground[3][2];
    }
}

void SC4ViewUtilities::CalculateViewportGroundPlanePositions(float* p0, float* p1, float* p2, float* p3,
                                                             const int* rectMin, const int* rectMax) const {   // 0x10017620c
    float hit[3] = {0, 0, 0};
    GetViewportPointIntersectionWithGroundPlane(rectMin[0], rectMin[1], hit);
    p0[0] = hit[0];
    p0[1] = hit[2];
    // Quirk kept: the second point takes its y from rectMin (0x100176261), not from rectMax.
    GetViewportPointIntersectionWithGroundPlane(rectMax[0], rectMin[1], hit);
    p1[0] = hit[0];
    p1[1] = hit[2];
    GetViewportPointIntersectionWithGroundPlane(rectMin[0], rectMax[1], hit);
    p2[0] = hit[0];
    p2[1] = hit[2];
    GetViewportPointIntersectionWithGroundPlane(rectMax[0], rectMax[1], hit);
    p3[0] = hit[0];
    p3[1] = hit[2];
}

// ---------------------------------------------------------------------------------------------------------------
// Rays and picking
// ---------------------------------------------------------------------------------------------------------------
bool SC4ViewUtilities::ViewportPositionToRay(int x, int y, float* origin, float* direction) const {   // 0x1001762f0
    if (!host_) return false;
    s3d::cS3DCamera* camera = host_->GetCamera();
    if (!camera) return false;
    cS3DVector3 rayOrigin{0.0f, 0.0f, 0.0f};
    cS3DVector3 rayDirection{0.0f, 0.0f, 0.0f};
    const cS3DVector2 screen{static_cast<float>(x), static_cast<float>(y)};
    if (!camera->GetEyeRay(screen, rayOrigin, rayDirection)) return false;
    const float inv =
        1.0f / std::sqrt(rayDirection.z * rayDirection.z + rayDirection.y * rayDirection.y + rayDirection.x * rayDirection.x);
    origin[0] = rayOrigin.x;
    origin[1] = rayOrigin.y;
    origin[2] = rayOrigin.z;
    direction[0] = rayDirection.x * inv;
    direction[1] = rayDirection.y * inv;
    direction[2] = rayDirection.z * inv;
    return true;
}

bool SC4ViewUtilities::GetViewportPointIntersectionWithGroundPlane(int x, int y, float* out) const {   // 0x1001763e2
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float direction[3] = {0.0f, 0.0f, 0.0f};
    ViewportPositionToRay(x, y, origin, direction);   // quirk: the result is ignored, a failed ray gives NaN below
    const float t = -origin[1] / direction[1];
    out[0] = direction[0] * t + origin[0];
    out[1] = direction[1] * t + origin[1];
    out[2] = direction[2] * t + origin[2];
    return true;
}

void SC4ViewUtilities::GetRayIntersectionWithGroundPlane(const float* origin, const float* direction, float* out) {   // 0x100176480
    const float t = -origin[1] / direction[1];
    out[0] = direction[0] * t + origin[0];
    out[1] = direction[1] * t + origin[1];
    out[2] = direction[2] * t + origin[2];
}

bool SC4ViewUtilities::TrianglePick(cSC4PickContext* context, const float* a, const float* b, const float* c) {   // 0x1001764d8
    float hit[3] = {0, 0, 0};
    if (!sc4::SC4GeometryUtilities::RayIntersectsTriangle(&context->origin.x, &context->direction.x, a, b, c, hit))
        return false;
    const float distance = sc4::SC4GeometryUtilities::LengthOfLine(&context->origin.x, hit);
    if (distance < context->maxDistance) {
        context->maxDistance = distance;
        return true;
    }
    return false;
}

bool SC4ViewUtilities::MeshPick(cSC4PickContext* context, const IMeshPickHost& mesh,
                                const cS3DTransform& transform) {   // 0x10017654c
    if (!mesh.HasPickableVertices()) return false;
    const int groupCount = mesh.PrimitiveGroupCount();
    if (groupCount == 0) return false;
    s3d::cS3DTriangleIterator iterator;
    for (int group = 0; group < groupCount; ++group) {
        mesh.InitTriangleIterator(iterator, group);
        cS3DVector3 points[3];
        while (iterator.GetNextTrianglePoints(points)) {
            cS3DVector3 world[3];
            nS3D::TransformPoints(transform, points, world, 3);
            float hit[3] = {0, 0, 0};
            if (!sc4::SC4GeometryUtilities::RayIntersectsTriangle(&context->origin.x, &context->direction.x,
                                                                  &world[0].x, &world[1].x, &world[2].x, hit))
                continue;
            const float distance = sc4::SC4GeometryUtilities::LengthOfLine(&context->origin.x, hit);
            if (distance < context->maxDistance) {
                context->maxDistance = distance;
                return true;
            }
        }
    }
    return false;
}

bool SC4ViewUtilities::ModelPick(cSC4PickContext* context, s3d::cIS3DModelInstance* instance,
                                 const IMeshMaterialHost& meshes) {   // 0x100176a3c
    const int meshCount = meshes.MeshCount();
    if (meshCount == 0) return false;
    bool any = false;
    for (int i = 0; i < meshCount; ++i) {
        const IMeshPickHost* mesh = meshes.Mesh(i);
        cS3DTransform transform;   // identity, exactly as the original initialises its local
        instance->GetTransform(transform);   // cIS3DModelInstance vslot 0x48
        // Quirk kept: every mesh is tested (the results are OR'd); MeshPick itself keeps the closest hit.
        if (MeshPick(context, *mesh, transform)) any = true;
    }
    return any;
}

void SC4ViewUtilities::TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box) {   // 0x100176ae6
    const float scale = transform.scale;
    float boxMin[3] = {box.min[0] * scale, box.min[1] * scale, box.min[2] * scale};
    float boxMax[3] = {box.max[0] * scale, box.max[1] * scale, box.max[2] * scale};
    for (int row = 0; row < 3; ++row) {
        float low = transform.translation[row];
        float high = transform.translation[row];
        for (int column = 0; column < 3; ++column) {
            const float a = boxMin[column] * transform.rotation[row][column];
            const float b = boxMax[column] * transform.rotation[row][column];
            low += (b <= a) ? b : a;
            high += (b <= a) ? a : b;
        }
        box.min[row] = low;
        box.max[row] = high;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Ground rectangle and block helpers
// ---------------------------------------------------------------------------------------------------------------
void SC4ViewUtilities::GetGroundPlaneVisibleAreaRectangle(float* midpointMin, float* midpointMax, float* blockMin,
                                                          float* blockMax) {   // 0x100176bac
    if (!visibleAreaValid_) {
        float p00[3] = {0, 0, 0}, p10[3] = {0, 0, 0}, p01[3] = {0, 0, 0}, p11[3] = {0, 0, 0};
        GetViewportPointIntersectionWithGroundPlane(0, 0, p00);
        GetViewportPointIntersectionWithGroundPlane(viewportW_, 0, p10);
        GetViewportPointIntersectionWithGroundPlane(0, viewportH_, p01);
        GetViewportPointIntersectionWithGroundPlane(viewportW_, viewportH_, p11);

        const float midTopX = (p00[0] + p10[0]) * 0.5f;
        const float midTopZ = (p00[2] + p10[2]) * 0.5f;
        const float midBottomX = (p01[0] + p11[0]) * 0.5f;
        const float midBottomZ = (p01[2] + p11[2]) * 0.5f;

        const auto min4 = [](float a, float b, float c, float d) {
            float result = std::min(FLT_MAX, a);
            result = std::min(b, result);
            result = std::min(result, c);
            return std::min(d, result);
        };
        const auto max4 = [](float a, float b, float c, float d) {
            float result = std::max(-FLT_MAX, a);
            result = std::max(b, result);
            result = std::max(result, c);
            return std::max(d, result);
        };
        visibleAreaBlockMin_[0] = min4(p00[0], p10[0], p01[0], p11[0]);
        visibleAreaBlockMin_[1] = 0.0f;
        visibleAreaBlockMin_[2] = min4(p00[2], p10[2], p01[2], p11[2]);
        visibleAreaBlockMax_[0] = max4(p00[0], p10[0], p01[0], p11[0]);
        visibleAreaBlockMax_[1] = 0.0f;
        visibleAreaBlockMax_[2] = max4(p00[2], p10[2], p01[2], p11[2]);

        visibleAreaMidMin_[0] = std::min(FLT_MAX, midTopX);
        visibleAreaMidMin_[0] = std::min(midBottomX, visibleAreaMidMin_[0]);
        visibleAreaMidMin_[1] = 0.0f;
        visibleAreaMidMin_[2] = std::min(FLT_MAX, midTopZ);
        visibleAreaMidMin_[2] = std::min(midBottomZ, visibleAreaMidMin_[2]);
        visibleAreaMidMax_[0] = std::max(-FLT_MAX, midTopX);
        visibleAreaMidMax_[0] = std::max(midBottomX, visibleAreaMidMax_[0]);
        visibleAreaMidMax_[1] = 0.0f;
        visibleAreaMidMax_[2] = std::max(-FLT_MAX, midTopZ);
        visibleAreaMidMax_[2] = std::max(midBottomZ, visibleAreaMidMax_[2]);

        // the block-aligned outer rectangle, stored back as floats (the original inlines floor / ceil by 64)
        visibleAreaBlockMin_[0] = RoundDownToNearestBlockBoundary(visibleAreaBlockMin_[0], kBlockSize);
        visibleAreaBlockMin_[2] = RoundDownToNearestBlockBoundary(visibleAreaBlockMin_[2], kBlockSize);
        visibleAreaBlockMax_[0] = RoundUpToNearestBlockBoundary(visibleAreaBlockMax_[0], kBlockSize);
        visibleAreaBlockMax_[2] = RoundUpToNearestBlockBoundary(visibleAreaBlockMax_[2], kBlockSize);
        visibleAreaValid_ = true;
    }
    if (midpointMin) Copy3(midpointMin, visibleAreaMidMin_);
    if (midpointMax) Copy3(midpointMax, visibleAreaMidMax_);
    if (blockMin) Copy3(blockMin, visibleAreaBlockMin_);
    if (blockMax) Copy3(blockMax, visibleAreaBlockMax_);
}

float SC4ViewUtilities::RoundDownToNearestBlockBoundary(float value, int block) {   // 0x100176fe6
    int index = static_cast<int>(value);
    if (index > 0) {
        index -= index % block;
    } else if (index % block != 0) {
        index -= block;
        index -= index % block;
    }
    return static_cast<float>(index);
}

float SC4ViewUtilities::RoundUpToNearestBlockBoundary(float value, int block) {   // 0x10017700e
    int index = static_cast<int>(value);
    if (index < 0) {
        index -= index % block;
    } else {
        const int remainder = index % block;
        index += remainder != 0 ? block : 0;
        index -= index % block;
    }
    return static_cast<float>(index);
}

void SC4ViewUtilities::GetDrawFrustumGroundPlanePositions(float* p0, float* p1, float* p2, float* p3) const {   // 0x10017703e
    float* points[4] = {p0, p1, p2, p3};
    for (int i = 0; i < 4; ++i) {
        if (!points[i]) continue;
        points[i][0] = draw_.ground[i][0];
        points[i][1] = draw_.ground[i][2];
    }
}

void SC4ViewUtilities::GetCityCornerPosition(int corner, float* out) const {   // 0x10017708a
    out[1] = 0.0f;
    if (static_cast<unsigned>(corner) > 3u) return;
    // Outcome per (rotation, corner). 0: (0,0,0) 1: (0,0,cityZ) 2: (cityX,0,0) 3: (cityX,0,cityZ)
    // 4: (cityX,0,cityX) - the original reads the x extent where it should read z (0x100177104).
    static const int kCorner[4][4] = {
        {1, 0, 2, 3},   // rotation 0
        {0, 2, 4, 1},   // rotation 1 (corner 2 is the quirk)
        {2, 3, 1, 0},   // rotation 2
        {3, 1, 0, 2},   // rotation 3
    };
    if (rotation_ < 0 || rotation_ > 3) return;
    switch (kCorner[rotation_][corner]) {
        case 0: out[0] = 0.0f; out[2] = 0.0f; break;
        case 1: out[0] = 0.0f; out[2] = citySizeZ_; break;
        case 2: out[0] = citySizeX_; out[2] = 0.0f; break;
        case 3: out[0] = citySizeX_; out[2] = citySizeZ_; break;
        case 4: out[0] = citySizeX_; out[2] = citySizeX_; break;
        default: break;
    }
}

float SC4ViewUtilities::GetObjectYRotationAngle(int rotation) {   // 0x1001771a0
    return static_cast<float>(rotation) * -kPiOver2;
}

float SC4ViewUtilities::GetYawAngle2D(float a, float b) {   // 0x1001771b2
    return std::atan2(-a, -b);
}

void SC4ViewUtilities::RotateVector(float* v, int quarterTurns) {   // 0x1001771ca
    const int turns = BlockModulo(quarterTurns);
    if (turns == 3) {
        const float x = v[0];
        v[0] = v[2];
        v[2] = -x;
    } else if (turns == 2) {
        v[0] = -v[0];
        v[2] = -v[2];
    } else if (turns == 1) {
        const float x = v[0];
        v[0] = -v[2];
        v[2] = x;
    }
}

void SC4ViewUtilities::GetVectorForEdge(float* v, int edge) {   // 0x10017723c
    static const float kX[4] = {0.0f, 1.0f, 0.0f, -1.0f};    // 0x100a19620
    static const float kZ[4] = {-1.0f, 0.0f, 1.0f, 0.0f};    // 0x100a19630
    const int index = BlockModulo(edge);
    v[1] = 0.0f;
    if (static_cast<unsigned>(index) < 4u) {
        v[0] = kX[index];
        v[2] = kZ[index];
    }
}

float SC4ViewUtilities::GetCityWidthFromOrthoView(float width, int zoom) {   // 0x100177278
    // every kSC4YViewAngle entry is the same angle, but the original still indexes the table with the zoom
    const float angle = kSC4YViewAngle[zoom];
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    return cosine * width - sine * width;
}
