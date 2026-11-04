// SC4ViewUtilities (47 symbols, 0x100173480..0x1001772b6): the city view's projection and culling helper. It caches
// the camera (eye, look-at, orthonormal basis, view volume, frustum corners, side planes), converts between world,
// view, viewport and fixed-view coordinates, builds the draw frustum for a viewport sub-rectangle, answers the
// terrain / occupant culling queries, unprojects ground points and rays, and picks model triangles.
//
// The class has no vtable (every method is a plain member or static function). The game keeps one instance in
// SL::spViewUtilities; cSC43DRender allocates it (0x478 bytes), stores itself in +8 (so the camera is reachable
// through cSC43DRender::GetCamera, vslot 0x70) and calls UpdateCameraPosition + BuildDrawFrustumDataFromViewportRect.
//
// Original layout: +0 ready, +8 owning render object (cSC43DRender in the game), +0x10 zoom as float, +0x14 zoom,
// +0x18 rotation, +0x1c yaw angle, +0x20 pitch angle, +0x24 rotation angle, +0x28 / +0x2c city size in x / z,
// +0x30 / +0x34 viewport size (int), +0x38 / +0x3c viewport size (float), +0x40 depth scale (1),
// +0x44 eye, +0x50 look-at, +0x5c view direction (unit), +0x68 right (unit), +0x74 up (unit),
// +0x80 view frustum bounds, +0x98 city-clipped view bounds, +0xb0 view frustum corners, +0x110 sub-view
// left / right / top / bottom, +0x120 near / far, +0x128 ground points of the viewport corners,
// +0x158 world units per viewport unit, +0x15c viewport units per world unit, +0x160 clip near / far distance,
// +0x168 view plane block, +0x210 view edge block, +0x230 draw frustum is a sub-rect flag, +0x234 viewport
// rectangle, +0x244 draw frustum bounds / corners / ground points (same layout as +0x80), +0x31c draw plane
// block, +0x3c4 draw edge block, +0x3e4 unknown tail (initialised, not touched by this class), +0x41c visible
// ground area (midpoint rectangle, then the 64-unit aligned rectangle), +0x44c cached-flag pair, +0x450 sin(-yaw),
// +0x454 cos(yaw), +0x458 sin(pitch), +0x45c cos(pitch), +0x460 viewport x scale, +0x464 viewport y scale,
// +0x468 viewport z scale, +0x46c..+0x474 their reciprocals.
//
// Quirks kept on purpose (each one is asserted in scripts/tests/sc4_viewutilities_test.cpp):
// * GetVisibleEdges / RotateVector / GetVectorForEdge wrap with a "round toward zero then subtract" modulo that
//   returns a negative index for negative inputs (GetVectorForEdge then leaves the vector untouched).
// * GetCityCornerPosition returns (cityX, 0, cityX) for corner 2 at rotation 1: the original reads the x extent
//   where it should read the z extent (0x100177104), so non-square cities get a wrong corner.
// * IsPointInViewFrustum clips x / y to the inclusive view volume and the depth to the strict open interval
//   (near, far) of -z (0x100173f8a).
// * ProjectBoundingBoxToViewportRectangle projects only four of the eight box corners, chosen by the rotation, and
//   rounds with the (int)(v -+ 0.5 + 12582912) - 12582912 trick.
// * GetViewportPointIntersectionWithGroundPlane ignores ViewportPositionToRay's failure (ViewportPositionToRay
//   itself zeroes the ray), so a failed ray divides zero by zero and the "intersection" is NaN; it still returns
//   true (0x1001763e2).
// * CalculateViewportGroundPlanePositions passes the *first* rectangle's y coordinate to the second ground query
//   (0x100176261), so the top-right point is computed with the top edge and the bottom edge's y twice.
// * BuildDrawFrustumDataFromViewportRect's sub-rectangle path builds the direction-plane group of the draw frustum
//   from the *view* direction and the view corners (0x100176...), and pulls the perspective far corners toward the
//   sub-rectangle centre point rather than the eye.
// * The plane block at +0x1c8..+0x1df drops -right.y: it stores right.x, right.z, -dot(right, k3), -right.x,
//   -right.z, dot(right, k0) (0x100175102).
// * MeshPick / ModelPick share the pick context's running maximum distance: ModelPick keeps testing every mesh even
//   after a hit (the results are OR'd) and MeshPick stops at the first closer triangle.
// * UpdateCameraPosition converts the (possibly fractional) camera zoom to an int with floor(zoom - 0.5), except
//   when zoom is exactly the next integer.
// * The constructor writes 0.3f at +0x418 and then zeroes that qword through the +0x414 store, so the field ends
//   up 0 (0x100173480).
// Compile without FMA contraction: the results follow the original single-precision operation order.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gz/StreamOps.h"
#include "s3d/Camera.h"
#include "s3d/ModelInstance.h"
#include "s3d/TriangleIterator.h"

namespace sc4 {
class cSC4CameraControl;
}

// ---------------------------------------------------------------------------------------------------------------
// The pick context (cSC4PickContext, not recovered as a class of its own). Only these members are observed by the
// recovered code: +0x00 ray origin, +0x0c ray direction, +0x30 running maximum distance (0x1001764d8). The middle
// of the record is opaque; the original constructor was not recovered. It is defined here (instead of behind a seam)
// because s3d/ModelInstance.h and s3d/ModelInstanceAnimatedSprite.h already pass it around as an opaque pointer, so
// completing the forward declaration is the smaller change.
// ---------------------------------------------------------------------------------------------------------------
class cSC4PickContext {
public:
    cS3DVector3 origin;                       // +0x00
    cS3DVector3 direction;                    // +0x0c
    std::uint8_t unknown18[0x18] = {};        // +0x18..+0x2f (not read by the recovered code)
    float maxDistance = 3.4028235e38f;        // +0x30

    cSC4PickContext() = default;
};
// Only the observed offsets are asserted; the original record may be larger than this model.
static_assert(offsetof(cSC4PickContext, origin) == 0x00, "pick context layout");
static_assert(offsetof(cSC4PickContext, direction) == 0x0c, "pick context layout");
static_assert(offsetof(cSC4PickContext, maxDistance) == 0x30, "pick context layout");

// ---------------------------------------------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------------------------------------------
// The owning render object the original stores at +8 (cSC43DRender in the game); its vtable slot 0x70 is GetCamera.
class IViewUtilitiesHost {
public:
    virtual ~IViewUtilitiesHost() = default;
    virtual s3d::cS3DCamera* GetCamera() = 0;   // cSC43DRender vslot 0x70, called at 0x1001745da / 0x100175444 / 0x1001762f0
};

// cS3DMesh (not recovered): the geometry MeshPick walks. Original layout: +0 vertex array (u16 vertex count at +6),
// +8 index array, +0x10 primitive array. Named I*Host to sit in the project's seam registry; it stands in for the
// mesh record, not for a host object.
class IMeshPickHost {
public:
    virtual ~IMeshPickHost() = default;
    // *mesh != 0 && mesh->vertices->vertexCount (+6) != 0
    virtual bool HasPickableVertices() const = 0;
    // ((indexArray->end - indexArray->begin) >> 2) * 0xAAAAAAAB, i.e. the index element count divided by three
    // (the original relies on the count being a multiple of three: 0xAAAAAAAB is the inverse of 3 modulo 2^32).
    virtual int PrimitiveGroupCount() const = 0;
    // cS3DTriangleIterator::Init(mesh, primitiveGroup) @ 0x1001a9f28
    virtual void InitTriangleIterator(s3d::cS3DTriangleIterator& iterator, int primitiveGroup) const = 0;
};

// cIS3DMeshAndMaterial (iid 0x26a68448) as ModelPick reaches it: vtable slots 0x18 / 0x20. Stands in for the real
// interface, which the recovered s3d classes keep as a concrete record instead.
class IMeshMaterialHost {
public:
    virtual ~IMeshMaterialHost() = default;
    virtual int MeshCount() const = 0;                              // vslot 0x18
    virtual const IMeshPickHost* Mesh(int index) const = 0;       // vslot 0x20
};

// ---------------------------------------------------------------------------------------------------------------
// The two copies of the frustum data (view at +0x80, draw at +0x244; identical layout, 0xd8 bytes).
// ---------------------------------------------------------------------------------------------------------------
struct FrustumBounds {
    float boundsMin[3] = {0, 0, 0};        // +0x00 (+0x80 / +0x244)            bounds of the 8 corners
    float boundsMax[3] = {0, 0, 0};        // +0x0c (+0x8c / +0x250)
    float clippedMin[3] = {0, 0, 0};       // +0x18 (+0x98 / +0x25c)  city-clipped bounds (y min 0)
    float clippedMax[3] = {0, 0, 0};       // +0x24 (+0xa4 / +0x268)  (y max = near corner 3 height)
    float corner[8][3] = {};               // +0x30 (+0xb0 / +0x274)  near 4 then far 4: +R+U, +R-U, -R-U, -R+U
    float subLeft = 0, subRight = 0;       // +0x90 (+0x110 / +0x2d4) sub-view left / right
    float subLow = 0, subHigh = 0;         // +0x98 (+0x118 / +0x2dc) sub-view low / high (top / bottom in screen space)
    float nearPlane = 0, farPlane = 0;     // +0xa0 (+0x120 / +0x2e4)
    float ground[4][3] = {};               // +0xa8 (+0x128 / +0x2ec) ground points of the four viewport corners
};
static_assert(sizeof(FrustumBounds) == 0xd8, "FrustumBounds layout");

// The plane data at +0x168 (view) and +0x31c (draw), 0xa8 bytes. The fields are stored, not a plane struct: each
// group is (normal xyz, d) with the sign the original uses.
struct ViewPlaneBlock {
    float right[3] = {0, 0, 0};            // +0x00 (+0x168 / +0x31c)
    float rightD = 0;                      // +0x0c (+0x174 / +0x328)  -dot(right, corner3)
    float negRight[3] = {0, 0, 0};         // +0x10 (+0x178 / +0x32c)
    float negRightD = 0;                   // +0x1c (+0x184 / +0x338)  dot(right, corner0)
    float up[3] = {0, 0, 0};               // +0x20 (+0x188 / +0x33c)
    float upD = 0;                         // +0x2c (+0x194 / +0x348)  -dot(up, corner2)
    float negUp[3] = {0, 0, 0};            // +0x30 (+0x198 / +0x34c)
    float negUpD = 0;                      // +0x3c (+0x1a4 / +0x358)  dot(up, corner3)
    float dir[3] = {0, 0, 0};              // +0x40 (+0x1a8 / +0x35c)
    float dirD = 0;                        // +0x4c (+0x1b4 / +0x368)  -dot(dir, corner3)
    float negDir[3] = {0, 0, 0};           // +0x50 (+0x1b8 / +0x36c)
    float negDirFarD = 0;                  // +0x5c (+0x1c4 / +0x378)  -dot(dir, corner7)
    float rightXZ[2] = {0, 0};             // +0x60 (+0x1c8 / +0x37c) right.x, right.z
    float rightPlaneCopy[4] = {0, 0, 0, 0}; // +0x68 (+0x1d0 / +0x384) see the header quirks (drops -right.y)
    float reserved[12] = {};               // +0x78 (+0x1e0 / +0x394) zeroed
};
static_assert(sizeof(ViewPlaneBlock) == 0xa8, "ViewPlaneBlock layout");

// The two 2D side edges at +0x210 (view) and +0x3c4 (draw), 0x20 bytes. Each edge is an origin (viewport-space
// corner of the frustum projected to the ground) plus a direction in (x, z); IsBoundingBoxIn*Frustum uses the
// cross-product sign against them.
struct ViewEdgeBlock {
    float leftOriginX = 0, leftOriginZ = 0;      // +0x00 (+0x210 / +0x3c4) corner3 x / z
    float leftDirX = 0, leftDirZ = 0;            // +0x08 (+0x218 / +0x3cc) corner7 - corner3
    float rightOriginX = 0, rightOriginZ = 0;    // +0x10 (+0x220 / +0x3d4) corner0 x / z
    float rightDirX = 0, rightDirZ = 0;          // +0x18 (+0x228 / +0x3dc) corner4 - corner0
};
static_assert(sizeof(ViewEdgeBlock) == 0x20, "ViewEdgeBlock layout");

// kSC4YViewAngle (0x100a19640) and kSC4PitchViewAngle (0x100a2a2a0), indexed by zoom 0..4.
extern const float kSC4YViewAngle[5];
extern const float kSC4PitchViewAngle[5];

class SC4ViewUtilities {
public:
    SC4ViewUtilities();                          // 0x100173480 (C2), 0x1001736c4 (C1 alias)
    ~SC4ViewUtilities() = default;               // 0x1001736d4

    // --- state ----------------------------------------------------------------------------------------------
    bool IsReady() const { return ready_; }                                             // 0x1001736da
    IViewUtilitiesHost* Host() const { return host_; }
    void SetHost(IViewUtilitiesHost* host) { host_ = host; }   // the original stores +8 from cSC43DRender::Init
    float CitySizeX() const { return citySizeX_; }
    float CitySizeZ() const { return citySizeZ_; }
    // The original writes +0x28 / +0x2c directly (cSC43DRender::Init stores 0x45800000 = 4096 there).
    void SetCitySize(float citySizeX, float citySizeZ) { citySizeX_ = citySizeX; citySizeZ_ = citySizeZ; }
    int Zoom() const { return zoom_; }
    const cS3DVector3& EyePosition() const { return eye_; }   // +0x44
    const cS3DVector3& RightVector() const { return right_; }   // +0x68
    const cS3DVector3& UpVector() const { return up_; }         // +0x74
    int Rotation() const { return rotation_; }
    float RotationAngle() const { return rotationAngle_; }                              // +0x24
    float PitchAngle() const { return pitch_; }                                         // +0x20
    const cS3DVector3& ViewDirection() const { return viewDir_; }                       // +0x5c (unit)
    bool DrawFrustumReduced() const { return drawReduced_; }
    const FrustumBounds& ViewFrustumBounds() const { return view_; }
    const FrustumBounds& DrawFrustumBounds() const { return draw_; }
    const ViewPlaneBlock& ViewPlanes() const { return viewPlanes_; }
    const ViewPlaneBlock& DrawPlanes() const { return drawPlanes_; }
    const ViewEdgeBlock& ViewEdges() const { return viewEdges_; }
    const ViewEdgeBlock& DrawEdges() const { return drawEdges_; }
    const std::int32_t* ViewportRect() const { return viewportRect_; }

    void ClipWorldPositionToCityPosition(float* position) const;                        // 0x1001736e2
    float ViewportUnitsToWorldUnits(float units) const;                                 // 0x10017374c
    void GetZoomAndRotation(int& zoom, int& rotation) const;                            // 0x10017375a
    void GetVisibleEdges(int& edgeA, int& edgeB) const;                                 // 0x10017376a
    void GetCameraViewAngles(float& yaw, float& pitch, float& screenPitch) const;       // 0x1001737ae
    static void GetCameraViewAnglesForRotation(int rotation, int zoom, float& yaw, float& pitch,
                                               float& screenPitch);                    // 0x1001737ca

    // --- projection -----------------------------------------------------------------------------------------
    void ProjectPointToViewCoordinates(float* point) const;                             // 0x10017383c
    void ProjectPointFromViewCoordinatesToViewportCoordinates(float* point) const;      // 0x1001738e0
    void ProjectPointToViewportCoordinates(float* point) const;                         // 0x100173940
    float ProjectPointToViewportZ(const float* point) const;                            // 0x1001739b8
    void ProjectPointToViewportCoordinates2D(float* point) const;                       // 0x100173a1a
    void ProjectPointToFixedViewCoordinates(float* point) const;                        // 0x100173a70
    void ProjectPointFromViewportCoordinatesToFixedViewCoordinates(float* point) const; // 0x100173af8
    void UnProjectPointFromViewportCoordinatesToWorldCoordinates(float* point) const;   // 0x100173b1a
    void UnProjectPointFromViewCoordinatesToWorldCoordinates(float* point) const;       // 0x100173b8c

    // --- culling --------------------------------------------------------------------------------------------
    void GetViewFrustumBoundingBox(float* boxMin, float* boxMax) const;                 // 0x100173c3c
    bool IsBoundingBoxInViewFrustum(const float* boxMin, const float* boxMax) const;    // 0x100173c76
    bool IsBoundingBoxInDrawFrustum(const float* boxMin, const float* boxMax) const;    // 0x100173dfe
    bool IsPointInViewFrustum(const float* point) const;                                // 0x100173f8a
    void ProjectBoundingBoxToViewportRectangle(const float* boxMin, const float* boxMax, int* rectMin,
                                               int* rectMax) const;                     // 0x10017405e

    // --- camera / frustum updates ---------------------------------------------------------------------------
    void UpdateCameraPosition(sc4::cSC4CameraControl* control);                         // 0x1001745da
    void GetFrustumAABBCityClipped(bool useView, cS3DVector3& outMin, cS3DVector3& outMax);   // 0x1001751f4
    static void RayIntersectsGroundPlane(const float* origin, const float* direction, float* out);   // 0x10017542a
    void BuildDrawFrustumDataFromViewportRect(const int* rectMin, const int* rectMax);  // 0x100175444
    // The pieces the renderer's draw pass reads directly (the original reaches them by offset):
    //  * view_ / draw_ are the two frustum copies at +0x80 / +0x244; GetDrawFrustumGroundPlanePositions and
    //    GetFrustumAABBCityClipped wrap the parts the renderer used, so the raw struct is only needed for the
    //    corner reads GetDrawFrustumContainerRect does (+0x234 viewport rect, +0x244 corners).
    const FrustumBounds& ViewFrustum() const { return view_; }              // +0x80
    const FrustumBounds& DrawFrustum() const { return draw_; }              // +0x244
    const std::int32_t* DrawFrustumViewportRect() const { return viewportRect_; }   // +0x234
    // +0x3e4: the fixed-view camera position the screen-shake falloff measures against (0x1007663a8).
    const float* FixedViewCameraPosition() const { return unk3e4_; }
    void GetViewportLimitsGroundPlanePositions(float* bottomLeft, float* bottomRight, float* topLeft,
                                               float* topRight);                        // 0x10017611c
    void CalculateViewportGroundPlanePositions(float* p0, float* p1, float* p2, float* p3, const int* rectMin,
                                               const int* rectMax) const;               // 0x10017620c

    // --- rays / picking -------------------------------------------------------------------------------------
    bool ViewportPositionToRay(int x, int y, float* origin, float* direction) const;    // 0x1001762f0
    bool GetViewportPointIntersectionWithGroundPlane(int x, int y, float* out) const;   // 0x1001763e2
    static void GetRayIntersectionWithGroundPlane(const float* origin, const float* direction, float* out);   // 0x100176480
    static bool TrianglePick(cSC4PickContext* context, const float* a, const float* b, const float* c);       // 0x1001764d8
    static bool MeshPick(cSC4PickContext* context, const IMeshPickHost& mesh, const cS3DTransform& transform);   // 0x10017654c
    static bool ModelPick(cSC4PickContext* context, s3d::cIS3DModelInstance* instance,
                          const IMeshMaterialHost& meshes);                           // 0x100176a3c
    static void TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box);   // 0x100176ae6

    // --- ground rectangle / block helpers -------------------------------------------------------------------
    void GetGroundPlaneVisibleAreaRectangle(float* midpointMin, float* midpointMax, float* blockMin,
                                            float* blockMax);                           // 0x100176bac
    static float RoundDownToNearestBlockBoundary(float value, int block);               // 0x100176fe6
    static float RoundUpToNearestBlockBoundary(float value, int block);                 // 0x10017700e
    void GetDrawFrustumGroundPlanePositions(float* p0, float* p1, float* p2, float* p3) const;   // 0x10017703e
    void GetCityCornerPosition(int corner, float* out) const;                           // 0x10017708a
    static float GetObjectYRotationAngle(int rotation);                                 // 0x1001771a0
    static float GetYawAngle2D(float a, float b);                                       // 0x1001771b2
    static void RotateVector(float* v, int quarterTurns);                               // 0x1001771ca
    static void GetVectorForEdge(float* v, int edge);                                   // 0x10017723c
    static float GetCityWidthFromOrthoView(float width, int zoom);                      // 0x100177278

private:
    static bool BoxInFrustum(const ViewPlaneBlock& planes, const ViewEdgeBlock& edges, const float* boxMin,
                             const float* boxMax);
    static void FillPlaneBlock(ViewPlaneBlock& block, const cS3DVector3& right, const cS3DVector3& up,
                               const cS3DVector3& dir, const float* corner0, const float* corner2,
                               const float* corner3, const float* dirCorner3, const float* dirCorner7);
    static void FillEdgeBlock(ViewEdgeBlock& edges, const FrustumBounds& frustum);

    bool ready_ = false;                          // +0x00
    [[maybe_unused]] std::uint8_t pad01_[7] = {};                  // +0x01..+0x07
    IViewUtilitiesHost* host_ = nullptr;          // +0x08 (the game stores cSC43DRender here)
    float zoomUnits_ = 0;                         // +0x10 camera zoom as a float
    int zoom_ = 0;                                // +0x14
    int rotation_ = 0;                            // +0x18
    float yaw_ = 0;                               // +0x1c
    float pitch_ = 0;                             // +0x20
    float rotationAngle_ = 0;                     // +0x24
    float citySizeX_ = 0, citySizeZ_ = 0;         // +0x28 / +0x2c
    int viewportW_ = 0, viewportH_ = 0;           // +0x30 / +0x34
    float viewportWidth_ = 0, viewportHeight_ = 0;   // +0x38 / +0x3c
    float depthScale_ = 0;                        // +0x40 (1.0 after UpdateCameraPosition)
    cS3DVector3 eye_;                             // +0x44
    cS3DVector3 lookAt_;                          // +0x50
    cS3DVector3 viewDir_;                         // +0x5c
    cS3DVector3 right_;                           // +0x68
    cS3DVector3 up_;                              // +0x74
    FrustumBounds view_;                          // +0x80..+0x157
    float worldPerViewportUnit_ = 0;              // +0x158
    float viewportUnitsPerWorldUnit_ = 0;         // +0x15c
    float clipNear_ = 0, clipFar_ = 0;            // +0x160 / +0x164
    ViewPlaneBlock viewPlanes_;                   // +0x168..+0x20f
    ViewEdgeBlock viewEdges_;                     // +0x210..+0x22f
    bool drawReduced_ = false;                    // +0x230
    [[maybe_unused]] std::uint8_t pad231_[3] = {};                 // +0x231..+0x233
    std::int32_t viewportRect_[4] = {};           // +0x234..+0x243
    FrustumBounds draw_;                          // +0x244..+0x31b
    ViewPlaneBlock drawPlanes_;                   // +0x31c..+0x3c3
    ViewEdgeBlock drawEdges_;                     // +0x3c4..+0x3e3
    [[maybe_unused]] float unk3e4_[6] = {};                        // +0x3e4..+0x3fb (initialised only; no method here touches it)
    [[maybe_unused]] float unk3fc_[2] = {1.0f, 1.0f};              // +0x3fc / +0x400 (constructor default)
    [[maybe_unused]] float unk404_[6] = {};                        // +0x404..+0x41b
    float visibleAreaMidMin_[3] = {};             // +0x41c / +0x420 / +0x424
    float visibleAreaMidMax_[3] = {};             // +0x428 / +0x42c / +0x430
    float visibleAreaBlockMin_[3] = {};           // +0x434 / +0x438 / +0x43c
    float visibleAreaBlockMax_[3] = {};           // +0x440 / +0x444 / +0x448
    bool visibleAreaValid_ = false;               // +0x44c
    bool viewportLimitsValid_ = false;            // +0x44d
    [[maybe_unused]] std::uint8_t pad44e_[2] = {};                 // +0x44e / +0x44f
    float sinYaw_ = 0;                            // +0x450 sin(-yaw)
    float cosYaw_ = 0;                            // +0x454
    float sinPitch_ = 0;                          // +0x458
    float cosPitch_ = 0;                          // +0x45c
    float scaleX_ = 0, scaleY_ = 0, scaleZ_ = 0;  // +0x460 / +0x464 / +0x468
    float invScaleX_ = 0, invScaleY_ = 0, invScaleZ_ = 0;   // +0x46c / +0x470 / +0x474
};

// The original object is 0x478 bytes; the constructor in ViewUtilities.cpp asserts the key offsets.
