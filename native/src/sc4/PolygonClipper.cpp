#include "sc4/PolygonClipper.h"

#include <cstring>
#include <utility>

#include "sc4/DrawContext.h"

namespace sc4 {

void cSC4PolygonClipper::SetBounds(const cS3DVector3& lo, const cS3DVector3& hi, bool yOnly) {
    lo_ = lo;
    hi_ = hi;
    yOnly_ = yOnly;
}

void cSC4PolygonClipper::SetVerts(const void* vertices, int stride) {
    source_ = static_cast<const std::uint8_t*>(vertices);
    stride_ = stride;
    indices_.reserve(12);
    vertices_.reserve(12);
}

void cSC4PolygonClipper::Reset() {
    vertices_.clear();
    indices_.clear();
    translated_.clear();
}

const sVF_V3F_C4UB_T2F& cSC4PolygonClipper::Source(std::uint16_t index) const {
    return *reinterpret_cast<const sVF_V3F_C4UB_T2F*>(source_ + static_cast<std::size_t>(index) * static_cast<std::size_t>(stride_));
}

sVF_V3F_C4UB_T2F cSC4PolygonClipper::Interpolate(const sVF_V3F_C4UB_T2F& a, const sVF_V3F_C4UB_T2F& b, float t) {
    sVF_V3F_C4UB_T2F out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.z = a.z + (b.z - a.z) * t;
    for (int k = 0; k < 4; ++k)
        out.rgba[k] = static_cast<std::uint8_t>(static_cast<int>(static_cast<float>(a.rgba[k]) +
                                                                 static_cast<float>(static_cast<int>(b.rgba[k]) - static_cast<int>(a.rgba[k])) * t));
    out.u = a.u + (b.u - a.u) * t;
    out.v = a.v + t * (b.v - a.v);
    return out;
}

// The below-x / below-y / below-z bits are set strictly below the low bound, the above bits strictly above the high one.
std::uint32_t cSC4PolygonClipper::OutCode(const sVF_V3F_C4UB_T2F& v) const {
    std::uint32_t code = v.x < lo_.x ? kBelowX : 0u;
    if (v.y < lo_.y) code += kBelowY;
    if (v.z < lo_.z) code += kBelowZ;
    if (hi_.x < v.x) code += kAboveX;
    if (hi_.y < v.y) code += kAboveY;
    if (hi_.z < v.z) code += kAboveZ;
    return code;
}

sVF_V3F_C4UB_T2F cSC4PolygonClipper::ComputeLineToRectIntersection(std::uint32_t code, const sVF_V3F_C4UB_T2F& a,
                                                                   const sVF_V3F_C4UB_T2F& b) const {
    float t = 0.0f;
    if (code & kBelowX) {
        const float s = (lo_.x - a.x) / (b.x - a.x);
        if (0.0f <= s) t = s;
    }
    const auto further = [&](float s) { if (!(s < t)) t = s; };
    if (code & kBelowY) further((lo_.y - a.y) / (b.y - a.y));
    if (code & kBelowZ) further((lo_.z - a.z) / (b.z - a.z));
    if (code & kAboveX) further((hi_.x - a.x) / (b.x - a.x));
    if (code & kAboveY) further((hi_.y - a.y) / (b.y - a.y));
    if (code & kAboveZ) further((hi_.z - a.z) / (b.z - a.z));
    return Interpolate(a, b, t);
}

std::uint16_t cSC4PolygonClipper::TranslateOriginalVertIndex(std::uint16_t index) {
    const auto next = static_cast<std::uint16_t>(vertices_.size());
    const auto inserted = translated_.emplace(index, next);
    if (inserted.second) vertices_.push_back(Source(index));
    return inserted.first->second;
}

void cSC4PolygonClipper::Fan(const std::uint16_t* polygon, int count) {
    for (int i = 2; i < count; ++i) {
        indices_.push_back(polygon[0]);
        indices_.push_back(polygon[i - 1]);
        indices_.push_back(polygon[i]);
    }
}

void cSC4PolygonClipper::ClipPolygonY(const std::uint16_t* polygon, const std::uint32_t* codes, int count) {
    std::uint16_t out[16];
    int emitted = 0;
    for (int i = 0; i < count; ++i) {
        const int next = i == count - 1 ? 0 : i + 1;
        const sVF_V3F_C4UB_T2F& previous = Source(polygon[i]);
        const sVF_V3F_C4UB_T2F& current = Source(polygon[next]);
        const std::uint32_t currentCode = codes[next] & (kBelowY | kAboveY);
        const std::uint32_t previousCode = codes[i] & (kBelowY | kAboveY);
        if (previousCode == 0) {
            out[emitted++] = TranslateOriginalVertIndex(polygon[i]);
            if (currentCode != 0) {
                out[emitted++] = static_cast<std::uint16_t>(vertices_.size());
                vertices_.push_back(ComputeLineToRectIntersection(currentCode, current, previous));
            }
        } else if (currentCode == 0) {
            out[emitted++] = static_cast<std::uint16_t>(vertices_.size());
            vertices_.push_back(ComputeLineToRectIntersection(previousCode, previous, current));
        } else if ((previousCode & currentCode) == 0) {
            // across the whole slab: both crossings, in the edge's direction
            float enter = (lo_.y - previous.y) / (current.y - previous.y);
            float leave = (hi_.y - previous.y) / (current.y - previous.y);
            if (leave < enter) std::swap(enter, leave);
            out[emitted++] = static_cast<std::uint16_t>(vertices_.size());
            vertices_.push_back(Interpolate(previous, current, enter));
            out[emitted++] = static_cast<std::uint16_t>(vertices_.size());
            vertices_.push_back(Interpolate(previous, current, leave));
        }
    }
    Fan(out, emitted);
}

void cSC4PolygonClipper::AddPrimitive(const std::uint16_t* indices, int start, int count) {
    const std::uint16_t* cursor = indices ? indices + start : nullptr;
    auto number = static_cast<std::uint16_t>(start);
    for (int i = 0; i < count - 2; i += 3, number = static_cast<std::uint16_t>(number + 3)) {
        std::uint16_t triangle[3];
        if (!cursor) {
            triangle[0] = number;
            triangle[1] = static_cast<std::uint16_t>(number + 1);
            triangle[2] = static_cast<std::uint16_t>(number + 2);
        } else {
            triangle[0] = cursor[0];
            triangle[1] = cursor[1];
            triangle[2] = cursor[2];
            cursor += 3;
        }
        const std::uint32_t codes[3] = {OutCode(Source(triangle[0])), OutCode(Source(triangle[1])), OutCode(Source(triangle[2]))};
        if (codes[0] & codes[1] & codes[2]) continue;   // wholly outside one plane
        if ((codes[0] | codes[1] | codes[2]) == 0 || !yOnly_) {
            // inside (or, with the unported x / z clip, kept whole)
            for (std::uint16_t corner : triangle) indices_.push_back(TranslateOriginalVertIndex(corner));
            continue;
        }
        ClipPolygonY(triangle, codes, 3);
    }
}

int cSC4PolygonClipper::AddPrimitives(::SC4MeshDrawRecord* records, const std::uint16_t* indices, int count) {
    ::SC4MeshDrawRecord* kept = records;
    for (int i = 0; i != count; ++i) {
        ::SC4MeshDrawRecord& record = records[i];
        record.start = static_cast<std::uint32_t>(indices_.size());
        AddPrimitive(indices, static_cast<int>(record.start), static_cast<int>(record.count));
        record.count = static_cast<std::uint32_t>(indices_.size()) - record.start;
        if (record.count == 0) continue;
        if (&record != kept) *kept = record;
        ++kept;
    }
    return static_cast<int>(kept - records);
}

std::uint16_t cSC4PolygonClipper::MoveTo(std::vector<std::uint8_t>& vertices, std::vector<std::uint16_t>& indices) {
    const std::size_t count = vertices_.size();
    vertices.assign(count * sizeof(sVF_V3F_C4UB_T2F), 0);
    if (count) std::memcpy(vertices.data(), vertices_.data(), count * sizeof(sVF_V3F_C4UB_T2F));
    indices_.swap(indices);
    return static_cast<std::uint16_t>(count);
}

}  // namespace sc4
