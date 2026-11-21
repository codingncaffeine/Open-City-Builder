// cSC4PolygonClipper<sVF_V3F_C4UB_T2F> (i386 0x008082fa..0x0080bfa2): cuts a mesh's triangles against an axis-aligned box.
// cSC4NetworkOccupantModelInstance::Clip runs it over each mesh of a bridge piece's model with only the y range set (the part
// of a pylon under the water surface or the ground is cut away). Every triangle is classified by the outcodes of its corners:
// all inside is kept (its corners renumbered into the clipper's own vertex list, each original vertex once), all outside one
// plane is dropped, anything else is clipped into a polygon that goes out as a triangle fan. The clipper ends with a new
// vertex list and index list for the mesh.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "s3d/S3DMath.h"

struct SC4MeshDrawRecord;   // sc4/DrawContext.h

namespace sc4 {

// sVF_V3F_C4UB_T2F: position, color (four bytes), one uv pair (24 bytes).
struct sVF_V3F_C4UB_T2F {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::uint8_t rgba[4] = {0, 0, 0, 0};
    float u = 0.0f, v = 0.0f;
};
static_assert(sizeof(sVF_V3F_C4UB_T2F) == 24, "the vertex record is 24 bytes");

class cSC4PolygonClipper {
public:
    // The outcode bits AddPrimitive gives a corner.
    static constexpr std::uint32_t kBelowX = 0x01, kBelowZ = 0x02, kAboveX = 0x04, kAboveZ = 0x08, kBelowY = 0x10, kAboveY = 0x20;

    // 0x008085ba: the box and whether only its y range clips (ClipPolygonY) or its x / z ranges (ClipPolygonXZ 0x0080b034, which
    // only a call with yOnly false reaches: the game makes none, and it is not ported; such a triangle is kept whole).
    void SetBounds(const cS3DVector3& lo, const cS3DVector3& hi, bool yOnly);
    // 0x0080be9e: the source vertices (`stride` bytes apart).
    void SetVerts(const void* vertices, int stride);
    // 0x0080bcd8: each draw record's triangles (indexed through `indices`, or numbered from the record's start without them) are
    // clipped; a record keeps its primitive and gets its new range in the output index list, a record left empty is dropped.
    // Returns the records kept (moved to the front). Quirk kept: each record's triangles are read from the input at the
    // output's length so far, not at the record's own start.
    int AddPrimitives(::SC4MeshDrawRecord* records, const std::uint16_t* indices, int count);
    // 0x00808fee: the clipped vertices into `vertices` (as bytes), the index list swapped into `indices`; returns the vertex count.
    std::uint16_t MoveTo(std::vector<std::uint8_t>& vertices, std::vector<std::uint16_t>& indices);
    // 0x0080bf72
    void Reset();

    // cSC4PolygonClipperCommon::Interpolate (0x008082fa): position and uv linear, each color byte truncated.
    static sVF_V3F_C4UB_T2F Interpolate(const sVF_V3F_C4UB_T2F& a, const sVF_V3F_C4UB_T2F& b, float t);

private:
    // 0x0080ba60: `count` indices from `start` (or `start`, `start` + 1, ... without indices), three per triangle.
    void AddPrimitive(const std::uint16_t* indices, int start, int count);
    // 0x0080ac30: the polygon against the y planes; from an inside corner to an outside one the edge's exit point is added, from
    // outside to inside its entry, across both planes both (in the edge's order); the result goes out as a fan.
    void ClipPolygonY(const std::uint16_t* polygon, const std::uint32_t* codes, int count);
    // 0x0080841c: the point where the edge from the outside corner `a` towards `b` crosses the planes `code` names (the
    // furthest crossing along it).
    sVF_V3F_C4UB_T2F ComputeLineToRectIntersection(std::uint32_t code, const sVF_V3F_C4UB_T2F& a, const sVF_V3F_C4UB_T2F& b) const;
    // 0x008093fe: an original vertex's index in the output (copied there on its first use).
    std::uint16_t TranslateOriginalVertIndex(std::uint16_t index);
    std::uint32_t OutCode(const sVF_V3F_C4UB_T2F& v) const;
    const sVF_V3F_C4UB_T2F& Source(std::uint16_t index) const;
    void Fan(const std::uint16_t* polygon, int count);

    cS3DVector3 lo_{}, hi_{};                       // +0x00 / +0x0c
    const std::uint8_t* source_ = nullptr;          // +0x18
    int stride_ = 0;                                // +0x1c
    std::vector<sVF_V3F_C4UB_T2F> vertices_;        // +0x20
    std::vector<std::uint16_t> indices_;            // +0x2c
    std::unordered_map<std::uint32_t, std::uint16_t> translated_;   // +0x38
    bool yOnly_ = false;                            // +0x4c
};

}  // namespace sc4
