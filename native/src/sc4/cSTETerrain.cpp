#include <cstdio>
#include <cstdlib>

#include "sc4/cSTETerrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "gz/CommandParameterSet.h"
#include "gz/CommandServer.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZPersistRecord.h"
#include "gz/cIGZPersistResourceManager.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZVariant.h"
#include "sc4/DBSegment.h"
#include "sc4/ISC4RegionalCity.h"
#include "sc4/Region.h"
#include "sc4/SCProperty.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kGroupTerrain = 0xe98f9525;
constexpr std::uint32_t kTypeMisc = 0x8a91e7e3, kInstanceMisc = 3;
constexpr std::uint32_t kTypeAltitudes = 0xa9dd6ff4, kInstanceAltitudes = 1;
constexpr std::uint32_t kTypeVertexProps = 0x8a91e7e0, kInstanceVertexProps = 2;

// cSC4AutoLoadStream / cSC4AutoSaveStream: a record opened for the duration of a scope with a
// leading u16 version.
class AutoLoad {
public:
    AutoLoad(cIGZPersistDBSegment* segment, std::uint32_t type, std::uint32_t group, std::uint32_t instance)
        : segment_(segment) {
        if (!segment_) return;
        if (!segment_->OpenRecord({type, group, instance}, &record_, 1) || !record_) { record_ = nullptr; return; }
        void* raw = nullptr;
        if (!record_->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &raw)) return;
        stream_ = static_cast<cIGZPersistDBSerialRecord*>(raw);
        stream_->GetFieldUint16(version_);
    }
    ~AutoLoad() {
        if (stream_) stream_->Release();
        if (record_) {
            segment_->CloseRecord(record_);
            record_->Release();
        }
    }
    bool IsOpen() const { return stream_ != nullptr; }
    cIGZPersistDBSerialRecord& Stream() { return *stream_; }
    std::uint16_t Version() const { return version_; }

private:
    cIGZPersistDBSegment* segment_;
    cIGZPersistDBRecord* record_ = nullptr;
    cIGZPersistDBSerialRecord* stream_ = nullptr;
    std::uint16_t version_ = 0;
};

class AutoSave {
public:
    AutoSave(cIGZPersistDBSegment* segment, std::uint32_t type, std::uint32_t group, std::uint32_t instance,
             std::uint16_t version)
        : segment_(segment) {
        if (!segment_) return;
        cGZPersistResourceKey key{type, group, instance};
        bool ok = segment_->TestForRecord(key) ? segment_->OpenRecord(key, &record_, 3)
                                               : segment_->CreateNewRecord(key, &record_);
        if (!ok || !record_) { record_ = nullptr; return; }
        void* raw = nullptr;
        if (!record_->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &raw)) return;
        stream_ = static_cast<cIGZPersistDBSerialRecord*>(raw);
        stream_->SetSize(0);
        stream_->SeekAbsolute(0);
        stream_->SetFieldUint16(version);
    }
    ~AutoSave() {
        if (stream_) stream_->Release();
        if (record_) {
            segment_->CloseRecord(record_);
            record_->Release();
        }
    }
    bool IsOpen() const { return stream_ != nullptr; }
    cIGZPersistDBSerialRecord& Stream() { return *stream_; }

private:
    cIGZPersistDBSegment* segment_;
    cIGZPersistDBRecord* record_ = nullptr;
    cIGZPersistDBSerialRecord* stream_ = nullptr;
};
}  // namespace

void cSTETerrain::LayOutGrid(std::uint32_t cellsX, std::uint32_t cellsZ) {
    cellsX_ = cellsX;
    cellsZ_ = cellsZ;
    lastCellX_ = cellsX - 1;
    lastCellZ_ = cellsZ - 1;
    vertsX_ = cellsX + 1;
    vertsZ_ = cellsZ + 1;
    totalVerts_ = vertsX_ * vertsZ_;
    widthMeters_ = static_cast<float>(cellsX) * kTerrainCellWidth;
    depthMeters_ = static_cast<float>(cellsZ) * kTerrainCellWidth;
    const auto vx = static_cast<std::int32_t>(vertsX_);
    const std::int32_t offsets[8] = {-1, ~vx, -vx, 1 - vx, 1, vx + 1, vx, vx - 1};
    std::copy(offsets, offsets + 8, neighborOffsets_);
    gridRect_ = {0, 0, static_cast<std::int32_t>(cellsX), static_cast<std::int32_t>(cellsZ)};   // +0x58
    // The original Init allocates the vertex array only when there is none (+0x88 == 0), so a terrain that Load read keeps its data.
    if (vertices_.size() != totalVerts_) vertices_.assign(totalVerts_, Vertex{});
    if (waterDepth_.size() != totalVerts_) waterDepth_.assign(totalVerts_, 0.0f);        // cSTESurfaceWater vertex +0x0
    if (sediment_.size() != totalVerts_) sediment_.assign(totalVerts_, 0.0f);          // cSTESurfaceWater vertex +0x1c
    if (tempWaterLevels_.size() != totalVerts_) tempWaterLevels_.assign(totalVerts_, 0.0f);   // +0x100 (allocated in the original's Init)
    surfaceCover_.assign(vertsZ_, std::vector<std::uint8_t>(vertsX_, 0));   // +0x200
    waterVertexCount_ = 0;                        // +0x108
    initialized_ = true;
}

// The port's fixture overload: lays the grid out directly instead of asking the city (Init 0x100739cb0).
bool cSTETerrain::Init(std::uint32_t cellsX, std::uint32_t cellsZ, const TerrainTuning& tuning) {
    tuning_ = tuning;
    LayOutGrid(cellsX, cellsZ);
    return true;
}

std::uint32_t cSTETerrain::FloorCell(float coordinate) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(coordinate * 0.0625f)));
}

std::uint32_t cSTETerrain::ClampCell(float coordinate, std::uint32_t limit) const {
    return std::min(FloorCell(coordinate), limit);
}

std::uint8_t cSTETerrain::GetNeighbors(std::int32_t x, std::int32_t z) const {
    std::uint8_t mask = 0x7c;
    if (0 < x) mask = 0xff;
    if (cellsX_ <= static_cast<std::uint32_t>(x)) mask &= 0xc7;
    if (z < 1) mask &= 0xf1;
    if (cellsZ_ <= static_cast<std::uint32_t>(z)) mask &= 0x1f;
    return mask;
}

// ------------------------------------------------------------------------------- altitude

float cSTETerrain::GetAltitude(float x, float z) const {
    if (!initialized_) return 0.0f;
    std::uint32_t cx = ClampCell(x, lastCellX_), cz = ClampCell(z, lastCellZ_);
    const std::uint32_t vx = vertsX_;
    const float h00 = vertices_[cz * vx + cx].altitude;
    const float fx = x - static_cast<float>(cx) * 16.0f;
    const float fz = z - static_cast<float>(cz) * 16.0f;
    const std::uint32_t rowNext = (cz + 1) * vx;
    std::uint32_t mid = cz * vx + 1;
    if (fx <= fz) mid = rowNext;
    const float lo = fx <= fz ? fx : fz;
    const float hi = fz < fx ? fx : fz;
    const float hMid = vertices_[mid + cx].altitude;
    const float h11 = vertices_[rowNext + 1 + cx].altitude;
    return ((hMid - h00) * hi + (h11 - hMid) * lo) * 0.0625f + h00;
}

float cSTETerrain::GetAltitude(std::int32_t index) const {
    return static_cast<std::uint32_t>(index) < totalVerts_ ? vertices_[static_cast<std::size_t>(index)].altitude : 0.0f;
}

float cSTETerrain::GetAltitudeAtVertex(std::int32_t x, std::int32_t z) const {
    if (static_cast<std::uint32_t>(x) < vertsX_ && static_cast<std::uint32_t>(z) < vertsZ_)
        return vertices_[vertsX_ * static_cast<std::uint32_t>(z) + static_cast<std::uint32_t>(x)].altitude;
    return 0.0f;
}

float cSTETerrain::GetAltitudeAtNearestGrid(float x, float z) const {
    if (!initialized_) return 0.0f;
    const auto gx = static_cast<std::int32_t>(std::nearbyintf(x * 0.0625f));
    const auto gz = static_cast<std::int32_t>(std::nearbyintf(z * 0.0625f));
    return vertices_[static_cast<std::uint32_t>(gx + gz * static_cast<std::int32_t>(vertsX_))].altitude;
}

void cSTETerrain::GetCellCornerAltitudes(std::uint32_t cx, std::uint32_t cz, float out[4]) const {
    if (cx >= cellsX_ || cz >= cellsZ_) return;
    const std::uint32_t i = cz * vertsX_ + cx;
    out[0] = vertices_[i].altitude;
    out[1] = vertices_[i + vertsX_].altitude;
    out[2] = vertices_[i + 1 + vertsX_].altitude;
    out[3] = vertices_[i + 1].altitude;
}

void cSTETerrain::GetAltitudes(const TerrainRect& r, float* out, std::ptrdiff_t rowStride, std::ptrdiff_t colStride) const {
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        float* p = out + (z - r.top) * rowStride;
        for (std::int32_t x = r.left; x <= r.right; ++x, p += colStride)
            *p = vertices_[static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x)].altitude;
    }
}

void cSTETerrain::FindMinAndMaxHeights() {
    if (vertices_.empty()) return;
    minHeight_ = maxHeight_ = vertices_[0].altitude;
    for (std::uint32_t i = 1; i < totalVerts_; ++i) {
        float h = vertices_[i].altitude;
        if (h < minHeight_) minHeight_ = h;
        else if (maxHeight_ < h) maxHeight_ = h;
    }
}

void cSTETerrain::FindMinAndMaxHeights(const TerrainRect& r, float& lo, float& hi) const {
    lo = 3.4028235e+38f;
    hi = -3.4028235e+38f;
    for (std::int32_t z = r.top; z <= r.bottom; ++z)
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            float h = vertices_[static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x)].altitude;
            if (h < lo) lo = h;
            if (hi < h) hi = h;
        }
}

void cSTETerrain::SetAltitude(std::int32_t index, float altitude) {
    Vertex& v = vertices_[static_cast<std::size_t>(index)];
    if (v.flags & kVertexLocked) return;
    float& depth = waterDepth_[static_cast<std::size_t>(index)];
    float surface = v.altitude + depth;
    v.altitude = altitude;
    if (0.0f < depth || altitude < tuning_.seaLevel) {
        // flooded vertices keep their water surface; dry ones that dip under the sea fill to it
        float reference = 0.0f < depth ? surface : tuning_.seaLevel;
        depth = reference - altitude;
    }
}

void cSTETerrain::SetAltitude(const TerrainRect& r, float altitude, bool recomputeWater, std::uint32_t flags) {
    // 0x10073e1f2: the rectangle's rows, then the water recompute and the redisplay (the original calls
    // the virtual SetAltitude per vertex, so a locked vertex is skipped).
    for (std::int32_t z = r.top; z <= r.bottom; ++z)
        for (std::int32_t x = r.left; x <= r.right; ++x)
            SetAltitude(static_cast<std::int32_t>(vertsX_) * z + x, altitude);
    if (recomputeWater) {
        RecomputeWaterLevel(r);
        RedisplayTerrain(true, true, r, flags);   // vslot 0x228
    }
}

void cSTETerrain::LockAltitude(const TerrainRect& r, bool lock) {
    for (std::int32_t z = r.top; z <= r.bottom; ++z)
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            std::uint16_t& f = Flags(static_cast<std::int32_t>(vertsX_) * z + x);
            f = lock ? static_cast<std::uint16_t>(f | kVertexLocked) : static_cast<std::uint16_t>(f & ~kVertexLocked);
        }
}

// --------------------------------------------------------------------------------- normals

void cSTETerrain::GetFaceNormal(float x, float z, TerrainVector3& out) const {
    if (!(0.0f <= x && 0.0f <= z && x < widthMeters_ && z < depthMeters_)) return;
    const std::uint32_t cx = FloorCell(x), cz = FloorCell(z);
    const std::uint32_t vx = vertsX_;
    const float h00 = vertices_[cz * vx + cx].altitude;
    const float diag = vertices_[(cz + 1) * vx + 1 + cx].altitude - h00;
    const float fx = x - static_cast<float>(cx) * 16.0f, fz = z - static_cast<float>(cz) * 16.0f;
    float nx, nz;
    if (fx <= fz) {
        float up = (vertices_[(cz + 1) * vx + cx].altitude - h00) * 16.0f;
        nx = up - diag * 16.0f;
        nz = diag * 0.0f - up;
    } else {
        float side = (vertices_[cz * vx + cx + 1].altitude - h00) * 16.0f;
        nx = diag * 0.0f - side;
        nz = side - diag * 16.0f;
    }
    float inv = 1.0f / std::sqrt(nz * nz + nx * nx + 65536.0f);
    out.x = nx * inv;
    out.y = inv * 256.0f;
    out.z = nz * inv;
}

void cSTETerrain::GetLerpedNormal(float x, float z, TerrainVector3& out) const {
    if (!initialized_) return;
    const float gx = x * 0.0625f, gz = z * 0.0625f;
    const std::uint32_t cx = FloorCell(x), cz = FloorCell(z);
    const float fx = gx - static_cast<float>(cx), fz = gz - static_cast<float>(cz);
    const std::uint32_t vx = vertsX_, i00 = cz * vx + cx;
    const Vertex& a = vertices_[i00];
    const Vertex& b = vertices_[i00 + 1];
    const Vertex& c = vertices_[i00 + vx];
    const Vertex& d = vertices_[i00 + vx + 1];
    float n[3];
    for (int k = 0; k < 3; ++k) {
        float top = (b.normal[k] - a.normal[k]) * fx + a.normal[k];
        float bottom = (d.normal[k] - c.normal[k]) * fx + c.normal[k];
        n[k] = (bottom - top) * fz + top;
    }
    float inv = 1.0f / std::sqrt(n[2] * n[2] + n[1] * n[1] + n[0] * n[0]);
    out = {n[0] * inv, n[1] * inv, n[2] * inv};
}

void cSTETerrain::GetVertexNormal(std::uint32_t x, std::uint32_t z, TerrainVector3& out) const {
    if (x < vertsX_ && z < vertsZ_) {
        const Vertex& v = vertices_[vertsX_ * z + x];
        out = {v.normal[0], v.normal[1], v.normal[2]};
    }
}

// ------------------------------------------------------------------------------------ water

bool cSTETerrain::SetSeaLevel(float level) {
    if (!(tuning_.minSeaLevel <= level && level <= tuning_.maxSeaLevel)) return false;
    tuning_.seaLevel = level;
    RecomputeWaterLevel({0, 0, static_cast<std::int32_t>(cellsX_), static_cast<std::int32_t>(cellsZ_)});
    return true;
}

void cSTETerrain::RecomputeWaterLevel(const TerrainRect& r) {
    for (std::int32_t z = r.top; z <= r.bottom; ++z)
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x);
            const float altitude = vertices_[i].altitude;
            waterDepth_[i] = altitude < tuning_.seaLevel ? tuning_.seaLevel - altitude : 0.0f;
        }
}

float cSTETerrain::GetWaterDepthAtVertex(std::int32_t index) const {
    return static_cast<std::uint32_t>(index) < totalVerts_ ? waterDepth_[static_cast<std::size_t>(index)] : 0.0f;
}

float cSTETerrain::GetWaterDepthAtVertex(std::int32_t x, std::int32_t z) const {
    if (static_cast<std::uint32_t>(x) < vertsX_ && static_cast<std::uint32_t>(z) < vertsZ_)
        return waterDepth_[vertsX_ * static_cast<std::uint32_t>(z) + static_cast<std::uint32_t>(x)];
    return 0.0f;
}

float cSTETerrain::GetWaterLevel(std::int32_t index) const {
    if (static_cast<std::uint32_t>(index) < totalVerts_)
        return vertices_[static_cast<std::size_t>(index)].altitude + waterDepth_[static_cast<std::size_t>(index)];
    return 0.0f;
}

float cSTETerrain::GetWaterLevel(float x, float z) const {
    if (!initialized_) return 0.0f;
    const std::size_t i = FloorCell(z) * vertsX_ + FloorCell(x);
    return vertices_[i].altitude + waterDepth_[i];
}

float cSTETerrain::GetLerpedWaterLevel(float x, float z) const {
    if (!initialized_) return 0.0f;
    const float gx = x * 0.0625f, gz = z * 0.0625f;
    const std::uint32_t cx = FloorCell(x), cz = FloorCell(z), vx = vertsX_;
    const std::size_t i = cz * vx + cx;
    auto level = [&](std::size_t k) { return vertices_[k].altitude + waterDepth_[k]; };
    const float fx = gx - static_cast<float>(cx), fz = gz - static_cast<float>(cz);
    const float v00 = level(i), v01 = level(i + vx);
    const float top = (level(i + 1) - v00) * fx + v00;
    const float bottom = (level(i + 1 + vx) - v01) * fx + v01;
    return (bottom - top) * fz + top;
}

float cSTETerrain::GetMaxSurfaceLevel(std::uint32_t cx, std::uint32_t cz) const {
    if (cx >= cellsX_ || cz >= cellsZ_) return 0.0f;
    const std::uint32_t vx = vertsX_, i = cz * vx + cx;
    auto surface = [&](std::uint32_t k) {
        float depth = waterDepth_[k];
        return (0.0f <= depth ? depth : 0.0f) + vertices_[k].altitude;
    };
    float best = surface(i);
    for (std::uint32_t k : {i + vx, i + vx + 1, i + 1}) best = std::max(best, surface(k));
    return best;
}

void cSTETerrain::GetWaterDepths(const TerrainRect& r, float* out, std::ptrdiff_t rowStride, std::ptrdiff_t colStride) const {
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        float* p = out + (z - r.top) * rowStride;
        for (std::int32_t x = r.left; x <= r.right; ++x, p += colStride)
            *p = waterDepth_[static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x)];
    }
}

bool cSTETerrain::IsSeaVertex(std::int32_t index) const {
    return vertices_[static_cast<std::size_t>(index)].altitude < tuning_.seaLevel;
}
bool cSTETerrain::IsSeaVertex(std::uint32_t x, std::uint32_t z) const {
    return x < vertsX_ && z < vertsZ_ && vertices_[vertsX_ * z + x].altitude < tuning_.seaLevel;
}
bool cSTETerrain::IsWaterVertex(std::int32_t index) const { return 0.0f < waterDepth_[static_cast<std::size_t>(index)]; }
bool cSTETerrain::IsWaterVertex(std::uint32_t x, std::uint32_t z) const {
    return x < vertsX_ && z < vertsZ_ && 0.0f < waterDepth_[vertsX_ * z + x];
}
bool cSTETerrain::IsLandVertex(std::int32_t index) const { return waterDepth_[static_cast<std::size_t>(index)] <= 0.0f; }
bool cSTETerrain::IsLandVertex(std::uint32_t x, std::uint32_t z) const {
    return x < vertsX_ && z < vertsZ_ && waterDepth_[vertsX_ * z + x] <= 0.0f;
}

bool cSTETerrain::IsShoreVertex(std::int32_t index) const {
    // 0x10073751a: a land vertex (strictly above the sea) with a sea neighbour. The original walks the four
    // even entries of the neighbour mask -- west, north, east and south (the loop counter is compared before
    // it is advanced, so 6 is still tested).
    const float sea = tuning_.seaLevel;
    if (!(sea < vertices_[static_cast<std::size_t>(index)].altitude)) return false;
    const std::uint32_t z = static_cast<std::uint32_t>(index) / vertsX_, x = static_cast<std::uint32_t>(index) % vertsX_;
    const std::uint8_t mask = GetNeighbors(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z));
    for (int dir = 0; dir < 8; dir += 2) {
        if (!((mask >> dir) & 1)) continue;
        if (vertices_[static_cast<std::size_t>(neighborOffsets_[dir] + index)].altitude < sea) return true;
    }
    return false;
}

std::int32_t cSTETerrain::GetNeighboringSeaVertex(std::int32_t index) const {
    // 0x100731a1c. The original walks the four even entries of kXIncrement / kZIncrement (0x100a29c10 /
    // 0x100a29c40) -- west, north, east and south -- and returns the first one below the sea level.
    const std::uint32_t vertsX = vertsX_, vertsZ = vertsZ_;
    for (int dir = 0; dir < 8; dir += 2) {
        // (sic) the row coordinate is divided by the *vertex count in Z*, not X: the original reads +0x54.
        const std::uint32_t x = static_cast<std::uint32_t>(index) % vertsX + static_cast<std::uint32_t>(kDirectionX[dir]);
        const std::uint32_t z = static_cast<std::uint32_t>(index) / vertsZ + static_cast<std::uint32_t>(kDirectionZ[dir]);
        if (vertsX <= x || vertsZ <= z) continue;
        const std::uint32_t neighbour = z * vertsX + x;
        if (tuning_.seaLevel <= vertices_[neighbour].altitude) continue;   // not a sea vertex
        return static_cast<std::int32_t>(neighbour);
    }
    return -1;
}

bool cSTETerrain::CellHasWater(std::uint32_t cx, std::uint32_t cz) const {
    if (cx >= cellsX_ || cz >= cellsZ_) return false;
    const std::uint32_t i = cz * vertsX_ + cx;
    // true unless all four corners are dry-or-... exactly as the original: any corner with depth <= 0
    // short-circuits to true, and when all of the first three are wet the last decides.
    if (waterDepth_[i] <= 0.0f && waterDepth_[i + 1] <= 0.0f && waterDepth_[i + vertsX_] <= 0.0f)
        return 0.0f < waterDepth_[i + vertsX_ + 1];
    return true;
}

bool cSTETerrain::CellHasLand(std::uint32_t cx, std::uint32_t cz) const {
    if (cx >= cellsX_ || cz >= cellsZ_) return false;
    const std::uint32_t i = cz * vertsX_ + cx;
    if (0.0f < waterDepth_[i] && 0.0f < waterDepth_[i + 1] && 0.0f < waterDepth_[i + vertsX_])
        return waterDepth_[i + vertsX_ + 1] <= 0.0f;
    return true;
}

// ------------------------------------------------------------------------- cell flags

std::uint16_t cSTETerrain::GetTerrainCellFlags(std::uint32_t cx, std::uint32_t cz) const {
    if (cx >= cellsX_ || cz >= cellsZ_) return 0;
    std::uint16_t f = vertices_[cz * vertsX_ + cx].flags;
    return static_cast<std::uint16_t>(((f >> 10) & 8) | ((f >> 7) & 4) | ((f >> 5) & 3));
}

bool cSTETerrain::SetTerrainCellFlags(std::uint32_t cx, std::uint32_t cz, std::uint32_t flags) {
    if (cx >= cellsX_ || cz >= cellsZ_) return false;
    std::uint16_t& f = vertices_[cz * vertsX_ + cx].flags;
    const std::uint16_t old = f;
    f = static_cast<std::uint16_t>(((flags & 3) << 5) | ((flags & 4) << 7) | (old & 0xfd9f));
    FlipExternalTriangulation(cx, cz, ((flags & 8) >> 3) != 0);
    CellChanged(cx, cz);
    return f != old;
}

void cSTETerrain::HideTerrainCell(std::uint32_t cx, std::uint32_t cz, bool hide) {
    if (cx >= cellsX_ || cz >= cellsZ_) return;
    std::uint16_t& f = vertices_[cz * vertsX_ + cx].flags;
    f = hide ? static_cast<std::uint16_t>(f | 0x60) : static_cast<std::uint16_t>(f & 0xff9f);
    CellChanged(cx, cz);
}

void cSTETerrain::HideEvenTriangle(std::uint32_t cx, std::uint32_t cz, bool hide) {
    if (cx >= cellsX_ || cz >= cellsZ_) return;
    std::uint16_t& f = vertices_[cz * vertsX_ + cx].flags;
    f = hide ? static_cast<std::uint16_t>(f | 0x20) : static_cast<std::uint16_t>(f & 0xffdf);
    CellChanged(cx, cz);
}

void cSTETerrain::HideOddTriangle(std::uint32_t cx, std::uint32_t cz, bool hide) {
    if (cx >= cellsX_ || cz >= cellsZ_) return;
    std::uint16_t& f = vertices_[cz * vertsX_ + cx].flags;
    f = hide ? static_cast<std::uint16_t>(f | 0x40) : static_cast<std::uint16_t>(f & 0xffbf);
    CellChanged(cx, cz);
}

bool cSTETerrain::IsEvenTriangleHidden(std::uint32_t cx, std::uint32_t cz) const {
    return cx < cellsX_ && cz < cellsZ_ && (vertices_[cz * vertsX_ + cx].flags & 0x20) != 0;
}
bool cSTETerrain::IsOddTriangleHidden(std::uint32_t cx, std::uint32_t cz) const {
    return cx < cellsX_ && cz < cellsZ_ && (vertices_[cz * vertsX_ + cx].flags & 0x40) != 0;
}
bool cSTETerrain::IsWholeCellHidden(std::uint32_t cx, std::uint32_t cz) const {
    return cx < cellsX_ && cz < cellsZ_ && (vertices_[cz * vertsX_ + cx].flags & 0x60) == 0x60;
}
bool cSTETerrain::IsTriangulationFlipped(std::uint32_t cx, std::uint32_t cz) const {
    return cx < cellsX_ && cz < cellsZ_ && (vertices_[cz * vertsX_ + cx].flags & 0x3000) != 0;
}

bool cSTETerrain::IsPointHidden(float x, float z) const {
    const std::uint32_t cx = ClampCell(x, lastCellX_), cz = ClampCell(z, lastCellZ_);
    const float fx = x - static_cast<float>(cx) * 16.0f, fz = z - static_cast<float>(cz) * 16.0f;
    const std::uint16_t f = vertices_[cz * vertsX_ + cx].flags;
    bool odd = fz < fx;
    if (f & 0x3000) odd = 16.0f < fx + fz;
    return (f & (odd ? 0x40 : 0x20)) != 0;
}

void cSTETerrain::FlipInternalTriangulation(std::uint32_t cx, std::uint32_t cz, bool flip) {
    if (cx >= cellsX_ || cz >= cellsZ_) return;
    std::uint16_t& f = vertices_[cz * vertsX_ + cx].flags;
    const std::uint16_t n = flip ? static_cast<std::uint16_t>(f | 0x1000) : static_cast<std::uint16_t>(f & 0xefff);
    if (n != f) {
        f = n;
        CellChanged(cx, cz);
    }
}

void cSTETerrain::FlipExternalTriangulation(std::uint32_t cx, std::uint32_t cz, bool flip) {
    if (cx >= cellsX_ || cz >= cellsZ_) return;
    std::uint16_t& f = vertices_[cz * vertsX_ + cx].flags;
    const std::uint16_t n = flip ? static_cast<std::uint16_t>(f | 0x2000) : static_cast<std::uint16_t>(f & 0xdfff);
    if (n != f) {
        f = n;
        CellChanged(cx, cz);
    }
}

// ------------------------------------------------------------------------------ persistence

bool cSTETerrain::LoadMiscTerrainData(cIGZPersistDBSegment* segment) {
    AutoLoad in(segment, kTypeMisc, kGroupTerrain, kInstanceMisc);
    if (!in.IsOpen()) return false;
    cIGZPersistDBSerialRecord& s = in.Stream();
    s.GetFieldUint32(vertsX_);
    s.GetFieldUint32(vertsZ_);
    s.GetFieldSint32(terrainType_);
    s.GetFieldFloat32(minHeight_);
    s.GetFieldFloat32(maxHeight_);
    s.GetFieldFloat32(maxNormalHeight_);
    if (in.Version() < 2 && terrainType_ > 0) terrainType_ = 0;
    totalVerts_ = vertsX_ * vertsZ_;
    // Port guard: the original assumes the saved size equals the one Init took from the city and never
    // re-sizes the per-vertex blocks. Keep every grid-indexed array consistent with the loaded dimensions.
    if (surfaceCover_.size() != vertsZ_ || (!surfaceCover_.empty() && surfaceCover_[0].size() != vertsX_))
        surfaceCover_.assign(vertsZ_, std::vector<std::uint8_t>(vertsX_, 0));
    if (tempWaterLevels_.size() != totalVerts_) tempWaterLevels_.assign(totalVerts_, 0.0f);
    if (sediment_.size() != totalVerts_) sediment_.assign(totalVerts_, 0.0f);
    return true;
}

bool cSTETerrain::LoadAltitudes(cIGZPersistDBSegment* segment) {
    AutoLoad in(segment, kTypeAltitudes, kGroupTerrain, kInstanceAltitudes);
    if (!in.IsOpen() || in.Version() != 2) return false;
    if (vertices_.size() != totalVerts_) vertices_.assign(totalVerts_, Vertex{});
    if (waterDepth_.size() != totalVerts_) waterDepth_.assign(totalVerts_, 0.0f);
    for (std::uint32_t i = 0; i < totalVerts_; ++i) in.Stream().GetFieldFloat32(vertices_[i].altitude);
    return true;
}

bool cSTETerrain::LoadVertexProperties(cIGZPersistDBSegment* segment) {
    AutoLoad in(segment, kTypeVertexProps, kGroupTerrain, kInstanceVertexProps);
    if (!in.IsOpen() || in.Version() != 1 || vertices_.empty()) return false;
    for (std::uint32_t i = 0; i < totalVerts_; ++i) in.Stream().GetFieldUint16(vertices_[i].flags);
    return true;
}

bool cSTETerrain::Load(cIGZPersistDBSegment* segment) {
    loadOk_ = false;
    if (LoadMiscTerrainData(segment) && LoadAltitudes(segment)) loadOk_ = LoadVertexProperties(segment);
    return true;   // (sic) the original always reports success; LastLoadSucceeded() has the truth
}

bool cSTETerrain::SaveMiscTerrainData(cIGZPersistDBSegment* segment) const {
    AutoSave out(segment, kTypeMisc, kGroupTerrain, kInstanceMisc, 2);
    if (!out.IsOpen()) return false;
    cIGZPersistDBSerialRecord& s = out.Stream();
    s.SetFieldUint32(vertsX_);
    s.SetFieldUint32(vertsZ_);
    s.SetFieldSint32(terrainType_);
    s.SetFieldFloat32(minHeight_);
    s.SetFieldFloat32(maxHeight_);
    s.SetFieldFloat32(maxNormalHeight_);
    return true;
}

bool cSTETerrain::SaveAltitudes(cIGZPersistDBSegment* segment) const {
    AutoSave out(segment, kTypeAltitudes, kGroupTerrain, kInstanceAltitudes, 2);
    if (!out.IsOpen()) return false;
    for (std::uint32_t i = 0; i < totalVerts_; ++i) out.Stream().SetFieldFloat32(vertices_[i].altitude);
    return true;
}

bool cSTETerrain::SaveVertexProperties(cIGZPersistDBSegment* segment) const {
    AutoSave out(segment, kTypeVertexProps, kGroupTerrain, kInstanceVertexProps, 1);
    if (!out.IsOpen()) return false;
    for (std::uint32_t i = 0; i < totalVerts_; ++i) out.Stream().SetFieldUint16(vertices_[i].flags);
    return true;
}

bool cSTETerrain::Save(cIGZPersistDBSegment* segment) const {
    SaveMiscTerrainData(segment);
    SaveAltitudes(segment);
    SaveVertexProperties(segment);
    for (int edge = 0; edge < 4; ++edge) SaveEdgeAltitudes(edge, segment);   // 0x10073f17a
    return true;
}


// ==================================================================================== identity

namespace {
// SL::spMS2 / spLightingManager (the seams this file talks to).
ITerrainHost*& HostSlot() {
    static ITerrainHost* host = nullptr;
    return host;
}
// sbDisplayPlumbingInfo: a class-wide display flag (cSTETerrain::sbDisplayPlumbingInfo).
bool& PlumbingDisplayFlag() {
    static bool flag = false;
    return flag;
}
// The original's round-to-nearest helper: the (v + 1.5 * 2^23) - 2^23 sequence Ghidra inlines in
// GetSedimentValue (0x1007394dc); ties go to even, as nearbyintf does.
std::int32_t RoundToNearestInt(float v) { return static_cast<std::int32_t>(std::nearbyintf(v)); }
}  // namespace

ITerrainHost* TerrainHost() { return HostSlot(); }
void SetTerrainHost(ITerrainHost* host) { HostSlot() = host; }

bool cSTETerrain::QueryInterface(std::uint32_t iid, void** out) {
    // 0x10073aa92: the original answers with this (+0, iid 1 / 0xab953253 / 0xc70bd37e), this + 0x8
    // (cIGZMessageTarget2) or this + 0x10 (cIGZSerializable). The port keeps a single cIGZSerializable
    // base, so every accepted id resolves to `this`; the serializable id is what the city-load
    // serializer (SC4ReadGZSerializable) asks for.
    if (iid != 0xab953253 && iid != 0xc70bd37e && iid != 0xe4fda3d4 && iid != 0x452294aa && iid != 1) return false;
    if (out) *out = static_cast<cIGZSerializable*>(this);
    AddRef();
    return true;
}

std::uint32_t cSTETerrain::AddRef() { return ++refCount_; }

std::uint32_t cSTETerrain::Release() {
    if (refCount_ <= 1) {
        delete this;   // the original calls its deleting destructor (vslot 0x2e0)
        return 0;
    }
    return --refCount_;
}

// ==================================================================================== messages

void cSTETerrain::PostMessageA(std::uint32_t type, std::uint64_t a, std::uint64_t b, std::uint64_t c,
                               std::uint64_t d) {
    ITerrainHost* host = TerrainHost();
    cIGZMessageServer2* server = host ? host->Messages() : nullptr;
    if (!server) return;
    cIGZMessage2Standard* message = host->NewMessage();
    if (!message) return;
    message->SetType(type);
    message->SetData1(a);
    message->SetData2(b);
    message->SetData3(c);
    message->SetData4(d);
    server->MessagePost(message, false);
    message->Release();
}

void cSTETerrain::PostMessageToTarget(std::uint32_t type, cIGZMessageTarget2* target) {
    ITerrainHost* host = TerrainHost();
    cIGZMessageServer2* server = host ? host->Messages() : nullptr;
    if (!server || !target) return;
    cIGZMessage2Standard* message = host->NewMessage();
    if (!message) return;
    message->SetType(type);
    server->GeneralMessagePostToTarget(message, target);
    message->Release();
}

void cSTETerrain::BeginLongTerrainOperation() { PostMessageA(0xea3b0af1, 0, 0, 0, 0); }
void cSTETerrain::EndLongTerrainOperation() { PostMessageA(0x6a3b0afd, 0, 0, 0, 0); }

// ==================================================================================== display

bool cSTETerrain::GetPlumbingInfoDisplayState() const { return PlumbingDisplayFlag(); }
void cSTETerrain::SetPlumbingInfoDisplayState(bool on) { PlumbingDisplayFlag() = on; }

void cSTETerrain::SetWaterSurfaceDisplayState(bool on) {
    if (displayWater_ == on) return;
    displayWater_ = on;
    ITerrainHost* host = TerrainHost();
    cIGZMessageServer2* server = host ? host->Messages() : nullptr;
    if (!server) return;
    cIGZMessage2Standard* message = host->NewMessage();
    if (!message) return;
    message->SetType(0x6ab97795);
    message->SetData1(on ? 1u : 0u);
    server->MessagePost(message, false);
    message->Release();
}

void cSTETerrain::StartAccumulatingTextureUpdates() {
    if (accumulating_) return;                                                // 0x10073e90e
    accumulating_ = true;                                                     // +0xe0
    accumulateRect_ = {0x7fffffff, 0x7fffffff, -0x7fffffff - 1, -0x7fffffff - 1};   // +0xe4
}

void cSTETerrain::StopAccumulatingTextureUpdates() {
    if (!accumulating_) return;                                               // 0x10073ea34
    accumulating_ = false;
    if (accumulateRect_.left <= accumulateRect_.right && accumulateRect_.top <= accumulateRect_.bottom) {
        AssignSurfaceTexturesAndColors(accumulateRect_);
        RedisplayTerrain(false, false, accumulateRect_, 0);                   // vslot 0x228
        accumulateRect_ = {0x7fffffff, 0x7fffffff, -0x7fffffff - 1, -0x7fffffff - 1};
    }
}

// ==================================================================================== statics

TerrainStatics& cSTETerrain::Statics() {
    static TerrainStatics statics;   // the 0x100b71xxx block (zero filled in the binary)
    return statics;
}

namespace {
// SC4Rect<int>::IntersectRect @ 0x100314ce8: the overlap of two inclusive rects; touching rects overlap.
bool IntersectTerrainRect(TerrainRect& out, const TerrainRect& a, const TerrainRect& b) {
    if (a.right < b.left || b.right < a.left || a.bottom < b.top || b.bottom < a.top) {
        out = {0, 0, 0, 0};
        return false;
    }
    out.left = a.left < b.left ? b.left : a.left;
    out.top = a.top < b.top ? b.top : a.top;
    out.right = a.right < b.right ? a.right : b.right;
    out.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    return true;
}
}  // namespace

void cSTETerrain::UnionRect(TerrainRect& dst, const TerrainRect& a, const TerrainRect& b) {
    dst.left = std::min(a.left, b.left);
    dst.top = std::min(a.top, b.top);
    dst.right = std::max(a.right, b.right);
    dst.bottom = std::max(a.bottom, b.bottom);
}

namespace {
// sbInited: the class-wide "statics read" flag of StaticInit / StaticShutdown.
bool& StaticInited() {
    static bool inited = false;
    return inited;
}

// sSurfaceCoverTextures (0x100b71cd8): texture id -> tSurfaceCoverInfo, filled by ReadTextureInfo.
std::map<std::uint32_t, TerrainSurfaceCoverInfo>& SurfaceCoverTexturesSlot() {
    static std::map<std::uint32_t, TerrainSurfaceCoverInfo> textures;
    return textures;
}
// sTexturesUsedInTerrainType (0x100b71cb0): one sorted, duplicate-free id list per terrain type. The
// original keeps five std::lists; the sorted vector is observationally the same for the view's setup.
std::array<std::vector<std::uint32_t>, 5>& TexturesUsedSlot() {
    static std::array<std::vector<std::uint32_t>, 5> lists;
    return lists;
}
// sHighCliffTexResIds / sLowCliffTexResIds / sBeachTexResIds (0x100b71cc8 / ccc / cd0). The original
// indexes these *four-byte* globals by terrain type; only terrain type 0 is in range, which is the quirk the
// port keeps (see the ledger note).
std::uint32_t& HighCliffTextureId() { static std::uint32_t id = 0; return id; }
std::uint32_t& LowCliffTextureId() { static std::uint32_t id = 0; return id; }
std::uint32_t& BeachTextureId() { static std::uint32_t id = 0; return id; }
// sSurfaceCoverMap (0x100b714b0): [terrain type][temperature 0..31][moisture 0..15] -> texture id. The
// binary allocates one 0x800-byte block; the port allocates four so a non-tropical city cannot read past
// the end (quirk noted in notes/ledger_pending).
std::array<std::array<std::uint32_t, 512>, 4>& SurfaceCoverMapSlot() {
    static std::array<std::array<std::uint32_t, 512>, 4> map{};
    return map;
}
// sTextureFamilyMemberForGrid (0x100b71410): five vectors of random 16-bit family members, indexed by the
// grid cell (x / randomization size, z / randomization size). sTextureGridCellCountX is their row stride.
std::array<std::vector<std::uint16_t>, 5>& TextureFamilyMemberSlot() {
    static std::array<std::vector<std::uint16_t>, 5> grids;
    return grids;
}
std::int32_t* TextureGridCellCountX() {
    static std::int32_t counts[5] = {0, 0, 0, 0, 0};
    return counts;
}

// ---- the property readers the exemplar parse needs (SCPropertyHelp is not in this test's link set) ----------
cIGZVariant* PropertyValue(const cISCPropertyHolder* holder, std::uint32_t id) {
    if (!holder) return nullptr;
    cISCProperty* property = holder->GetProperty(id);
    return property ? property->GetPropertyValue() : nullptr;
}
bool ReadPropertyFloat(const cISCPropertyHolder* holder, std::uint32_t id, float& out) {
    cIGZVariant* value = PropertyValue(holder, id);
    return value && value->GetValFloat32(out);
}
bool ReadPropertyUint32(const cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t& out) {
    cIGZVariant* value = PropertyValue(holder, id);
    return value && value->GetValUint32(out);
}
bool ReadPropertyInt32(const cISCPropertyHolder* holder, std::uint32_t id, std::int32_t& out) {
    std::uint32_t word = 0;
    if (!ReadPropertyUint32(holder, id, word)) return false;
    out = static_cast<std::int32_t>(word);
    return true;
}
bool ReadPropertyUint8(const cISCPropertyHolder* holder, std::uint32_t id, std::uint8_t& out) {
    cIGZVariant* value = PropertyValue(holder, id);
    if (!value) return false;
    if (value->GetType() == cIGZVariant::kTypeUint8) return value->GetValUint8(out);
    std::uint32_t word = 0;
    if (!value->GetValUint32(word)) return false;
    out = static_cast<std::uint8_t>(word);
    return true;
}
bool ReadPropertyFloatVector(const cISCPropertyHolder* holder, std::uint32_t id, std::vector<float>& out) {
    cIGZVariant* value = PropertyValue(holder, id);
    if (!value) return false;
    const float* data = value->RefFloat32();
    const std::uint32_t count = value->GetCount();
    out.assign(data, data + count);
    return true;
}
bool ReadPropertyUintVector(const cISCPropertyHolder* holder, std::uint32_t id, std::vector<std::uint32_t>& out) {
    cIGZVariant* value = PropertyValue(holder, id);
    if (!value) return false;
    const std::uint32_t* data = value->RefUint32();
    const std::uint32_t count = value->GetCount();
    out.assign(data, data + count);
    return true;
}

// The response-curve evaluation cSC4ResponseCurve::GetOutputValue @ 0x1000ac4f6 provides for the
// tSurfaceCoverInfo curves. The port keeps the points directly (the curve class is not linked here) and
// mirrors its segment search and clamp exactly.
void BuildCurvePoints(const float* interleaved, std::uint32_t valueCount, std::vector<TerrainPoint>& out) {
    out.clear();
    for (std::uint32_t i = 0; i + 1 < valueCount; i += 2) out.push_back({interleaved[i], interleaved[i + 1]});
}
float EvaluateCurve(const std::vector<TerrainPoint>& points, float input) {
    const std::size_t count = points.size();
    if (count == 0) return 0.0f;                       // the original dereferences a null array here
    if (count == 1) return points[0].z;
    const std::size_t last = count - 1;
    std::size_t lo = 0, hi = last;
    do {
        const std::size_t mid = (lo + hi) >> 1;
        if (input < points[mid].x) hi = mid; else lo = mid + 1;
    } while (lo < hi);
    if (lo == 0) return points[0].z;
    if (lo >= last && points[last].x <= input) return points[last].z;
    const TerrainPoint& a = points[lo - 1];
    const TerrainPoint& b = points[lo];
    const float dx = b.x - a.x;
    if (dx == 0.0f) return a.z;
    return (input - a.x) * ((b.z - a.z) / dx) + a.z;
}
}  // namespace

// =============================================================================== life cycle

bool cSTETerrain::StaticInit() {
    // 0x10073a346: the flag, the shutdown hook, the resource-manager hold, then the four data readers.
    if (StaticInited()) return true;
    StaticInited() = true;
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "cSTETerrain::StaticInit host %p\n", static_cast<void*>(TerrainHost()));
    if (ITerrainHost* host = TerrainHost()) {
        host->RegisterStaticShutdown();        // SL::spSC4 vslot 0x218 (StaticShutdown, 0)
        host->AcquireResourceManager();        // spResMan = SL::spRM, one reference held
    }
    ReadTextureInfos();                        // 0x10073d8a4
    ReadTerrainIniFile();                      // 0x10073bb96
    ReadTuningParameters();                    // 0x10073d95e
    ReadBrushDescriptions();                   // 0x10073b49c
    return true;
}

bool cSTETerrain::StaticShutdown(void* unused) {
    (void)unused;   // 0x10073e9de (the second argument of the spSC4 shutdown callback, always 0)
    if (!StaticInited()) return true;
    StaticInited() = false;
    if (ITerrainHost* host = TerrainHost()) {
        host->UnregisterStaticShutdown();      // SL::spSC4 vslot 0x220
        host->ReleaseResourceManager();        // spResMan release (+0x10)
    }
    ReleaseBrushDescriptions();
    return true;
}

// =============================================================================== data readers

namespace {
// sMaxTextureId (0x100b71d00): the highest texture-resource instance seen, plus one after the enumeration.
std::uint32_t& MaxTextureId() {
    static std::uint32_t id = 0;
    return id;
}

// RZ::ConvertToUint @ 0x10001a307 (a decimal parse unless the text starts with 0x or contains a hex digit).
std::uint32_t ParseTerrainUint(const char* text) {
    if (!text) return 0;
    int base = 10;
    if (text[0] == '0' && (static_cast<unsigned char>(text[1]) | 0x20) == 'x') {
        base = 16;
    } else {
        for (const char* p = text;; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) { base = 16; break; }
            if (c == '\0' || c < '0' || c > '9') break;
        }
    }
    return static_cast<std::uint32_t>(std::strtoul(text, nullptr, base));
}

// RZAtofEnglishA @ 0x100019b4c (the process is in the C locale, so strtod is the same parse).
double ParseTerrainFloat(const char* text) { return text ? std::strtod(text, nullptr) : 0.0; }

// The ini entries all live in one of three sections (the first is kTerrainTypeNames[0] + "TextureMapTable").
constexpr const char* kTerrainTextureMapSection = "TropicalTextureMapTable";
constexpr const char* kMiscTunablesSection = "MiscTerrainTunables";
constexpr const char* kWindErosionSection = "WindErosion";
constexpr const char* kHydrologySection = "Hydrology";
}  // namespace

std::map<std::uint32_t, TerrainSurfaceCoverInfo>& cSTETerrain::SurfaceCoverTextures() {
    return SurfaceCoverTexturesSlot();
}

std::vector<std::uint32_t>& cSTETerrain::TexturesUsedInTerrainType(std::int32_t terrainType) {
    static std::vector<std::uint32_t> emptyList;
    if (terrainType < 0 || 5 <= terrainType) return emptyList;
    return TexturesUsedSlot()[static_cast<std::size_t>(terrainType)];
}

void cSTETerrain::ReadTextureInfos() {
    // 0x10073d8a4: enumerate every resource of group 0x6534284a / type 0x89c2a517 through
    // cSCResFilterFixedGroupAndTypeId, read each one's property holder, then bump the highest instance.
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    MaxTextureId() = 0;
    std::vector<cGZPersistResourceKey> keys;
    host->ListTerrainTextureResources(keys);
    for (const cGZPersistResourceKey& key : keys) ReadTextureInfo(key, nullptr);
    MaxTextureId() += 1;
}

void cSTETerrain::ReadTextureInfo(const cGZPersistResourceKey& key, void* unused) {
    // 0x10073d22a: the resource's property 0x9c2aef0 is the cover weight and 0x9c2aef3..0x9c2aef7 are five
    // interleaved (input, output) float pairs, one response curve per zoom. The last input of each curve is
    // also its "max spread" in cells (ceil(last * 0.0625)), which raises sMaxSurfaceTextureSpread.
    (void)unused;
    if (MaxTextureId() < key.instance) MaxTextureId() = key.instance;
    auto& textures = SurfaceCoverTexturesSlot();
    auto found = textures.find(key.instance);
    if (found == textures.end()) found = textures.emplace(key.instance, TerrainSurfaceCoverInfo{}).first;
    TerrainSurfaceCoverInfo& info = found->second;
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    cISCPropertyHolder* holder = host->GetPropertyHolder(key);
    if (!holder) return;
    ReadPropertyFloat(holder, 0x9c2aef0, info.weight);
    for (std::uint32_t layer = 0; layer < 5; ++layer) {
        cIGZVariant* value = PropertyValue(holder, 0x9c2aef3 + layer);
        if (!value) continue;
        const float* data = value->RefFloat32();
        const std::uint32_t count = value->GetCount();
        if (count < 2) continue;
        const float maxInput = data[count - 2];
        const auto spread = static_cast<std::int32_t>(std::ceil(maxInput * 0.0625f));   // ROUNDSS mode 10
        info.maxSpread[layer] = spread;
        if (Statics().maxSurfaceTextureSpread[layer] < spread) Statics().maxSurfaceTextureSpread[layer] = spread;
        BuildCurvePoints(data, count, info.curves[layer]);
        info.curveMaxInput[layer] = maxInput;
    }
    holder->Release();
}

void cSTETerrain::AddToTerrainTypeTextureList(std::int32_t terrainType, std::uint32_t textureId) {
    // 0x10073159e: insert into the terrain type's std::list in ascending order, ignoring duplicates.
    if (terrainType < 0 || 5 <= terrainType) return;
    std::vector<std::uint32_t>& list = TexturesUsedSlot()[static_cast<std::size_t>(terrainType)];
    const auto position = std::lower_bound(list.begin(), list.end(), textureId);
    if (position == list.end() || *position != textureId) list.insert(position, textureId);
}

void cSTETerrain::AddTemperatureAndMoistureToSurfaceCoverEntry(const cRZString& key, const cRZString& value,
                                                               void* terrainTypePointer) {
    // 0x100731418: the key is the temperature row (0..31); the value is up to 15 comma-separated texture ids for the moisture
    // columns 0..14 (cRZString::SplitToken on ','; the first token lands on the map base, 0x100b714b0). The context selects the
    // terrain type.
    const std::uint32_t temperature = ParseTerrainUint(key.ToChar()) & 0xfffffff;
    const std::uint32_t terrainType =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(terrainTypePointer)) & 0x7fffff;
    std::array<std::uint32_t, 512>& table = SurfaceCoverMapSlot()[terrainType < 4 ? terrainType : 0];
    const char* cursor = value.ToChar();
    for (std::uint32_t token = 0; token < 15; ++token) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        char text[64];
        std::size_t length = 0;
        while (*cursor != '\0' && *cursor != ',' && length + 1 < sizeof(text)) text[length++] = *cursor++;
        while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r')) --length;
        text[length] = '\0';
        if (*cursor == ',') ++cursor;
        const std::uint32_t textureId = ParseTerrainUint(text);
        table[(temperature & 31) * 16 + token] = textureId;
        AddToTerrainTypeTextureList(static_cast<std::int32_t>(terrainType), textureId);
    }
}

void cSTETerrain::DetermineTextureFamilyMembersForGridCells() {
    // 0x100737430: per zoom, a (randGrid+1) x (0x101/randGrid + 1) table of random 16-bit family members.
    ITerrainHost* host = TerrainHost();
    const std::vector<std::uint32_t>& gridSizes = Statics().textureRandomizationGridSize;
    if (!host || gridSizes.empty()) return;      // no exemplar: the original would divide by zero
    for (std::size_t zoom = 0; zoom < 5; ++zoom) {
        const std::uint32_t gridSize = zoom < gridSizes.size() ? gridSizes[zoom] : gridSizes.front();
        if (gridSize == 0) continue;
        const std::int32_t rows = static_cast<std::int32_t>(0x101 / gridSize);
        const std::int32_t columns = rows + 1;
        TextureGridCellCountX()[zoom] = columns;
        std::vector<std::uint16_t>& family = TextureFamilyMemberSlot()[zoom];
        family.resize(static_cast<std::size_t>((rows + 1) * columns));
        for (std::int32_t row = 0; row < rows; ++row) {
            for (std::int32_t column = 0; column < columns; ++column) {
                const auto member = static_cast<std::uint16_t>(host->RandomDoubleRangeUniform(0.0, 0xffff));
                family[static_cast<std::size_t>(row * columns + column)] = member;
            }
        }
    }
}

void cSTETerrain::ReadMiscTexturesForTerrainTypes(ITerrainIniFile& ini) {
    // 0x10073b602: the cliff and beach texture ids of terrain type 0, plus the fixed deep-water ids.
    const cRZString section(kTerrainTextureMapSection);
    cRZString value;
    if (ini.ReadEntry(section, cRZString("HighCliff"), value)) {
        HighCliffTextureId() = ParseTerrainUint(value.ToChar());
        AddToTerrainTypeTextureList(0, HighCliffTextureId());
    }
    if (ini.ReadEntry(section, cRZString("LowCliff"), value)) {
        LowCliffTextureId() = ParseTerrainUint(value.ToChar());
        AddToTerrainTypeTextureList(0, LowCliffTextureId());
    }
    if (ini.ReadEntry(section, cRZString("Beach"), value)) {
        BeachTextureId() = ParseTerrainUint(value.ToChar());
        AddToTerrainTypeTextureList(0, BeachTextureId());
    }
    AddToTerrainTypeTextureList(0, 0x13);      // deep sea bed (below kMinWaterDepthForDeepSeaBedTexture)
    AddToTerrainTypeTextureList(0, 0x14);      // shallow sea bed
    AddToTerrainTypeTextureList(0, 0xe0);
    AddToTerrainTypeTextureList(0, 0x11);
    AddToTerrainTypeTextureList(0, 0x10);
    AddToTerrainTypeTextureList(0, 0x15);
}

void cSTETerrain::ReadTerrainIniFile() {
    // 0x10073bb96: the resource {0x8a5971c5, 0, 0xaa597172} as an ini. The temperature/moisture table of the
    // first terrain type, then the texture ids, then the wind-erosion and hydrology tunables. Every entry is
    // optional (a missing key leaves the __data default alone).
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    ITerrainIniFile* ini = host->OpenTerrainIni();
    if (!ini) return;
    ini->EnumEntries(cRZString(kTerrainTextureMapSection), &AddTemperatureAndMoistureToSurfaceCoverEntry, nullptr);
    ReadMiscTexturesForTerrainTypes(*ini);

    auto readFloat = [ini](const char* section, const char* key, float& target) {
        cRZString value;
        if (ini->ReadEntry(cRZString(section), cRZString(key), value))
            target = static_cast<float>(ParseTerrainFloat(value.ToChar()));
    };
    auto readInt = [ini](const char* section, const char* key, std::int32_t& target) {
        cRZString value;
        if (ini->ReadEntry(cRZString(section), cRZString(key), value))
            target = static_cast<std::int32_t>(ParseTerrainUint(value.ToChar()));
    };
    auto readByte = [ini](const char* section, const char* key, std::uint8_t& target) {
        cRZString value;
        if (ini->ReadEntry(cRZString(section), cRZString(key), value))
            target = static_cast<std::uint8_t>(ParseTerrainUint(value.ToChar()));
    };

    TerrainStatics& statics = Statics();
    readFloat(kMiscTunablesSection, "RaiseLowerAmount", statics.vertexHeightRaiseLowerAmount);
    {
        cRZString value;
        if (ini->ReadEntry(cRZString(kMiscTunablesSection), cRZString("EnableSimulation"), value))
            statics.simulationEnabled = ParseTerrainUint(value.ToChar()) != 0;
    }
    readInt(kWindErosionSection, "NumIterationsPerUserClick", statics.numWindErosionIterationsPerUserClick);
    readInt(kWindErosionSection, "NumIterationsPerSimMonth", statics.numWindErosionIterationsPerSimMonth);
    {
        cRZString value;
        if (ini->ReadEntry(cRZString(kWindErosionSection), cRZString("TalusAngle"), value)) {
            statics.talusAngle = static_cast<float>(ParseTerrainUint(value.ToChar())) * 0.017453292f;
        }
    }
    // The two thresholds are recomputed from the talus angle whether or not the entry was present.
    const float tangent = std::tan(statics.talusAngle);
    statics.crossHeightDifferenceThreshold = tangent * 16.0f;
    statics.diagonalHeightDifferenceThreshold = statics.crossHeightDifferenceThreshold * 1.4142135f;
    readFloat(kWindErosionSection, "ErosionFactor", statics.windErosionFactor);
    readInt(kHydrologySection, "NumWaterErosionIterationsPerPlayerClick",
            statics.numWaterErosionIterationsPerPlayerClick);
    readInt(kHydrologySection, "NumFlowStepsForGlobalWaterErosion", statics.numFlowStepsForGlobalWaterErosion);
    readFloat(kHydrologySection, "SurfaceWaterThreshold", statics.surfaceWaterThreshold);
    readFloat(kHydrologySection, "RainFallFactor", statics.rainFallFactor);
    readFloat(kHydrologySection, "WaterSourceCapacity", statics.waterSourceCapacity);
    readFloat(kHydrologySection, "WaterSinkCapacity", statics.waterSinkCapacity);
    readFloat(kHydrologySection, "MaxSedimentPerUnitWater", statics.maxSedimentPerUnitWater);
    readFloat(kHydrologySection, "MinErosionRate", statics.minHydraulicErosionRate);
    readFloat(kHydrologySection, "MaxErosionRate", statics.maxHydraulicErosionRate);
    {
        cRZString value;   // the ramp endpoints are read as degrees and converted here
        if (ini->ReadEntry(cRZString(kHydrologySection), cRZString("HydrErosionRateRampBegin"), value))
            statics.hydraulicErosionRateRampBegin =
                static_cast<float>(std::tan(ParseTerrainFloat(value.ToChar()) * 0.017453292)) * 16.0f;
        if (ini->ReadEntry(cRZString(kHydrologySection), cRZString("HydrErosionRateRampEnd"), value))
            statics.hydraulicErosionRateRampEnd =
                static_cast<float>(std::tan(ParseTerrainFloat(value.ToChar()) * 0.017453292)) * 16.0f;
    }
    readFloat(kHydrologySection, "DepositRate", statics.hydraulicDepositRate);
    readFloat(kHydrologySection, "SedimentSpreadRateUnderSea", statics.sedimentSpreadRateUnderSea);
    if (1.0f < statics.sedimentSpreadRateUnderSea) statics.sedimentSpreadRateUnderSea = 1.0f;
    readFloat(kHydrologySection, "DepositRateUnderSea", statics.hydraulicDepositRateUnderSea);
    readFloat(kHydrologySection, "SedimentInfluenceOnWaterColor", statics.sedimentInfluenceOnWaterColor);
    readFloat(kHydrologySection, "MaxSedimentAlpha", statics.maxSedimentAlpha);
    if (1.0f < statics.maxSedimentAlpha) statics.maxSedimentAlpha = 1.0f;
    readByte(kHydrologySection, "SedimentColorRed", statics.sedimentColorRed);
    readByte(kHydrologySection, "SedimentColorGreen", statics.sedimentColorGreen);
    readByte(kHydrologySection, "SedimentColorBlue", statics.sedimentColorBlue);
    delete ini;
}

void cSTETerrain::ReadTuningParameters() {
    // 0x10073d95e: the terrain-tuning exemplar {0x6534284a, 0x88cd66e9, 1}'s property holder. Every value is
    // optional. The response curve 0x48cd7c00 (kEdgeReconcilliationSmoothingRadiusVsHeightDiff) is decoded
    // as interleaved (input, output) floats, the same layout the surface-cover curves use.
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    const cGZPersistResourceKey key{0x6534284a, 0x88cd66e9, 1};
    cISCPropertyHolder* holder = host->GetPropertyHolder(key);
    if (!holder) return;
    TerrainStatics& statics = Statics();
    const cISCPropertyHolder* h = holder;
    ReadPropertyUintVector(h, 0x89a312c2, statics.textureRandomizationGridSize);
    DetermineTextureFamilyMembersForGridCells();
    ReadPropertyFloatVector(h, 0x48cd7b20, statics.maxNormalTerrainHeight);
    ReadPropertyFloat(h, 0x48cd7b26, statics.maxTerrainHeight);
    ReadPropertyFloat(h, 0x48cd7b27, statics.minTerrainHeight);
    ReadPropertyFloat(h, 0x48cd7b21, statics.seaLevel);
    if (statics.seaLevel < statics.minTerrainHeight) statics.seaLevel = statics.minTerrainHeight;
    ReadPropertyFloat(h, 0x48cd7b2a, statics.defaultTerrainHeight);
    if (statics.defaultTerrainHeight < statics.minTerrainHeight)
        statics.defaultTerrainHeight = statics.minTerrainHeight;
    ReadPropertyFloatVector(h, 0x48cd7b22, statics.minRegularHeightFactor);
    ReadPropertyFloatVector(h, 0x48cd7b23, statics.maxRegularHeightFactor);
    ReadPropertyFloatVector(h, 0x48cd7b24, statics.minMountainHeightFactor);
    ReadPropertyFloatVector(h, 0x48cd7b25, statics.maxMountainHeightFactor);
    ReadPropertyFloat(h, 0x48cd7b28, statics.imageImportScaleFactor);
    ReadPropertyFloat(h, 0x48cd7b30, statics.temperatureRangeFactor);
    ReadPropertyFloat(h, 0x48cd7b31, statics.waterProximityRangeFactor);
    ReadPropertyInt32(h, 0x48cd7b40, statics.numInitialErosionPasses);
    ReadPropertyInt32(h, 0x48cd7b41, statics.numHydraulicIterationsPerInitialErosionPass);
    ReadPropertyInt32(h, 0x48cd7b42, statics.numWindIterationsPerInitialErosionPass);
    ReadPropertyFloatVector(h, 0x48cd7b43, statics.seaLevelChangeDueToWaterSlider);
    ReadPropertyInt32(h, 0x48cd7b50, statics.minCellsInUsefulFlatArea);
    ReadPropertyFloat(h, 0x48cd7b51, statics.maxHeightChangeToFlattenFactor);
    ReadPropertyFloat(h, 0x48cd7b52, statics.maxTerraceAltitudeFactor);
    ReadPropertyFloat(h, 0x48cd7b53, statics.minTerraceAltitudeFactor);
    ReadPropertyInt32(h, 0x48cd7b54, statics.autoFlatAreaEdgeSmootheningRadius);
    ReadPropertyFloat(h, 0x69ac5640, statics.minWaterDepthForDeepSeaBedTexture);
    ReadPropertyFloat(h, 0x69ac5641, statics.maxWaterDepthForDeepSeaBedTexture);
    ReadPropertyUint32(h, 0x48cd7b60, statics.minShoreCliffSize);
    ReadPropertyUint32(h, 0x48cd7b64, statics.maxShoreCliffSize);
    ReadPropertyFloat(h, 0x48cd7b65, statics.maxNormalYForCliff);
    ReadPropertyFloat(h, 0x48cd7b61, statics.minShoreCliffCandidateHeightSum);
    statics.minShoreCliffCandidateHeightSum *= 6.0f;
    ReadPropertyFloat(h, 0x48cd7b62, statics.minShoreCliffHeight);
    ReadPropertyFloat(h, 0x48cd7b63, statics.maxShoreCliffHeight);
    ReadPropertyFloat(h, 0x48cd7b70, statics.minSeaLevel);
    ReadPropertyFloat(h, 0x48cd7b71, statics.maxSeaLevel);
    ReadPropertyFloat(h, 0x48cd7b72, statics.seaRaiseLowerAmount);
    ReadPropertyFloat(h, 0x48cd7b73, statics.terrainRaiseLowerAmount);
    ReadPropertyUint8(h, 0xa9c283d0, statics.maxBeachWidth);
    ReadPropertyFloat(h, 0xa9c283d1, statics.maxBeachAltitude);
    {
        std::uint8_t flag = 0;
        if (ReadPropertyUint8(h, 0x48cd7b80, flag)) statics.autoFlattenUponCreation = flag != 0;
        if (ReadPropertyUint8(h, 0x48cd7b81, flag)) statics.shoreCliffsUponCreation = flag != 0;
    }
    if (ReadPropertyFloatVector(h, 0x48cd7ba0, statics.typicalBrushAngleIncrement))
        for (float& angle : statics.typicalBrushAngleIncrement) angle *= 0.017453292f;
    ReadPropertyFloat(h, 0x89861864, statics.volcanicAshLifeTime);
    statics.volcanicAshUpdateInterval = statics.volcanicAshLifeTime / 20.0f;
    ReadPropertyUintVector(h, 0x48cd7bf1, statics.numFlowStepsPerWaterErosionBrushApplication);
    ReadPropertyInt32(h, 0x48cd7bf3, statics.numSmoothingIterationsAfterWaterErosion);
    ReadPropertyInt32(h, 0x48cd7c01, statics.edgeReconciliationSmoothingIterations);
    ReadPropertyFloat(h, 0x48cd7c02, statics.edgeReconciliationHeightDifferenceThreshold);
    {
        std::vector<float> curve;
        if (ReadPropertyFloatVector(h, 0x48cd7c00, curve))
            BuildCurvePoints(curve.data(), static_cast<std::uint32_t>(curve.size()),
                             statics.edgeReconciliationSmoothingRadiusCurve);
    }
    holder->Release();
}

void cSTETerrain::ReleaseOne(cIGZUnknown*& object) {
    if (!object) return;
    object->Release();
    object = nullptr;
}

void cSTETerrain::ReleaseServices() {
    // Shutdown's release chain: +0x138, +0x148, +0x140, then the three overlay managers (+0x130, +0x128,
    // +0x120), the view (+0x110, shut down first), the surface water (+0x38) and the city (+0x28).
    ReleaseOne(effectsManager_);
    ReleaseOne(floraSimulator_);
    ReleaseOne(landSimulator_);
    for (int i = 2; i >= 0; --i) ReleaseOne(overlayManagers_[i]);
    if (view_) {
        view_->Shutdown();
        view_ = nullptr;             // the view is host-owned in the port (the original Releases it)
        displayEnabled_ = false;
    }
    ReleaseOne(surfaceWater_);
    ReleaseOne(city_);
}

bool cSTETerrain::Init() {
    // 0x100739cb0: take the city and the surface water, size the grid from the city, grab the simulators,
    // register the ten messages and the seven script commands, then hook into the simulator / ATM.
    if (!StaticInit()) return false;                 // the original only calls it when sbInited is clear
    if (initialized_) Shutdown();                    // vslot 0x20
    ITerrainHost* host = TerrainHost();
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "cSTETerrain::Init host %p\n", static_cast<void*>(host));
    if (!host) return false;
    initialized_ = true;
    if (!city_) {
        city_ = host->City();
        if (city_) city_->AddRef();
    }
    if (!surfaceWater_) {
        surfaceWater_ = host->SurfaceWater();
        if (surfaceWater_) surfaceWater_->AddRef();
    }
    LayOutGrid(host->CityCellCountX(), host->CityCellCountZ());
    cityTerrainType_ = host->CityTerrainType();      // +0x70 (spCity slot 0x2b8)
    // +0x98: the terrain type's normal-height limit. (The original indexes the table without a mask.)
    maxNormalHeight_ = Statics().maxNormalTerrainHeight.empty()
                           ? Statics().maxTerrainHeight
                           : Statics().maxNormalTerrainHeight[static_cast<std::size_t>(cityTerrainType_) &
                                                              (Statics().maxNormalTerrainHeight.size() - 1)];
    // +0xc4: the water proximity radius, clamped to a byte.
    const float range = Statics().waterProximityRangeFactor * 64.0f;
    std::int32_t factor = static_cast<std::int32_t>(range);
    if (factor < 1) factor = 0;
    waterProximityMax_ = static_cast<std::uint8_t>(factor < 0x100 ? factor : 0xff);
    if (!landSimulator_) {
        landSimulator_ = host->CityLandSimulator();          // spCity 0x230
        if (landSimulator_) landSimulator_->AddRef();
    }
    if (!floraSimulator_) {
        floraSimulator_ = host->CityFloraSimulator();        // spCity 0x1d8
        if (floraSimulator_) floraSimulator_->AddRef();
    }
    effectsManager_ = host->CityEffectsManager();            // spCity 0x2a8
    if (effectsManager_) effectsManager_->AddRef();
    // cSTETriangleGridIterator::Init(0x10) at +0x1a0: the iterator class is not recovered (TERRAIN-1).
    tuning_.seaLevel = Statics().seaLevel;                   // +0x9c = kSeaLevel
    if (!loadOk_) {   // a loaded terrain keeps the range it was saved with
        minHeight_ = 0.0f;                                   // +0x90 / +0x94
        maxHeight_ = 0.0f;
    }
    emptyRect_ = kEmptyTerrainRect;                          // +0xcc
    InitBrushParameters();
    if (host->Messages()) {
        for (std::uint32_t type : kTerrainMessageTypes) host->AddTerrainNotification(this, type);
    }
    if (cIGZCommandServer* commands = host->CommandServer()) {
        // RZGetFramework()->GetSystemService(0xeb903a32, 0xcb90253a): the seven commands of the terrain.
        for (const TerrainScriptCommand& command : kTerrainScriptCommands)
            commands->RegisterCommand(command.id, command.name, nullptr);
    }
    simulator_ = host->Simulator();                          // SL::spSimulator
    if (simulator_) {
        simulator_->AddRef();
        host->RegisterWithSimulator(this);                   // vslot 0x98
    }
    host->AddToAutomataManager(this);                        // SL::ATM() vslot 0x28
    if (!createdFromImage_ && !loadOk_) {   // +0x80 is set by Create and by a successful Load
        host->SurfaceWaterInit();                            // spSurfaceWater vslot 0x18
        Create();                                            // 0x100735cfc (height image import + erosion)
    }
    return true;
}

bool cSTETerrain::Shutdown() {
    // 0x10073e5c6: the reverse of Init. The original reports success whether or not the model was inited.
    if (!initialized_) return true;
    initialized_ = false;
    if (ITerrainHost* host = TerrainHost()) {
        host->RemoveFromAutomataManager(this);               // SL::ATM() vslot 0x30
        if (simulator_) {
            host->UnregisterFromSimulator(this);             // vslot 0xa8
            simulator_->Release();
            simulator_ = nullptr;
        }
        // (sic) Shutdown removes 0xc4a6af3, which Init does not add, and leaves 0xc4a6af6 subscribed.
        for (std::uint32_t type : kTerrainShutdownMessageTypes) host->RemoveTerrainNotification(this, type);
        if (cIGZCommandServer* commands = host->CommandServer()) {
            for (const TerrainScriptCommand& command : kTerrainScriptCommands) commands->UnregisterCommand(command.id);
        }
    }
    ReleaseServices();
    tempWaterLevels_.clear();                                // operator delete[] at +0x100
    vertices_.clear();                                       // operator delete[] at +0x88
    return true;
}

void cSTETerrain::DoPostCityInit() {
    // 0x10073847e: after the city is up the view is created and initialised, the whole grid is re-lit and
    // re-textured, and the view's three overlay managers are taken (they are only released in Shutdown).
    if (postCityInitDone_) return;
    postCityInitDone_ = true;
    ITerrainHost* host = TerrainHost();
    if (host) zoom_ = host->ViewZoomLevel();                 // SL::spViewUtilities +0x14
    if (!view_ && host) view_ = host->CreateTerrainView();   // GZCOM 0xc9b84e10 / 0x6771477d
    if (view_) view_->Init(this);                            // cSTETerrainView3D::Init @0x10074607c
    displayEnabled_ = true;                                  // +0x118
    if (host && landSimulator_) host->ResetLandSimulator(landSimulator_);   // +0x140 vslot 0x28
    CalculateNormalsAndAssignCliffTextures(gridRect_);      // 0x100734dc8
    AssignSurfaceTexturesAndColors(gridRect_);              // 0x100733bc8
    if (view_) {
        overlayManagers_[0] = view_->OverlayManager(0);      // view +0xa8
        overlayManagers_[1] = view_->OverlayManager(1);      // view +0xb0
        overlayManagers_[2] = view_->OverlayManager(2);      // view vslot 0xb0 with 0
        for (cIGZUnknown*& manager : overlayManagers_)
            if (manager) manager->AddRef();
        waveSystem_ = view_->WaveSystem();                   // +0x128 (the same object the original caches)
    }
}

namespace {
// The message's cIGZMessage2Standard payload interface, released again (the original reads the fields of
// the object it already holds; the port has to ask for the interface).
class StandardMessageRef {
public:
    explicit StandardMessageRef(cIGZMessage2* message) {
        void* raw = nullptr;
        if (message && message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw))
            standard_ = static_cast<cIGZMessage2Standard*>(raw);
    }
    ~StandardMessageRef() {
        if (standard_) standard_->Release();
    }
    StandardMessageRef(const StandardMessageRef&) = delete;
    StandardMessageRef& operator=(const StandardMessageRef&) = delete;
    cIGZMessage2Standard* operator->() const { return standard_; }
    explicit operator bool() const { return standard_ != nullptr; }

private:
    cIGZMessage2Standard* standard_ = nullptr;
};
}  // namespace

bool cSTETerrain::DoMessage(cIGZMessage2* message) {
    // 0x100737d3e. Every branch returns true: the terrain never fails a message. The script commands
    // (0xc4a6af0..6) carry the input parameter set in data 1 and the output set in data 2.
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type == 0x86ad10ee) {              // animation tick (elapsed ms in data 2)
        StandardMessageRef standard(message);
        if (standard) DoAnimationTick(static_cast<std::uint32_t>(standard->GetData2()));
        return true;
    }
    if (type == 0xa6b79602) {              // zoom changed: data 2 is the new zoom level
        StandardMessageRef standard(message);
        const auto* zoom = standard ? static_cast<const std::int32_t*>(standard->GetVoid2()) : nullptr;
        if (!zoom || *zoom == static_cast<std::int32_t>(zoom_)) return true;
        zoom_ = static_cast<std::uint32_t>(*zoom);
        TerrainRect r{0, 0, static_cast<std::int32_t>(cellsX_), static_cast<std::int32_t>(cellsZ_)};
        AssignSurfaceTexturesAndColors(r);
        return true;
    }
    if (type == 0x26d31ec1) {              // the city is up
        DoPostCityInit();
        return true;
    }
    if (type == 0x470bd214) {              // erosion (the style is in data 1)
        StandardMessageRef standard(message);
        if (standard) DoErosion(static_cast<std::uint32_t>(standard->GetData1()), false);
        TerrainRect r{0, 0, static_cast<std::int32_t>(cellsX_), static_cast<std::int32_t>(cellsZ_)};
        RedisplayTerrain(true, true, r, 1);
        return true;
    }
    if (type == 0x66956816 || type == 0x6716a4e6) return true;
    if (type == 0x2a612643) {
        ITerrainHost* host = TerrainHost();
        if (!host) return true;
        if (host->CityLoadInProgress()) {          // RZ::WM() vslot 0x150: busy, retry later
            if (cIGZMessageTarget2* target = host->TerrainMessageTarget())
                PostMessageToTarget(0x2a612643, target);
            return true;
        }
        ReconcileEdgesWithNeighboringCities(false, true, true);   // vslot 0x2c8
        return true;
    }
    if (type < 0xc4a6af0) return true;
    StandardMessageRef standard(message);
    if (!standard) return true;
    auto* params = static_cast<cIGZCommandParameterSet*>(standard->GetVoid1());
    auto* result = static_cast<cIGZCommandParameterSet*>(standard->GetVoid2());
    if (!params || !result) return true;
    switch (type) {
    case 0xc4a6af0: {                      // GetAltitude(x, z) -> float
        const float x = (*params)[0].GetValFloat32();
        const float z = (*params)[1].GetValFloat32();
        if (!LocationIsInBounds(x, z)) {
            result->SetStatusParameterValue(2);
            return true;
        }
        result->SetParameter(0, cRZVariant(GetAltitude(x, z)));
        break;
    }
    case 0xc4a6af1: {                      // SetAltitude(x, z, altitude)
        const float x = (*params)[0].GetValFloat32();
        const float z = (*params)[1].GetValFloat32();
        if (!LocationIsInBounds(x, z)) {
            result->SetStatusParameterValue(2);
            return true;
        }
        const float altitude = (*params)[2].GetValFloat32();
        // (sic) the original passes the *x* coordinate as the row too.
        const auto cell = static_cast<std::int32_t>(x * 0.0625f);
        SetAltitudeAtVertex(cell, cell, altitude);
        break;
    }
    case 0xc4a6af2: {                      // GetCellAltitude(cellX, cellZ) -> float
        const std::uint32_t x = (*params)[0].GetValUint32();
        const std::uint32_t z = (*params)[1].GetValUint32();
        if (vertsX_ <= x || vertsZ_ <= z) {
            result->SetStatusParameterValue(2);
            return true;
        }
        result->SetParameter(0, cRZVariant(GetAltitudeAtVertex(static_cast<std::int32_t>(x),
                                                              static_cast<std::int32_t>(z))));
        break;
    }
    case 0xc4a6af3: {                      // SetCellAltitude(cellX, cellZ[, cellX2, cellZ2], altitude)
        const std::uint32_t x = (*params)[0].GetValUint32();
        const std::uint32_t z = (*params)[1].GetValUint32();
        std::uint32_t x2 = x, z2 = z;
        std::uint32_t altitudeIndex = 2;
        if (3 < params->GetParameterCount()) {
            x2 = (*params)[2].GetValUint32();
            z2 = (*params)[3].GetValUint32();
            altitudeIndex = 4;
        }
        if (vertsX_ <= x || vertsX_ <= x2 || vertsZ_ <= z || vertsZ_ <= z2) {
            result->SetStatusParameterValue(2);
            return true;
        }
        const float altitude = (*params)[altitudeIndex].GetValFloat32();
        TerrainRect r{static_cast<std::int32_t>(x), static_cast<std::int32_t>(z),
                      static_cast<std::int32_t>(x2), static_cast<std::int32_t>(z2)};
        SetAltitude(r, altitude, true, 2);
        break;
    }
    case 0xc4a6af4:                        // GetSeaLevel() -> float
        result->SetParameter(0, cRZVariant(GetSeaLevel()));
        break;
    case 0xc4a6af5: {                      // SetSeaLevel(level)
        const float level = (*params)[0].GetValFloat32();
        if (!SetSeaLevel(level)) {
            result->SetStatusParameterValue(2);
            return true;
        }
        break;
    }
    case 0xc4a6af6: {                      // ApplyTerrainBrush(brushType, x1, z1, x2, z2[, strength[, radius]])
        if (params->GetParameterCount() < 5) {
            result->SetStatusParameterValue(2);
            return true;
        }
        TerrainBrushParameters brush;
        brush.previousX = (*params)[1].GetValFloat32();
        brush.previousZ = (*params)[2].GetValFloat32();
        brush.currentX = (*params)[3].GetValFloat32();
        brush.currentZ = (*params)[4].GetValFloat32();
        if (!LocationIsInBounds(brush.previousX, brush.previousZ) ||
            !LocationIsInBounds(brush.currentX, brush.currentZ)) {
            result->SetStatusParameterValue(2);
            return true;
        }
        const std::uint32_t brushType = (*params)[0].GetValUint32();
        brush.strengthScale = params->GetParameterCount() > 5 ? (*params)[5].GetValFloat32() : 1.0f;
        brush.radiusScale = params->GetParameterCount() > 6 ? (*params)[6].GetValFloat32() : 1.0f;
        brush.redisplayFlags = 2;
        brush.levelAltitude = GetAltitude(brush.currentX, brush.currentZ);
        if (!ApplyBrush(brushType, brush)) {
            result->SetStatusParameterValue(2);
            return true;
        }
        break;
    }
    default:
        break;
    }
    return true;
}

void cSTETerrain::DoAnimationTick(std::uint32_t elapsedMs) {
    // 0x10073759e: the first two ticks arm the display (via the region-changed message or a re-post of
    // the reconcile message), then the flood / ash / water-proximity subsystems are stepped.
    if (view_ && static_cast<std::int32_t>(animationTick_) < 2) {
        animationTick_ += 1;
        if (animationTick_ == 2) {
            ITerrainHost* host = TerrainHost();
            if (host && !host->PreferencesFlag4ec()) {
                PostMessageA(0xca63d517, 0, 0, 0, 0);        // kRegionalCityMessageCityChanged
            }
            // else: re-post 0x2a612643 to the terrain itself (the message-target adapter, TERRAIN-1).
        }
    }
    if (floodActive_) DoFloodFlowStep();                          // +0xf4
    TickVolcanicAshRegions(elapsedMs);                            // 0x1007379be
    const std::uint32_t next = elapsedMs + waterProximityTick_;   // +0xc8
    waterProximityTick_ = next;
    const bool emptyRect = IsEmptyTerrainRect(emptyRect_);
    if (9999999 < next && !emptyRect) ComputeWaterProximityValues(gridRect_);   // the 10 s sweep
}

void cSTETerrain::RedisplayTerrain(bool adjustAltitudes, bool redisplayWater, const TerrainPoint& a,
                                   const TerrainPoint& b, std::uint32_t flags) {
    // 0x10073de82: sort the corner pair, widen by one cell and clamp to the grid, then the rect variant.
    const TerrainPoint* low = &a;
    const TerrainPoint* high = &b;
    if (b.x < a.x) std::swap(low, high);
    std::int32_t left = static_cast<std::int32_t>(low->x);
    std::int32_t top = static_cast<std::int32_t>(a.z);
    std::int32_t bottom = static_cast<std::int32_t>(b.z);
    if (b.z < a.z) {
        top = static_cast<std::int32_t>(b.z);
        bottom = static_cast<std::int32_t>(a.z);
    }
    std::uint32_t right = static_cast<std::uint32_t>(static_cast<std::int32_t>(high->x) + 1);
    if (cellsX_ < right) right = cellsX_;
    std::uint32_t bottomCell = static_cast<std::uint32_t>(bottom + 1);
    if (cellsZ_ < bottomCell) bottomCell = cellsZ_;
    TerrainRect r{left, top, static_cast<std::int32_t>(right), static_cast<std::int32_t>(bottomCell)};
    RedisplayTerrain(adjustAltitudes, redisplayWater, r, flags);
}

void cSTETerrain::RedisplayTerrain(bool adjustAltitudes, bool redisplayWater, const TerrainRect& r,
                                   std::uint32_t flags) {
    // 0x10073defa.
    if (!displayEnabled_) return;
    TerrainRect surfaceRect = r;
    if (adjustAltitudes) {
        FindMinAndMaxHeights();
        TerrainRect normalsRect{std::max(r.left - 1, 0), std::max(r.top - 1, 0), r.right + 1, r.bottom + 1};
        if (static_cast<std::int32_t>(cellsX_) <= r.right) normalsRect.right = static_cast<std::int32_t>(cellsX_);
        if (static_cast<std::int32_t>(cellsZ_) <= r.bottom) normalsRect.bottom = static_cast<std::int32_t>(cellsZ_);
        CalculateNormalsAndAssignCliffTextures(normalsRect);   // 0x100734dc8
        if (view_) view_->SetLightingUpdateRect(&r);           // view vslot 0x88
        const std::uint32_t packed0 = static_cast<std::uint32_t>(r.left) * 0x10000u + static_cast<std::uint32_t>(r.right);
        const std::uint32_t packed1 = static_cast<std::uint32_t>(r.top) * 0x10000u + static_cast<std::uint32_t>(r.bottom);
        if (!accumulating_) {
            AssignSurfaceTexturesAndColors(surfaceRect);
            UnionRect(surfaceRect, surfaceRect, normalsRect);
        } else {
            UnionRect(accumulateRect_, accumulateRect_, r);   // +0xe4
            surfaceRect = normalsRect;
        }
        PostMessageA(0x278e6003, packed0, packed1, 0, flags);
    }
    if (redisplayWater) {
        const std::uint32_t packed0 = static_cast<std::uint32_t>(r.left) * 0x10000u + static_cast<std::uint32_t>(r.right);
        const std::uint32_t packed1 = static_cast<std::uint32_t>(r.top) * 0x10000u + static_cast<std::uint32_t>(r.bottom);
        PostMessageA(0x678e6e3a, packed0, packed1, 0, flags);
    }
    float lo = 0.0f, hi = 0.0f;
    ComputeMinAndMaxAltitudeForTerrainRedisplay(surfaceRect, lo, hi);
    const std::uint32_t range0 = static_cast<std::uint32_t>(surfaceRect.left) * 0x10000u +
                                 static_cast<std::uint32_t>(surfaceRect.right);
    const std::uint32_t range1 = static_cast<std::uint32_t>(surfaceRect.top) * 0x10000u +
                                 static_cast<std::uint32_t>(surfaceRect.bottom);
    // (sic) the pair is packed like the rectangles: the minimum in the high half, the maximum in the low.
    const std::uint32_t packed = static_cast<std::uint32_t>(static_cast<std::int32_t>(hi)) +
                                 static_cast<std::uint32_t>(static_cast<std::int32_t>(lo)) * 0x10000u;
    PostMessageA(0x870bc918, range0, range1, packed, flags);
}

void cSTETerrain::ComputeMinAndMaxAltitudeForTerrainRedisplay(const TerrainRect& r, float& lo, float& hi) const {
    // 0x100735a58: the altitude range the view has to cover. The whole-grid rect answers from the cached
    // min/max; the city-edge rects additionally force the minimum to sea level when the edge is on screen.
    if (r.left == gridRect_.left && r.top == gridRect_.top && r.right == gridRect_.right &&
        r.bottom == gridRect_.bottom) {
        lo = minHeight_;                                       // +0x90
        hi = maxHeight_;                                       // +0x94
    } else {
        FindMinAndMaxHeights(r, lo, hi);
    }
    bool forceMinToZero = false;
    if (ITerrainHost* host = TerrainHost()) {
        // edge flags: 0 = north (top), 1 = east (right), 2 = south (bottom), 3 = west (left).
        std::uint8_t edge[4];
        edge[0] = r.top < 1 ? 1 : 0;
        edge[1] = cellsX_ <= static_cast<std::uint32_t>(r.right) ? 1 : 0;
        edge[2] = cellsZ_ <= static_cast<std::uint32_t>(r.bottom) ? 1 : 0;
        edge[3] = r.left < 1 ? 1 : 0;
        std::int32_t horizontal = 0, vertical = 0;
        host->VisibleEdges(horizontal, vertical);             // cSC4ViewUtilities::GetVisibleEdges
        if ((edge[horizontal & 3] || edge[vertical & 3])) {
            lo = 0.0f;
            forceMinToZero = true;
        }
    }
    // view +0x94 / +0x98. (sic) with no view the original falls back to +FLT_MAX / -FLT_MAX, which makes the
    // clamps below produce that same garbage range: the function assumes a display exists (quirk kept).
    const float viewTop = view_ ? view_->MaxDisplayAltitude() : 3.4028235e+38f;      // view +0x94
    const float viewBottom = view_ ? view_->MinDisplayAltitude() : -3.4028235e+38f;  // view +0x98
    const bool topSame = viewTop == maxHeight_;
    const bool topLess = viewTop < maxHeight_;
    if (!topLess && !topSame) hi = viewTop;
    if (!forceMinToZero) {
        if (minHeight_ <= viewBottom) {
            const float lowered = lo - 20.0f;
            lo = (viewBottom <= lowered) ? lowered : viewBottom;
        } else {
            lo = viewBottom;
        }
    }
    if (topLess || topSame) {
        const float raised = hi + 20.0f;
        const float limit = (viewTop <= maxHeight_) ? maxHeight_ : viewTop;
        hi = (limit < raised) ? limit : raised;
    }
    if (lo <= 0.0f && lo != 0.0f) lo = 0.0f;                   // negative clamps to 0 (-0.0 is kept)
    if (Statics().maxTerrainHeight < hi) hi = Statics().maxTerrainHeight;
    if (hi < tuning_.seaLevel) hi = tuning_.seaLevel;          // never below the sea level (+0x9c)
}

void cSTETerrain::SetTerrainType(std::int32_t type) {
    // 0x10073e4e6: announce the new type, re-texture the whole city and redisplay it without a second
    // water pass.
    if (terrainType_ == type) return;
    terrainType_ = type;
    PostMessageA(0xc922eb91, static_cast<std::uint64_t>(static_cast<std::uint32_t>(type)), 0, 0, 0);
    TerrainRect r{0, 0, static_cast<std::int32_t>(cellsX_), static_cast<std::int32_t>(cellsZ_)};
    AssignSurfaceTexturesAndColors(r);
    RedisplayTerrain(false, false, r, 0);
}

// ================================================================================ display state

void cSTETerrain::EnsureMinAllowedHeight() {
    // 0x1007361a0: when the cached minimum dropped below the 5 m floor, raise every vertex to the floor.
    if (!(minHeight_ <= Statics().minTerrainHeight) || minHeight_ == Statics().minTerrainHeight) return;
    for (Vertex& vertex : vertices_)
        if (vertex.altitude < Statics().minTerrainHeight) vertex.altitude = Statics().minTerrainHeight;
    minHeight_ = Statics().minTerrainHeight;                            // +0x90
}

void cSTETerrain::ChangeTerrainLevel(bool raise, float amount) {
    // 0x100735746: the whole grid, one vertex at a time through the clamped raise/lower helpers.
    for (std::uint32_t z = 0; z < vertsZ_; ++z) {
        for (std::uint32_t x = 0; x < vertsX_; ++x) {
            const auto index = static_cast<std::uint32_t>(vertsX_ * z + x);
            if (raise) {
                RaiseVertex(index, amount);
            } else {
                LowerVertex(index, amount);
            }
        }
    }
    RedisplayTerrain(true, true, gridRect_, 1);                // +0x58, flags 1
}

void cSTETerrain::ResetWaterToSeaLevel(const TerrainRect& r) {
    // cSTESurfaceWater::ResetWaterToSeaLevel @ 0x10072e648: the port owns the two channels the surface
    // water keeps (depth at vertex +0x0, sediment at +0x1c) and the vertex normals the call also zeroes.
    for (std::int32_t z = r.top; z <= r.bottom; ++z)
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x);
            waterDepth_[i] = tuning_.seaLevel - vertices_[i].altitude;
            vertices_[i].normal[0] = vertices_[i].normal[1] = vertices_[i].normal[2] = 0.0f;
            sediment_[i] = 0.0f;
        }
}

void cSTETerrain::DoErosion(std::uint32_t style, bool fromImage) {
    // 0x100736216. Style 1 is wind only; style 0 runs the water erosion passes and then hands
    // *kNumSmoothingIterationsAfterWaterErosion* to DoWindErosion (sic -- not the wind click count, which
    // style 1 uses). The "from image" variants are cSC4WinRegionScreen's statics (see TerrainStatics).
    std::int32_t windIterations = Statics().numWindErosionIterationsPerUserClick;
    if (style != 1) {
        if (style != 0) return;
        const std::int32_t smoothing = fromImage ? Statics().imageImportSmoothingIterations
                                                 : Statics().numSmoothingIterationsAfterWaterErosion;
        const std::int32_t flowSteps = fromImage ? Statics().imageImportFlowSteps
                                                 : Statics().numFlowStepsForGlobalWaterErosion;
        const std::int32_t waterIterations = fromImage ? Statics().imageImportWaterErosionIterations
                                                       : Statics().numWaterErosionIterationsPerPlayerClick;
        for (std::int32_t pass = 0; pass < waterIterations; ++pass) DoWaterErosion(gridRect_, flowSteps);
        windIterations = smoothing;
    }
    DoWindErosion(windIterations);
}

void cSTETerrain::DoWindErosion(std::int32_t iterations) {
    // 0x100736baa: the talus pass. Every vertex that is more than the cross (axis) or diagonal threshold
    // above a neighbour drops by the excess times kWindErosionFactor and the neighbour rises by the same.
    if (iterations <= 0) return;
    const TerrainStatics& statics = Statics();
    const std::int32_t vertsX = static_cast<std::int32_t>(vertsX_);
    for (std::int32_t pass = 0; pass < iterations; ++pass) {
        for (std::uint32_t index = 0; index < totalVerts_; ++index) {
            const auto signedIndex = static_cast<std::int32_t>(index);
            const std::uint8_t mask = GetNeighbors(signedIndex % vertsX, signedIndex / vertsX);
            for (int dir = 0; dir < 8; ++dir) {
                if (!((mask >> dir) & 1)) continue;
                const auto neighbour = static_cast<std::uint32_t>(signedIndex + neighborOffsets_[dir]);
                const float threshold = (dir & 1) ? statics.diagonalHeightDifferenceThreshold
                                                  : statics.crossHeightDifferenceThreshold;
                const float excess = (vertices_[index].altitude - vertices_[neighbour].altitude) - threshold;
                if (!(0.0f < excess)) continue;
                const float amount = excess * statics.windErosionFactor;
                LowerVertex(index, amount);
                RaiseVertex(neighbour, amount);
            }
        }
    }
}

void cSTETerrain::DoWaterErosion(const TerrainRect& r, std::int32_t flowSteps) {
    // 0x1007335e6: the hydraulic erosion kernel.
    //  1. Fill the rectangle's water to 15 m below the sea level and clear its sediment.
    //  2. Per flow step: rain on everything above that line, then for every wet vertex move the water to
    //     the lowest neighbour -- interior vertices only, using the level difference halved and capped at
    //     the vertex's own depth -- carrying sediment up to kMaxSedimentPerUnitWater, eroding the source by
    //     the rate the slope allows and depositing the surplus back when the vertex keeps water.
    //  3. Reset the rectangle (padded by one cell) to a sea-level water surface
    //     (cSTESurfaceWater::ResetWaterToSeaLevel 0x10072e648).
    static const float kOneByRoot2 = 0.70710677f;      // the original's function-local static
    const TerrainStatics& statics = Statics();
    const float seaLine = tuning_.seaLevel - 15.0f;
    const float rateSlope = (statics.maxHydraulicErosionRate - statics.minHydraulicErosionRate) /
                            (statics.hydraulicErosionRateRampEnd - statics.hydraulicErosionRateRampBegin);
    for (std::int32_t z = r.top; z <= r.bottom; ++z)
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x);
            const float altitude = vertices_[i].altitude;
            waterDepth_[i] = altitude < seaLine ? seaLine - altitude : 0.0f;
            sediment_[i] = 0.0f;
        }
    for (std::int32_t step = 0; step < flowSteps; ++step) {
        // rain: every vertex above the sea line gains depth proportional to its altitude
        for (std::int32_t z = r.top; z <= r.bottom; ++z)
            for (std::int32_t x = r.left; x <= r.right; ++x) {
                const std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x);
                if (seaLine < vertices_[i].altitude) waterDepth_[i] += vertices_[i].altitude * statics.rainFallFactor;
            }
        for (std::int32_t z = r.top; z <= r.bottom; ++z) {
            for (std::int32_t x = r.left; x <= r.right; ++x) {
                const std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x);
                const float depth = waterDepth_[i];
                if (!(0.0f < depth)) continue;
                const float altitude = vertices_[i].altitude;
                if (!(seaLine <= altitude)) {
                    // below the sea line the water surface is pinned to it
                    waterDepth_[i] = seaLine - altitude;
                    continue;
                }
                std::int32_t target = -1;
                std::int32_t direction = -1;
                float amount = depth;
                if (x != r.right && z != r.top && z != r.bottom && x != r.left) {
                    direction = GetNeighborWithLowestWaterLevel(static_cast<std::int32_t>(i));
                    if (direction < 0) continue;          // nowhere to flow: the vertex keeps its water
                    target = static_cast<std::int32_t>(i) + neighborOffsets_[direction];
                    const auto t = static_cast<std::size_t>(target);
                    const float transfer = ((depth + altitude) -
                                            (waterDepth_[t] + vertices_[t].altitude)) * 0.5f;
                    if (transfer <= depth) amount = transfer;      // NaN keeps the full depth
                }
                if (!(0.0f < amount)) continue;
                const float capacity = statics.maxSedimentPerUnitWater * amount;
                float sediment = sediment_[i];
                float carried = 0.0f;
                if (sediment <= capacity) {
                    float slope = 0.0f;
                    if (0 <= target) {
                        slope = altitude - vertices_[static_cast<std::size_t>(target)].altitude;
                        if ((direction & 1) != 0) slope *= kOneByRoot2;
                    }
                    float erodeRate = statics.minHydraulicErosionRate;
                    if (statics.hydraulicErosionRateRampBegin < slope) {
                        erodeRate = statics.maxHydraulicErosionRate;
                        if (slope < statics.hydraulicErosionRateRampEnd)
                            erodeRate = (slope - statics.hydraulicErosionRateRampBegin) * rateSlope +
                                        statics.minHydraulicErosionRate;
                    }
                    if ((vertices_[i].flags & kVertexLocked) == 0) {
                        const float eroded = (capacity - sediment) * erodeRate;
                        vertices_[i].altitude = altitude - eroded;
                        carried = eroded + sediment_[i];
                        sediment_[i] = 0.0f;
                    }
                } else {
                    sediment_[i] = sediment - capacity;
                    carried = capacity;
                }
                float remaining = depth - amount;
                waterDepth_[i] = remaining;
                if (0 <= target && seaLine < vertices_[static_cast<std::size_t>(target)].altitude) {
                    waterDepth_[static_cast<std::size_t>(target)] += amount;
                    sediment_[static_cast<std::size_t>(target)] += carried;
                    remaining = waterDepth_[i];
                }
                if (remaining <= 0.0f) {
                    sediment_[i] = 0.0f;
                } else {
                    const float depositBase = sediment_[i];
                    if (0.0f < depositBase && (vertices_[i].flags & kVertexLocked) == 0) {
                        const float deposit = depositBase * statics.hydraulicDepositRate;
                        vertices_[i].altitude += deposit;
                        sediment_[i] = depositBase - deposit;
                    }
                }
            }
        }
    }
    TerrainRect reset;
    reset.left = std::max(r.left - 1, 0);
    reset.top = std::max(r.top - 1, 0);
    reset.right = std::min(r.right + 1, static_cast<std::int32_t>(cellsX_));
    reset.bottom = std::min(r.bottom + 1, static_cast<std::int32_t>(cellsZ_));
    ResetWaterToSeaLevel(reset);
}

void cSTETerrain::DoFloodFlowStep() {
    // 0x100737712: one step of the flood. While the step counter is below 501 the wet vertices of the
    // flood rectangle hand their water to their lowest neighbour (interior vertices only, the level
    // difference halved and capped at the vertex's own depth), then the rectangle grows by one cell. From
    // step 350 on the water also evaporates towards the depth StartFlood saved. After 500 steps the flood
    // ends and the saved depths are restored over the whole grid.
    static const std::int32_t kFloodDryStartIteration = 350;      // the original's function-local static
    const std::int32_t cellsX = static_cast<std::int32_t>(cellsX_);
    const std::int32_t cellsZ = static_cast<std::int32_t>(cellsZ_);
    const float sea = tuning_.seaLevel;
    if (static_cast<std::int32_t>(floodStep_) < 0x1f5) {
        for (std::int32_t z = floodRect_.top; z < floodRect_.bottom; ++z) {
            for (std::int32_t x = floodRect_.left; x < floodRect_.right; ++x) {
                const std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x);
                if (kFloodDryStartIteration < static_cast<std::int32_t>(floodStep_))
                    waterDepth_[i] = (waterDepth_[i] - tempWaterLevels_[i]) * -0.01f + waterDepth_[i];
                float amount = waterDepth_[i];
                std::int32_t target = -1;
                if (x != 0 && z != 0 && x != cellsX && z != cellsZ) {
                    const std::int32_t direction = GetNeighborWithLowestWaterLevel(static_cast<std::int32_t>(i));
                    if (direction < 0) continue;                  // nowhere to flow
                    target = static_cast<std::int32_t>(i) + neighborOffsets_[direction];
                    const auto t = static_cast<std::size_t>(target);
                    const float transfer = ((vertices_[i].altitude + waterDepth_[i]) -
                                            (waterDepth_[t] + vertices_[t].altitude)) * 0.5f;
                    if (transfer <= waterDepth_[i]) amount = transfer;
                }
                if (!(0.0f < amount)) continue;
                if (0 <= target) {
                    const auto t = static_cast<std::size_t>(target);
                    // only a dry-land target (above the sea, strictly) keeps the water
                    if (sea <= vertices_[t].altitude && vertices_[t].altitude != sea) waterDepth_[t] += amount;
                }
                waterDepth_[i] -= amount;
            }
        }
        PadRectangle(floodRect_, 1);                               // +0x218
        floodStep_ += 1;                                           // +0x228
    } else {
        floodActive_ = false;                                      // +0xf4
        RestoreWaterLevelsFromTempArray(gridRect_);
    }
    PostMessageA(0x678e6e3a, static_cast<std::uint32_t>(floodRect_.left * 0x10000 + floodRect_.right),
                 static_cast<std::uint32_t>(floodRect_.top * 0x10000 + floodRect_.bottom), 0, 0);
    RedisplayTerrain(false, true, floodRect_, 0);                  // vslot 0x228
}

void cSTETerrain::StartFlood() {
    // 0x10073e932: save the current depths, then flood the 20..40 cell block by 500 m of water and take
    // the first flow step. The original's loops are not clamped to the city size (they walk the literal
    // 20..40 block); the port keeps the clamps so a small test grid stays in bounds.
    SaveWaterLevelsInTempArray(gridRect_);
    floodStep_ = 0;                                            // +0x228
    floodRect_ = {20, 20, 40, 40};                             // +0x218
    const std::int32_t z0 = std::max(floodRect_.top, 0);
    const std::int32_t z1 = std::min(floodRect_.bottom, static_cast<std::int32_t>(cellsZ_));
    const std::int32_t x0 = std::max(floodRect_.left, 0);
    const std::int32_t x1 = std::min(floodRect_.right, static_cast<std::int32_t>(cellsX_));
    for (std::int32_t z = z0; z < z1; ++z)
        for (std::int32_t x = x0; x < x1; ++x)
            waterDepth_[static_cast<std::size_t>(vertsX_) * static_cast<std::uint32_t>(z) +
                        static_cast<std::uint32_t>(x)] += 500.0f;
    floodActive_ = true;                                       // +0xf4
    DoFloodFlowStep();
}

// ==================================================================================== surfaces

void cSTETerrain::UpdateMoistureValues(const TerrainRect& r) {
    // 0x100733d98: `if (+0x140 != null) (+0x140)->vslot(0x80)()`. The city's land/moisture simulator is
    // reached through the host seam; the rectangle is not passed on (the original ignores it too).
    (void)r;
    if (!landSimulator_) return;
    if (ITerrainHost* host = TerrainHost()) host->UpdateLandSimulator(landSimulator_);
}

void cSTETerrain::AddLandTypeAtVertex(std::uint32_t index, std::uint16_t textureKey, float weight) {
    // 0x100731338: three (key, weight) slots per vertex at +0x16/+0x18, +0x1a/+0x1c, +0x1e/+0x20. An empty
    // slot takes the pair, a matching key keeps the larger weight, otherwise the smallest weight is dropped
    // when the new one is larger.
    Vertex& vertex = vertices_[index];
    std::uint16_t slots[3];
    std::uint16_t weights[3];
    for (int slot = 0; slot < 3; ++slot) {
        std::memcpy(&slots[slot], &vertex.surface[6 + slot * 4], sizeof(std::uint16_t));
        std::memcpy(&weights[slot], &vertex.surface[8 + slot * 4], sizeof(std::uint16_t));
    }
    const auto quantised = static_cast<std::uint16_t>(static_cast<int>(weight * 255.0f + 1.0f));
    for (int slot = 0; slot < 3; ++slot) {
        if (slots[slot] == 0) {
            slots[slot] = textureKey;
            weights[slot] = quantised;
            break;
        }
        if (slots[slot] == textureKey) {
            if (weights[slot] < quantised) weights[slot] = quantised;
            break;
        }
        if (slot == 2) {
            int smallest = 0;
            for (int candidate = 1; candidate < 3; ++candidate)
                if (weights[candidate] < weights[smallest]) smallest = candidate;
            if (weights[smallest] < quantised) {
                slots[smallest] = textureKey;
                weights[smallest] = quantised;
            }
            break;
        }
    }
    for (int slot = 0; slot < 3; ++slot) {
        std::memcpy(&vertex.surface[6 + slot * 4], &slots[slot], sizeof(std::uint16_t));
        std::memcpy(&vertex.surface[8 + slot * 4], &weights[slot], sizeof(std::uint16_t));
    }
}

bool cSTETerrain::GetEffectIDProperty(cISCPropertyHolder* holder, std::uint32_t propertyID,
                                      std::uint32_t out[2]) {
    // 0x100738dcc: `cSCPropertyHelp::GetPropertyValue(holder, id, vector<u32>&)` and then the check that
    // the array is exactly two dwords long (the original compares the vector's byte size with 8). The port
    // reads the variant directly so this file does not add a link dependency on SCPropertyHelp.
    if (!holder) return false;
    cISCProperty* property = holder->GetProperty(propertyID);
    if (!property) return false;
    bool found = false;
    if (const cIGZVariant* value = property->GetPropertyValue()) {
        if (value->GetType() == static_cast<std::uint16_t>(cIGZVariant::kTypeUint32 | cIGZVariant::kTypeArrayFlag) &&
            value->GetCount() == 2) {
            if (const std::uint32_t* data = value->RefUint32()) {
                out[0] = data[0];
                out[1] = data[1];
                found = true;
            }
        }
    }
    // (the holder hands out the property without a reference, so it must not be released here)
    return found;
}

void cSTETerrain::ComputeWaterProximityValues(const TerrainRect& r) {
    // 0x100733c62: accumulate the dirty rect and, once it covers more than 99 cells, refill the
    // per-vertex water-proximity grid (0 = water, kMaxBeachWidth + 2 = dry) and flood fill it.
    UnionRect(emptyRect_, emptyRect_, r);
    const std::int64_t width = static_cast<std::int64_t>(emptyRect_.right) - emptyRect_.left;
    const std::int64_t height = static_cast<std::int64_t>(emptyRect_.bottom) - emptyRect_.top;
    if (width * height <= 99) return;
    emptyRect_ = kEmptyTerrainRect;                         // +0xcc reset to kEmptyRect
    waterProximityTick_ = 0;                                // +0xc8
    waterVertexCount_ = 0;                                  // +0x108
    const std::uint8_t dry = static_cast<std::uint8_t>(Statics().maxBeachWidth + 2);
    const std::uint32_t vertsX = vertsX_, vertsZ = vertsZ_;
    for (std::uint32_t z = 0; z < vertsZ; ++z) {
        std::vector<std::uint8_t>& row = surfaceCover_[z];
        const std::size_t base = static_cast<std::size_t>(z) * vertsX;
        for (std::uint32_t x = 0; x < vertsX; ++x) {
            if (waterDepth_[base + x] <= 0.0f) {
                row[x] = dry;
            } else {
                row[x] = 0;
                ++waterVertexCount_;
            }
        }
    }
    const bool noWater = waterVertexCount_ < 1;
    if (ITerrainHost* host = TerrainHost()) {
        // The source kind is -1 when there is no water at all and the threshold is kMaxBeachWidth + 1 + (0/1).
        host->FloodFillDistance(surfaceCover_, noWater ? -1 : 0,
                                Statics().maxBeachWidth + 1 + (noWater ? 1 : 0));
    }
}

void cSTETerrain::AdjustLandTypeRatios(const TerrainRect& r) {
    // 0x10073173a: when the three land-type weights of a vertex add up to more than 250, scale them down to
    // 250 (integer division), keeping the keys.
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            Vertex& vertex = vertices_[static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(x)];
            std::uint16_t slots[3];
            std::uint16_t weights[3];
            for (int slot = 0; slot < 3; ++slot) {
                std::memcpy(&slots[slot], &vertex.surface[6 + slot * 4], sizeof(std::uint16_t));
                std::memcpy(&weights[slot], &vertex.surface[8 + slot * 4], sizeof(std::uint16_t));
            }
            std::uint32_t sum = 0;
            for (int slot = 0; slot < 3 && slots[slot] != 0; ++slot) sum += weights[slot];
            if (sum <= 0xfa) continue;
            for (int slot = 0; slot < 3 && slots[slot] != 0; ++slot)
                weights[slot] = static_cast<std::uint16_t>(weights[slot] * 0xfa / sum);
            for (int slot = 0; slot < 3; ++slot)
                std::memcpy(&vertex.surface[8 + slot * 4], &weights[slot], sizeof(std::uint16_t));
        }
    }
}

void cSTETerrain::AssignSurfaceTexturesAndColors(TerrainRect& r) {
    // 0x100733bc8: water proximity, pad by the zoom's texture spread, moisture, clear the three layers, mark
    // the view's dirty rows, decide the cover, spray the volcanic ash (pending, TERRAIN-9) and scale the
    // land-type ratios down to 250.
    ComputeWaterProximityValues(r);
    PadRectangle(r, Statics().maxSurfaceTextureSpread[zoom_ & 7]);
    UpdateMoistureValues(r);
    ClearSurfaceTextures(r);
    if (displayEnabled_ && view_) {
        const std::int32_t firstRow = r.top < 1 ? 0 : r.top - 1;
        std::uint32_t lastRow = static_cast<std::uint32_t>(r.bottom) + 1;
        if (static_cast<std::uint32_t>(lastCellZ_) < lastRow) lastRow = static_cast<std::uint32_t>(lastCellZ_);
        view_->MarkTextureSpanInfoDirty(firstRow, static_cast<std::int32_t>(lastRow));   // @0x10074d7d4
    }
    DecideSurfaceCover(r);
    // SprayVolcanicAsh(r) is still pending (TERRAIN-9).
    AdjustLandTypeRatios(r);
}

void cSTETerrain::DecideSurfaceCover(const TerrainRect& r) {
    // 0x100733e48. Every vertex inside the padded rect picks a texture id from
    //   - the altitude's temperature row and the land simulator's moisture column (sSurfaceCoverMap),
    //   - the beach id when it is inside the beach band (water proximity <= kMaxBeachWidth and altitude below
    //     sea level + kMaxBeachAltitude),
    //   - the deep/shallow sea bed ids when it is under water,
    //   - the low-cliff id when MarkCliff flagged it.
    // The texture's family member jitters the id by a 0x1000 stride, and the texture's own response curve is
    // spread over its neighbourhood, adding a weighted land type to every vertex in range.
    ITerrainHost* host = TerrainHost();
    if (!host) return;                                                       // the view statics need the host
    TerrainStatics& statics = Statics();
    host->ChangeTerrainType(terrainType_);                                  // view 0x1007467ca
    host->SetUpTexturesForCurrentTerrainTypeAndZoom(static_cast<std::int32_t>(zoom_));   // 0x10074688c

    if (std::getenv("SC4_DEBUG")) {
        std::fprintf(stderr, "DecideSurfaceCover: zoom %u terrainType %d covers %zu spread %d type0-map[%d]=%u rect %d,%d,%d,%d gridSize %zu familyCols %d\n", unsigned(zoom_), int(terrainType_), SurfaceCoverTexturesSlot().size(),
                     int(statics.maxSurfaceTextureSpread[zoom_ & 7]), 0x1f * 16, unsigned(SurfaceCoverMapSlot()[0][0x1f * 16]), r.left, r.top, r.right, r.bottom, statics.textureRandomizationGridSize.size(), int(TextureGridCellCountX()[0]));
        std::size_t nonzeroMap = 0;
        for (auto v : SurfaceCoverMapSlot()[0]) nonzeroMap += v != 0;
        std::fprintf(stderr, "DecideSurfaceCover: %zu surface-cover map entries set; water depth[mid]=%.2f\n", nonzeroMap, waterDepth_[vertsX_ * 64 + 64]);
    }
    const std::int32_t spread = statics.maxSurfaceTextureSpread[zoom_ & 7];
    const std::int32_t rowStart = std::max(r.top - spread, 0);
    const std::int32_t rowEnd = std::min(r.bottom + spread, static_cast<std::int32_t>(cellsZ_));
    const std::int32_t colStart = std::max(r.left - spread, 0);
    const std::int32_t colEnd = std::min(r.right + spread, static_cast<std::int32_t>(cellsX_));
    if (rowEnd < rowStart || colEnd < colStart) return;

    const float maxBeachAltitude = tuning_.seaLevel + statics.maxBeachAltitude;
    const float heightRange = (statics.maxTerrainHeight - maxBeachAltitude) * statics.temperatureRangeFactor;
    const std::int32_t terrainType = terrainType_;
    const std::uint32_t cliffTexture = LowCliffTextureId() & 0xffff;    // (&sLowCliffTexResIds)[terrainType]
    const std::uint32_t beachTexture = BeachTextureId();                // (&sBeachTexResIds)[terrainType]
    const std::uint32_t spreadZoom = zoom_ & 7;                 // snMaxSurfaceTextureSpread has eight entries
    const std::uint32_t zoom = spreadZoom < 5 ? spreadZoom : 4; // the curve/family tables have five
    const std::uint32_t gridSize =
        statics.textureRandomizationGridSize.empty()
            ? 1u
            : statics.textureRandomizationGridSize[zoom < statics.textureRandomizationGridSize.size()
                                                       ? zoom
                                                       : statics.textureRandomizationGridSize.size() - 1];
    const std::int32_t familyColumns = TextureGridCellCountX()[zoom];
    std::vector<std::uint16_t>& familyGrid = TextureFamilyMemberSlot()[zoom];
    const std::int32_t vertsX = static_cast<std::int32_t>(vertsX_);

    auto familyMemberFor = [&](std::uint32_t index) -> std::uint16_t {
        if (gridSize == 0 || familyColumns <= 0 || familyGrid.empty()) return 0;
        const std::int32_t row = static_cast<std::int32_t>(index) / vertsX;
        const std::int32_t column = static_cast<std::int32_t>(index) % vertsX;
        const std::size_t cell = static_cast<std::size_t>((row / static_cast<std::int32_t>(gridSize)) * familyColumns +
                                                          (column / static_cast<std::int32_t>(gridSize)));
        return cell < familyGrid.size() ? familyGrid[cell] : 0;
    };
    auto textureKeyFor = [&](std::uint32_t textureId, std::uint32_t index) -> std::uint16_t {
        const std::uint32_t count = host->TerrainTextureSubTextureCount(static_cast<std::int32_t>(zoom), textureId);
        if (count == 0) return static_cast<std::uint16_t>(textureId);
        const std::uint32_t family = familyMemberFor(index);
        return static_cast<std::uint16_t>((family % count) * 0x1000u + textureId);
    };

    for (std::int32_t z = rowStart; z <= rowEnd; ++z) {
        for (std::int32_t x = colStart; x <= colEnd; ++x) {
            const auto index = static_cast<std::uint32_t>(z) * vertsX_ + static_cast<std::uint32_t>(x);
            const auto vertexIndex = static_cast<std::int32_t>(index);
            const float depth = waterDepth_[index];                          // cSTESurfaceWater +0x0
            if (statics.maxWaterDepthForDeepSeaBedTexture < depth) continue; // too deep: no sea bed texture
            if ((Flags(vertexIndex) & kVertexCliffMark) != 0) {
                AddLandTypeAtVertex(index, static_cast<std::uint16_t>(cliffTexture), 50.0f);
                continue;
            }
            std::uint32_t textureId = 0;
            std::int32_t temperature = 0x1f, moisture = 0;
            bool selectLand = true, selectWater = false;
            if (depth <= 0.0f) {                                             // land
                const float altitude = vertices_[index].altitude;
                if (statics.maxBeachWidth < surfaceCover_[static_cast<std::size_t>(z)][static_cast<std::size_t>(x)] ||
                    maxBeachAltitude < altitude) {
                    // Not beach: the temperature row comes from the altitude, the column from the land simulator.
                    if (maxBeachAltitude < altitude && heightRange != 0.0f) {
                        const float ratio = ((altitude - tuning_.seaLevel) / heightRange) * 30.0f;
                        const auto rounded = static_cast<std::int32_t>(std::nearbyint(ratio - 0.5f));
                        temperature = 0x1f - rounded;
                        if (temperature < 0) temperature = 0;
                        if (0x1f < temperature) temperature = 0x1f;
                    }
                    moisture = landSimulator_ ? host->MoistureIndexAtX(static_cast<float>(x) * 16.0f) : 0;
                    std::uint32_t column = static_cast<std::uint32_t>(temperature * 16 + (moisture & 15));
                    if (column < 512)
                        textureId = SurfaceCoverMapSlot()[0 <= terrainType && terrainType < 4 ? terrainType : 0][column];
                } else {
                    textureId = beachTexture;
                    selectWater = true;                                       // the beach spreads over water too
                    if (r.left <= x && r.top <= z && x <= r.right && z <= r.bottom)
                        AddLandTypeAtVertex(index, textureKeyFor(textureId, index), 1.0f);
                }
            } else {                                                          // water
                textureId = (depth < statics.minWaterDepthForDeepSeaBedTexture) ? 0x14u : 0x13u;
                selectLand = false;
            }
            const std::uint16_t textureKey = textureKeyFor(textureId, index);

            auto info = SurfaceCoverTexturesSlot().find(textureId);
            if (std::getenv("SC4_DEBUG")) {
                static int zero = 0, missing = 0, ok = 0, shown = 0;
                if (textureId == 0) ++zero; else if (info == SurfaceCoverTexturesSlot().end()) ++missing; else ++ok;
                if ((++shown & 4095) == 0) std::fprintf(stderr, "DecideSurfaceCover: ids zero %d missing-info %d ok %d (last temperature %d moisture %d id %u)\n", zero, missing, ok, int(temperature), int(moisture), unsigned(textureId));
            }
            if (info == SurfaceCoverTexturesSlot().end()) continue;
            TerrainSurfaceCoverInfo& cover = info->second;
            const std::int32_t radius = cover.maxSpread[zoom];
            TerrainRect area{std::max(x - radius, 0), std::max(z - radius, 0),
                             std::min(x + radius, static_cast<std::int32_t>(cellsX_)),
                             std::min(z + radius, static_cast<std::int32_t>(cellsZ_))};
            TerrainRect clipped{std::max(area.left, r.left), std::max(area.top, r.top),
                                std::min(area.right, r.right), std::min(area.bottom, r.bottom)};
            if (clipped.right < clipped.left || clipped.bottom < clipped.top) continue;

            const float maxInput = cover.curveMaxInput[zoom];
            const float maxDistanceSquared = maxInput * maxInput * 0.00390625f;   // / 256
            const std::int32_t dx = clipped.left - x;
            const std::int32_t dz = clipped.top - z;
            const float firstStepX = static_cast<float>(dx + dx + 1);   // the x step resets every row
            float stepZ = static_cast<float>(dz + dz + 1);
            float distanceSquared = static_cast<float>(dz * dz + dx * dx);
            auto target = static_cast<std::uint32_t>(clipped.top) * vertsX_ + static_cast<std::uint32_t>(clipped.left);
            for (std::int32_t row = clipped.top; row <= clipped.bottom; ++row) {
                float rowDistance = distanceSquared;
                float stepX = firstStepX;
                for (std::int32_t column = clipped.left; column <= clipped.right; ++column) {
                    const float water = waterDepth_[target];
                    const float altitude = vertices_[target].altitude;
                    const bool kindOk = (water <= 0.0f || selectWater) && (selectLand || 0.0f < water);
                    const bool nearOk = rowDistance < maxDistanceSquared;
                    const bool shallowOk = altitude <= maxBeachAltitude
                                               ? (rowDistance < maxDistanceSquared &&
                                                  (statics.maxBeachWidth <
                                                       surfaceCover_[static_cast<std::size_t>(row)]
                                                                   [static_cast<std::size_t>(column)] ||
                                                   0.0f < water))
                                               : nearOk;
                    if (kindOk && shallowOk) {
                        const float distance = rowDistance <= 0.0f ? 0.0f : std::sqrt(rowDistance);
                        float weight = EvaluateCurve(cover.curves[zoom], distance * 16.0f) * cover.weight;
                        if (0.0f < weight) AddLandTypeAtVertex(target, textureKey, weight);
                    }
                    rowDistance += stepX;
                    stepX += 2.0f;
                    ++target;
                }
                distanceSquared += stepZ;
                stepZ += 2.0f;
                target += vertsX_ - static_cast<std::uint32_t>(clipped.right - clipped.left + 1);
            }
        }
    }
}

void cSTETerrain::MarkCliff(std::int32_t index, bool evenIsCliff, bool oddIsCliff) {
    // 0x100735398: mark the vertices of the cliff triangle(s) with kVertexCliffMark. When both triangles are
    // cliffs the two diagonals decide the internal triangulation flip; the flip is applied with
    // FlipInternalTriangulation (vslot 0x2f0) and the vertex gets kVertexCliffFlip as well.
    const float maxNormalY = Statics().maxNormalYForCliff;
    const auto setMark = [this](std::uint32_t vertex, std::uint16_t mark) {
        if (vertex < vertices_.size()) vertices_[vertex].flags |= mark;
    };
    if (evenIsCliff && oddIsCliff) {
        const std::uint32_t vertsX = vertsX_;
        const auto base = static_cast<std::uint32_t>(index);
        const float a = vertices_[base].altitude;
        const float b = vertices_[base + 1].altitude;
        const float c = vertices_[base + vertsX].altitude;
        const float d = vertices_[base + vertsX + 1].altitude;
        // The two diagonal normals: A/B/C = (x,z), (x+1,z), (x,z+1); the first uses the (C-A, C-B) edges
        // and the second the (D-A, D-B) edges (MOVSLDUP/INSERTPS in the packed original).
        const float firstLengthSquared =
            65536.0f + 256.0f * ((c - a) * (c - a) + (c - b) * (c - b));
        const float secondLengthSquared =
            65536.0f + 256.0f * ((d - a) * (d - a) + (d - b) * (d - b));
        const float y1 = 256.0f / std::sqrt(firstLengthSquared);
        const float y2 = 256.0f / std::sqrt(secondLengthSquared);
        if (maxNormalY <= y1 || maxNormalY <= y2) {
            FlipInternalTriangulation(static_cast<std::uint32_t>(index) % vertsX,
                                      static_cast<std::uint32_t>(index) / vertsX, true);
            setMark(base, kVertexCliffFlip);
            if (y1 < maxNormalY) {
                setMark(base, kVertexCliffMark);
                setMark(base + vertsX, kVertexCliffMark);
                setMark(base + 1, kVertexCliffMark);
            }
            if (y2 < maxNormalY) {
                setMark(base + 1, kVertexCliffMark);
                setMark(base + vertsX, kVertexCliffMark);
                setMark(base + vertsX + 1, kVertexCliffMark);
            }
            return;
        }
    }
    if (evenIsCliff) {
        setMark(static_cast<std::uint32_t>(index), kVertexCliffMark);
        setMark(static_cast<std::uint32_t>(index) + vertsX_, kVertexCliffMark);
        setMark(static_cast<std::uint32_t>(index) + 1 + vertsX_, kVertexCliffMark);
    }
    if (oddIsCliff) {
        setMark(static_cast<std::uint32_t>(index), kVertexCliffMark);
        setMark(static_cast<std::uint32_t>(index) + 1 + vertsX_, kVertexCliffMark);
        setMark(static_cast<std::uint32_t>(index) + 1, kVertexCliffMark);
    }
}

void cSTETerrain::CalculateNormalsAndAssignCliffTextures(const TerrainRect& r) {
    // 0x100734dc8: two passes. The first computes the even/odd triangle normal of every cell in the rect
    // (minus one on the top/left, clamped to the grid) into a per-row scratch buffer; the second averages the
    // six triangles around each vertex (three from the row above, three from its own row) into the vertex
    // normal. A triangle whose normal's y falls below kMaxNormalYForCliff makes MarkCliff flag the vertices and
    // flip the cell's internal triangulation. The original's packed SSE sequence reduces to the scalar form
    // below: the normals are (16*(C-D), 256, 16*(D-C)) and (16*(D-B), 256, 16*(B-D)).
    TerrainStatics& statics = Statics();
    ClearVertexPropertyFlags(r, kVertexCliffMark);
    const std::int32_t xStart = std::max(r.left - 1, 0);
    const std::int32_t zStart = std::max(r.top - 1, 0);
    TerrainRect flagsRect{xStart, zStart, std::min(r.right, static_cast<std::int32_t>(lastCellX_)),
                          std::min(r.bottom, static_cast<std::int32_t>(lastCellZ_))};
    ClearVertexCliffFlipTriangulationFlags(flagsRect);

    const std::int32_t xEnd = std::min(r.right, static_cast<std::int32_t>(lastCellX_));
    const std::int32_t zEnd = std::min(r.bottom, static_cast<std::int32_t>(lastCellZ_));
    const std::int32_t vertsX = static_cast<std::int32_t>(vertsX_);
    const std::size_t bufferSize = static_cast<std::size_t>(cellsX_) * 2 * 3;
    for (std::vector<float>& buffer : triangleNormals_)
        if (buffer.size() < bufferSize) buffer.assign(bufferSize, 0.0f);
    if (zEnd < zStart) return;

    std::vector<float>* previous = &triangleNormals_[0];
    std::vector<float>* current = &triangleNormals_[1];
    for (std::int32_t z = zStart; z <= zEnd; ++z) {
        if (xEnd >= xStart) {
            for (std::int32_t x = xStart; x <= xEnd; ++x) {
                auto base = static_cast<std::uint32_t>(z) * vertsX_ + static_cast<std::uint32_t>(x);
                const float b = vertices_[base + 1].altitude;
                const float c = vertices_[base + static_cast<std::uint32_t>(vertsX)].altitude;
                const float d = vertices_[base + static_cast<std::uint32_t>(vertsX) + 1].altitude;
                // even triangle normal: only the (C-D) edge difference enters
                float evenX = (c - d) * 16.0f;
                float evenZ = -evenX;
                float evenInv = 1.0f / std::sqrt(evenX * evenX + 65536.0f + evenZ * evenZ);
                // odd triangle normal: only the (D-B) edge difference enters
                float oddX = (d - b) * 16.0f;
                float oddZ = -oddX;
                float oddInv = 1.0f / std::sqrt(oddX * oddX + 65536.0f + oddZ * oddZ);
                const std::size_t slot = static_cast<std::size_t>(x) * 6;
                (*current)[slot + 0] = evenX * evenInv;
                (*current)[slot + 1] = 256.0f * evenInv;
                (*current)[slot + 2] = evenZ * evenInv;
                (*current)[slot + 3] = oddX * oddInv;
                (*current)[slot + 4] = 256.0f * oddInv;
                (*current)[slot + 5] = oddZ * oddInv;
                const bool evenIsCliff = std::fabs((*current)[slot + 1]) < statics.maxNormalYForCliff;
                const bool oddIsCliff = std::fabs((*current)[slot + 4]) < statics.maxNormalYForCliff;
                if (evenIsCliff || oddIsCliff) MarkCliff(static_cast<std::int32_t>(base), evenIsCliff, oddIsCliff);
            }
        }
        // The vertex normals of this row use the row's own cells and the row above.
        if (r.top <= z && z <= zEnd && r.left <= r.right) {
            for (std::int32_t x = r.left; x <= r.right; ++x) {
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                auto add = [&](const std::vector<float>& buffer, std::size_t cell) {
                    const std::size_t at = cell * 3;
                    if (at + 2 < buffer.size()) { nx += buffer[at]; ny += buffer[at + 1]; nz += buffer[at + 2]; }
                };
                if (1 <= x) {
                    if (z < static_cast<std::int32_t>(cellsZ_)) add(*current, static_cast<std::size_t>(2 * x - 1));
                    if (z != 0) {
                        add(*previous, static_cast<std::size_t>(2 * x - 1));
                        add(*previous, static_cast<std::size_t>(2 * x - 2));
                    }
                }
                if (x < static_cast<std::int32_t>(cellsX_)) {
                    if (z < static_cast<std::int32_t>(cellsZ_)) {
                        add(*current, static_cast<std::size_t>(2 * x));
                        add(*current, static_cast<std::size_t>(2 * x + 1));
                    }
                    if (z != 0) add(*previous, static_cast<std::size_t>(2 * x));
                }
                float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length == 0.0f) { nx = 1.0f; ny = 1.0f; nz = 1.0f; length = std::sqrt(3.0f); }
                const float inverse = 1.0f / length;
                Vertex& vertex = vertices_[static_cast<std::uint32_t>(z) * vertsX_ + static_cast<std::uint32_t>(x)];
                vertex.normal[0] = nx * inverse;
                vertex.normal[1] = ny * inverse;
                vertex.normal[2] = nz * inverse;
            }
        }
        std::swap(current, previous);
    }
}

// ==================================================================================== counts

int cSTETerrain::NumberOfWaterVertices(const TerrainRect& r) const {
    int count = 0;
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(r.left);
        for (std::int32_t x = r.left; x <= r.right; ++x, ++i)
            count += 0.0f < waterDepth_[i] ? 1 : 0;
    }
    return count;
}

bool cSTETerrain::AreSeaVertices(const TerrainRect& r) const {
    if (static_cast<std::uint32_t>(r.left) >= vertsX_) return false;
    if (static_cast<std::uint32_t>(r.top) >= vertsZ_) return false;
    if (vertsX_ <= static_cast<std::uint32_t>(r.right)) return false;
    if (static_cast<std::uint32_t>(r.bottom) >= vertsZ_) return false;
    if (r.bottom < r.top) return true;   // (sic) an empty range counts as "all sea"
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(r.left);
        for (std::int32_t x = r.left; x <= r.right; ++x, ++i) {
            const float altitude = vertices_[i].altitude;
            if (tuning_.seaLevel <= altitude) return false;
        }
    }
    return true;
}

float cSTETerrain::GetAltitudeCheckSum(const TerrainRect& r) const {
    if (static_cast<std::uint32_t>(r.left) >= vertsX_) return 0.0f;
    if (static_cast<std::uint32_t>(r.top) >= vertsZ_) return 0.0f;
    if (static_cast<std::uint32_t>(r.right) >= vertsX_) return 0.0f;
    if (static_cast<std::uint32_t>(r.bottom) >= vertsZ_) return 0.0f;
    if (r.bottom < r.top) return 0.0f;
    float sum = 0.0f;
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(r.left);
        for (std::int32_t x = r.left; x <= r.right; ++x, ++i) sum += vertices_[i].altitude;
    }
    return sum;
}

// ==================================================================================== editing

void cSTETerrain::LowerVertex(std::uint32_t index, float amount) {
    Vertex& v = vertices_[index];
    if (v.flags & kVertexLocked) return;
    float altitude = v.altitude - amount;
    v.altitude = altitude;
    if (altitude < Statics().minTerrainHeight) {
        v.altitude = Statics().minTerrainHeight;
        altitude = Statics().minTerrainHeight;
    }
    // The original rewrites the surface-water depth unconditionally, so a vertex raised above the sea
    // keeps a negative depth instead of drying out (kept on purpose).
    waterDepth_[index] = tuning_.seaLevel - altitude;
}

void cSTETerrain::RaiseVertex(std::uint32_t index, float amount) {
    Vertex& v = vertices_[index];
    if (v.flags & kVertexLocked) return;
    float altitude = amount + v.altitude;
    v.altitude = altitude;
    if (Statics().maxTerrainHeight < altitude) {
        v.altitude = Statics().maxTerrainHeight;
        altitude = Statics().maxTerrainHeight;
    }
    waterDepth_[index] = tuning_.seaLevel - altitude;   // negative above the sea (see LowerVertex)
}

void cSTETerrain::SetAltitudeAtVertex(std::int32_t x, std::int32_t z, float altitude) {
    const std::uint32_t index = vertsX_ * static_cast<std::uint32_t>(z) + static_cast<std::uint32_t>(x);
    const float current = vertices_[index].altitude;
    if (current < altitude) {
        RaiseVertex(index, altitude - current);
    } else if (altitude < current) {
        LowerVertex(index, current - altitude);
    }
}

void cSTETerrain::ClearVertexPropertyFlags(std::uint16_t mask) {
    for (std::uint32_t i = 0; i < totalVerts_; ++i) vertices_[i].flags &= static_cast<std::uint16_t>(~mask);
}

void cSTETerrain::ClearVertexPropertyFlags(const TerrainRect& r, std::uint16_t mask) {
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(r.left);
        for (std::int32_t x = r.left; x <= r.right; ++x, ++i)
            vertices_[i].flags &= static_cast<std::uint16_t>(~mask);
    }
}

void cSTETerrain::ClearVertexCliffFlipTriangulationFlags(const TerrainRect& r) {
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(r.left);
        for (std::int32_t x = r.left; x <= r.right; ++x, ++i) {
            if (vertices_[i].flags & kVertexCliffFlip) {
                FlipInternalTriangulation(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), false);
                vertices_[i].flags &= static_cast<std::uint16_t>(~kVertexCliffFlip);
            }
        }
    }
}

bool cSTETerrain::IsVertexWithMark1(std::int32_t index) const {
    return (vertices_[static_cast<std::size_t>(index)].flags & kVertexMark1) != 0;
}

bool cSTETerrain::IsLandAndNotMark1(std::int32_t index) const {
    const Vertex& v = vertices_[static_cast<std::size_t>(index)];
    if (v.flags & kVertexMark1) return false;
    return tuning_.seaLevel < v.altitude;   // strictly above the sea (IsLandVertex is <=)
}

bool cSTETerrain::IsVertexWithMark3(std::int32_t index) const {
    return (vertices_[static_cast<std::size_t>(index)].flags & kVertexMark3) != 0;
}

bool cSTETerrain::IsLandAndNotMark3(std::int32_t index) const {
    const Vertex& v = vertices_[static_cast<std::size_t>(index)];
    if (v.flags & kVertexMark3) return false;
    return tuning_.seaLevel < v.altitude;
}

void cSTETerrain::PadRectangle(TerrainRect& r, int amount) const {
    r.left -= amount;
    r.top -= amount;
    r.right += amount;
    r.bottom += amount;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (static_cast<std::uint32_t>(r.right) > cellsX_) r.right = static_cast<std::int32_t>(cellsX_);
    if (static_cast<std::uint32_t>(r.bottom) > cellsZ_) r.bottom = static_cast<std::int32_t>(cellsZ_);
}

void cSTETerrain::CreateVertRectAroundVertex(TerrainRect& r, int x, int z, int radius) const {
    r.left = x - radius;
    if (r.left < 0) r.left = 0;
    std::uint32_t right = static_cast<std::uint32_t>(x) + static_cast<std::uint32_t>(radius);
    r.right = static_cast<std::int32_t>(cellsX_ < right ? cellsX_ : right);
    r.top = z - radius;
    if (r.top < 0) r.top = 0;
    std::uint32_t bottom = static_cast<std::uint32_t>(z) + static_cast<std::uint32_t>(radius);
    r.bottom = static_cast<std::int32_t>(cellsZ_ < bottom ? cellsZ_ : bottom);
}

// ==================================================================================== water

void cSTETerrain::SaveWaterLevelsInTempArray(TerrainRect& r) {
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        const std::size_t row = static_cast<std::size_t>(vertsX_) * static_cast<std::uint32_t>(z);
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::size_t i = row + static_cast<std::uint32_t>(x);
            tempWaterLevels_[i] = waterDepth_[i];
        }
    }
}

void cSTETerrain::RestoreWaterLevelsFromTempArray(TerrainRect& r) {
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        const std::size_t row = static_cast<std::size_t>(vertsX_) * static_cast<std::uint32_t>(z);
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::size_t i = row + static_cast<std::uint32_t>(x);
            waterDepth_[i] = tempWaterLevels_[i];
        }
    }
}

float cSTETerrain::GetSedimentValue(float x, float z) const {
    if (!LocationIsInBounds(x, z)) return 0.0f;
    // The original rounds (coordinate / 16 - 0.5) to the nearest integer grid line.
    const std::int32_t gx = RoundToNearestInt(x * 0.0625f - 0.5f);
    const std::int32_t gz = RoundToNearestInt(z * 0.0625f - 0.5f);
    const std::size_t i = static_cast<std::size_t>(gz) * vertsX_ + static_cast<std::size_t>(gx);
    return i < sediment_.size() ? sediment_[i] : 0.0f;   // cSTESurfaceWater vertex +0x1c
}

std::int32_t cSTETerrain::GetNeighborWithLowestWaterLevel(std::int32_t index) const {
    const std::uint32_t z = static_cast<std::uint32_t>(index) / vertsX_;
    const std::uint32_t x = static_cast<std::uint32_t>(index) % vertsX_;
    const std::uint8_t mask = GetNeighbors(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z));
    const float level = vertices_[static_cast<std::size_t>(index)].altitude + waterDepth_[static_cast<std::size_t>(index)];
    std::int32_t best = -1;
    float bestDrop = 0.0f;
    // Straight neighbours first: the even directions 0, 2, 4 and 6 (the original compares the counter
    // before advancing it, so south is included).
    for (std::int32_t dir = 0; dir <= 6; dir += 2) {
        if (!((mask >> dir) & 1)) continue;
        const std::size_t n = static_cast<std::size_t>(neighborOffsets_[dir] + index);
        const float drop = level - (vertices_[n].altitude + waterDepth_[n]);
        if (bestDrop < drop) {
            best = dir;
            bestDrop = drop;
        }
    }
    // Then the diagonals 1, 3, 5 and 7, compared against sqrt(2) times the best straight drop.
    static const float kRoot2 = 1.4142135f;
    bestDrop *= kRoot2;
    for (std::int32_t dir = 1; dir <= 7; dir += 2) {
        if (!((mask >> dir) & 1)) continue;
        const std::size_t n = static_cast<std::size_t>(neighborOffsets_[dir] + index);
        const float drop = level - (vertices_[n].altitude + waterDepth_[n]);
        if (bestDrop < drop) {
            best = dir;
            bestDrop = drop;
        }
    }
    return best;
}

// ==================================================================================== surfaces

void cSTETerrain::ClearSurfaceTextures(const TerrainRect& r) {
    // The three surface-texture layer keys live at vertex +0x16, +0x1a and +0x1e (u16 each); the
    // blend weights at +0x18/+0x1c/+0x20 are left alone.
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        std::size_t i = static_cast<std::size_t>(z) * vertsX_ + static_cast<std::size_t>(r.left);
        for (std::int32_t x = r.left; x <= r.right; ++x, ++i) {
            for (int layer = 0; layer < 3; ++layer) {
                std::uint16_t zero = 0;
                std::memcpy(&vertices_[i].surface[6 + layer * 4], &zero, sizeof zero);
            }
        }
    }
}

// ============================================================================ brushed regions

void cSTETerrain::ComputeBoundsOfMarkedRegion(TerrainRect& r, std::uint16_t mask) const {
    r = {0x7fffffff, 0x7fffffff, -0x7fffffff - 1, -0x7fffffff - 1};
    for (std::uint32_t z = 0; z < vertsZ_; ++z) {
        std::size_t i = static_cast<std::size_t>(vertsX_) * z;
        for (std::uint32_t x = 0; x < vertsX_; ++x, ++i) {
            if ((vertices_[i].flags & mask) == 0) continue;
            if (static_cast<std::int32_t>(x) < r.left) r.left = static_cast<std::int32_t>(x);
            if (r.right < static_cast<std::int32_t>(x)) r.right = static_cast<std::int32_t>(x);
            if (static_cast<std::int32_t>(z) < r.top) r.top = static_cast<std::int32_t>(z);
            if (r.bottom < static_cast<std::int32_t>(z)) r.bottom = static_cast<std::int32_t>(z);
        }
    }
}

void cSTETerrain::ComputeRectAroundPt(TerrainPoint out[4], float x, float z, float width, float height,
                                      float angle) {
    const float sinAngle = std::sin(angle);
    const float cosAngle = std::cos(angle);
    const float halfW = width * -0.5f;
    const float halfH = height * -0.5f;
    // Corners in the original's order: (-w/2,-h/2), (-w/2,+h/2), (+w/2,+h/2), (+w/2,-h/2).
    const float dx[4] = {halfW, halfW, -halfW, -halfW};
    const float dz[4] = {halfH, -halfH, -halfH, halfH};
    for (int i = 0; i < 4; ++i) {
        const float cornerX = dx[i], cornerZ = dz[i];
        out[i].x = (cosAngle * cornerX - sinAngle * cornerZ) + x;
        out[i].z = (cornerZ * cosAngle + cornerX * sinAngle) + z;
    }
}

void cSTETerrain::ComputeSpansForRectangle(const TerrainPoint* quad, int* spanMin, int* spanMax,
                                           int& firstRow, int& lastRow) const {
    // Row range: the four quad points' z/16, clamped. The original tracks the maximum of only the
    // first three points for its "negative" test (kept quirk), then clamps the fourth-point maximum
    // with an *unsigned* compare, which turns an all-negative quad into "every row".
    std::int32_t minRow = 0x7fffffff, maxRow = -0x7fffffff - 1, maxOfFirstThree = -0x7fffffff - 1;
    for (int k = 0; k < 4; ++k) {
        const std::int32_t row = static_cast<std::int32_t>(quad[k].z * 0.0625f);
        if (maxRow < row) maxRow = row;
        if (row < minRow) minRow = row;
        if (k != 3) maxOfFirstThree = maxRow;
    }
    if (maxOfFirstThree < 0) minRow = 0;
    // A brush hanging over the city's near edge has rows below zero even when the first three corners are on the map:
    // left unclamped they indexed before the span tables and below vertex 0 (the user's crash raising terrain at the edge).
    if (minRow < 0) minRow = 0;
    if (static_cast<std::uint32_t>(maxRow) > cellsZ_) maxRow = static_cast<std::int32_t>(cellsZ_);
    firstRow = minRow;
    lastRow = maxRow;

    for (std::int32_t z = minRow; z <= maxRow; ++z) {
        spanMin[z] = 0x7fffffff;
        spanMax[z] = -0x7fffffff - 1;
        const float rowZ = static_cast<float>(z) * 16.0f;
        std::int32_t rowMax = -0x7fffffff - 1;
        for (int edge = 1; edge != 5; ++edge) {
            const float z0 = quad[edge - 1].z;
            const int next = edge == 4 ? 0 : edge;
            const float z1 = quad[next].z;
            if (!((z0 <= rowZ && rowZ < z1) || (rowZ < z0 && z1 <= rowZ))) continue;
            float x0 = quad[edge - 1].x;
            const float x1 = quad[next].x;
            if (x0 != x1) {
                // (sic) the intersection is truncated to an integer *before* being divided by 16.
                x0 = static_cast<float>(static_cast<std::int32_t>((rowZ - z0) * ((x1 - x0) / (z1 - z0)) + x0));
            }
            const std::int32_t column = static_cast<std::int32_t>(x0 * 0.0625f);
            if (column < spanMin[z]) {
                spanMin[z] = column;
                rowMax = spanMax[z];
            }
            if (rowMax < column) {
                spanMax[z] = column;
                rowMax = column;
            }
        }
        if (spanMin[z] < 0) spanMin[z] = 0;
        std::uint32_t right = static_cast<std::uint32_t>(rowMax + 1);
        spanMax[z] = static_cast<std::int32_t>(right);
        if (static_cast<std::uint32_t>(cellsX_) < right) spanMax[z] = static_cast<std::int32_t>(cellsX_);
    }
}

bool cSTETerrain::ComputeBooleanFnForRect(const TerrainPoint* quad, bool (cSTETerrain::*fn)(int) const,
                                          float fraction) const {
    int firstRow = 0, lastRow = 0;
    int spanMin[260];
    int spanMax[260];
    ComputeSpansForRectangle(quad, spanMin, spanMax, firstRow, lastRow);
    int tested = 0;
    int hits = 0;
    for (std::int32_t z = firstRow; z <= lastRow; ++z) {
        const std::int32_t rowStart = static_cast<std::int32_t>(vertsX_) * z;
        for (std::int32_t x = spanMin[z]; x <= spanMax[z]; ++x) {
            ++tested;
            if ((this->*fn)(rowStart + x)) ++hits;
        }
    }
    return static_cast<int>(static_cast<float>(tested) * fraction) <= hits;
}

// =============================================================================== shore cliffs

void cSTETerrain::DetectShoreVertices() {
    // 0x100736ef0: clear the shore mark over the whole grid rect, then re-mark every shore vertex.
    ClearVertexPropertyFlags(gridRect_, kVertexShore);
    for (std::uint32_t index = 0; index < totalVerts_; ++index)
        if (IsShoreVertex(static_cast<std::int32_t>(index)))
            vertices_[index].flags |= kVertexShore;
}

void cSTETerrain::GetShoreStartingAtVert(std::int32_t vertex, std::deque<std::int32_t>& shore,
                                         std::uint16_t mark) {
    // 0x100736f5e.
    shore.clear();
    shore.push_back(vertex);
    Flags(vertex) |= mark;
    TrackShoreWithLandToTheRight(vertex, shore, mark);
    TrackShoreWithLandToTheLeft(vertex, shore, mark);
}

void cSTETerrain::TrackShoreWithLandToTheRight(std::int32_t vertex, std::deque<std::int32_t>& shore,
                                               std::uint16_t mark) {
    // 0x100739610: look for the first sea neighbour over directions 0..6 (south-west is never the first
    // pick, sic), then walk from vertex to vertex while keeping the land on the right. The scan rotates
    // about the previous direction and stops as soon as the shore turns inland.
    const std::uint32_t vertsX = vertsX_, vertsZ = vertsZ_;
    const float sea = tuning_.seaLevel;
    std::int32_t x = static_cast<std::int32_t>(static_cast<std::uint32_t>(vertex) % vertsX);
    std::int32_t z = static_cast<std::int32_t>(static_cast<std::uint32_t>(vertex) / vertsX);
    std::int32_t found = -1;
    for (std::int32_t dir = 0; dir < 7; ++dir) {
        const std::uint32_t nx = static_cast<std::uint32_t>(x + kDirectionX[dir]);
        if (vertsX <= nx) continue;
        const std::uint32_t nz = static_cast<std::uint32_t>(z + kDirectionZ[dir]);
        if (vertsZ <= nz) continue;
        if (vertices_[nz * vertsX + nx].altitude < sea) {
            found = dir;
            break;
        }
    }
    if (found < 0) return;
    std::uint32_t stop = static_cast<std::uint32_t>(found + 1);
    std::uint32_t direction = static_cast<std::uint32_t>(found + 2) & 7u;
    std::uint32_t previous = stop;
    if (direction == stop) return;
    for (;;) {
        bool added = false;
        const std::uint32_t nx = static_cast<std::uint32_t>(x + kDirectionX[direction]);
        if (nx < vertsX) {
            const std::uint32_t nz = static_cast<std::uint32_t>(z + kDirectionZ[direction]);
            if (nz < vertsZ) {
                const std::uint32_t next = nz * vertsX + nx;
                const std::uint16_t flags = vertices_[next].flags;
                if ((flags & kVertexShore) != 0 && (flags & mark) == 0) {
                    vertices_[next].flags = static_cast<std::uint16_t>(flags | mark);
                    shore.push_back(static_cast<std::int32_t>(next));
                    x = static_cast<std::int32_t>(nx);
                    z = static_cast<std::int32_t>(nz);
                    added = true;
                } else if (sea < vertices_[next].altitude) {
                    return;                                       // the shore turns inland
                }
            }
        }
        if (added) {
            stop = (previous + 5) & 7u;
            direction = (previous + 6) & 7u;
            previous = stop;
        } else {
            const std::uint32_t nextDirection = (direction + 1) & 7u;
            previous = direction;
            direction = nextDirection;
        }
        if (direction == stop) return;
    }
}

void cSTETerrain::TrackShoreWithLandToTheLeft(std::int32_t vertex, std::deque<std::int32_t>& shore,
                                              std::uint16_t mark) {
    // 0x100739798: the mirror image -- the first sea neighbour is searched over directions 7 down to 0
    // (all eight of them) and the walk then rotates the other way, pushing in front of the deque.
    const std::uint32_t vertsX = vertsX_, vertsZ = vertsZ_;
    const float sea = tuning_.seaLevel;
    std::int32_t x = static_cast<std::int32_t>(static_cast<std::uint32_t>(vertex) % vertsX);
    std::int32_t z = static_cast<std::int32_t>(static_cast<std::uint32_t>(vertex) / vertsX);
    std::int32_t found = -1;
    for (std::int32_t dir = 7; dir >= 0; --dir) {
        const std::uint32_t nx = static_cast<std::uint32_t>(x + kDirectionX[dir]);
        if (vertsX <= nx) continue;
        const std::uint32_t nz = static_cast<std::uint32_t>(z + kDirectionZ[dir]);
        if (vertsZ <= nz) continue;
        if (vertices_[nz * vertsX + nx].altitude < sea) {
            found = dir;
            break;
        }
    }
    if (found < 0) return;
    std::uint32_t stop = static_cast<std::uint32_t>(found);
    std::uint32_t direction = (found == 0) ? 7u : static_cast<std::uint32_t>(found - 1);
    for (;;) {
        if (direction == stop) return;                            // the walk came full circle
        bool added = false;
        bool inland = false;
        const std::uint32_t nx = static_cast<std::uint32_t>(x + kDirectionX[direction]);
        if (nx < vertsX) {
            const std::uint32_t nz = static_cast<std::uint32_t>(z + kDirectionZ[direction]);
            if (nz < vertsZ) {
                const std::uint32_t next = nz * vertsX + nx;
                const std::uint16_t flags = vertices_[next].flags;
                if ((flags & kVertexShore) != 0 && (flags & mark) == 0) {
                    vertices_[next].flags = static_cast<std::uint16_t>(flags | mark);
                    shore.push_front(static_cast<std::int32_t>(next));
                    x = static_cast<std::int32_t>(nx);
                    z = static_cast<std::int32_t>(nz);
                    added = true;
                } else if (sea < vertices_[next].altitude) {
                    inland = true;
                }
            }
        }
        if (!added) {
            if (inland) return;
            const std::uint32_t nextDirection = (direction == 0) ? 7u : direction - 1;
            if (nextDirection != stop) {
                direction = nextDirection;
                continue;
            }
            direction = stop;
            return;
        }
        // The next sweep starts four directions on and ends one before that.
        stop = (direction + 4) & 7u;
        direction = (((direction + 4) & 7u) + 7) & 7u;
        if (direction == stop) return;
    }
}

bool cSTETerrain::AnalyzeShoreAndCreateCliffs(std::deque<std::int32_t>& shore) {
    // 0x1007317fe: for every vertex of the run, step inland from its sea neighbour (one vertex per unit of
    // the shore->sea offset) and sum the heights above the sea level over five candidates. A sum above
    // kMinShoreCliffCandidateHtSum keeps the vertex, anything else flushes the accumulated run into
    // CreateShoreCliff when it is longer than kMinShoreCliffSize (a run of exactly that size is dropped, sic).
    const TerrainStatics& statics = Statics();
    const std::uint32_t count = static_cast<std::uint32_t>(shore.size());
    if (count < statics.minShoreCliffSize) return false;
    if (static_cast<std::int32_t>(count) < 1) return false;
    const float sea = tuning_.seaLevel;
    std::vector<tCliffVertInfo> cliffs;
    bool created = false;
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(count); ++i) {
        const std::int32_t vertex = shore[static_cast<std::size_t>(i)];
        tCliffVertInfo info;
        info.shoreVertex = vertex;
        info.seaVertex = GetNeighboringSeaVertex(vertex);
        const std::int32_t step = vertex - info.seaVertex;
        float heightSum = vertices_[static_cast<std::size_t>(vertex)].altitude - sea;
        std::int32_t candidate = vertex * 2 - info.seaVertex;
        bool flush = false;
        for (int k = 0; k < 5; ++k) {
            info.candidate[k] = candidate;
            const std::uint32_t index = static_cast<std::uint32_t>(candidate);
            // (sic) only the row end is checked (and only against vertsZ): a negative candidate wraps to a
            // huge unsigned row and flushes there.
            if (vertsZ_ <= index / vertsX_ ||
                vertices_[static_cast<std::size_t>(candidate)].altitude < sea) {
                flush = true;
                break;
            }
            heightSum += vertices_[static_cast<std::size_t>(candidate)].altitude - sea;
            candidate += step;
        }
        if (!flush && statics.minShoreCliffCandidateHeightSum < heightSum) {
            cliffs.push_back(info);
            continue;
        }
        if (static_cast<std::uint64_t>(cliffs.size()) > statics.minShoreCliffSize) {
            CreateShoreCliff(cliffs);
            created = true;
        }
        cliffs.clear();
    }
    if (static_cast<std::uint64_t>(cliffs.size()) > statics.minShoreCliffSize) {
        CreateShoreCliff(cliffs);
        created = true;
    }
    return created;
}

void cSTETerrain::CreateShoreCliff(std::vector<tCliffVertInfo>& cliffs) {
    // 0x100731ab0: one random cliff height above the sea (uniform kMinShoreCliffHt..kMaxShoreCliffHt), then
    // every candidate vertex gets that height, jittered by a gaussian whose sign depends on whether the
    // shore vertex is below the target, and by a uniform -8..8 m. The heights go through the virtual
    // SetAltitude (vslot 0x188).
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    const float sea = tuning_.seaLevel;
    const double random = host->RandomDoubleRangeUniform(Statics().minShoreCliffHeight,
                                                         Statics().maxShoreCliffHeight);
    std::uint32_t count = static_cast<std::uint32_t>(cliffs.size());
    if (Statics().maxShoreCliffSize < count) count = Statics().maxShoreCliffSize;
    if (static_cast<std::int32_t>(count) <= 0) return;
    const float target = static_cast<float>(random + static_cast<double>(sea));
    for (std::uint32_t i = 0; i < count; ++i) {
        const tCliffVertInfo& info = cliffs[i];
        if (static_cast<std::uint32_t>(info.seaVertex) >= totalVerts_ ||
            static_cast<std::uint32_t>(info.shoreVertex) >= totalVerts_) {
            // The original indexes the vertex array with GetNeighboringSeaVertex's -1 (a corrupting write on
            // non-square grids); the port cannot reproduce that and skips the entry instead (quirk noted).
            continue;
        }
        Flags(info.seaVertex) |= kVertexMark3;
        Flags(info.shoreVertex) |= kVertexMark3;
        const double gaussian = host->RandomDoubleRangeGaussian(0.0, 2.0);
        float height;
        if (vertices_[static_cast<std::size_t>(info.shoreVertex)].altitude <= target) {
            height = target - static_cast<float>(gaussian);
        } else {
            height = static_cast<float>(gaussian) + target;
        }
        SetAltitude(info.shoreVertex, height);
        const double base = static_cast<double>(height);
        for (int k = 0; k < 5; ++k) {
            const std::int32_t index = info.candidate[k];
            if (static_cast<std::uint32_t>(index) >= totalVerts_) continue;   // see the note above
            const double jitter = host->RandomDoubleRangeUniform(-8.0, 8.0);
            SetAltitude(index, static_cast<float>(jitter + base));
        }
    }
}

bool cSTETerrain::CreateShoreCliffs(bool redisplay) {
    // 0x100736d3e: mark the shoreline, trace a run from every unvisited shore vertex, raise the cliffs on
    // the promising runs, then halo the cliff vertices by eight cells, smoothen that halo 20 times and
    // clear the three scratch marks over the whole grid.
    DetectShoreVertices();
    bool created = false;
    if (vertsZ_ != 0) {
        for (std::uint32_t row = 0; row < vertsZ_; ++row) {
            const std::uint32_t rowStart = row * vertsX_;
            for (std::uint32_t col = 0; col < vertsX_; ++col) {
                // (sic) the test also requires the mark-1 bit to be clear: (flags & 0x11) == 0x10.
                if ((vertices_[rowStart + col].flags & (kVertexShore | kVertexMark1)) != kVertexShore) continue;
                std::deque<std::int32_t> shore;
                GetShoreStartingAtVert(static_cast<std::int32_t>(rowStart + col), shore, kVertexMark1);
                if (AnalyzeShoreAndCreateCliffs(shore)) created = true;
            }
        }
        if (created) {
            MarkRegion(8, &cSTETerrain::IsVertexWithMark3, &cSTETerrain::IsLandAndNotMark3, kVertexMark2);
            TerrainRect marked;
            ComputeBoundsOfMarkedRegion(marked, kVertexMark2);
            SmoothenMarkedRegion(marked, 0x14, kVertexMark2);
            if (redisplay) RedisplayTerrain(true, true, gridRect_, 0);
        }
    }
    if (totalVerts_ != 0)
        for (std::uint32_t index = 0; index < totalVerts_; ++index)
            vertices_[index].flags &= static_cast<std::uint16_t>(0xfff8);   // clear marks 1, 2 and 3
    return created;
}

// ============================================================================= marked regions

void cSTETerrain::MarkRegion(std::int32_t radius, bool (cSTETerrain::*vertexFn)(std::int32_t) const,
                             bool (cSTETerrain::*markFn)(std::int32_t) const, std::uint16_t mark) {
    // 0x100736728. Both member function pointers call through their (ptr, this-adjustment) pair, so the
    // original passes them as two-argument pointers; the port calls them directly. The rectangle is clamped
    // to cellsX / cellsZ (the last *cell*, not the last vertex), as the original does.
    const std::int32_t vertsX = static_cast<std::int32_t>(vertsX_);
    const std::int32_t vertsZ = static_cast<std::int32_t>(vertsZ_);
    const std::int32_t cellsX = static_cast<std::int32_t>(cellsX_);
    const std::int32_t cellsZ = static_cast<std::int32_t>(cellsZ_);
    if (vertsZ == 0) return;
    for (std::int32_t z = 0; z < vertsZ; ++z) {
        for (std::int32_t x = 0; x < vertsX; ++x) {
            if (!(this->*vertexFn)(z * vertsX + x)) continue;
            const std::int32_t left = std::max(x - radius, 0);
            const std::int32_t right = std::min(x + radius, cellsX);
            const std::int32_t top = std::max(z - radius, 0);
            const std::int32_t bottom = std::min(z + radius, cellsZ);
            if (bottom < top) continue;
            for (std::int32_t zz = top; zz <= bottom; ++zz) {
                if (right < left) break;
                for (std::int32_t xx = left; xx <= right; ++xx) {
                    const std::int32_t index = vertsX * zz + xx;
                    if ((this->*markFn)(index)) Flags(index) |= mark;
                }
            }
        }
    }
}

void cSTETerrain::SmoothenMarkedRegion(const TerrainRect& r, std::int32_t iterations, std::uint16_t mark) {
    // 0x10073697a: talus-angle relaxation. For every marked vertex, each existing neighbour closer than the
    // cross (axis) or diagonal threshold pulls the pair together by the excess times kWindErosionFactor;
    // only the endpoints that carry `mark` move.
    if (iterations <= 0) return;
    const TerrainStatics& statics = Statics();
    const std::int32_t vertsX = static_cast<std::int32_t>(vertsX_);
    for (std::int32_t pass = 0; pass < iterations; ++pass) {
        for (std::int32_t z = r.top; z <= r.bottom; ++z) {
            for (std::int32_t x = r.left; x <= r.right; ++x) {
                const std::int32_t index = vertsX * z + x;
                const std::uint16_t flags = vertices_[static_cast<std::size_t>(index)].flags;
                const std::uint8_t mask = GetNeighbors(x, z);
                for (int dir = 0; dir < 8; ++dir) {
                    if (!((mask >> dir) & 1)) continue;
                    const std::int32_t neighbour = index + neighborOffsets_[dir];
                    const std::uint16_t neighbourFlags = vertices_[static_cast<std::size_t>(neighbour)].flags;
                    if (((flags | neighbourFlags) & mark) == 0) continue;
                    const float difference =
                        vertices_[static_cast<std::size_t>(index)].altitude -
                        vertices_[static_cast<std::size_t>(neighbour)].altitude;
                    const float threshold = (dir & 1) ? statics.diagonalHeightDifferenceThreshold
                                                      : statics.crossHeightDifferenceThreshold;
                    if (threshold < difference) {
                        const float amount = (difference - threshold) * statics.windErosionFactor;
                        if ((flags & mark) != 0) LowerVertex(static_cast<std::uint32_t>(index), amount);
                        if ((neighbourFlags & mark) != 0)
                            RaiseVertex(static_cast<std::uint32_t>(neighbour), amount);
                    } else if (difference < -threshold) {
                        const float amount = (-difference - threshold) * statics.windErosionFactor;
                        if ((flags & mark) != 0) RaiseVertex(static_cast<std::uint32_t>(index), amount);
                        if ((neighbourFlags & mark) != 0)
                            LowerVertex(static_cast<std::uint32_t>(neighbour), amount);
                    }
                }
            }
        }
    }
}

// ==================================================================================== brushes

std::map<std::uint32_t, TerrainBrushDescription>& cSTETerrain::BrushDescriptions() {
    static std::map<std::uint32_t, TerrainBrushDescription> map;   // sBrushDescriptionMap
    return map;
}

void cSTETerrain::ReleaseBrushDescriptions() { BrushDescriptions().clear(); }

float cSTETerrain::BrushRadius(std::uint32_t brushType) const {
    float radius = 32.0f;                                        // 0x42000000: the default radius
    const auto& map = BrushDescriptions();
    const auto it = map.find(brushType);
    if (it != map.end()) {
        // The original reads radiusPerZoom[zoom] with no bounds check; the port clamps (the vector holds
        // the five zoom entries the exemplar provides).
        const auto& radii = it->second.radiusPerZoom;
        if (!radii.empty()) radius = radii[zoom_ < radii.size() ? zoom_ : radii.size() - 1];
    }
    return radius;
}

std::int64_t cSTETerrain::GetCostPerBrushApplication(std::uint32_t brushType) const {
    const auto& map = BrushDescriptions();
    const auto it = map.find(brushType);
    if (it != map.end()) return static_cast<std::int64_t>(it->second.costPerZoom[zoom_ < 5 ? zoom_ : 4]);
    return 32;                                                   // 0x20: the default cost
}

bool cSTETerrain::GetUIElementsForBrush(std::uint32_t brushType, TerrainBrushUIElements& out) const {
    const auto& map = BrushDescriptions();
    const auto it = map.find(brushType);
    if (it == map.end()) return false;
    out = it->second.ui;
    return true;
}

bool cSTETerrain::GetBrushExemplarID(std::int32_t brushType, std::uint32_t& exemplarID) const {
    // The table at 0x100a29bf0 (one exemplar id per cISTETerrain::eBrushTypes value).
    static const std::uint32_t kExemplarIDForBrushType[8] = {0xea312ef9, 0xca313035, 0xea312f77, 0x2a312f97,
                                                             0xea313bb9, 0x8a31301c, 0x2a31304e, 0};
    exemplarID = kExemplarIDForBrushType[brushType & 7];
    return brushType != 7;   // eBrushType 7 (none) has no exemplar
}

void cSTETerrain::InitBrushParameters() {
    // Eight brush phases in [0, 2*pi) (+0x248..+0x264), drawn from the class's sRand.
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    for (float& phase : brushPhase_) phase = static_cast<float>(host->RandomDoubleRangeUniform(0.0, 6.2831854820251465));
}

// ------------------------------------------------------------------------------- the brush path

bool cSTETerrain::ApplyBrush(std::uint32_t brushType, TerrainBrushParameters& parameters) {
    // 0x100731c50: reset the dirty rect, look the brush up in sBrushDescriptionMap (a lower_bound walk in the
    // original) and apply every component. A non-empty rect is redisplaayed with the caller's flags; the
    // original's return value is false only when the brush is unknown.
    parameters.rect = kEmptyTerrainRect;
    const auto& map = BrushDescriptions();
    const auto it = map.find(brushType);
    if (it == map.end()) return false;
    ApplyBrushComponents(const_cast<TerrainBrushDescription&>(it->second), parameters);
    parameters.valid = true;
    if (parameters.valid && parameters.rect.left <= parameters.rect.right &&
        parameters.rect.top <= parameters.rect.bottom) {
        RedisplayTerrain(true, true, parameters.rect, parameters.redisplayFlags);   // vslot 0x228
    }
    return true;
}

void cSTETerrain::ApplyBrushComponents(TerrainBrushDescription& description, TerrainBrushParameters& parameters) {
    // 0x100731d10. The current point's cell is the brush centre; a zero-length drag keeps the previous
    // angle. Each component's radius/strength are scaled by the parameters, the texture is loaded lazily
    // and sampled per cell by ApplyBrushAtLocation.
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    const std::uint32_t cellX = static_cast<std::uint32_t>(parameters.currentX * 0.0625f);
    if (vertsX_ <= cellX) return;                                // +0x50 = vertsX
    const std::uint32_t cellZ = static_cast<std::uint32_t>(parameters.currentZ * 0.0625f);
    if (vertsZ_ <= cellZ) return;                                // +0x54 = vertsZ

    TerrainRect rect{static_cast<std::int32_t>(cellX), static_cast<std::int32_t>(cellZ),
                     static_cast<std::int32_t>(cellX), static_cast<std::int32_t>(cellZ)};

    TerrainBrushContext context;
    context.x = parameters.currentX;
    context.z = parameters.currentZ;
    context.previousX = parameters.previousX;
    context.previousZ = parameters.previousZ;
    context.zoom = parameters.zoom >= 0 ? parameters.zoom : static_cast<std::int32_t>(zoom_);
    context.levelAltitude = parameters.levelAltitude;
    context.parameters = &parameters;
    context.description = &description;

    if (description.rotateToDrag) {
        // one-in-five backwards: cRZRandom::RandomUint32UniformBelow(sRand, 5) == 0
        if (host->RandomUint32UniformBelow(5) == 0) {
            description.rotationPhase -= description.angleIncrement;
            if (description.rotationPhase < 0.0f) description.rotationPhase += 6.2831855f;
        } else {
            description.rotationPhase += description.angleIncrement;
            if (6.2831855f <= description.rotationPhase) description.rotationPhase -= 6.2831855f;
        }
    } else {
        description.rotationPhase = 0.0f;
    }
    if (parameters.currentX != parameters.previousX || parameters.currentZ != parameters.previousZ) {
        description.currentAngle = std::atan2(parameters.currentZ - parameters.previousZ,
                                              parameters.currentX - parameters.previousX) + 1.5707963f;
    }

    for (TerrainBrushComponent& component : description.components) {
        if (component.exemplarID != 0 && !component.resource) {
            // GetResource({0x7ab50e44, 0x891b0e1a, exemplarID}, 0x1ab50c3f, &component->resource). A failed
            // load aborts the whole brush in the original.
            component.resource = host->LoadBrushComponentResource(component.exemplarID);
            if (!component.resource) return;
        }
        void* textureInfo = component.resource ? host->BrushComponentTextureInfo(component.resource) : nullptr;
        const auto zoomIndex = static_cast<std::size_t>(context.zoom);
        const float radius = (zoomIndex < component.radiusPerZoom.size() ? component.radiusPerZoom[zoomIndex]
                                                                         : 0.0f) *
                             parameters.radiusScale;
        float strength = (zoomIndex < component.strengthPerZoom.size() ? component.strengthPerZoom[zoomIndex]
                                                                        : 0.0f) *
                         parameters.strengthScale;
        const float angle = component.useRotationAngle ? description.currentAngle : description.rotationPhase;
        if (component.type == 2 && 1.0f < strength) strength = 1.0f;   // the smoothen clamp
        if (component.type == 1) InitFloraBrushInfo();
        context.strength = strength;
        context.component = &component;

        using BrushFn = bool (cSTETerrain::*)(std::int32_t, const TerrainBrushContext&);
        BrushFn fn = &cSTETerrain::BrushFnRaiseLower;
        switch (component.type) {
        case 0:
            break;
        case 1:
            if (floraBrushCount_ != 0) {
                fn = &cSTETerrain::BrushFnFlora;
                break;
            }
            DoPostBrushApplicationOperation(component.type, context, rect);
            UnionRect(parameters.rect, parameters.rect, rect);
            continue;
        case 2:
            fn = &cSTETerrain::BrushFnSmoothen;
            break;
        case 3:
            fn = &cSTETerrain::BrushFnPaint;
            break;
        case 4:
            fn = &cSTETerrain::BrushFnPlaceEffect;
            break;
        case 5:
            fn = &cSTETerrain::BrushFnLevel;
            break;
        case 6:
            ApplyWaterErosionBrush(context, static_cast<std::int32_t>(strength * 0.0625f), rect);
            DoPostBrushApplicationOperation(component.type, context, rect);
            UnionRect(parameters.rect, parameters.rect, rect);
            continue;
        case 7:
            fn = &cSTETerrain::BrushFnVolcanoAsh;
            break;
        case 8:
            fn = &cSTETerrain::BrushFnLakeBed;
            break;
        case 9: {
            // cSTESurfaceWater::Rain returns the per-vertex brush to run, or null when Rain_Static did the work.
            const std::int32_t index = GetVertIndex(static_cast<std::int32_t>(cellX),
                                                    static_cast<std::int32_t>(cellZ));   // vslot 0x48
            if (!host->SurfaceWaterRain(index, context, rect, context.strength)) {
                DoPostBrushApplicationOperation(component.type, context, rect);
                UnionRect(parameters.rect, parameters.rect, rect);
                continue;
            }
            fn = &cSTETerrain::BrushFnRain;
            break;
        }
        case 10:
            host->SurfaceWaterCreateSpring(GetVertIndex(static_cast<std::int32_t>(cellX),
                                                        static_cast<std::int32_t>(cellZ)));
            DoPostBrushApplicationOperation(component.type, context, rect);
            UnionRect(parameters.rect, parameters.rect, rect);
            continue;
        default:
            DoPostBrushApplicationOperation(component.type, context, rect);
            UnionRect(parameters.rect, parameters.rect, rect);
            continue;
        }

        if (textureInfo) {
            ApplyBrushAtLocation(textureInfo, static_cast<std::int32_t>(cellX), static_cast<std::int32_t>(cellZ),
                                 radius, angle, fn, context, rect);
        } else {
            ApplyBrushAtLocation(static_cast<std::int32_t>(cellX), static_cast<std::int32_t>(cellZ), radius, angle,
                                 fn, 1, context, rect);
        }
        DoPostBrushApplicationOperation(component.type, context, rect);
        UnionRect(parameters.rect, parameters.rect, rect);
    }
}

void cSTETerrain::ApplyBrushAtLocation(void* textureInfo, std::int32_t cellX, std::int32_t cellZ, float radius,
                                       float angle,
                                       bool (cSTETerrain::*fn)(std::int32_t, const TerrainBrushContext&),
                                       TerrainBrushContext& context, TerrainRect& rect) {
    // 0x1007321b8: the brush is a rotated rectangle the size of the component's texture; every cell centre
    // is mapped back into texture space and the sampled byte goes to context.textureValue.
    ITerrainHost* host = TerrainHost();
    if (!host || !fn) return;
    std::uint32_t texWidth = 0, texHeight = 0;
    if (!host->BrushTextureInfoSize(textureInfo, texWidth, texHeight)) return;
    if (texWidth == 0 || texHeight == 0) return;
    const float widthScale = radius / static_cast<float>(texWidth);
    const float height = widthScale * static_cast<float>(texHeight);
    const float sinAngle = std::sin(angle);
    const float cosAngle = std::cos(angle);

    TerrainPoint quad[4];
    ComputeRectAroundPt(quad, static_cast<float>(cellX) * 16.0f, static_cast<float>(cellZ) * 16.0f, radius, height,
                        angle);
    int spanMin[260], spanMax[260];
    int firstRow = 0, lastRow = 0;
    ComputeSpansForRectangle(quad, spanMin, spanMax, firstRow, lastRow);
    rect = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    const std::uint8_t* pixels = host->BrushTexturePixels(textureInfo);
    if (!pixels || lastRow < firstRow) return;

    const float halfWidth = static_cast<float>(texWidth) * 0.5f;
    const float halfHeight = static_cast<float>(texHeight) * 0.5f;
    const float step = 16.0f / widthScale;                     // = 16 * texture width / radius
    const float deltaTx = -sinAngle * step;
    const float deltaTy = cosAngle * step;
    for (std::int32_t row = firstRow; row <= lastRow; ++row) {
        const std::int32_t firstColumn = spanMin[row];
        const std::int32_t lastColumn = spanMax[row];
        if (lastColumn < firstColumn) continue;
        float u = (static_cast<float>(firstColumn) - static_cast<float>(cellX)) * step;
        float v = (static_cast<float>(row) - static_cast<float>(cellZ)) * step;
        float tx = cosAngle * v - sinAngle * u + halfHeight;   // the texture row (index / width)
        float ty = sinAngle * v + cosAngle * u + halfWidth;    // the texture column
        for (std::int32_t column = firstColumn; column <= lastColumn; ++column) {
            const std::int32_t texelX = static_cast<std::int32_t>(tx);
            const std::int32_t texelY = static_cast<std::int32_t>(ty);
            if (static_cast<std::uint32_t>(texelX) < texHeight && static_cast<std::uint32_t>(texelY) < texWidth) {
                context.textureValue = pixels[texelX * static_cast<std::int32_t>(texWidth) + texelY];
                const std::int32_t index = GetVertIndex(column, row);   // vertsX * row + column
                if (static_cast<std::uint32_t>(index) >= totalVerts_) { tx += deltaTx; ty += deltaTy; continue; }   // port guard: never outside the grid
                if ((this->*fn)(index, context)) {
                    if (column < rect.left) rect.left = column;
                    if (column > rect.right) rect.right = column;
                    if (row < rect.top) rect.top = row;
                    if (row > rect.bottom) rect.bottom = row;
                }
            }
            tx += deltaTx;
            ty += deltaTy;
        }
    }
}

void cSTETerrain::ApplyBrushAtLocation(std::int32_t cellX, std::int32_t cellZ, float radius, float angle,
                                       bool (cSTETerrain::*fn)(std::int32_t, const TerrainBrushContext&),
                                       std::uint32_t repeat, TerrainBrushContext& context, TerrainRect& rect) {
    // 0x1007327de: the square (no texture) variant. The whole covered area is visited `repeat` times; the
    // original calls this with 1 and lets the script command repeat a scrub.
    if (!fn) return;
    TerrainPoint quad[4];
    ComputeRectAroundPt(quad, static_cast<float>(cellX) * 16.0f, static_cast<float>(cellZ) * 16.0f, radius, radius,
                        angle);
    int spanMin[260], spanMax[260];
    int firstRow = 0, lastRow = 0;
    ComputeSpansForRectangle(quad, spanMin, spanMax, firstRow, lastRow);
    rect = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (std::uint32_t pass = 0; pass < repeat; ++pass) {
        for (std::int32_t row = firstRow; row <= lastRow; ++row) {
            const std::int32_t firstColumn = spanMin[row];
            const std::int32_t lastColumn = spanMax[row];
            for (std::int32_t column = firstColumn; column <= lastColumn; ++column) {
                const std::int32_t index = GetVertIndex(column, row);
                if ((this->*fn)(index, context)) {
                    if (column < rect.left) rect.left = column;
                    if (column > rect.right) rect.right = column;
                    if (row < rect.top) rect.top = row;
                    if (row > rect.bottom) rect.bottom = row;
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------- the brush callbacks

bool cSTETerrain::BrushFnRain(std::int32_t index, const TerrainBrushContext& context) {
    // 0x10073083e: add strength/10 of water where the sampled texture value moved.
    if (!context.component) return false;
    if (context.textureValue != context.component->textureValue) {
        ITerrainHost* host = TerrainHost();
        if (host) host->SurfaceWaterAddRain(index, context.strength / 10.0f);
    }
    return false;   // the original always returns 0 (the surface water already accounted for the cell)
}

bool cSTETerrain::BrushFnRaiseLower(std::int32_t index, const TerrainBrushContext& context) {
    // 0x1007329cc: raise or lower by the texture difference times the component strength.
    if (!context.component) return false;
    const std::uint8_t wanted = context.textureValue;
    const std::uint8_t current = context.component->textureValue;
    if (wanted == current) return false;
    if (current < wanted) {
        RaiseVertex(static_cast<std::uint32_t>(index),
                    static_cast<float>(static_cast<std::int32_t>(wanted) - static_cast<std::int32_t>(current)) *
                        context.strength);
    } else {
        LowerVertex(static_cast<std::uint32_t>(index),
                    static_cast<float>(static_cast<std::int32_t>(current) - static_cast<std::int32_t>(wanted)) *
                        context.strength);
    }
    return true;
}

bool cSTETerrain::BrushFnFlora(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100732a0e: 31% of the cells whose texture value is above 9 get a flora instance at a random point in
    // the cell, but only on dry land (the surface water's depth at the vertex <= 0) and inside the grid.
    ITerrainHost* host = TerrainHost();
    if (!host) return false;
    if (context.textureValue <= 9) return false;
    if (host->RandomUint32UniformBelow(100) < 0x1f) {
        if (GetWaterDepthAtVertex(index) <= 0.0f) {
            const std::uint32_t row = static_cast<std::uint32_t>(index) / vertsX_;
            const std::uint32_t column = static_cast<std::uint32_t>(index) % vertsX_;
            if (column < cellsX_ && row < cellsZ_) {
                const float x = static_cast<float>(host->RandomDoubleRangeUniform(static_cast<double>(column) * 16.0,
                                                                                  static_cast<double>(column) * 16.0 + 16.0));
                const float z = static_cast<float>(host->RandomDoubleRangeUniform(static_cast<double>(row) * 16.0,
                                                                                  static_cast<double>(row) * 16.0 + 16.0));
                (void)host->RandomUint32UniformBelow(floraBrushCount_);   // the original picks an id and drops it
                const float y = GetAltitude(x, z);                        // vslot 0x60
                host->AddFloraAt(x, y, z);
                if (host->CityEstablished()) host->AddFloraAura(x, z);
            }
        } else {
            // the original posts "cannot plant here" through the sound server
            host->PostBrushSound(0xaa62c9d5, 0, 0);
        }
    }
    return false;
}

bool cSTETerrain::BrushFnSmoothen(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100732bd2: relax every existing neighbour that is further below than the talus threshold.
    if (context.textureValue == 0) return false;
    const float amount = context.strength;
    const std::uint32_t row = static_cast<std::uint32_t>(index) / vertsX_;
    const std::uint32_t column = static_cast<std::uint32_t>(index) % vertsX_;
    std::uint8_t neighbourMask = 0x7c;                       // west off at the first column
    if (0 < static_cast<std::int32_t>(column)) neighbourMask = 0xff;
    if (cellsX_ <= column) neighbourMask &= 0xc7;            // east off at the last column
    if (row < 1) neighbourMask &= 0xf1;                      // north off at the first row
    if (cellsZ_ <= row) neighbourMask &= 0x1f;               // south off at the last row
    bool changed = false;
    for (std::int32_t direction = 0; direction < 8; ++direction) {
        if ((neighbourMask & (1u << direction)) == 0) continue;
        const std::int32_t neighbour = neighborOffsets_[direction] + index;
        const float threshold = (direction & 1) ? Statics().diagonalHeightDifferenceThreshold
                                                : Statics().crossHeightDifferenceThreshold;
        float difference = vertices_[static_cast<std::size_t>(index)].altitude -
                           vertices_[static_cast<std::size_t>(neighbour)].altitude - threshold;
        if (0.0f < difference) {
            difference = difference * static_cast<float>(context.textureValue) * amount * 0.00390625f;
            LowerVertex(static_cast<std::uint32_t>(index), difference);
            RaiseVertex(static_cast<std::uint32_t>(neighbour), difference);
            changed = true;
        }
    }
    return changed;
}

bool cSTETerrain::BrushFnPaint(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100732d22: add one decal to the view's third overlay manager, at most maxDecals per cell.
    ITerrainHost* host = TerrainHost();
    if (!host || !overlayManagers_[2] || !context.component) return false;
    if (context.textureValue == 0) return false;
    const std::uint32_t row = static_cast<std::uint32_t>(index) / vertsX_;
    const std::uint32_t column = static_cast<std::uint32_t>(index) % vertsX_;
    if (cellsX_ <= column || cellsZ_ <= row) return false;
    std::uint32_t& count = decalMap_[static_cast<std::uint32_t>(index)];
    if (context.component->maxDecals < count) return false;
    ++count;
    const cGZPersistResourceKey key{0x7ab50e44, 0x891b0e1a, context.component->decalInstance};
    float randomAmount = 0.0f;
    if (0.0f < context.component->randomAmount) {
        randomAmount = static_cast<float>(host->RandomDoubleRangeUniform(0.0, context.component->randomAmount));
    }
    const float x = static_cast<float>(host->RandomDoubleRangeUniform(
        static_cast<double>(column) * 16.0,
        static_cast<double>(widthMeters_ < static_cast<float>(column) * 16.0f + 16.0f
                                ? widthMeters_
                                : static_cast<float>(column) * 16.0f + 16.0f)));
    const float z = static_cast<float>(host->RandomDoubleRangeUniform(
        static_cast<double>(row) * 16.0,
        static_cast<double>(depthMeters_ < static_cast<float>(row) * 16.0f + 16.0f
                                ? depthMeters_
                                : static_cast<float>(row) * 16.0f + 16.0f)));
    host->AddOverlayDecal(overlayManagers_[2], key, TerrainPoint{x, z}, context.strength, randomAmount);
    return false;
}

bool cSTETerrain::BrushFnPlaceEffect(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100732f92: create one effect instance on the cell, at a random point inside it.
    ITerrainHost* host = TerrainHost();
    if (!host || !context.component) return false;
    if (context.textureValue == context.component->textureValue) return false;
    const std::uint32_t row = static_cast<std::uint32_t>(index) / vertsX_;
    const std::uint32_t column = static_cast<std::uint32_t>(index) % vertsX_;
    std::uint32_t& count = decalMap_[static_cast<std::uint32_t>(index)];
    if (context.component->maxDecals < count) return false;
    ITerrainEffect* effect = host->CreateTerrainEffect(context.component->effectID[0],
                                                       context.component->effectID[1]);
    if (!effect) return false;
    ++count;
    const float x = static_cast<float>(host->RandomDoubleRangeUniform(
        static_cast<double>(column) * 16.0,
        static_cast<double>(widthMeters_ < static_cast<float>(column) * 16.0f + 16.0f
                                ? widthMeters_
                                : static_cast<float>(column) * 16.0f + 16.0f)));
    const float z = static_cast<float>(host->RandomDoubleRangeUniform(
        static_cast<double>(row) * 16.0,
        static_cast<double>(depthMeters_ < static_cast<float>(row) * 16.0f + 16.0f
                                ? depthMeters_
                                : static_cast<float>(row) * 16.0f + 16.0f)));
    TerrainEffectPlacement placement;
    placement.flags = 1;                                     // the original sets byte 0 and bit 0 of byte 1
    placement.pad[0] = 1;
    placement.x = x;
    placement.y = std::max(GetAltitude(x, z), GetSeaLevel());
    placement.z = z;
    effect->SetPlacement(placement);
    effect->Start();
    effect->Release();
    return false;
}

bool cSTETerrain::BrushFnLevel(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100733230: drive the vertex toward context.levelAltitude, by at most textureValue * strength / 256.
    if (context.textureValue == 0) return false;
    const float step = static_cast<float>(context.textureValue) * context.strength * 0.00390625f;
    const float difference = vertices_[static_cast<std::size_t>(index)].altitude - context.levelAltitude;
    if (0.0f < difference) {
        if (difference <= step) {
            SetAltitude(index, context.levelAltitude);        // vslot 0x188
        } else {
            LowerVertex(static_cast<std::uint32_t>(index), step);
        }
    } else if (difference < 0.0f) {
        if (-step <= difference) {
            SetAltitude(index, context.levelAltitude);
        } else {
            RaiseVertex(static_cast<std::uint32_t>(index), step);
        }
    } else {
        return false;
    }
    return true;
}

bool cSTETerrain::BrushFnVolcanoAsh(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100733338: mark the vertex as covered by volcanic ash (flag 0x400).
    if (context.textureValue == 0) return false;
    vertices_[static_cast<std::size_t>(index)].flags |= 0x400;   // +0x22
    return true;
}

bool cSTETerrain::BrushFnLakeBed(std::int32_t index, const TerrainBrushContext& context) {
    // 0x100733360: move the vertex toward levelAltitude + difference*strength; land is only raised and water
    // is only lowered (the original tests the surface-water depth before choosing).
    if (!context.component) return false;
    const auto& strengths = context.component->strengthPerZoom;
    const float strength = static_cast<std::size_t>(context.zoom) < strengths.size()
                               ? strengths[static_cast<std::size_t>(context.zoom)]
                               : 0.0f;
    const float amount = static_cast<float>(static_cast<std::int32_t>(context.textureValue) -
                                            static_cast<std::int32_t>(context.component->textureValue)) *
                         strength;
    if (amount == 0.0f) return false;
    const float level = context.levelAltitude + amount;
    const float magnitude = std::fabs(amount * 0.0625f);
    const float difference = level - vertices_[static_cast<std::size_t>(index)].altitude;
    if (0.0f < difference) {
        if (0.0f < waterDepth_[static_cast<std::size_t>(index)]) return false;   // under water: no fill
        if (magnitude <= difference) {
            RaiseVertex(static_cast<std::uint32_t>(index), amount);
        } else {
            SetAltitude(index, level);
        }
        return true;
    }
    if (difference < 0.0f) {
        if (magnitude > -difference) {
            SetAltitude(index, level);
        } else {
            LowerVertex(static_cast<std::uint32_t>(index), amount);
        }
        return true;
    }
    return false;
}

bool cSTETerrain::GetTextureValue(std::int32_t cellX, std::int32_t cellZ, TerrainBrushContext& context) {
    // 0x100733436: map the cell centre into the component's texture using the current brush angle.
    ITerrainHost* host = TerrainHost();
    if (!host || !context.component || !context.parameters || !context.description) return false;
    void* textureInfo = context.component->resource ? host->BrushComponentTextureInfo(context.component->resource)
                                                    : nullptr;
    if (!textureInfo) return false;
    std::uint32_t width = 0, height = 0;
    if (!host->BrushTextureInfoSize(textureInfo, width, height)) return false;
    const auto& radii = context.component->radiusPerZoom;
    const float radius = (static_cast<std::size_t>(context.zoom) < radii.size()
                              ? radii[static_cast<std::size_t>(context.zoom)]
                              : 0.0f) *
                         context.parameters->radiusScale;
    if (radius == 0.0f || width == 0) return false;
    const float scale = 16.0f / (radius / static_cast<float>(width));
    const float angle = context.component->useRotationAngle ? context.description->currentAngle
                                                            : context.description->rotationPhase;
    const float u = (static_cast<float>(cellX) - static_cast<float>(static_cast<std::int32_t>(context.x * 0.0625f))) *
                    scale;
    const float v = (static_cast<float>(cellZ) - static_cast<float>(static_cast<std::int32_t>(context.z * 0.0625f))) *
                    scale;
    const float sinAngle = std::sin(angle);
    const float cosAngle = std::cos(angle);
    const std::int32_t texelX = static_cast<std::int32_t>(sinAngle * v + cosAngle * u +
                                                          static_cast<float>(width) * 0.5f);
    const std::int32_t texelY = static_cast<std::int32_t>(cosAngle * v - sinAngle * u +
                                                          static_cast<float>(height) * 0.5f);
    if (static_cast<std::uint32_t>(texelX) < width && static_cast<std::uint32_t>(texelY) < height) {
        const std::uint8_t* pixels = host->BrushTexturePixels(textureInfo);
        if (!pixels) return false;
        context.textureValue = pixels[texelY * static_cast<std::int32_t>(width) + texelX];
        return true;
    }
    return false;
}

void cSTETerrain::ApplyWaterErosionBrush(TerrainBrushContext& context, std::int32_t pad, TerrainRect& rect) {
    // 0x1007332ae: the rect is the brush's cell padded by half the flow distance, then one DoWaterErosion per
    // strength unit with the zoom's flow-step count.
    const std::int32_t cellX = static_cast<std::int32_t>(context.x * 0.0625f);
    const std::int32_t cellZ = static_cast<std::int32_t>(context.z * 0.0625f);
    rect = {cellX, cellZ, cellX, cellZ};
    PadRectangle(rect, pad / 2);
    const std::int32_t iterations = static_cast<std::int32_t>(context.strength);
    const auto& steps = Statics().numFlowStepsPerWaterErosionBrushApplication;
    const std::uint32_t stepIndex = static_cast<std::uint32_t>(context.zoom);
    const std::int32_t flowSteps = stepIndex < steps.size() ? static_cast<std::int32_t>(steps[stepIndex]) : 0;
    for (std::int32_t i = 0; i < iterations; ++i) DoWaterErosion(rect, flowSteps);
}

void cSTETerrain::DoPostBrushApplicationOperation(std::int32_t componentType, TerrainBrushContext& context,
                                                  TerrainRect& rect) {
    // 0x10073340e: the volcano-ash brush registers the rect, the smoothen brush widens it by one cell.
    (void)context;
    if (componentType == 7) {
        AddVolcanicAshRegion(rect);
        return;
    }
    if (componentType == 2) PadRectangle(rect, 1);
}

void cSTETerrain::InitFloraBrushInfo() {
    // 0x1007385e2: the flora-instance ids the brush may place, minus the ones whose occupant is not brushable
    // (occupant +0x180 & 3 == 2 keeps only the "flora" occupants).
    if (floraBrushCount_ != 0) return;
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    const std::uint32_t count = host->FloraBrushCount();
    floraBrushIDs_.assign(count, 0);
    if (count != 0) host->FloraBrushList(floraBrushIDs_);
    floraBrushCount_ = count;
    for (std::uint32_t i = 0; i < floraBrushCount_;) {
        if (i < floraBrushIDs_.size() && host->FloraOccupantIsBrushable(floraBrushIDs_[i])) {
            ++i;
        } else {
            if (i < floraBrushIDs_.size()) floraBrushIDs_.erase(floraBrushIDs_.begin() + static_cast<std::ptrdiff_t>(i));
            --floraBrushCount_;
        }
    }
    floraBrushIDs_.resize(floraBrushCount_);
}

void cSTETerrain::BeginBrushScrub(std::uint32_t brushType) {
    // 0x100734a7e: drop the decal map, post the brush sound and begin the long terrain operation.
    decalMap_.clear();                                        // +0x268
    ITerrainHost* host = TerrainHost();
    const auto& map = BrushDescriptions();
    const auto it = map.find(brushType);
    if (host && it != map.end()) {
        if (it->second.exemplarID != 0) host->PostBrushSound(0xea451903, it->second.exemplarID, it->second.brushID);
    }
    BeginLongTerrainOperation();                              // vslot 0x2b8
}

void cSTETerrain::EndBrushScrub(std::uint32_t brushType, std::uint32_t arg) {
    // 0x100738704: drop the decal map, post the two end sounds, end the surface-water scrub and the long
    // terrain operation.
    decalMap_.clear();
    ITerrainHost* host = TerrainHost();
    const auto& map = BrushDescriptions();
    const auto it = map.find(brushType);
    if (host && it != map.end()) {
        if (it->second.exemplarID != 0) host->PostBrushSound(0x6a451908, it->second.brushID, 0);
        if (it->second.soundID != 0) host->PostBrushSound(0xea451903, it->second.soundID, it->second.brushID + 1);
    }
    if (host) host->SurfaceWaterEndBrushScrub(brushType, arg);
    EndLongTerrainOperation();                                // vslot 0x2c0
}

// ------------------------------------------------------------------------------- the brush readers

bool cSTETerrain::ReadBrushWholeProperties(cISCPropertyHolder* holder, TerrainBrushDescription& description) {
    // 0x10073b26a: the angle, the five per-zoom costs and the two ids.
    float angle = 0.0f;
    float phase = 0.0f;
    if (!ReadPropertyFloat(holder, 0x8a020102, angle)) {
        description.rotateToDrag = true;
        std::uint32_t bits = 0x3c0efa35;                       // the binary's fallback (0.5 degree in radians)
        std::memcpy(&description.angleIncrement, &bits, sizeof(bits));
        if (ITerrainHost* host = TerrainHost())
            phase = static_cast<float>(host->RandomDoubleRangeUniform(0.0, 6.2831854820251465));
    } else {
        description.rotateToDrag = angle != 0.0f;
        description.angleIncrement = angle * 0.017453292f;
        if (angle != 0.0f) {
            if (ITerrainHost* host = TerrainHost())
                phase = static_cast<float>(host->RandomDoubleRangeUniform(0.0, 6.2831854820251465));
        }
    }
    description.rotationPhase = phase;                        // +0x84
    description.currentAngle = 0.0f;                          // +0x88
    std::vector<float> costs;
    if (ReadPropertyFloatVector(holder, 0x8a020101, costs)) {
        for (std::size_t i = 0; i < 5 && i < costs.size(); ++i) description.costPerZoom[i] = costs[i];
    } else {
        for (float& cost : description.costPerZoom) cost = 0.0f;
    }
    if (!ReadPropertyUint32(holder, 0x8a020103, description.exemplarID)) description.exemplarID = 0;
    if (!ReadPropertyUint32(holder, 0x8a020104, description.soundID)) description.soundID = 0;
    return true;
}

bool cSTETerrain::ReadBrushUIProperties(cISCPropertyHolder* holder, TerrainBrushDescription& description) {
    // 0x10073b3b8: four effect-id pairs and two uints. The defaults are the exemplar's own values.
    // the four defaults are 8-byte writes: {0xea3169e6, 2}, {.., 1}, {.., 4}, {.., 3}
    static const std::uint32_t kDefaults[4][2] = {{0xea3169e6, 2}, {0xea3169e6, 1},
                                                  {0xea3169e6, 4}, {0xea3169e6, 3}};
    for (int i = 0; i < 4; ++i) {
        std::uint32_t pair[2] = {0, 0};
        if (!GetEffectIDProperty(holder, 0xaa2ed800u + static_cast<std::uint32_t>(i), pair)) {
            pair[0] = kDefaults[i][0];
            pair[1] = kDefaults[i][1];
        }
        description.ui.values[i * 2] = pair[0];
        description.ui.values[i * 2 + 1] = pair[1];
    }
    if (!ReadPropertyUint32(holder, 0xaa2ed804, description.ui.values[8])) description.ui.values[8] = 0x71e5700;
    if (!ReadPropertyUint32(holder, 0xaa2ed805, description.ui.values[9])) description.ui.values[9] = 0x71e5705;
    return true;
}

bool cSTETerrain::ReadBrushComponentProperties(cISCPropertyHolder* holder, TerrainBrushDescription& description) {
    // 0x10073aafe: up to four component slots (property ids stride by 0x100); the loop stops at the first
    // slot without a type property, and the description is dropped when a required property is missing.
    description.components.clear();
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        const std::uint32_t base = slot * 0x100;
        std::uint32_t type = 0;
        if (!ReadPropertyUint32(holder, base + 0x8a020205, type)) break;
        if (11 < type) return false;
        std::vector<float> radius, strength;
        if (!ReadPropertyFloatVector(holder, base + 0x8a020202, radius)) return false;
        if (!ReadPropertyFloatVector(holder, base + 0x8a020203, strength)) return false;
        // each slot's array only raises the description's first five per-zoom radii (the original walks
        // exactly five entries and keeps the larger value)
        for (std::size_t i = 0; i < 5 && i < radius.size(); ++i) {
            if (description.radiusPerZoom[i] <= radius[i] && description.radiusPerZoom[i] != radius[i])
                description.radiusPerZoom[i] = radius[i];
        }

        std::uint8_t useRotation = 0;
        ReadPropertyUint8(holder, base + 0x8a020201, useRotation);
        TerrainBrushComponent component;
        component.type = static_cast<std::int32_t>(type);
        component.useRotationAngle = useRotation != 0;
        component.radiusPerZoom = radius;
        component.strengthPerZoom = strength;
        if (type == 6 || type == 10) {
            component.exemplarID = 0;
            component.textureValue = 0;
            component.maxDecals = 0;
            component.decalInstance = 0;
            component.randomAmount = 6.2831855f;
        } else {
            if (!ReadPropertyUint32(holder, base + 0x8a020200, component.exemplarID)) return false;
            std::uint8_t value = 0;
            if (!ReadPropertyUint8(holder, base + 0x8a020204, value)) return false;
            component.textureValue = value;
            if (type == 3 || type == 4) {
                if (!ReadPropertyUint32(holder, base + 0x8a020208, component.maxDecals)) return false;
            } else {
                component.maxDecals = 0;
            }
            if (type == 3) {
                if (!ReadPropertyUint32(holder, base + 0x8a020206, component.decalInstance)) return false;
                float degrees = 0.0f;
                if (!ReadPropertyFloat(holder, base + 0x8a020207, degrees)) return false;
                component.randomAmount = degrees / 360.0f;
            } else {
                component.decalInstance = 0;
                component.randomAmount = 6.2831855f;
            }
        }
        if (type == 4) {
            if (!GetEffectIDProperty(holder, base + 0x8a020209, component.effectID)) return false;
        } else {
            component.effectID[0] = component.effectID[1] = 0;
        }
        description.components.push_back(std::move(component));
    }
    return true;
}

void cSTETerrain::ReadBrushDescription(const cGZPersistResourceKey& key, void* unused) {
    // 0x10073aefe: load the exemplar's property holder, build the description and insert it into the map
    // (the original inserts a default first and erases it again when the component reader fails).
    (void)unused;
    auto& map = BrushDescriptions();
    if (map.count(key.instance) != 0) return;
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    cISCPropertyHolder* holder = host->GetPropertyHolder(key);
    if (!holder) return;
    TerrainBrushDescription description;
    description.brushID = key.instance;
    description.radiusPerZoom.assign(5, 0.0f);
    ReadBrushWholeProperties(holder, description);
    if (!ReadBrushComponentProperties(holder, description)) {
        holder->Release();
        return;
    }
    ReadBrushUIProperties(holder, description);
    holder->Release();
    map[key.instance] = std::move(description);
}

void cSTETerrain::ReadBrushDescriptions() {
    // 0x10073b49c: every resource of {0x6534284a, 0x6a01fc2a} (the brush exemplars), through the resource
    // manager's enumerator (vslot 0x88 with a cSCResFilterFixedGroupAndTypeId).
    ReleaseBrushDescriptions();
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    std::vector<cGZPersistResourceKey> keys;
    host->ListBrushResources(keys);
    for (const cGZPersistResourceKey& key : keys) ReadBrushDescription(key, nullptr);
}

// ==================================================================================== volcanic ash

void cSTETerrain::AddVolcanicAshRegion(TerrainRect& r) {
    // 0x100731628: union every touching region into `r` (the original unlinks and deletes them), then push
    // the union as a fresh region at the end of the list.
    if (r.right < r.left || r.bottom < r.top) return;
    for (auto it = volcanicAshRegions_.begin(); it != volcanicAshRegions_.end();) {
        const TerrainRect& other = it->rect;
        if (other.right < r.left || r.right < other.left || other.bottom < r.top || r.bottom < other.top) {
            ++it;
            continue;
        }
        UnionRect(r, r, other);
        it = volcanicAshRegions_.erase(it);
    }
    tVolcanicAshRegion region;
    region.rect = r;
    region.age = 0;
    region.updateTimer = 0;
    volcanicAshRegions_.push_back(region);
}

void cSTETerrain::SprayVolcanicAsh(const TerrainRect& r) {
    // 0x100734724: every ash-marked vertex (flag 0x400) in the regions spreads the volcanic-ash texture
    // (0xe0) over its neighbourhood, scaled by the region's remaining lifetime and the texture's weight.
    if (volcanicAshRegions_.empty()) return;
    auto& textures = SurfaceCoverTextures();
    const std::uint32_t ashTexture = 0xe0;                    // kVolcanicAshTexture
    const auto texture = textures.find(ashTexture);
    if (texture == textures.end()) return;
    const TerrainSurfaceCoverInfo& info = texture->second;
    const std::uint32_t zoomIndex = zoom_ < 5 ? zoom_ : 4;
    const std::int32_t spread = info.maxSpread[zoomIndex];
    const float lifetime = Statics().volcanicAshLifeTime;
    for (const tVolcanicAshRegion& region : volcanicAshRegions_) {
        TerrainRect area{r.left - spread, r.top - spread, r.right + spread, r.bottom + spread};
        if (area.left < 0) area.left = 0;
        if (area.top < 0) area.top = 0;
        if (static_cast<std::uint32_t>(area.right) > cellsX_) area.right = static_cast<std::int32_t>(cellsX_);
        if (static_cast<std::uint32_t>(area.bottom) > cellsZ_) area.bottom = static_cast<std::int32_t>(cellsZ_);
        TerrainRect overlap{};
        if (!IntersectTerrainRect(overlap, area, region.rect)) continue;
        if (overlap.left == region.rect.left && overlap.top == region.rect.top &&
            overlap.right == region.rect.right && overlap.bottom == region.rect.bottom) {
            const_cast<tVolcanicAshRegion&>(region).updateTimer = 0;
        }
        for (std::int32_t z = overlap.top; z <= overlap.bottom; ++z) {
            for (std::int32_t x = overlap.left; x <= overlap.right; ++x) {
                const std::int32_t index = GetVertIndex(x, z);
                if ((vertices_[static_cast<std::size_t>(index)].flags & 0x400) == 0) continue;
                TerrainRect local{x - spread, z - spread, x + spread, z + spread};
                if (local.left < 0) local.left = 0;
                if (local.top < 0) local.top = 0;
                if (static_cast<std::uint32_t>(local.right) > cellsX_) local.right = static_cast<std::int32_t>(cellsX_);
                if (static_cast<std::uint32_t>(local.bottom) > cellsZ_) local.bottom = static_cast<std::int32_t>(cellsZ_);
                TerrainRect clipped{};
                if (!IntersectTerrainRect(clipped, local, r)) continue;
                for (std::int32_t nz = clipped.top; nz <= clipped.bottom; ++nz) {
                    for (std::int32_t nx = clipped.left; nx <= clipped.right; ++nx) {
                        const float dx = static_cast<float>(nx - x);
                        const float dz = static_cast<float>(nz - z);
                        float amount = EvaluateCurve(info.curves[zoomIndex], std::sqrt(dx * dx + dz * dz) * 16.0f);
                        amount = amount * (1.0f - static_cast<float>(region.age) / lifetime) * info.weight;
                        if (0.0f < amount) AddLandTypeAtVertex(GetVertIndex(nx, nz), ashTexture, amount);
                    }
                }
            }
        }
    }
}

void cSTETerrain::TickVolcanicAshRegions(std::uint32_t elapsedMs) {
    // 0x1007379be: age every region, re-spray on the update interval, and remove (and clear) the expired
    // ones; the union of everything touched is re-textured and redisplaayed.
    if (volcanicAshRegions_.empty()) return;
    TerrainRect dirty{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (auto it = volcanicAshRegions_.begin(); it != volcanicAshRegions_.end();) {
        const std::uint32_t previousAge = it->age;
        it->age = previousAge + elapsedMs;
        if (it->age < Statics().volcanicAshLifeTime) {
            const std::uint32_t previousTimer = it->updateTimer;
            it->updateTimer = previousTimer + elapsedMs;
            if (Statics().volcanicAshUpdateInterval <= static_cast<float>(previousTimer + elapsedMs)) {
                it->updateTimer = 0;
                UnionRect(dirty, it->rect, dirty);
            }
            ++it;
        } else {
            UnionRect(dirty, it->rect, dirty);
            ClearVertexPropertyFlags(it->rect, 0x400);
            it = volcanicAshRegions_.erase(it);
        }
    }
    if (dirty.left <= dirty.right && dirty.top <= dirty.bottom) {
        AssignSurfaceTexturesAndColors(dirty);
        RedisplayTerrain(false, false, dirty, 0);             // vslot 0x228
    }
}

// ==================================================================================== water waves

bool cSTETerrain::IsGoodWaveProducerLocation(float x, float z, float orientation) const {
    // 0x10073a4f6: a wave rectangle (kMaxWaveWidth wide, kWaveSeaTravelDist long, rotated by the
    // orientation) must lie inside the terrain and be 80% sea; the same rectangle shifted by
    // kMaxWaveTravelDist must also be 10% land.
    if (!waveSystem_) return false;
    ITerrainWaveSystem* waves = waveSystem_;
    const float seaTravel = waves->WaveSeaTravelDistance();
    const float halfWidth = waves->MaxWaveWidth() * 0.5f;
    const float maxTravel = waves->MaxWaveTravelDistance();
    const float cosSide = std::cos(orientation + 1.5707964f) * halfWidth;
    const float sinSide = std::sin(orientation + 1.5707964f) * halfWidth;
    const float cosDirection = std::cos(orientation);
    const float sinDirection = std::sin(orientation);
    auto inside = [this](const TerrainPoint* quad) {
        for (int i = 0; i < 4; ++i) {
            if (quad[i].x < 0.0f || quad[i].x >= widthMeters_) return false;
            if (quad[i].z < 0.0f || quad[i].z >= depthMeters_) return false;
        }
        return true;
    };
    TerrainPoint quad[4] = {
        {x + cosSide, z + sinSide},
        {x + seaTravel * cosDirection + cosSide, z + seaTravel * sinDirection + sinSide},
        {x + seaTravel * cosDirection - cosSide, z + seaTravel * sinDirection - sinSide},
        {x - cosSide, z - sinSide},
    };
    if (!inside(quad)) return false;
    if (!ComputeBooleanFnForRect(quad, &cSTETerrain::IsSeaVertex, 0.8f)) return false;

    TerrainPoint shifted[4] = {
        {quad[0].x + maxTravel * cosDirection, quad[0].z + maxTravel * sinDirection},
        {quad[1].x + maxTravel * cosDirection, quad[1].z + maxTravel * sinDirection},
        {quad[2].x + maxTravel * cosDirection, quad[2].z + maxTravel * sinDirection},
        {quad[3].x + maxTravel * cosDirection, quad[3].z + maxTravel * sinDirection},
    };
    if (!inside(shifted)) return false;
    return ComputeBooleanFnForRect(shifted, &cSTETerrain::IsLandVertex, 0.1f);
}

bool cSTETerrain::TryToCreateWaveProducerForShore(std::deque<std::int32_t>& shore, int first, int last,
                                                  float& outX, float& outZ, float& outOrientation) {
    // 0x10073eb1e: orient the wave along the shore run's chord, reject it when it is too far from the mean
    // wave orientation, and place the producer `shore distance` back from the run's middle.
    if (!waveSystem_) return false;
    const float shoreDistance = waveSystem_->DistanceFromShoreToWaveStart();
    const float maxOrientationDifference = waveSystem_->MaxOrientationDifference();
    float meanOrientation = 0.0f;
    const bool hasMean = waveSystem_->GetMeanWaveOrientation(meanOrientation);
    const std::int32_t firstVertex = shore[static_cast<std::size_t>(first)];
    const std::int32_t lastVertex = shore[static_cast<std::size_t>(last)];
    const float dx = static_cast<float>((lastVertex % static_cast<std::int32_t>(vertsX_)) * 16 -
                                        (firstVertex % static_cast<std::int32_t>(vertsX_)) * 16);
    const float dz = static_cast<float>((lastVertex / static_cast<std::int32_t>(vertsX_)) * 16 -
                                        (firstVertex / static_cast<std::int32_t>(vertsX_)) * 16);
    float angle = std::atan2(dz, dx) + 6.2831855f;
    if (6.2831855f <= angle) angle -= 6.2831855f;
    angle += 1.5707964f;
    if (6.2831855f <= angle) angle -= 6.2831855f;
    outOrientation = angle;
    if (hasMean) {
        float difference = std::fabs(angle - meanOrientation);
        if (3.1415927f < difference) difference = 6.2831855f - difference;
        if (maxOrientationDifference < difference && maxOrientationDifference < 6.2831855f - difference)
            return false;
    }
    const std::int32_t middle = (first + last) / 2;
    const std::int32_t middleVertex = shore[static_cast<std::size_t>(middle)];
    const float middleX = static_cast<float>((middleVertex % static_cast<std::int32_t>(vertsX_)) * 16);
    const float middleZ = static_cast<float>((middleVertex / static_cast<std::int32_t>(vertsX_)) * 16);
    outX = middleX - std::cos(angle) * shoreDistance;
    outZ = middleZ - std::sin(angle) * shoreDistance;
    return IsGoodWaveProducerLocation(outX, outZ, outOrientation);   // vslot 0x208
}

void cSTETerrain::UpdateWaveProducers() {
    // 0x10073ed40: walk every shore vertex, cut each shore run into segments and try to place a producer.
    if (!waveSystem_) return;
    std::int32_t produced = waveSystem_->ProducerCount();
    const std::int32_t maximum = waveSystem_->MaxProducerCount();
    if (maximum <= produced) return;
    const float gap = waveSystem_->InterWaveGap();
    const float widest = waveSystem_->MaxWaveWidth() * 1.5f;
    const std::int32_t gapCells = static_cast<std::int32_t>(gap + 0.5f) >> 4;
    const std::int32_t widthCells = static_cast<std::int32_t>(widest + 0.5f) >> 4;
    const std::int32_t halfWidthCells = static_cast<std::int32_t>(widest + 0.5f) >> 5;
    // PORT-ONLY guard: the real constants make both steps positive (340*1.5 -> 31 and 15 cells); a
    // test-sized wave tuning could round them to zero and the original's do-while would never advance.
    if (widthCells <= 0 || halfWidthCells <= 0) return;
    DetectShoreVertices();
    if (vertsZ_ == 0) return;
    for (std::uint32_t row = 0; row < vertsZ_; ++row) {
        for (std::uint32_t column = 0; column < vertsX_; ++column) {
            const std::int32_t index = GetVertIndex(static_cast<std::int32_t>(column), static_cast<std::int32_t>(row));
            if ((vertices_[static_cast<std::size_t>(index)].flags & 0x11) != 0x10) continue;
            std::deque<std::int32_t> shore;
            GetShoreStartingAtVert(index, shore, 1);
            const std::int32_t length = static_cast<std::int32_t>(shore.size());
            if (length <= 0) continue;
            std::int32_t first = 0;
            std::int32_t next = widthCells;
            while (next < length) {
                float x = 0, z = 0, orientation = 0;
                if (!TryToCreateWaveProducerForShore(shore, first, next, x, z, orientation) ||
                    !waveSystem_->AddProducer(x, z, orientation)) {
                    first += halfWidthCells;
                    next += halfWidthCells;
                } else {
                    ++produced;
                    if (maximum <= produced) {
                        for (std::uint32_t i = 0; i < totalVerts_; ++i)
                            vertices_[i].flags &= static_cast<std::uint16_t>(~1u);
                        return;
                    }
                    first = next + gapCells;
                    next = first + widthCells;
                }
            }
        }
    }
    for (std::uint32_t i = 0; i < totalVerts_; ++i) vertices_[i].flags &= static_cast<std::uint16_t>(~1u);
}

// ==================================================================================== create / image import

bool cSTETerrain::CreateFromGrayScaleImage(ITerrainHeightImage* image) {
    // 0x100735f50: sample the image over the vertex grid (the original rounds the fractional source
    // coordinate), scale by kImageImportScaleFactor and release the buffer.
    if (!image) return false;
    const std::uint32_t width = image->Width();
    if (width == 0) return false;
    const std::uint32_t height = image->Height();
    if (height == 0 || vertsZ_ == 0) return false;
    float sourceZ = 0.0f;
    const float stepZ = static_cast<float>(height) / static_cast<float>(vertsZ_);
    for (std::uint32_t z = 0; z < vertsZ_; ++z) {
        const std::int32_t imageZ = static_cast<std::int32_t>(sourceZ + 0.5f);
        float sourceX = 0.0f;
        const float stepX = static_cast<float>(width) / static_cast<float>(vertsX_);
        for (std::uint32_t x = 0; x < vertsX_; ++x) {
            const std::int32_t imageX = static_cast<std::int32_t>(sourceX + 0.5f);
            vertices_[z * vertsX_ + x].altitude =
                static_cast<float>(image->Gray(static_cast<std::uint32_t>(imageX),
                                               static_cast<std::uint32_t>(imageZ))) *
                Statics().imageImportScaleFactor;
            sourceX += stepX;
        }
        sourceZ += stepZ;
    }
    image->Release();
    createdFromImage_ = true;                                 // +0xa0
    return true;
}

bool cSTETerrain::CreateFromGrayScaleImage(const cRZString& path) {
    // 0x10073610c: load the named height image through the GZCOM image loader and import it.
    ITerrainHost* host = TerrainHost();
    if (!host) return false;
    ITerrainHeightImage* image = host->LoadHeightImage(path);
    if (!image) return false;
    return CreateFromGrayScaleImage(image);
}

bool cSTETerrain::Create() {
    // 0x100735cfc: the first-boot terrain. The new-city specification either carries an obliterated
    // segment (loaded as-is), a height image (imported), a name (loaded from file) or nothing (a flat city
    // at the region's base terrain height). Everything then gets eroded and reconciled with the neighbours.
    ITerrainHost* host = TerrainHost();
    if (!host) return false;
    const cRZString name = host->NewCityName();
    if (cIGZPersistDBSegment* segment = host->NewCityObliteratedTerrainSegment()) {
        LoadObliterated(segment);                             // vslot 0x300
        ResetWaterToSeaLevel(gridRect_);
    } else {
        ITerrainHeightImage* image = host->NewCityHeightImage();
        bool fromImage = false;
        if (name.Strlen() == 0) {
            if (!image) {
                float height = Statics().defaultTerrainHeight;
                float base = 0.0f;
                if (host->NewCityBaseAltitude(base)) height = base;
                for (Vertex& vertex : vertices_) vertex.altitude = height;
                minHeight_ = height;                          // +0x90 / +0x94
                maxHeight_ = height;
                ResetWaterToSeaLevel(gridRect_);
                createDone_ = true;                           // +0x80
                return true;
            }
            CreateFromGrayScaleImage(image);
            fromImage = true;
        } else if (image) {
            CreateFromGrayScaleImage(image);
            fromImage = true;
        } else {
            CreateFromGrayScaleImage(name);
            fromImage = false;
        }
        EnsureMinAllowedHeight();
        ResetWaterToSeaLevel(gridRect_);
        DoErosion(0, fromImage);
        ResetWaterToSeaLevel(gridRect_);
        ReconcileEdgesWithNeighboringCities(false, false, false);   // vslot 0x2c8
        FindMinAndMaxHeights();
    }
    createDone_ = true;
    return true;
}

bool cSTETerrain::LevelAreaStartingAt(std::int32_t startVertex, float maxHeightDifference, std::uint16_t mark) {
    // 0x1007364ec: flood-fill the same-height neighbourhood (west/north/east/south, the original's four
    // kOffset entries), stopping at the sea, the mark and the maximum height difference. A big enough
    // region is levelled to the seed's altitude; a small one only loses its marks.
    std::vector<std::int32_t> region;
    region.push_back(startVertex);
    const float seedAltitude = vertices_[static_cast<std::size_t>(startVertex)].altitude;
    const float seaLevel = tuning_.seaLevel;
    std::size_t count = 1;
    for (std::size_t i = 0; i < count; ++i) {
        const std::int32_t current = region[i];
        const std::int32_t row = current / static_cast<std::int32_t>(vertsX_);
        const std::int32_t column = current % static_cast<std::int32_t>(vertsX_);
        for (int direction = 0; direction < 4; ++direction) {
            const std::int32_t x = column + kDirectionX[direction * 2];
            const std::int32_t z = row + kDirectionZ[direction * 2];
            if (x < 0 || z < 0 || static_cast<std::uint32_t>(x) >= vertsX_ ||
                static_cast<std::uint32_t>(z) >= vertsZ_)
                continue;
            const std::int32_t neighbour = GetVertIndex(x, z);
            const Vertex& vertex = vertices_[static_cast<std::size_t>(neighbour)];
            if ((vertex.flags & mark) != 0) continue;
            if (vertex.altitude <= seaLevel) continue;
            if (maxHeightDifference <= std::fabs(vertex.altitude - seedAltitude)) continue;
            vertices_[static_cast<std::size_t>(neighbour)].flags |= mark;
            region.push_back(neighbour);
            ++count;
        }
    }
    if (Statics().minCellsInUsefulFlatArea < static_cast<std::int32_t>(count)) {
        for (std::int32_t index : region) SetAltitude(index, seedAltitude);   // vslot 0x188
        return true;
    }
    for (std::int32_t index : region)
        vertices_[static_cast<std::size_t>(index)].flags &= static_cast<std::uint16_t>(~mark);
    return false;
}

void cSTETerrain::CreateFlatAreas(bool redisplay) {
    // 0x100736326: level every terrace inside the altitude band, then widen the mark by the auto-flatten
    // radius, smooth the marked region and clear the scratch marks.
    FindMinAndMaxHeights();
    if (vertsZ_ == 0) return;
    const float seaLevel = tuning_.seaLevel;
    const float minTerrace = Statics().minTerraceAltitudeFactor * maxHeight_ + seaLevel;
    const float maxTerrace = maxHeight_ * Statics().maxTerraceAltitudeFactor + seaLevel;
    const float maxChange = Statics().maxTerrainHeight * Statics().maxHeightChangeToFlattenFactor;
    bool flattened = false;
    for (std::uint32_t z = 0; z < vertsZ_; ++z) {
        for (std::uint32_t x = 0; x < vertsX_; ++x) {
            const std::int32_t index = GetVertIndex(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z));
            const Vertex& vertex = vertices_[static_cast<std::size_t>(index)];
            if ((vertex.flags & 1) != 0) continue;
            if (!(minTerrace < vertex.altitude && vertex.altitude < maxTerrace)) continue;
            if (LevelAreaStartingAt(index, maxChange, 1)) flattened = true;
        }
    }
    if (!flattened) return;
    MarkRegion(Statics().autoFlatAreaEdgeSmootheningRadius, &cSTETerrain::IsVertexWithMark1,
               &cSTETerrain::IsLandAndNotMark1, 2);
    TerrainRect bounds{};
    ComputeBoundsOfMarkedRegion(bounds, 2);
    SmoothenMarkedRegion(bounds, Statics().numSmoothingPassesAfterAutoFlattening, 2);
    for (Vertex& vertex : vertices_) vertex.flags &= static_cast<std::uint16_t>(0xfffc);
    if (redisplay) RedisplayTerrain(true, true, gridRect_, 0);   // vslot 0x228
}

// ==================================================================================== neighbour-city edge reconcile

void cSTETerrain::ReconcileEdgesWithNeighboringCities(bool a, bool b, bool c) {
    // 0x10073f916: collect the heights the neighbours impose on the shared edges, tell the player, then
    // drag the terrain toward them (locking the edge vertices and smoothing the surroundings).
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    if (!host->PopupDialogsEnabled()) return;
    TerrainEdgeHeights heights;
    ComputeEdgeVerticesToReconcile(heights, c);
    const bool hasWork = !heights.empty();
    bool proceed = hasWork;
    if (a && !hasWork) host->ShowEdgeReconcileDialog(0);      // "no mismatch"
    if (hasWork && !a && b) proceed = host->ShowEdgeReconcileDialog(1);   // "mismatch, fix?"
    if (!proceed) {
        PostMessageA(0xca63d517, a ? 1u : 0u, hasWork ? 1u : 0u, 0, 0);
        return;
    }
    std::unordered_set<int> cells, vertices;
    ComputeCellsAndVertsAffectedByReconcilliation(heights, cells, vertices);
    bool done = true;
    if (b) {
        HighlightCellsAffectedByEdgeReconcilliation(cells);
        const bool proceedDamage = host->ShowEdgeReconcileDialog(2);   // "damage, continue?"
        if (view_) view_->ClearCurrentSelections();           // vslot 0x68
        if (!proceedDamage) {
            PostMessageA(0xca63d517, 1, 1, 0, 0);
            return;
        }
        done = true;
    }
    LockEdgeVerticesAndSmoothen(heights, vertices);
    PostMessageA(0xca63d517, a ? 1u : 0u, hasWork ? 1u : 0u, done ? 1u : 0u, 0);
}

void cSTETerrain::ComputeEdgeVerticesToReconcile(TerrainEdgeHeights& heights, bool useDefaultHeight) {
    // 0x10073fb82: walk the four edges of the current city, look up the neighbouring city in the region
    // and read each neighbour's shared edge altitudes out of its save game.
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    cISC4Region* region = host->Region();
    if (!region) return;
    sc4::SC4RectT<int> regionBounds;
    region->GetBoundingRect(regionBounds);                    // vslot 0xc0
    cISC4RegionalCity* city = host->RegionalCity();
    if (!city) return;
    TerrainRect cityVertRect;
    GetCityVertRect(*city, cityVertRect);
    std::int32_t cityX = 0, cityZ = 0, cityWidth = 0, cityHeight = 0;
    host->RegionalCityPosition(*city, cityX, cityZ);          // vslot 0x28
    host->RegionalCitySize(*city, cityWidth, cityHeight);     // vslot 0x38
    float defaultHeight = 0.0f;
    host->NewCityBaseAltitude(defaultHeight);
    for (int edge = 0; edge < 4; ++edge) {
        std::int32_t count = 0, x = cityX, z = cityZ;
        if ((edge | 2) == 2) {
            count = cityHeight;
            x = edge == 0 ? cityX - 1 : cityX + cityWidth;
        } else {
            count = cityWidth;
            z = edge == 1 ? cityZ - 1 : cityZ + cityHeight;
        }
        if (count <= 0) continue;
        if (x < regionBounds.x0 || z < regionBounds.z0 || regionBounds.x1 < x || regionBounds.z1 < z) continue;
        for (std::int32_t i = 0; i < count; ++i) {
            cRZAutoRefCount<cISC4RegionalCity>* found = region->GetCity(x, z);   // vslot 0x58
            cISC4RegionalCity* neighbour = found ? found->get() : nullptr;
            if (!neighbour) {
                if (useDefaultHeight) {
                    UpdateMaxHeightsForEdgeVerticesAssumingDefaultNeighborHt(edge, cityX, cityZ, x, z,
                                                                            defaultHeight, heights);
                }
            } else {
                ComputeMaxHeightsForEdgeVertices(edge, cityVertRect, neighbour, heights);
            }
            x += kDirectionX[edge * 2];
            z += kDirectionZ[edge * 2];
        }
    }
    // Drop the vertices that already match (or are within the threshold) when at least one vertex really
    // differs; a fully matching edge leaves the map untouched (the original clears it instead).
    std::vector<int> matching, close;
    const float threshold = Statics().edgeReconciliationHeightDifferenceThreshold;
    for (const auto& entry : heights) {
        const float altitude = vertices_[static_cast<std::size_t>(entry.first)].altitude;
        if (altitude == entry.second) {
            matching.push_back(entry.first);
            close.push_back(entry.first);
        } else if (std::fabs(altitude - entry.second) <= threshold) {
            close.push_back(entry.first);
        }
    }
    if (close.size() == heights.size()) {
        heights.clear();
    } else {
        for (int index : matching) heights.erase(index);
    }
}

void cSTETerrain::ComputeCellsAndVertsAffectedByReconcilliation(const TerrainEdgeHeights& heights,
                                                                std::unordered_set<int>& cells,
                                                                std::unordered_set<int>& verts) {
    // 0x100740080: for every reconciled vertex, the smoothing radius is looked up from the height
    // difference and both the cells and the vertices inside that radius are collected.
    for (const auto& entry : heights) {
        const std::int32_t index = entry.first;
        const std::int32_t row = index / static_cast<std::int32_t>(vertsX_);
        const std::int32_t column = index % static_cast<std::int32_t>(vertsX_);
        const float difference = std::fabs(entry.second - vertices_[static_cast<std::size_t>(index)].altitude);
        const std::int32_t radius =
            static_cast<std::int32_t>(EvaluateCurve(Statics().edgeReconciliationSmoothingRadiusCurve, difference));
        // vertices (vertsX stride): rows row-radius..row+radius, clamped to cellsZ
        std::int32_t rowStart = row - radius;
        if (rowStart < 0) rowStart = 0;
        std::int32_t rowEnd = row + radius;
        if (static_cast<std::uint32_t>(rowEnd) > cellsZ_) rowEnd = static_cast<std::int32_t>(cellsZ_);
        std::int32_t columnStart = column - radius;
        if (columnStart < 0) columnStart = 0;
        std::int32_t columnEnd = column + radius;
        if (static_cast<std::uint32_t>(columnEnd) > cellsX_) columnEnd = static_cast<std::int32_t>(cellsX_);
        for (std::int32_t z = rowStart; z <= rowEnd; ++z)
            for (std::int32_t x = columnStart; x <= columnEnd; ++x)
                verts.insert(GetVertIndex(x, z));
        // cells (cellsX stride): one row/column wider towards the start, clamped to the last cell
        std::int32_t cellRowStart = row <= radius ? 0 : row - radius - 1;
        std::int32_t cellRowEnd = row + radius;
        if (static_cast<std::uint32_t>(cellRowEnd) > static_cast<std::uint32_t>(lastCellZ_))
            cellRowEnd = static_cast<std::int32_t>(lastCellZ_);
        std::int32_t cellColumnStart = column <= radius ? 0 : column - radius - 1;
        std::int32_t cellColumnEnd = column + radius;
        if (static_cast<std::uint32_t>(cellColumnEnd) > static_cast<std::uint32_t>(lastCellX_))
            cellColumnEnd = static_cast<std::int32_t>(lastCellX_);
        for (std::int32_t z = cellRowStart; z <= cellRowEnd; ++z)
            for (std::int32_t x = cellColumnStart; x <= cellColumnEnd; ++x)
                cells.insert(static_cast<int>(static_cast<std::uint32_t>(z) * cellsX_ +
                                              static_cast<std::uint32_t>(x)));
    }
}

void cSTETerrain::HighlightCellsAffectedByEdgeReconcilliation(const std::unordered_set<int>& cells) {
    // 0x1007406d4: the view marks the cells (it draws them until the reconcile completes).
    if (!view_) return;
    std::vector<TerrainPointI> points;
    points.reserve(cells.size());
    for (int cell : cells) {
        points.push_back(TerrainPointI{cell % static_cast<int>(cellsX_), cell / static_cast<int>(cellsX_)});
    }
    view_->HighlightCells(points, true, false);               // vslot 0x40
}

void cSTETerrain::LockEdgeVerticesAndSmoothen(const TerrainEdgeHeights& heights,
                                              const std::unordered_set<int>& verts) {
    // 0x1007407bc: apply the reconciled altitudes, mark the untouched vertices next to them and smooth the
    // marked area, then clear the marks and redisplay.
    TerrainRect rect{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const auto& entry : heights) SetAltitude(entry.first, entry.second);   // vslot 0x188
    for (int index : verts) {
        const std::uint32_t row = static_cast<std::uint32_t>(index) / vertsX_;
        const std::uint32_t column = static_cast<std::uint32_t>(index) % vertsX_;
        if (static_cast<std::int32_t>(column) < rect.left) rect.left = static_cast<std::int32_t>(column);
        if (static_cast<std::int32_t>(column) > rect.right) rect.right = static_cast<std::int32_t>(column);
        if (static_cast<std::int32_t>(row) < rect.top) rect.top = static_cast<std::int32_t>(row);
        if (static_cast<std::int32_t>(row) > rect.bottom) rect.bottom = static_cast<std::int32_t>(row);
        if (heights.find(index) != heights.end()) continue;
        if (column != 0 && vertsX_ <= static_cast<std::uint32_t>(index) && column != cellsX_ && row != cellsZ_) {
            vertices_[static_cast<std::size_t>(index)].flags |= 1;
        }
    }
    SmoothenMarkedRegion(rect, Statics().edgeReconciliationSmoothingIterations, 1);
    ClearVertexPropertyFlags(rect, 1);
    RedisplayTerrain(true, true, rect, 1);                    // vslot 0x228
}

void cSTETerrain::GetCityVertRect(const cISC4RegionalCity& city, TerrainRect& r) {
    // 0x100740930: the city's region cells scaled to terrain cells (a 1x1 region cell is 64 cells).
    ITerrainHost* host = TerrainHost();
    if (!host) {
        r = {0, 0, 0, 0};
        return;
    }
    std::int32_t x = 0, z = 0, width = 0, height = 0;
    host->RegionalCityPosition(city, x, z);
    host->RegionalCitySize(city, width, height);
    r.left = x << 6;
    r.top = z << 6;
    r.right = r.left + width * 0x40;
    r.bottom = r.top + height * 0x40;
}

void cSTETerrain::ComputeMaxHeightsForEdgeVertices(int edge, const TerrainRect& cityRect,
                                                   const cISC4RegionalCity* city,
                                                   TerrainEdgeHeights& heights) {
    // 0x100740992: open the neighbour's save game, load its edge altitudes for the shared edge (the edge
    // opposite ours: (edge + 2) % 4) and merge them into the map.
    ITerrainHost* host = TerrainHost();
    if (!host || !city) return;
    TerrainRect neighbourRect;
    GetCityVertRect(*city, neighbourRect);
    cRZString path;
    city->GetCitySaveFilePath(path);                          // vslot 0xf8
    cIGZPersistDBSegment* segment = host->OpenSaveGame(path, false, true);
    if (!segment) return;
    std::vector<float> edgeAltitudes;
    const int neighbourEdge = (edge + 2) & 3;
    if (LoadEdgeAltitudes(neighbourEdge, segment, edgeAltitudes)) {
        TerrainPointI ourEdge, theirEdge;
        if (GetEdgeSharedWithNeighboringCity(cityRect, neighbourRect, ourEdge, theirEdge)) {
            ComputeMaxHeightsForEdgeVertices(cityRect, neighbourRect, ourEdge, theirEdge, edgeAltitudes, heights);
        }
    }
    host->CloseSaveGame(segment);
}

void cSTETerrain::UpdateMaxHeightsForEdgeVerticesAssumingDefaultNeighborHt(int edge, int startX, int startZ,
                                                                           int x, int z, float defaultHeight,
                                                                           TerrainEdgeHeights& heights) {
    // 0x100740ada: walk up to 0x41 vertices along the edge and record the default height where the map
    // does not already hold a higher value.
    std::int32_t index = 0;
    std::int32_t step = 1;
    const std::int32_t width = static_cast<std::int32_t>(vertsX_);
    if ((edge & 1) == 0) {
        index = (z - startZ) * width * 0x40;
        if (edge == 2) index += static_cast<std::int32_t>(cellsX_);
    } else {
        index = (x - startX) * 0x40;
        if (edge == 3) index += static_cast<std::int32_t>(vertsX_ * cellsZ_);
    }
    for (int i = 0; i < 0x41; ++i) {
        // PORT-ONLY guard: the original's index formula assumes the edge is 64 terrain cells long; a fixture
        // grid can be smaller, so stop at the end of the vertex array instead of writing out of bounds.
        if (index < 0 || static_cast<std::uint32_t>(index) >= totalVerts_) break;
        float& value = heights[index];
        if (value <= defaultHeight && value != defaultHeight) value = defaultHeight;
        index += step;
    }
}

bool cSTETerrain::GetEdgeSharedWithNeighboringCity(const TerrainRect& a, const TerrainRect& b,
                                                   TerrainPointI& aEdge, TerrainPointI& bEdge) {
    // 0x100740bbe: the shared edge is a vertical line when the two rects touch left-to-right, otherwise a
    // horizontal one; the start/end points only cover the overlapping span.
    if (b.right == a.left || b.left == a.right) {
        aEdge.x = bEdge.x = b.right == a.left ? b.right : b.left;
        aEdge.z = a.top < b.top ? b.top : a.top;
        bEdge.z = a.bottom < b.bottom ? a.bottom : b.bottom;
        return true;
    }
    std::int32_t edgeZ = 0;
    if (b.bottom == a.top) {
        edgeZ = b.bottom;
    } else if (b.top == a.bottom) {
        edgeZ = b.top;
    } else {
        return false;
    }
    aEdge.z = bEdge.z = edgeZ;
    aEdge.x = a.left < b.left ? b.left : a.left;
    bEdge.x = a.right < b.right ? a.right : b.right;
    return true;
}

void cSTETerrain::ComputeMaxHeightsForEdgeVertices(const TerrainRect& a, const TerrainRect& b,
                                                   const TerrainPointI& aEdge, const TerrainPointI& bEdge,
                                                   const std::vector<float>& heights,
                                                   TerrainEdgeHeights& out) {
    // 0x100740c2e: walk the shared span and keep the maximum of each pair of edge altitudes.
    std::int32_t index = (aEdge.x - a.left) + (aEdge.z - a.top) * ((a.right - a.left) + 1);
    std::int32_t count = bEdge.z - aEdge.z;
    std::int32_t step = static_cast<std::int32_t>(static_cast<std::uint32_t>(a.right - a.left) + 1);
    std::int32_t first = aEdge.z;
    std::int32_t offset = aEdge.z;
    if (count == 0) {
        count = bEdge.x - aEdge.x;
        step = 1;
        first = aEdge.x;
        offset = aEdge.x - b.left;
    } else {
        offset = aEdge.z - b.top;
    }
    if (count < 0) return;
    for (std::int32_t i = 0; i <= count; ++i) {
        if (static_cast<std::size_t>(offset) >= heights.size()) break;
        const float height = heights[static_cast<std::size_t>(offset)];
        float& value = out[index];
        if (value <= height && value != height) value = height;
        index += step;
        ++offset;
    }
    (void)first;
}

// ==================================================================================== lighting

void cSTETerrain::DoLighting(const TerrainRect& r) {
    ITerrainHost* host = TerrainHost();
    if (!host) return;
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        const std::size_t i = static_cast<std::size_t>(vertsX_) * static_cast<std::uint32_t>(z) +
                              static_cast<std::uint32_t>(r.left);
        host->ApplyTerrainLightingRow(z, r.left, (1 - r.left) + r.right, &vertices_[i].normal[0], 0x24,
                                      vertices_[i].surface, 0x24);
    }
}

// =============================================================================== persistence (streams)

bool cSTETerrain::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    if (ITerrainHost* host = TerrainHost()) host->WriteGZSerializable(stream, view_ ? view_->Serializable() : nullptr);
    cIGZPersistDBSegment* segment = nullptr;
    void* raw = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw) && raw) {
        auto* out = static_cast<cISC4DBSegmentOStream*>(raw);
        cISC4DBSegment* dbSegment = out->GetSegment();     // vslot 0xc8
        out->Release();
        if (dbSegment) segment = dbSegment->AsIGZPersistDBSegment();   // vslot 0x18
    }
    Save(segment);
    return true;
}

bool cSTETerrain::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (2 < version) return false;
    // The view (CLSID 0xc9b84e10, cSTETerrainView3D) is read back into +0x110 here; the host wraps the
    // freshly loaded object into the ITerrainView the port uses.
    cIGZPersistDBSegment* segment = nullptr;
    void* raw = nullptr;
    if (ITerrainHost* host = TerrainHost()) {
        void* viewObject = nullptr;
        if (host->ReadGZSerializable(stream, 0xc9b84e10, &viewObject))    // kCLSID_cSTETerrainView3D
            view_ = host->AdoptLoadedTerrainView(viewObject);
    }
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &raw) && raw) {
        auto* in = static_cast<cISC4DBSegmentIStream*>(raw);
        cISC4DBSegment* dbSegment = in->GetSegment();
        in->Release();
        if (dbSegment) segment = dbSegment->AsIGZPersistDBSegment();
    }
    Load(segment);
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "cSTETerrain::Read: version %u segment %p loadOk %d verts %u x %u\n", unsigned(version), static_cast<void*>(segment), int(loadOk_), vertsX_, vertsZ_);
    return true;
}

// =============================================================================== persistence (edge subfiles)

bool cSTETerrain::SaveEdgeAltitudes(int edge, cIGZPersistDBSegment* segment) const {
    // The four edge subfiles share the altitude type id and differ in the instance (edge + 100).
    AutoSave out(segment, kTypeAltitudes, kGroupTerrain, static_cast<std::uint32_t>(edge + 100), 1);
    if (!out.IsOpen()) return false;
    cIGZPersistDBSerialRecord& s = out.Stream();
    if ((edge | 2) == 2) {
        // West (0) and east (2): one column of vertices, stored bottom to top.
        const std::uint32_t rowStride = vertsX_;
        std::uint32_t index = 0;
        std::uint32_t last = rowStride * cellsZ_;
        if (edge == 2) {
            index = cellsX_;
            last += index;
        }
        s.SetFieldUint32(vertsZ_);
        for (; index <= last; index += rowStride) s.SetFieldFloat32(vertices_[index].altitude);
    } else {
        // North (1) and south (3): one row of vertices, left to right.
        std::uint32_t first = 0;
        std::uint32_t last = cellsX_;
        if (edge == 3) {
            first = cellsZ_ * vertsX_;
            last += first;
        }
        s.SetFieldUint32(vertsX_);
        for (std::uint32_t index = first; index <= last; ++index) s.SetFieldFloat32(vertices_[index].altitude);
    }
    return true;
}

bool cSTETerrain::LoadEdgeAltitudes(int edge, cIGZPersistDBSegment* segment, std::vector<float>& out) const {
    AutoLoad in(segment, kTypeAltitudes, kGroupTerrain, static_cast<std::uint32_t>(edge + 100));
    if (!in.IsOpen() || in.Version() != 1) return false;
    std::uint32_t count = 0;
    in.Stream().GetFieldUint32(count);
    out.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) in.Stream().GetFieldFloat32(out[i]);
    return true;
}

void cSTETerrain::SaveObliterated(cIGZPersistDBSegment* segment) {
    ClearVertexPropertyFlags(gridRect_, 0xffe7);
    Save(segment);
}

void cSTETerrain::LoadObliterated(cIGZPersistDBSegment* segment) { Load(segment); }

}  // namespace sc4
