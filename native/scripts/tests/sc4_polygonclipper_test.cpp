// cSC4PolygonClipper (the y clip the bridge pieces' model instances run): triangles inside the y range are kept with their
// corners renumbered once each, triangles below or above it are dropped, a triangle across a plane becomes the fan of its
// clipped polygon (new corners interpolated in position, color and uv), a triangle across the whole range keeps the band
// between both planes; the draw records keep their primitive, get their new ranges and empty ones are dropped.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include "sc4/DrawContext.h"
#include "sc4/PolygonClipper.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

using namespace sc4;

namespace {

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

sVF_V3F_C4UB_T2F V(float x, float y, float z, std::uint8_t c, float u, float v) {
    sVF_V3F_C4UB_T2F out;
    out.x = x; out.y = y; out.z = z;
    out.rgba[0] = out.rgba[1] = out.rgba[2] = c;
    out.rgba[3] = 255;
    out.u = u; out.v = v;
    return out;
}

struct Result {
    std::vector<sVF_V3F_C4UB_T2F> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<SC4MeshDrawRecord> records;
};

Result Clip(const std::vector<sVF_V3F_C4UB_T2F>& source, const std::vector<std::uint16_t>& indices,
            std::vector<SC4MeshDrawRecord> records, float low, float high) {
    cSC4PolygonClipper clipper;
    clipper.SetBounds(cS3DVector3{-1e30f, low, -1e30f}, cS3DVector3{1e30f, high, 1e30f}, true);
    clipper.SetVerts(source.data(), static_cast<int>(sizeof(sVF_V3F_C4UB_T2F)));
    Result result;
    records.resize(static_cast<std::size_t>(clipper.AddPrimitives(records.data(), indices.empty() ? nullptr : indices.data(),
                                                                  static_cast<int>(records.size()))));
    result.records = records;
    std::vector<std::uint8_t> bytes;
    const std::uint16_t count = clipper.MoveTo(bytes, result.indices);
    result.vertices.resize(count);
    if (count) std::memcpy(result.vertices.data(), bytes.data(), bytes.size());
    return result;
}

float LowestY(const Result& r) {
    float low = 1e30f;
    for (std::uint16_t i : r.indices) low = std::min(low, r.vertices[i].y);
    return low;
}

}  // namespace

int main() {
    // ---- inside: kept, each corner once (a shared corner is not copied twice) ----
    {
        const std::vector<sVF_V3F_C4UB_T2F> quad = {V(0, 1, 0, 10, 0, 0), V(1, 1, 0, 20, 1, 0), V(1, 2, 0, 30, 1, 1), V(0, 2, 0, 40, 0, 1)};
        const Result r = Clip(quad, {0, 1, 2, 0, 2, 3}, {{4, 0, 6}}, 0.0f, 100.0f);
        CHECK(r.vertices.size() == 4);
        CHECK(r.indices == (std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3}));
        CHECK(r.records.size() == 1 && r.records[0].primitive == 4 && r.records[0].start == 0 && r.records[0].count == 6);
    }
    // ---- wholly below: dropped with its record ----
    {
        const std::vector<sVF_V3F_C4UB_T2F> tri = {V(0, -3, 0, 0, 0, 0), V(1, -3, 0, 0, 0, 0), V(0, -1, 1, 0, 0, 0)};
        const Result r = Clip(tri, {0, 1, 2}, {{4, 0, 3}}, 0.0f, 100.0f);
        CHECK(r.records.empty() && r.indices.empty() && r.vertices.empty());
    }
    // ---- one corner below: a quad above y 0 as two fan triangles, the new corners on the plane, color and uv interpolated ----
    {
        const std::vector<sVF_V3F_C4UB_T2F> tri = {V(0, 10, 0, 100, 0, 0), V(10, 10, 0, 100, 1, 0), V(0, -10, 0, 200, 0, 1)};
        const Result r = Clip(tri, {}, {{4, 0, 3}}, 0.0f, 100.0f);   // no index list: the record numbers its corners
        CHECK(r.records.size() == 1 && r.records[0].count == 6);
        CHECK(r.vertices.size() == 4);
        CHECK(Near(LowestY(r), 0.0f));
        // corner 2's two edges cross y 0 at (5, 0) and (0, 0), halfway: color 150, v 0.5
        bool five = false, zero = false;
        for (const auto& v : r.vertices) {
            if (Near(v.y, 0.0f) && Near(v.x, 5.0f)) five = v.rgba[0] == 150 && Near(v.u, 0.5f) && Near(v.v, 0.5f);
            if (Near(v.y, 0.0f) && Near(v.x, 0.0f)) zero = v.rgba[0] == 150 && Near(v.u, 0.0f) && Near(v.v, 0.5f);
        }
        CHECK(five && zero);
        CHECK(r.indices == (std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3}));
    }
    // ---- across both planes: the band 0..5 between them ----
    {
        const std::vector<sVF_V3F_C4UB_T2F> tri = {V(0, -10, 0, 0, 0, 0), V(10, 10, 0, 0, 0, 0), V(0, 10, 0, 0, 0, 0)};
        const Result r = Clip(tri, {0, 1, 2}, {{4, 0, 3}}, 0.0f, 5.0f);
        float low = 1e30f, high = -1e30f;
        for (std::uint16_t i : r.indices) {
            low = std::min(low, r.vertices[i].y);
            high = std::max(high, r.vertices[i].y);
        }
        CHECK(Near(low, 0.0f) && Near(high, 5.0f));
        CHECK(r.vertices.size() == 4 && r.indices.size() == 6);   // four crossings, two triangles
        // every fan triangle keeps the source triangle's winding (its crossings come in the edge's direction)
        bool wound = true;
        for (std::size_t i = 0; i + 2 < r.indices.size(); i += 3) {
            const auto& a = r.vertices[r.indices[i]];
            const auto& b = r.vertices[r.indices[i + 1]];
            const auto& c = r.vertices[r.indices[i + 2]];
            wound = wound && (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f;
        }
        CHECK(wound);
    }
    // ---- several records: an empty one is dropped and the next moves up; the next record's triangles are read at the
    //      output's length so far (the original's quirk) ----
    {
        const std::vector<sVF_V3F_C4UB_T2F> verts = {V(0, -5, 0, 0, 0, 0), V(1, -5, 0, 0, 0, 0), V(0, -4, 0, 0, 0, 0),
                                                     V(0, 1, 0, 0, 0, 0), V(1, 1, 0, 0, 0, 0), V(0, 2, 0, 0, 0, 0)};
        const Result r = Clip(verts, {0, 1, 2, 3, 4, 5}, {{4, 0, 3}, {5, 3, 3}}, 0.0f, 100.0f);
        // the first record (below) is dropped; the second is read from the input at 0 (the output is still empty), so it
        // is the first triangle again and is dropped too
        CHECK(r.records.empty());
        const Result kept = Clip(verts, {3, 4, 5, 0, 1, 2}, {{4, 0, 3}, {5, 3, 3}}, 0.0f, 100.0f);
        // the first record keeps its triangle (output 3 long), the second reads input from 3: the triangle below, dropped
        CHECK(kept.records.size() == 1 && kept.records[0].primitive == 4 && kept.records[0].count == 3);
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sc4_polygonclipper: all passed\n");
    return 0;
}
