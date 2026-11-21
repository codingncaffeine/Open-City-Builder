// The bridges of the network tool (the bridge types are read in NetworkBridgeTypes.cpp): the part of cSC4NetworkTool's solve
// that turns a drag across water into a bridge (IdentifyBridgeCells and its helpers, the parametric layout of the pieces) and
// inserts its pieces. Recovered from the Mac
// i386 build with symbols; the i386 addresses and offsets are in the comments (tool: +0x44 terrain, +0x48 occupant manager,
// +0x54 dragged steps, +0x60 dragged cells, +0x74 straight steps, +0x78 valid steps, +0x7c direction, +0xd8 bridges,
// +0x21c possible bridges, +0x228 preferred bridge, +0x22c bridge altitude override, +0x2d0 the placement block,
// +0x37c / +0x388 water / land bridge types, +0x398 the type info, +0x3a4 the network OR filter).
#include "sc4/NetworkTool.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sc4/Exemplar.h"
#include "sc4/NetworkBridgeOccupant.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/SCProperty.h"
#include "sc4/TileNeighborhood.h"

namespace sc4 {

namespace {

// ConstructParametricBridge(cSC4NetworkTool::tBridge&)::kRefSpanVert (i386 0x00845740): for a deck rotation, the corner a ramp
// piece's low edge starts at.
constexpr int kRefSpanVert[4] = {1, 0, 3, 2};

float MinCellCornerAltitude(INetworkToolHost* host, std::uint32_t x, std::uint32_t z) {
    // GetMinSurfaceLevel (i386 0x0014c488): the lowest of the cell's four land corners.
    float corners[4] = {0, 0, 0, 0};
    if (host) host->CellCornerAltitudes(x, z, corners);
    return std::min(std::min(corners[2], corners[3]), std::min(corners[0], corners[1]));
}

float MaxSurface(INetworkToolHost* host, std::uint32_t x, std::uint32_t z) { return host ? host->CellMaxSurfaceLevel(x, z) : 0.0f; }
bool CellHasWater(INetworkToolHost* host, std::uint32_t x, std::uint32_t z) {
    // SL::spTerrain vslot 0x88 CellHasWater (the host's TerrainAllowsNetwork answers it).
    return host && host->TerrainAllowsNetwork(x, z);
}

}  // namespace

// ============================================================================================================
// IdentifyBridgeCells and its helpers
// ============================================================================================================

// i386 0x0014c8ae: no network piece of the tool's types stands in the step's cell rectangle grown by one cell.
bool cSC4NetworkTool::IsStepGoodForBridge(int step) {
    const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
    const SC4PointU& a = cells_[s.firstCell];
    const SC4PointU& b = cells_[s.lastCell];
    const int x0 = static_cast<int>(std::min(a.x, b.x)) - 1, z0 = static_cast<int>(std::min(a.z, b.z)) - 1;
    const int x1 = static_cast<int>(std::max(a.x, b.x)) + 1, z1 = static_cast<int>(std::max(a.z, b.z)) + 1;
    INetworkToolHost* host = GetNetworkToolHost();
    return !host || !host->NetworkOccupantInRect(x0, z0, x1, z1, placementFlagMask_);
}

// i386 0x0014c4e0: every cell of the step that already holds a piece holds only this network type and has no edge the bridge
// would cut (the edges across the drag direction; the end cells may also keep the edge along it on their outer side).
bool cSC4NetworkTool::CheckBridgeStepEdgeTypes(int step, std::uint32_t networkType) {
    const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
    for (std::uint32_t c = s.firstCell; c <= s.lastCell; ++c) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(cells_[c].x, cells_[c].z);
        if (!cell) continue;
        if ((~kFlagForNetworkType[networkType] & cell->networkFlags & 0x1fff) != 0) return false;
        std::uint32_t mask;
        if ((directionA_ & 2) == 0) {
            if (s.lastCell == s.firstCell) mask = 0xfffdfffdu;
            else if (c == s.firstCell) mask = 0xfbfdfffdu;
            else mask = c != s.lastCell ? 0xfbfdfbfdu : 0xfffdfbfdu;
        } else {
            if (s.lastCell == s.firstCell) mask = 0xfdfffdffu;
            else if (c == s.firstCell) mask = 0xfdfbfdffu;
            else mask = c != s.lastCell ? 0xfdfbfdfbu : 0xfdfffdfbu;
        }
        if ((cell->tileNeighborhoods[networkType] & mask) != 0) return false;
    }
    return true;
}

// i386 0x0014c73a: widen [start, end] at both ends (alternating, start first) over regular steps whose edges allow a bridge
// until it spans minSpan steps. The start may not go below step 2, the end not past the valid steps.
bool cSC4NetworkTool::PadStepsToMakeMinBridgeSpan(int minSpan, int& start, int& end, std::uint32_t networkType) {
    const int span = end - start + 1;
    if (span >= minSpan) return true;
    bool growStart = start - 1 > 0;
    bool growEnd = true;
    int need = minSpan - span;
    for (;;) {
        if (need <= 0) return end < straightSteps_ && minSpan <= end - start + 1;
        if (growStart) {
            const int old = start;
            const int s = old - 1;
            start = s;
            bool ok = false;
            if (s > 1 && steps_[static_cast<std::size_t>(s)].direction == 0 && steps_[static_cast<std::size_t>(old - 2)].direction == 0)
                ok = CheckBridgeStepEdgeTypes(s, networkType);
            if (ok) {
                --need;
                growStart = true;
                if (need <= 0) continue;
            } else {
                start = s + 1;
                growStart = false;
            }
            if (!growEnd) continue;
        } else if (!growEnd) {
            return false;
        }
        const int e = end + 1;
        end = e;
        if (e < validSteps_ && steps_[static_cast<std::size_t>(e)].direction == 0 && CheckBridgeStepEdgeTypes(e, networkType)) {
            --need;
            growEnd = true;
        } else {
            end = e - 1;
            growEnd = false;
        }
    }
}

// i386 0x0014d41c: the bridge types (water or land list, or only the preferred one) whose clearance window holds `depth` and
// whose span range fits [start, end] (padding the span to a type's minimum when that is possible). The type whose minimum span
// is closest to the span wins, one that needs no padding before any that does; the start / end are padded for it. The
// candidates become the possible bridges. -1 when none fits.
int cSC4NetworkTool::FindBestBridgeType(int& start, int& end, bool water, float depth, std::uint32_t networkType) {
    if (!typeInfo_) return -1;
    const std::vector<cSC4NetworkBridgeTypeInfo>& types = typeInfo_->BridgeTypes();
    const std::vector<int>& list = water ? waterBridgeTypes_ : landBridgeTypes_;
    const std::uint32_t span = static_cast<std::uint32_t>(end - start + 1);
    std::vector<int> candidates;
    for (int type : list) {
        if (preferredBridge_ >= 0 && type != preferredBridge_) continue;
        const cSC4NetworkBridgeTypeInfo& info = types[static_cast<std::size_t>(type)];
        if (!(info.minWaterDepth <= depth && depth <= info.maxWaterDepth)) continue;
        int s = start, e = end;
        if (span > info.maxSpan) continue;
        if (span < info.minSpan && !PadStepsToMakeMinBridgeSpan(static_cast<int>(info.minSpan), s, e, networkType)) continue;
        candidates.push_back(type);
    }
    if (candidates.empty()) return -1;
    int best = -1;
    int bestDiff = 0x7fffffff;
    for (int type : candidates) {
        std::uint32_t diff = span - types[static_cast<std::size_t>(type)].minSpan;
        if (diff > 0x7fffffffu) diff -= 100000;
        const int d = static_cast<int>(diff);
        if (std::abs(d) < std::abs(bestDiff)) {
            bestDiff = d;
            best = type;
        }
    }
    if (bestDiff < 0) PadStepsToMakeMinBridgeSpan(static_cast<int>(types[static_cast<std::size_t>(best)].minSpan), start, end, networkType);
    possibleBridges_.swap(candidates);
    return best;
}

// i386 0x0014c9b4: the deck altitude and the water depth under it. Outwards from both ends of the water, over regular steps
// that allow a bridge, the banks are scanned for rises: a step up more than 14 x the slope (radians, placement +0x00) to the
// next step's surface, or more than 4 x the slope, records that surface. The altitude is the lower of the two "steep" records
// and the higher of the two "steeper" ones (or the player's altitude override); then both ends move outwards while the bank
// stays below it. The depth is the largest altitude-over-surface of the first water cell of each step.
bool cSC4NetworkTool::AdjustBridgeSpan(int& start, int& end, float& depth, float& altitude, std::uint32_t networkType) {
    INetworkToolHost* host = GetNetworkToolHost();
    float slope = 0.0f;
    std::memcpy(&slope, &placement_.unknown00[0], sizeof slope);
    const float base = 16.0f * slope;
    const float steepLimit = base * 7.0f * 0.125f;   // [0x00a70778] * slope * [0x00a708d0] * [0x00a708d4]
    const float riseLimit = base * 0.25f;            // [0x00a70778] * slope * [0x00a70780]
    const auto stepMax = [&](int step, auto&& value) {
        float best = 0.0f;
        const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
        for (std::uint32_t c = s.firstCell; c <= s.lastCell; ++c) {
            const float v = value(cells_[c].x, cells_[c].z);
            if (v > best) best = v;
        }
        return best;
    };
    const auto minCorner = [&](std::uint32_t x, std::uint32_t z) { return MinCellCornerAltitude(host, x, z); };
    const auto maxSurface = [&](std::uint32_t x, std::uint32_t z) { return MaxSurface(host, x, z); };
    const auto regular = [&](int step) { return steps_[static_cast<std::size_t>(step)].direction == 0; };

    float startSteep = 0.0f, startRise = 0.0f;
    int startStop = 1;
    for (int s = start; s > 1; --s) {
        if (!regular(s) || !regular(s - 1) || !regular(s - 2) || !IsStepGoodForBridge(s) || !CheckBridgeStepEdgeTypes(s, networkType)) {
            startStop = s;
            break;
        }
        const float low = stepMax(s, minCorner);
        const float high = stepMax(s - 1, maxSurface);
        const float d = high - low;
        if (!(steepLimit >= d)) startSteep = high;
        if (!(riseLimit >= d)) startRise = high;
    }

    float endSteep = 0.0f, endRise = 0.0f;
    int endStop = validSteps_ - 2;
    bool settled = false;
    for (int e = end; e < endStop; ++e) {
        if (!regular(e) || !regular(e + 1) || !regular(e + 2) || !IsStepGoodForBridge(e) || !CheckBridgeStepEdgeTypes(e, networkType)) {
            endStop = e;
            break;
        }
        if (settled) continue;
        const float low = stepMax(e, minCorner);
        const float high = stepMax(e + 1, maxSurface);
        const float d = high - low;
        if (!(steepLimit >= d)) endSteep = high;
        if (riseLimit < d) {
            endRise = high;
            settled = false;
        } else {
            settled = true;
        }
    }

    altitude = std::min(std::min(startRise, endRise), std::max(startSteep, endSteep));
    if (bridgeAltitudeOverride_ >= 0.0f) altitude = bridgeAltitudeOverride_;
    while (startStop < start) {
        if (stepMax(start, maxSurface) >= altitude) break;
        --start;
    }
    while (endStop > end) {
        if (stepMax(end, maxSurface) >= altitude) break;
        ++end;
    }
    if (static_cast<double>(altitude) < 0.01) {
        const SC4PointU& cell = cells_[steps_[static_cast<std::size_t>(start)].firstCell];
        altitude = MaxSurface(host, cell.x, cell.z);
    }
    depth = 0.0f;
    for (int st = start; st < end; ++st) {
        const tDraggedStep& s = steps_[static_cast<std::size_t>(st)];
        for (std::uint32_t c = s.firstCell; c <= s.lastCell; ++c) {
            if (!CellHasWater(host, cells_[c].x, cells_[c].z)) continue;
            const float v = altitude - MaxSurface(host, cells_[c].x, cells_[c].z);
            if (v > depth) depth = v;
            break;
        }
    }
    return true;
}

// i386 0x0014d654: the bridge over the water found at `step` starts one step before it (a regular step that allows a bridge)
// and ends at the first step past the water; every step in between must allow a bridge. The span is then adjusted to the
// banks and the best water bridge type chosen.
bool cSC4NetworkTool::SelectBridgeStartAndEnd(int step, int& start, int& end, int& bridgeType, float& altitude,
                                              std::uint32_t networkType) {
    INetworkToolHost* host = GetNetworkToolHost();
    start = step - 1;
    if (start < 0) return false;
    if (steps_[static_cast<std::size_t>(start)].direction != 0 || !CheckBridgeStepEdgeTypes(start, networkType)) return false;
    end = step;
    for (;;) {
        ++end;
        if (validSteps_ - 1 <= end || steps_[static_cast<std::size_t>(end)].direction != 0) return false;
        if (!IsStepGoodForBridge(end) || !CheckBridgeStepEdgeTypes(end, networkType)) return false;
        const tDraggedStep& s = steps_[static_cast<std::size_t>(end)];
        bool water = false;
        for (std::uint32_t c = s.firstCell; c <= s.lastCell && !water; ++c) water = CellHasWater(host, cells_[c].x, cells_[c].z);
        if (water) continue;
        if (straightSteps_ <= end) return false;
        float depth = 0.0f;
        AdjustBridgeSpan(start, end, depth, altitude, networkType);
        bridgeType = FindBestBridgeType(start, end, true, depth, networkType);
        return bridgeType >= 0;
    }
}

// i386 0x0014c5e2: the deck height: the altitude override, raised to clear every step's surface (its last cell: water
// clearance over water, the ramps' full rise over land).
void cSC4NetworkTool::SelectBridgeElevation(tBridge& bridge) {
    INetworkToolHost* host = GetNetworkToolHost();
    const cSC4NetworkBridgeTypeInfo& type = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridge.bridgeType)];
    bridge.elevation = bridgeAltitudeOverride_;
    float height = 0.0f;
    for (int st = bridge.startStep; st < bridge.endStep; ++st) {
        const tDraggedStep& s = steps_[static_cast<std::size_t>(st)];
        for (std::uint32_t c = s.firstCell; c <= s.lastCell; ++c) {
            const std::uint32_t x = cells_[c].x, z = cells_[c].z;
            height = MaxSurface(host, x, z) + (CellHasWater(host, x, z) ? type.waterClearance : type.landClearance);
        }
        if (bridge.elevation <= height && height != bridge.elevation) bridge.elevation = height;
    }
}

// i386 0x0014bdd4. The span runs from the first piece to the last row's piece along the step direction (the next row minus
// the first); the far edge of the cell on the advancing side counts as the end. Along that axis the catenary
// (cosh(t) - 1) / (cosh(1) - 1) runs from 0 at the low point to 1 at a tower half a cell inside the span's end: mode 0 has
// its low point at the start, mode 1 in the middle, mode 2 at the far end. Every corner's wire height is lowered to
// sag + (1 - sag) x catenary (kDeltaForPos, i386 0x008456c0, gives the corners' offsets in the cell).
void cSC4NetworkTool::PlotBridgeSuspensionWires(tBridgePiece* pieces, int rows, int width, float sag, int mode) {
    static constexpr float kDeltaForPos[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
    const float dx = static_cast<float>(static_cast<int>(pieces[width].x - pieces[0].x));
    const float dz = static_cast<float>(static_cast<int>(pieces[width].z - pieces[0].z));
    float firstX = static_cast<float>(pieces[0].x), firstZ = static_cast<float>(pieces[0].z);
    const tBridgePiece& last = pieces[(rows - 1) * width];
    float lastX = static_cast<float>(last.x), lastZ = static_cast<float>(last.z);
    if (dx > 0.0f) lastX += 1.0f;
    else if (dx < 0.0f) firstX += 1.0f;
    if (dz > 0.0f) lastZ += 1.0f;
    else if (dz < 0.0f) firstZ += 1.0f;
    float low = 0.0f, tower = 0.0f;
    if (mode == 1) {
        low = ((firstX + lastX) * dx + (firstZ + lastZ) * dz) * 0.5f;
        tower = (lastX * dx + lastZ * dz) - 0.5f;
    } else if (mode == 2) {
        low = lastX * dx + lastZ * dz;
        tower = firstX * dx + firstZ * dz + 0.5f;
    } else if (mode == 0) {
        low = firstX * dx + firstZ * dz;
        tower = (lastX * dx + lastZ * dz) - 0.5f;
    }
    const float rise = 1.0f - sag;
    for (int i = 0; i < rows * width; ++i) {
        tBridgePiece& piece = pieces[i];
        const float x = static_cast<float>(piece.x), z = static_cast<float>(piece.z);
        for (int corner = 0; corner != 4; ++corner) {
            const float t = ((x + kDeltaForPos[corner][0]) * dx + (z + kDeltaForPos[corner][1]) * dz - low) / (tower - low);
            const double cosh2 = std::exp(static_cast<double>(t)) + std::exp(static_cast<double>(-t));
            const float height = static_cast<float>((cosh2 - 2.0) / 1.0861612696304874) * rise + sag;   // [0x00a70e90] / [0x00a70e98]
            if (height < piece.wire[corner]) piece.wire[corner] = height;
        }
    }
}

// i386 0x0014d848. The bridge-local ends become world points (x along the step direction, z to its left, the origin at the
// corner of the first piece's cell the direction starts from). The cable is cut where it crosses a multiple of 16 in x or z;
// each stretch goes to the piece whose cell holds its middle (none: dropped). A cable that rises steeply is drawn thicker,
// by its length over its ground length, up to four times.
void cSC4NetworkTool::PlotBridgeCable(tBridgePiece* pieces, int rows, int width, const float from[3], const float to[3],
                                      std::uint32_t texture, float halfWidth) {
    constexpr float kMinLength = 0.01f;   // cSC4Tank::sfBarrelVelocityScalingFactor (i386 0x00a706d0)
    if (!(std::fabs(from[0] - to[0]) >= kMinLength) && std::fabs(from[2] - to[2]) < kMinLength) return;
    const std::uint32_t x0 = pieces[0].x, z0 = pieces[0].z;
    const int stepX = static_cast<int>(pieces[width].x - x0);
    const int stepZ = static_cast<int>(pieces[width].z - z0);
    float originX = static_cast<float>(x0 << 4), originZ = static_cast<float>(z0 << 4);
    if (stepX < 0 || stepZ > 0) originX += 16.0f;
    if ((stepX | stepZ) < 0) originZ += 16.0f;
    const float fz = static_cast<float>(stepZ), fx = static_cast<float>(stepX);
    const float sz = from[0] * fz + originZ + from[2] * fx;
    const float sy = from[1];
    const float sx = (from[0] * fx + originX) - from[2] * fz;
    const float ez = fz * to[0] + originZ + fx * to[2];
    const float ey = to[1];
    const float ex = (fx * to[0] + originX) - fz * to[2];
    const float ddx = ex - sx, ddy = ey - sy, ddz = ez - sz;
    std::vector<float> cuts{0.0f, 1.0f};
    const float maxX = std::max(sx, ex), minX = std::min(sx, ex);
    const float maxZ = std::max(sz, ez), minZ = std::min(sz, ez);
    // The first grid line at or above the low end (the original's 1.5 x 2^23 rounding works out to a ceiling).
    for (float g = static_cast<float>(static_cast<int>(std::ceil(minX * 0.0625f)) << 4); g < maxX; g += 16.0f)
        cuts.push_back((g - sx) / ddx);
    for (float g = static_cast<float>(static_cast<int>(std::ceil(minZ * 0.0625f)) << 4); g < maxZ; g += 16.0f)
        cuts.push_back((g - sz) / ddz);
    std::sort(cuts.begin(), cuts.end());
    tBridgeCablePiece stretch;
    stretch.halfWidth = halfWidth;
    stretch.texture = texture;
    const float ground = std::sqrt(ddx * ddx + 0.0f + ddz * ddz);
    if (kMinLength < ground) {
        const float length = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        stretch.halfWidth = static_cast<double>(length) <= static_cast<double>(ground) * 4.0 ? length / ground * halfWidth
                                                                                            : halfWidth * 4.0f;
    }
    float px = cuts[0] * ddx + sx, py = cuts[0] * ddy + sy, pz = cuts[0] * ddz + sz;
    for (std::size_t i = 1; i < cuts.size(); ++i) {
        const float t = cuts[i];
        const float qx = t * ddx + sx, qy = t * ddy + sy, qz = t * ddz + sz;
        const int cellX = static_cast<int>(std::floor((px + qx) * 0.5f * 0.0625f));
        const int cellZ = static_cast<int>(std::floor((pz + qz) * 0.5f * 0.0625f));
        for (int k = 0; k < rows * width; ++k) {
            tBridgePiece& piece = pieces[k];
            if (static_cast<int>(piece.x) == cellX && static_cast<int>(piece.z) == cellZ) {
                stretch.from[0] = px;
                stretch.from[1] = py;
                stretch.from[2] = pz;
                stretch.to[0] = qx;
                stretch.to[1] = qy;
                stretch.to[2] = qz;
                piece.cables.push_back(stretch);
                break;
            }
        }
        px = qx;
        py = qy;
        pz = qz;
    }
}

// i386 0x0014de7a: cable i runs from a + i (b - a) / (count - 1) to c + i (d - c) / (count - 1). With one cable the original
// divides by zero and its cable lands on no piece; the port draws none.
void cSC4NetworkTool::PlotBridgeCables(tBridgePiece* pieces, int rows, int width, const float a[3], const float b[3],
                                       const float c[3], const float d[3], int count, std::uint32_t texture, float halfWidth) {
    if (count < 2) return;
    const float n = static_cast<float>(count - 1);
    const float ab[3] = {(b[0] - a[0]) / n, (b[1] - a[1]) / n, (b[2] - a[2]) / n};
    const float cd[3] = {(d[0] - c[0]) / n, (d[1] - c[1]) / n, (d[2] - c[2]) / n};
    for (int i = 0; i < count; ++i) {
        const float f = static_cast<float>(i);
        const float to[3] = {f * cd[0] + c[0], f * cd[1] + c[1], f * cd[2] + c[2]};
        const float from[3] = {f * ab[0] + a[0], f * ab[1] + a[1], f * ab[2] + a[2]};
        PlotBridgeCable(pieces, rows, width, from, to, texture, halfWidth);
    }
}

// i386 0x0014e4a0: the pieces of a bridge without a span-specific layout. Ramp pieces at both ends (mirrored at the far end),
// pylon groups every `spacing` steps between the inner ramp ends (their count clamped to the type's range, the first moved
// inwards by the spacing factor), span pieces elsewhere. Every piece gets its model, rotation (the drag direction added), the
// deck corner heights and the model height.
// A suspension type (both wire textures set, a tower placed) then gets its wire heights, a cable type its cables.
void cSC4NetworkTool::ConstructParametricBridge(tBridge& bridge) {
    const cSC4NetworkBridgeTypeInfo& bt = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridge.bridgeType)];
    const std::uint8_t rot0 = static_cast<std::uint8_t>((directionA_ / 2 + 3) % 4);
    const int start = bridge.startStep;
    int pylonStart = static_cast<int>(bt.lastSupportedRamp) + start + static_cast<int>(bt.pylonInset);
    int pylonEnd = bridge.endStep - static_cast<int>(bt.lastSupportedRamp) - static_cast<int>(bt.pylonInset);
    int spacing = 100000;
    int nextPylon = -1;
    if (bt.pylonLength != 0) {
        if (pylonStart > pylonEnd) {
            if (bt.minPylons != 0) pylonStart = pylonEnd = (start + bridge.endStep) / 2;
        } else {
            int room = pylonEnd - pylonStart;
            bool moved = false;
            const float spanCells = static_cast<float>(bridge.endStep - start + 1);
            for (;;) {
                // A type with pylons but no spacing would divide by zero in the original; no pylons are placed instead.
                std::uint32_t n = bt.pylonSpacing ? static_cast<std::uint32_t>(room) / bt.pylonSpacing : 0;
                if (n > bt.maxPylons && bt.maxPylons != 1) n = bt.maxPylons ? bt.maxPylons - 1 : 0;
                if (n < bt.minPylons && bt.minPylons != 1) n = bt.minPylons ? bt.minPylons - 1 : 0;
                int count = static_cast<int>(n);
                if (count > room / 2) count = room / 2;
                if (count <= 0) {
                    nextPylon = pylonStart;
                    spacing = 100000;
                    break;
                }
                spacing = room / count;
                if (moved) {
                    nextPylon = pylonStart;
                    break;
                }
                const float f = bt.pylonSpacingFactor;
                const float v = spanCells / (static_cast<float>(count) + (f + f)) * f;
                // floor(v) through the 1.5 * 2^23 rounding trick of the original.
                const float t = (v - 0.5f) + 12582912.0f;
                std::int32_t bits = 0;
                std::memcpy(&bits, &t, sizeof bits);
                std::int32_t shift = bits - 0x4b3fffff;
                if (static_cast<float>(shift) != v) shift = bits - 0x4b400000;
                shift -= static_cast<int>(bt.lastSupportedRamp + bt.pylonInset);
                if (shift <= 0) {
                    nextPylon = pylonStart;
                    break;
                }
                pylonStart += shift;
                pylonEnd -= shift;
                room -= shift * 2;
                moved = true;
            }
        }
    }

    int prevPylon = -1;
    std::uint32_t modelIndex = 0;
    const std::uint32_t width = bt.width ? bt.width : 1;
    for (int step = start; step <= bridge.endStep; ++step) {
        const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
        const int dir = rot0 <= 1 ? 1 : -1;
        std::uint32_t cell = rot0 <= 1 ? s.firstCell : s.lastCell;
        const std::uint32_t stop = rot0 <= 1 ? s.lastCell + 1 : s.firstCell - 1;
        for (std::uint32_t col = 0; cell != stop; cell += static_cast<std::uint32_t>(dir), ++col) {
            bridge.pieces.emplace_back();
            tBridgePiece& piece = bridge.pieces.back();
            piece.supportHeight = -1.0f;
            piece.x = cells_[cell].x;
            piece.z = cells_[cell].z;
            bool mirrored = false;
            if (static_cast<std::uint32_t>(step) < bt.rampLength + static_cast<std::uint32_t>(start)) {
                piece.kind = 0;
                modelIndex = static_cast<std::uint32_t>(step - start) * width + col;
                piece.model = bt.rampModels[modelIndex].id;
                piece.rotation = bt.rampModels[modelIndex].rotation;
                piece.supportHeight = bt.rampSupportHeights[modelIndex / width];
            } else if (static_cast<std::uint32_t>(step) > static_cast<std::uint32_t>(bridge.endStep) - bt.rampLength) {
                piece.kind = 0;
                modelIndex = static_cast<std::uint32_t>(bridge.endStep - step) * width + (width - col) - 1;
                piece.model = bt.rampModels[modelIndex].id;
                piece.rotation = static_cast<std::uint8_t>((bt.rampModels[modelIndex].rotation + 2) & 3);
                piece.supportHeight = bt.rampSupportHeights[modelIndex / width];
                mirrored = true;
            } else {
                const std::uint32_t sinceLast = static_cast<std::uint32_t>(step - prevPylon);
                if (prevPylon >= 0 && sinceLast < bt.pylonLength) {
                    // the rest of the pylon group that started at the last pylon step
                    const std::uint32_t index = sinceLast * width + col;
                    piece.kind = 1;
                    piece.model = bt.pylonModels[index].id;
                    piece.rotation = bt.pylonModels[index].rotation;
                    piece.supportHeight = bt.pylonSupportHeights[sinceLast];
                } else if (nextPylon > 0) {
                    const int toNext = nextPylon - step;
                    const std::uint32_t index = static_cast<std::uint32_t>(toNext >= 0 ? toNext : 0) * width + col;
                    if (toNext <= 0) {
                        piece.kind = 1;
                        piece.model = bt.pylonModels[index].id;
                        piece.rotation = bt.pylonModels[index].rotation;
                        piece.supportHeight = bt.pylonSupportHeights[0];
                        const int following = spacing + nextPylon;
                        prevPylon = nextPylon;
                        nextPylon = pylonEnd >= following ? following : -1;
                    } else if (static_cast<std::uint32_t>(toNext) < bt.pylonLength) {
                        // the lead-in half of the next pylon group, mirrored
                        piece.kind = 1;
                        piece.model = bt.pylonModels[index].id;
                        piece.rotation = static_cast<std::uint8_t>((bt.pylonModels[index].rotation + 2) & 3);
                        piece.supportHeight = bt.pylonSupportHeights[static_cast<std::size_t>(toNext)];
                    } else {
                        piece.kind = 2;
                        piece.model = bt.deckModel;
                        piece.rotation = bt.deckRotation;
                    }
                } else {
                    piece.kind = 2;
                    piece.model = bt.deckModel;
                    piece.rotation = bt.deckRotation;
                }
                if (piece.kind == 2) piece.rotation = static_cast<std::uint8_t>((piece.rotation + col * 2) % 4);
            }
            piece.rotation = static_cast<std::uint8_t>((piece.rotation + rot0) & 3);
            if (piece.kind != 0) {
                piece.modelHeight = bridge.elevation + bt.deckOffset;
                if (piece.kind == 1) piece.modelHeight += bt.pylonDeckOffset;
                for (float& corner : piece.corners) corner = bridge.elevation;
            } else {
                const std::uint32_t segment = modelIndex / width;
                const int r = kRefSpanVert[mirrored ? (rot0 + 2) & 3 : rot0];
                piece.modelHeight = (bridge.elevation - bt.rampDrop[segment]) + bt.deckOffset;
                const float nearHeight = bridge.elevation - bt.rampRise[segment];
                piece.corners[(r + 1) & 3] = nearHeight;
                piece.corners[r] = nearHeight;
                const float farHeight = bridge.elevation - bt.rampRise[segment + 1];
                piece.corners[(r + 3) & 3] = farHeight;
                piece.corners[(r + 2) & 3] = farHeight;
            }
            if ((prevPylon == -1 && nextPylon == -1) || bt.suspensionTopTexture == 0 || bt.suspensionHangerTexture == 0) {
                piece.suspension = 0;
            } else {
                const int r = kRefSpanVert[rot0];
                std::uint8_t flags = static_cast<std::uint8_t>((r + ((r + 3) & 3) == ((r + 1) & 3) + ((r + 2) & 3)) + 1);
                if (prevPylon == step) flags |= 4;
                piece.suspension = flags;
                for (float& w : piece.wire) w = 1.0f;
                piece.wireValue = bt.suspensionHeight;
                if (bt.width > 1) {
                    const bool firstColumn = col == 0;
                    const bool side = rot0 == 0 || rot0 == 3;
                    piece.suspension |= side == firstColumn ? 8 : 0x10;
                }
            }
        }
    }
    const int rows = bridge.endStep - start + 1;
    const int across = static_cast<int>(bt.width);
    tBridgePiece* pieces = bridge.pieces.data();
    if (bt.suspensionTopTexture != 0 && bt.suspensionHangerTexture != 0 && prevPylon != -1) {
        // The wires: the end span up to the first tower, each span between towers, the end span after the last.
        PlotBridgeSuspensionWires(pieces, pylonStart - start + 1, across, bt.suspensionEndSag, 0);
        int at = pylonStart;
        while (at < prevPylon) {
            const int next = std::min(at + spacing, prevPylon);
            PlotBridgeSuspensionWires(pieces + across * (at - start), next - at + 1, across, bt.suspensionSag, 1);
            at = next;
        }
        PlotBridgeSuspensionWires(pieces + across * (prevPylon - start), bridge.endStep - prevPylon + 1, across,
                                  bt.suspensionEndSag, 2);
    } else if (bt.hasCables) {
        // The cable fans in bridge-local coordinates (x = 16 per step from the bridge start plus half a cell, z = half a cell
        // plus the offset across): from the deck before each tower up to its front (tower A), and from its back (tower B)
        // down to the deck after it (the deck points mirrored). The last fan always has ten cables.
        const float e = bridge.elevation;
        const int count = static_cast<int>(bt.cableCount);
        const auto deckPoint = [e](const float v[3], float x, float out[3]) {
            out[0] = x + v[0] + 8.0f;
            out[1] = v[1] + 0.0f + e;
            out[2] = v[2] + 0.0f + 8.0f;
        };
        const auto towerPoint = [e](const float v[3], float x, float out[3]) {
            out[0] = v[0] + 8.0f + x;
            out[1] = e + v[1] + 0.0f;
            out[2] = v[2] + 8.0f + 0.0f;
        };
        float a[3], b[3], c[3], d[3];
        const int lastTower = prevPylon - start;
        int tower = pylonStart - start;
        const float first = static_cast<float>(tower * 16);
        deckPoint(bt.cableDeck[0], first, a);
        deckPoint(bt.cableDeck[1], first, b);
        towerPoint(bt.cableTowerA[0], first, c);
        towerPoint(bt.cableTowerA[1], first, d);
        PlotBridgeCables(pieces, rows, across, a, b, c, d, count, bt.cableTexture, bt.cableHalfWidth);
        while (tower < lastTower) {
            const int next = std::min(spacing + tower, lastTower);
            const float here = static_cast<float>(tower << 4), there = static_cast<float>(next << 4);
            towerPoint(bt.cableTowerB[0], here, a);
            towerPoint(bt.cableTowerB[1], here, b);
            deckPoint(bt.cableDeckMirrored[0], here, c);
            deckPoint(bt.cableDeckMirrored[1], here, d);
            PlotBridgeCables(pieces, rows, across, a, b, c, d, count, bt.cableTexture, bt.cableHalfWidth);
            deckPoint(bt.cableDeck[0], there, a);
            deckPoint(bt.cableDeck[1], there, b);
            towerPoint(bt.cableTowerA[0], there, c);
            towerPoint(bt.cableTowerA[1], there, d);
            PlotBridgeCables(pieces, rows, across, a, b, c, d, count, bt.cableTexture, bt.cableHalfWidth);
            tower = next;
        }
        const float lastX = static_cast<float>(lastTower * 16);
        towerPoint(bt.cableTowerB[0], lastX, a);
        towerPoint(bt.cableTowerB[1], lastX, b);
        deckPoint(bt.cableDeckMirrored[0], lastX, c);
        deckPoint(bt.cableDeckMirrored[1], lastX, d);
        PlotBridgeCables(pieces, rows, across, a, b, c, d, 10, bt.cableTexture, bt.cableHalfWidth);
    }
}

// i386 0x0014dff6: the pieces of a bridge whose span the specialization script lists, in drag order: each takes the script
// piece's model, rotation (the drag direction added), support height and model height over the deck; the deck corners come
// from the step boundary heights. Pieces between a suspension tower or end and the next get suspension flags; at each
// step that closes such a stretch its wires are plotted (tower to tower: the main sag, an end to a tower or a tower to an
// end: the end span sag; end to end: none), and a tower opens the next stretch.
void cSC4NetworkTool::ConstructSpecializedBridge(tBridge& bridge, const cSC4NetworkBridgeSpecialization& spec) {
    const cSC4NetworkBridgeTypeInfo& bt = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridge.bridgeType)];
    const std::uint8_t rot0 = static_cast<std::uint8_t>((directionA_ / 2 + 3) % 4);
    const int across = static_cast<int>(bt.width);
    std::size_t next = 0;      // the script piece
    int open = -1;             // the step (from the bridge start) that opened the current suspension stretch
    int openAttribute = 0;
    for (int step = bridge.startStep, index = 0; step <= bridge.endStep; ++step, ++index) {
        const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
        const int dir = rot0 <= 1 ? 1 : -1;
        const std::uint32_t first = rot0 <= 1 ? s.firstCell : s.lastCell;
        const std::uint32_t stop = rot0 <= 1 ? s.lastCell + 1 : s.firstCell - 1;
        int attribute = 0;
        for (std::uint32_t cell = first; cell != stop; cell += static_cast<std::uint32_t>(dir)) {
            if (next >= spec.pieces.size() || static_cast<std::size_t>(index + 1) >= spec.heights.size()) return;
            const cSC4NetworkBridgeSpecialization::Piece& from = spec.pieces[next++];
            bridge.pieces.emplace_back();
            tBridgePiece& piece = bridge.pieces.back();
            piece.supportHeight = from.supportHeight;
            piece.x = cells_[cell].x;
            piece.z = cells_[cell].z;
            piece.kind = 0;
            piece.model = from.model;
            piece.rotation = static_cast<std::uint8_t>((rot0 + from.rotation) & 3);
            piece.modelHeight = bridge.elevation + from.height + bt.deckOffset;
            piece.suspension = 0;
            const int r = kRefSpanVert[rot0];
            const float nearHeight = bridge.elevation + spec.heights[static_cast<std::size_t>(index)];
            const float farHeight = bridge.elevation + spec.heights[static_cast<std::size_t>(index + 1)];
            piece.corners[r] = nearHeight;
            piece.corners[(r + 1) & 3] = nearHeight;
            piece.corners[(r + 2) & 3] = farHeight;
            piece.corners[(r + 3) & 3] = farHeight;
            if (from.attribute == 0 && open < 0) continue;
            for (float& w : piece.wire) w = 1.0f;
            piece.wireValue = bt.suspensionHeight;
            std::uint8_t flags = (rot0 == 1 || rot0 == 3) ? 2 : 1;
            if (from.attribute == 1) flags |= 4;
            if (bt.width > 1) {
                const bool side = rot0 == 0 || rot0 == 3;
                flags |= (cell == first) == side ? 8 : 0x10;
            }
            piece.suspension = flags;
            if (from.attribute != 0) attribute = from.attribute;
        }
        if (attribute == 0) continue;
        if (open < 0) {
            open = index;
            openAttribute = attribute;
            continue;
        }
        tBridgePiece* stretch = bridge.pieces.data() + open * across;
        const int rows = index - open + 1;
        if (openAttribute == 1) {
            if (attribute == 2) {
                PlotBridgeSuspensionWires(stretch, rows, across, bt.suspensionEndSag, 2);
                open = -1;
                continue;
            }
            PlotBridgeSuspensionWires(stretch, rows, across, bt.suspensionSag, 1);
        } else {
            if (attribute == 2) {
                open = -1;
                continue;
            }
            PlotBridgeSuspensionWires(stretch, rows, across, bt.suspensionEndSag, 0);
        }
        open = index;
        openAttribute = attribute;
    }
}

// i386 0x0014f73a: a straight drag of more than three steps that reaches water gets a bridge for every stretch of water whose
// steps (and the two before each) are regular and allow one: the steps from the bank before it to the bank after it become
// bridge steps (direction 1), and the bridge is laid out. A stretch no bridge type fits ends the valid steps there.
void cSC4NetworkTool::IdentifyBridgeCells(int& steps, std::uint32_t networkType) {
    if (!AllowBridges() || steps <= 3 || steps > straightSteps_ || !typeInfo_ || typeInfo_->BridgeTypes().empty()) return;
    INetworkToolHost* host = GetNetworkToolHost();
    int step = 2;
    for (; step < steps; ++step) {
        if (steps_[static_cast<std::size_t>(step)].direction != 0 || steps_[static_cast<std::size_t>(step - 1)].direction != 0 ||
            steps_[static_cast<std::size_t>(step - 2)].direction != 0)
            continue;
        if (!IsStepGoodForBridge(step)) continue;
        const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
        bool water = false;
        for (std::uint32_t c = s.firstCell; c <= s.lastCell; ++c)
            if (CellHasWater(host, cells_[c].x, cells_[c].z)) water = true;
        if (!CheckBridgeStepEdgeTypes(step, networkType) || !water) continue;
        int start = 0, end = 0, bridgeType = -1;
        float altitude = 0.0f;
        if (!SelectBridgeStartAndEnd(step, start, end, bridgeType, altitude, networkType) || bridgeType < 0) break;
        const cSC4NetworkBridgeTypeInfo& type = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridgeType)];
        if (end - start <= 3 || type.minSpan > static_cast<std::uint32_t>(end - start + 1)) continue;
        for (int k = start; k <= end; ++k) steps_[static_cast<std::size_t>(k)].direction = 1;
        bridges_.emplace_back();
        tBridge& bridge = bridges_.back();
        bridge.networkType = networkType;
        bridge.startStep = start;
        bridge.endStep = end;
        bridge.bridgeType = bridgeType;
        bridge.referenceAltitude = altitude;
        bridge.landBridge = false;
        SelectBridgeElevation(bridge);
        // The specialization script's layout for this exact span, else the parametric one.
        const auto special = type.specializations.find(end - start + 1);
        if (special == type.specializations.end()) ConstructParametricBridge(bridge);
        else ConstructSpecializedBridge(bridge, special->second);
    }
    steps = step;
}

// i386 0x0014fa90: a land bridge over steps [start, end] at the deck height of the bank it leaves. The best land bridge type
// (padding the span to its minimum), then no occupant of any kind may stand in the span's cell rectangle grown by one cell;
// otherwise the possible bridges are forgotten and there is no bridge.
bool cSC4NetworkTool::CreateLandBridge(int start, int end, float deck, std::uint32_t networkType) {
    const int bridgeType = FindBestBridgeType(start, end, false, 0.0f, networkType);
    if (bridgeType < 0) return false;
    const SC4PointU& a = cells_[steps_[static_cast<std::size_t>(start)].firstCell];
    const SC4PointU& b = cells_[steps_[static_cast<std::size_t>(end)].lastCell];
    const int x0 = static_cast<int>(std::min(a.x, b.x)) - 1, z0 = static_cast<int>(std::min(a.z, b.z)) - 1;
    const int x1 = static_cast<int>(std::max(a.x, b.x)) + 1, z1 = static_cast<int>(std::max(a.z, b.z)) + 1;
    INetworkToolHost* host = GetNetworkToolHost();
    if (host && host->OccupantInRect(x0, z0, x1, z1)) {
        possibleBridges_.clear();
        return false;
    }
    for (int k = start; k <= end; ++k) steps_[static_cast<std::size_t>(k)].direction = 1;
    bridges_.emplace_back();
    tBridge& bridge = bridges_.back();
    bridge.networkType = networkType_;   // i386 +0x39c, the tool's own type
    bridge.startStep = start;
    bridge.endStep = end;
    bridge.bridgeType = bridgeType;
    bridge.elevation = deck;
    bridge.referenceAltitude = deck;
    bridge.landBridge = true;
    const cSC4NetworkBridgeTypeInfo& type = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridgeType)];
    const auto special = type.specializations.find(end - start + 1);
    if (special == type.specializations.end()) ConstructParametricBridge(bridge);
    else ConstructSpecializedBridge(bridge, special->second);
    return true;
}

// i386 0x0014fd56. Along the straight regular steps that allow a bridge, each step's cross-section edge (the two vertices of
// the step's first cell on the drag direction's side, Init's corner table) gives a low and a high terrain height. A step whose
// high is below the previous step's low starts a dip at the previous step (the deck at that step's high). Inside the dip the
// lowest point is tracked; the dip ends at the first step that climbs back to the deck (or above the first step down): when it
// spans more than three steps, sinks deeper than the placement's +0x3c below the deck and its far bank is within +0x40 of the
// deck, it becomes a land bridge. An irregular step or one whose edges forbid a bridge ends the dip.
void cSC4NetworkTool::IdentifyLandBridgeCells(int validSteps, std::uint32_t networkType) {
    if (!AllowBridges()) return;
    const int last = std::min(straightSteps_, validSteps) - 1;
    if (last <= 3) return;
    INetworkToolHost* host = GetNetworkToolHost();
    const int corner = directionA_ & 3;
    const float minDepth = placement_.float40[2];      // i386 +0x30c
    const float bankTolerance = placement_.float40[3]; // i386 +0x310
    int start = 0x7fffffff;
    bool inDip = false;
    float prevLow = 0.0f, prevHigh = 0.0f;
    float deck = 0.0f, firstDrop = 0.0f;
    float lowest = FLT_MAX;
    for (int s = 1; s != last; ++s) {
        if (steps_[static_cast<std::size_t>(s)].direction != 0 || !CheckBridgeStepEdgeTypes(s, networkType)) {
            inDip = false;
            continue;
        }
        const SC4PointU& cell = cells_[steps_[static_cast<std::size_t>(s)].firstCell];
        const std::int32_t base = static_cast<std::int32_t>(cell.z) * widthPlusOne_ + static_cast<std::int32_t>(cell.x);
        const float a = host ? host->TerrainVertexHeight(base + initScratch_[corner]) : 0.0f;
        const float b = host ? host->TerrainVertexHeight(base + initScratch_[8 + corner]) : 0.0f;
        const float high = std::max(b, a > -FLT_MAX ? a : -FLT_MAX);
        const float low = std::min(b, a >= FLT_MAX ? FLT_MAX : a);
        if (!inDip) {
            if (prevLow > high) {
                start = s - 1;
                deck = prevHigh;
                firstDrop = high;
                inDip = true;
                lowest = FLT_MAX;
            } else {
                prevLow = low;
                prevHigh = high;
            }
            continue;
        }
        if (!(high >= deck) && (prevHigh < firstDrop || high < firstDrop)) {
            if (lowest > low) lowest = low;
            inDip = true;
            continue;
        }
        // the dip ends at this step
        if (deck > high) {
            ++start;
            deck = firstDrop;
        }
        if (s - start + 1 > 3 && deck - lowest > minDepth && !(bankTolerance < std::fabs(deck - high)))
            CreateLandBridge(start, s, deck, networkType);
        inDip = false;
    }
}

// i386 0x004cbdfc: the cells of the bridges within the valid steps are not solved as regular pieces.
void cSC4NetworkTool::RemoveBridgePiecesFromSetOfCellsToBeSolved(std::unordered_set<std::uint32_t>& cells) const {
    for (const tBridge& bridge : bridges_) {
        if (validSteps_ <= bridge.endStep) break;
        for (const tBridgePiece& piece : bridge.pieces) cells.erase(piece.x + (piece.z << 16));
    }
}

// i386 0x0014d1f2. Quirk kept: the pieces are matched to the bridge's cells in drag order, which is the pieces' order only when
// the deck was laid from the first cell of each step (one cell per step makes no difference).
void cSC4NetworkTool::CopyBridgeElevationConstraintsIntoCellInfo(std::vector<tImmovablePortion>& portions) {
    for (const tBridge& bridge : bridges_) {
        if (validSteps_ <= bridge.endStep) break;
        const cSC4NetworkBridgeTypeInfo& type = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridge.bridgeType)];
        const float height = bridge.elevation - type.rampRise[0];
        portions.push_back(tImmovablePortion{bridge.startStep, bridge.endStep, height, height});
        std::size_t index = 0;
        for (int st = bridge.startStep; st <= bridge.endStep; ++st) {
            const tDraggedStep& s = steps_[static_cast<std::size_t>(st)];
            for (std::uint32_t c = s.firstCell; c <= s.lastCell; ++c, ++index) {
                cSC4NetworkCellInfo* cell = GetCellInfo(cells_[c].x, cells_[c].z);
                if (!cell || index >= bridge.pieces.size()) continue;
                const tBridgePiece& piece = bridge.pieces[index];
                for (int i = 0; i != 4; ++i) {
                    cell->range70[i].high = piece.corners[i];
                    cell->range70[i].low = piece.corners[i];
                    cell->vertexHeights[i] = piece.corners[i];
                }
            }
        }
    }
}

// ============================================================================================================
// inserting the pieces
// ============================================================================================================

// i386 0x00124496
void cSC4NetworkTool::SetExemplarForOccupant(cSC4NetworkOccupant* occupant, std::uint32_t exemplarId, const float* offset,
                                             std::uint8_t rotation, std::uint32_t group) {
    void* raw = nullptr;
    if (!occupant || !occupant->QueryInterface(kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel, &raw) || !raw) return;
    auto* piece = static_cast<cSC4NetworkOccupantWithPreBuiltModel*>(static_cast<cSC4NetworkOccupant*>(raw));
    if (cIGZUnknown* found = GetOccupantExemplar(exemplarId, group)) {
        void* exemplar = nullptr;
        if (found->QueryInterface(kGZIID_cISCResExemplar, &exemplar) && exemplar) {
            piece->AddModel(static_cast<cISCResExemplar*>(exemplar), offset, rotation);
            static_cast<cISCResExemplar*>(exemplar)->Release();
        }
        found->Release();
    }
    piece->Release();
}

// i386 0x00151034: the pieces of the bridges within the valid steps.
void cSC4NetworkTool::InsertBridgePieces(std::list<cSC4NetworkOccupant*>* out) {
    if (!typeInfo_) return;
    for (const tBridge& bridge : bridges_) {
        if (validSteps_ <= bridge.endStep) break;
        const cSC4NetworkBridgeTypeInfo& type = typeInfo_->BridgeTypes()[static_cast<std::size_t>(bridge.bridgeType)];
        for (const tBridgePiece& piece : bridge.pieces) {
            cSC4NetworkCellInfo* cell = GetCellInfo(piece.x + (piece.z << 16));
            if (!cell) continue;
            cell->hasOccupant = true;
            InsertBridgePiece(piece, cell, type, out);
        }
    }
}

namespace {

// AddSuspendedTileStructures (i386 0x0014c0f6, tri1 / tri2 / tri1back / tri2back at 0x00845714 / 08 / 456fc / 456f0): the
// vertical strip between two wire points (pos 0 / 1 on top, 2 / 3 below them), both faces. Each end hangs `height` above
// its `base`; the top tile (texture `top`) is at most `tile` high, the rest is filled downwards with hanger tiles of
// `tile` height (texture `hanger`), the v coordinate scaled to the part of a tile drawn.
void AddSuspendedTileStructures(cSC4NetworkOccupant* occupant, float (*pos)[3], float (*uv)[2], float tile, float baseA,
                                float heightA, float baseB, float heightB, std::uint32_t top, std::uint32_t hanger) {
    static constexpr std::uint32_t kTri1[3] = {0, 1, 2}, kTri2[3] = {2, 1, 3};
    static constexpr std::uint32_t kTri1Back[3] = {0, 2, 1}, kTri2Back[3] = {2, 3, 1};
    const auto add = [&](std::uint32_t texture) {
        occupant->AddPart(pos, uv, kTri1, texture);   // cISC4NetworkOccupant vslot 0x7c
        occupant->AddPart(pos, uv, kTri2, texture);
        occupant->AddPart(pos, uv, kTri1Back, texture);
        occupant->AddPart(pos, uv, kTri2Back, texture);
    };
    const auto strip = [&](float a, float b) {
        uv[2][1] = a / tile;
        uv[3][1] = b / tile;
        pos[0][1] = baseA + heightA;
        pos[1][1] = baseB + heightB;
        pos[2][1] = pos[0][1] - a;
        pos[3][1] = pos[1][1] - b;
    };
    if (!(tile <= heightA) && !(tile <= heightB)) {
        strip(heightA, heightB);
        add(top);
        return;
    }
    float first = std::min(heightA, heightB);
    if (tile <= first) first = tile;
    strip(first, first);
    add(top);
    heightA -= first;
    heightB -= first;
    while (heightA > 0.0f || heightB > 0.0f) {
        const float a = tile <= heightA ? tile : heightA;
        const float b = tile <= heightB ? tile : heightB;
        strip(a, b);
        add(hanger);
        heightA -= a;
        heightB -= b;
    }
}

}  // namespace

// i386 0x00316554 (columnTriangleIndices 0x0084ad5c, oppColumnTriangleIndices 0x0084ad50): four side quads between the rings
// (v = height / 16) and the top cap.
void cSC4NetworkTool::AddSupportColumnToOccupant(cSC4NetworkOccupant* occupant, const float (*top)[3], const float (*bottom)[3],
                                                 bool opposite) {
    static constexpr std::uint32_t kColumn[3] = {0, 1, 2}, kOpposite[3] = {0, 2, 1};
    const std::uint32_t* indices = opposite ? kOpposite : kColumn;
    float pos[3][3];
    float uv[3][2];
    const auto set = [](float (&v)[3], const float* p) { v[0] = p[0]; v[1] = p[1]; v[2] = p[2]; };
    for (int k = 0; k != 4; ++k) {
        const int n = (k + 1) & 3;
        set(pos[0], top[k]);
        set(pos[1], bottom[k]);
        set(pos[2], bottom[n]);
        uv[0][0] = 0.0f; uv[0][1] = top[k][1] * 0.0625f;
        uv[1][0] = 0.0f; uv[1][1] = bottom[k][1] * 0.0625f;
        uv[2][0] = 1.0f; uv[2][1] = bottom[n][1] * 0.0625f;
        occupant->AddPart(pos, uv, indices, 0x8080000);
        set(pos[1], bottom[n]);
        set(pos[2], top[n]);
        uv[1][0] = 1.0f; uv[1][1] = bottom[n][1] * 0.0625f;
        uv[2][0] = 1.0f; uv[2][1] = top[n][1] * 0.0625f;
        occupant->AddPart(pos, uv, indices, 0x8080000);
    }
    set(pos[0], top[0]);
    set(pos[1], top[1]);
    set(pos[2], top[2]);
    uv[0][0] = 0.0f; uv[0][1] = 0.0f;
    uv[1][0] = 0.0f; uv[1][1] = 1.0f;
    uv[2][0] = 1.0f; uv[2][1] = 1.0f;
    occupant->AddPart(pos, uv, indices, 0x912220e);
    set(pos[1], top[2]);
    set(pos[2], top[3]);
    uv[1][0] = 1.0f; uv[1][1] = 1.0f;
    uv[2][0] = 1.0f; uv[2][1] = 0.0f;
    occupant->AddPart(pos, uv, indices, 0x912220e);
}

namespace {

// The two triangles of a strip between four corners (0-1 along the top, 2-3 below them) and the same two turned the other
// way, so the strip shows from both sides (kSideLipTriIndices 0x0084ace0, kWallTriIndices 0x0084ad20).
constexpr std::uint32_t kStripTriangles[4][3] = {{0, 2, 1}, {1, 2, 3}, {0, 1, 2}, {1, 3, 2}};

// AddSupportStructures' kQuadVertMap (0x0084ade0) and kFlippedQuadVertMap (0x0084ad80): a support record's corner (0-3, 4 =
// the middle) in the piece's own frame -> the cell corner it lands on, by rotation.
constexpr std::uint32_t kQuadVertMap[4][5] = {{0, 1, 2, 3, 4}, {3, 0, 1, 2, 4}, {2, 3, 0, 1, 4}, {1, 2, 3, 0, 4}};
constexpr std::uint32_t kFlippedQuadVertMap[4][5] = {{3, 2, 1, 0, 4}, {0, 3, 2, 1, 4}, {1, 0, 3, 2, 4}, {2, 1, 0, 3, 4}};

// cSC4Tank::sfBarrelVelocityScalingFactor (0x00a706d0), the constant the original shares for "above the terrain".
constexpr float kRaisedThreshold = 0.01f;

}  // namespace

// i386 0x00316260.
void cSC4NetworkTool::AddNetworkSideLip(cSC4NetworkOccupant* occupant, const float* a, const float* b) {
    float pos[4][3] = {{a[0], a[1] + 1.5f, a[2]}, {b[0], b[1] + 1.5f, b[2]}, {a[0], a[1] - 3.0f, a[2]}, {b[0], b[1] - 3.0f, b[2]}};
    float uv[4][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 1.0f}};
    const int along = a[0] != b[0] ? 0 : 2;   // u runs along x unless the edge runs along z
    for (int i = 0; i != 4; ++i) uv[i][0] = pos[i][along] * 0.0625f;
    for (const auto& triangle : kStripTriangles) occupant->AddPart(pos, uv, triangle, 0xbb0200);
}

// i386 0x003163d6.
void cSC4NetworkTool::AddRetainingWall(cSC4NetworkOccupant* occupant, const tSupportInstance& instance, const float* a,
                                       const float* b, float inset) {
    float pos[4][3] = {{a[0], a[1] - inset, a[2]}, {b[0], b[1] - inset, b[2]}, {a[0], instance.terrainA, a[2]},
                       {b[0], instance.terrainB, b[2]}};
    float uv[4][2];
    const int along = a[0] != b[0] ? 0 : 2;
    for (int i = 0; i != 4; ++i) {
        uv[i][0] = pos[i][along] * 0.0625f;
        uv[i][1] = pos[i][1] * 0.0625f;
    }
    for (const auto& triangle : kStripTriangles) occupant->AddPart(pos, uv, triangle, 0xbb0000);
}

// i386 0x0031684e.
void cSC4NetworkTool::AddSupportPylon(cSC4NetworkOccupant* occupant, tSupportInstance& instance, cSC4NetworkCellInfo& cell) {
    INetworkToolHost* host = GetNetworkToolHost();
    const bool flipped = occupant->GetFlip() != 0;
    const float centerX = (static_cast<float>(cell.x) + 0.5f) * cellSize_;
    const float centerZ = (static_cast<float>(cell.z) + 0.5f) * cellSize_;
    float ground = FLT_MAX;
    for (int i = 0; i != 4; ++i) ground = std::min(ground, host ? host->TerrainVertexHeight(cell.vertexBaseHeights[i]) : 0.0f);
    const std::vector<float>& footprint = instance.info->footprint;
    std::size_t index = static_cast<std::size_t>(occupant->GetRotation()) * 8;
    float top[4][3], bottom[4][3];
    for (int k = 0; k != 4; ++k, index += 2) {
        const float fx = index < footprint.size() ? footprint[index] : 0.0f;
        const float fz = index + 1 < footprint.size() ? footprint[index + 1] : 0.0f;
        top[k][0] = bottom[k][0] = flipped ? centerX - fx : centerX + fx;
        top[k][2] = bottom[k][2] = centerZ + fz;
        top[k][1] = occupant->GetAltitude(top[k][0], top[k][2]) - 1.3f;              // vslot 0xe0
        bottom[k][1] = ground;
    }
    AddSupportColumnToOccupant(occupant, top, bottom, flipped);
}

// i386 0x00316a68.
void cSC4NetworkTool::AddSupportStructures(cSC4NetworkOccupant* occupant, cSC4NetworkCellInfo& cell,
                                           const cSC4NetworkSupportStructInfo* info, int level) {
    INetworkToolHost* host = GetNetworkToolHost();
    const std::uint32_t rotation = occupant->GetRotation() & 3u;                        // vslot 0x60
    const bool flipped = occupant->GetFlip() != 0;                                       // vslot 0x64
    const auto terrain = [&](std::uint32_t v) {
        return host ? host->TerrainVertexHeight(cell.vertexBaseHeights[v]) : 0.0f;       // world cache vslot 0x34
    };
    const std::uint32_t row = static_cast<std::uint32_t>(widthPlusOne_);
    // A corner's ground height and how far the piece stands above it there (the middle averages the four corners).
    const auto heights = [&](std::uint32_t v, float& ground) {
        if (v == 4) {
            ground = (terrain(0) + terrain(1) + terrain(2) + terrain(3)) * 0.25f;
            return (cell.vertexHeights[0] + cell.vertexHeights[1] + cell.vertexHeights[2] + cell.vertexHeights[3]) * 0.25f - ground;
        }
        ground = terrain(v);
        return cell.vertexHeights[v] - ground;
    };
    const auto corner = [&](std::uint32_t v, float* out) {
        if (v == 4) {
            const auto id0 = static_cast<std::uint32_t>(cell.vertexBaseHeights[0]), id2 = static_cast<std::uint32_t>(cell.vertexBaseHeights[2]);
            out[0] = (static_cast<float>(id0 % row) * cellSize_ + static_cast<float>(id2 % row) * cellSize_) * 0.5f;
            out[2] = (cellSize_ * static_cast<float>(id0 / row) + cellSize_ * static_cast<float>(id2 / row)) * 0.5f;
            out[1] = (cell.vertexHeights[0] + cell.vertexHeights[1] + cell.vertexHeights[2] + cell.vertexHeights[3]) * 0.25f;
        } else {
            const auto id = static_cast<std::uint32_t>(cell.vertexBaseHeights[v]);
            out[0] = static_cast<float>(id % row) * cellSize_;
            out[2] = static_cast<float>(id / row) * cellSize_;
            out[1] = cell.vertexHeights[v];
        }
    };
    tSupportInstance instance;
    instance.info = info;
    const std::vector<std::uint32_t>& edges = info->edges;
    for (std::size_t k = 0; k + 1 < edges.size(); k += 2) {
        // A mirrored piece walks its edges the other way round.
        const std::uint32_t first = edges[flipped ? k + 1 : k], second = edges[flipped ? k : k + 1];
        if (first > 4 || second > 4) continue;
        instance.vertexA = flipped ? kFlippedQuadVertMap[rotation][first] : kQuadVertMap[rotation][first];
        instance.vertexB = flipped ? kFlippedQuadVertMap[rotation][second] : kQuadVertMap[rotation][second];
        instance.raisedA = heights(instance.vertexA, instance.terrainA) > kRaisedThreshold;
        instance.raisedB = heights(instance.vertexB, instance.terrainB) > kRaisedThreshold;
        if (!instance.raisedA && !instance.raisedB) continue;
        float a[3], b[3];
        corner(instance.vertexA, a);
        corner(instance.vertexB, b);
        // An edge along a cell side gets a lip only when nothing connects across that side; any other edge always does.
        bool open = true;
        const std::uint32_t va = instance.vertexA, vb = instance.vertexB;
        if (va != 4 && vb != 4 && (va == ((vb + 1) & 3) || vb == ((va + 1) & 3))) {
            const std::uint32_t start = va == ((vb + 1) & 3) ? vb : va;
            const int type = GetFirstNetworkTypeFromFlags(cell.networkFlags);
            const std::uint32_t compact = type >= 0 && type < 13 ? cell.tileNeighborhoods[type] : 0;
            open = !cSC4TileNeighborhoodCompact::HasConnectionOnEdge(compact, static_cast<int>((0u - start) & 3u));
        }
        if (open) AddNetworkSideLip(occupant, a, b);
        if (level == 1) AddRetainingWall(occupant, instance, a, b, open ? 3.0f : 0.01f);
    }
    if (level == 2) AddSupportPylon(occupant, instance, cell);
}

// i386 0x0014ffcc. The suspension flags: bits 0-1 pick which corners are the two deck edges (1: corners 0-1 and 2-3, else
// 1-2 and 3-0), 0x10 drops the first edge and 8 the second (the inner edges of a wide deck), 4 marks a tower piece. Each edge
// runs between its two corners' terrain vertices, moved in towards the other edge by the type's inset; its wire heights are
// the piece's wire fractions times the tower height, above the deck corners plus the type's offset. A tower piece's edge is
// two strips meeting at the full tower height in the middle of the cell (u to 0.5).
void cSC4NetworkTool::AddSuspendedBridgeStructures(cSC4NetworkOccupant* occupant, const tBridgePiece& piece,
                                                   const cSC4NetworkCellInfo* cell, const cSC4NetworkBridgeTypeInfo& type) {
    if (piece.suspension == 0) return;
    static constexpr int kEdgesA[4] = {0, 1, 2, 3}, kEdgesB[4] = {1, 2, 3, 0};
    const int* corner = (piece.suspension & 3) == 1 ? kEdgesA : kEdgesB;
    const std::uint32_t row = static_cast<std::uint32_t>(widthPlusOne_);
    const auto vertexX = [&](int c) {
        return static_cast<float>(static_cast<std::uint32_t>(cell->vertexBaseHeights[c]) % row) * cellSize_;
    };
    const auto vertexZ = [&](int c) {
        return static_cast<float>(static_cast<std::uint32_t>(cell->vertexBaseHeights[c]) / row) * cellSize_;
    };
    const auto towards = [](float from, float to) { return from == to ? 0.0f : (to <= from ? -1.0f : 1.0f); };
    for (int side = 0; side != 2; ++side) {
        if (side == 0 && (piece.suspension & 0x10)) continue;
        if (side == 1 && (piece.suspension & 8)) continue;
        const int base = side * 2;
        const int a = corner[base], b = corner[base + 1], other = corner[(base + 3) & 3];
        const float dx = towards(vertexX(a), vertexX(other)) * type.suspensionInset;
        const float dz = towards(vertexZ(a), vertexZ(other)) * type.suspensionInset;
        const float ax = vertexX(a) + dx, az = vertexZ(a) + dz;
        const float bx = vertexX(b) + dx, bz = vertexZ(b) + dz;
        float tile = type.suspensionTileHeight;
        if (tile == 0.0f) tile = static_cast<float>(std::max(static_cast<double>(std::fabs(az - bz)), static_cast<double>(std::fabs(ax - bx))));
        const float baseA = type.suspensionDeckOffset + piece.corners[a];
        const float baseB = type.suspensionDeckOffset + piece.corners[b];
        const float heightA = piece.wireValue * piece.wire[a];
        const float heightB = piece.wireValue * piece.wire[b];
        float pos[4][3] = {};
        const auto place = [&](float x0, float z0, float x1, float z1) {
            pos[0][0] = x0; pos[0][2] = z0;
            pos[1][0] = x1; pos[1][2] = z1;
            pos[2][0] = x0; pos[2][2] = z0;
            pos[3][0] = x1; pos[3][2] = z1;
        };
        if (piece.suspension & 4) {
            float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.0f, 0.0f}, {0.5f, 0.0f}};
            const float mx = (ax + bx) * 0.5f, mz = (az + bz) * 0.5f;
            const float baseMid = (baseA + baseB) * 0.5f;
            place(ax, az, mx, mz);
            AddSuspendedTileStructures(occupant, pos, uv, tile, baseA, heightA, baseMid, piece.wireValue,
                                       type.suspensionTopTexture, type.suspensionHangerTexture);
            place(mx, mz, bx, bz);
            AddSuspendedTileStructures(occupant, pos, uv, tile, baseMid, piece.wireValue, baseB, heightB,
                                       type.suspensionTopTexture, type.suspensionHangerTexture);
        } else {
            float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}};
            place(ax, az, bx, bz);
            AddSuspendedTileStructures(occupant, pos, uv, tile, baseA, heightA, baseB, heightB, type.suspensionTopTexture,
                                       type.suspensionHangerTexture);
        }
    }
}

// i386 0x00150814: a bridge occupant over the cell, its model placed at the piece's height, the passage under it below the
// cell's lowest corner by the type's clearance (unless the model exemplar has property 0xabe1b5a3), immovable, inserted.
// Then the support column, the suspension wires and the cable stretches become added parts of the occupant.
void cSC4NetworkTool::InsertBridgePiece(const tBridgePiece& piece, cSC4NetworkCellInfo* cell, const cSC4NetworkBridgeTypeInfo& type,
                                        std::list<cSC4NetworkOccupant*>* out) {
    INetworkToolHost* host = GetNetworkToolHost();
    cSC4NetworkOccupant* occupant = nullptr;
    float corners[4][3];
    CreateNetworkOccupant(cell, &occupant, corners, kGZCLSID_cSC4NetworkBridgeOccupant);   // vslot 0xe0
    if (!occupant) return;
    float position[3] = {0, 0, 0};
    occupant->GetPosition(position);                                                       // cISC4Occupant vslot 0x20
    const float offset[3] = {0.0f, piece.modelHeight - position[1], 0.0f};
    SetExemplarForOccupant(occupant, piece.model, offset, piece.rotation, 0);
    occupant->SetQuadDraw(piece.model, piece.rotation);                                    // vslot 0x78
    occupant->ClearNetworkFlag(0x2000000);
    occupant->ClearNetworkFlag(0x4000000);
    occupant->SetNetworkFlag(0x8000000);
    if (occupant->GetNetworkFlag() & 0x10000000u) occupant->ClearNetworkFlag(0x4000);
    else occupant->SetNetworkFlag(0x4000);
    if (cIGZUnknown* found = GetOccupantExemplar(piece.model, 0)) {
        void* exemplar = nullptr;
        if (found->QueryInterface(kGZIID_cISCResExemplar, &exemplar) && exemplar) {
            const cISCPropertyHolder* holder = static_cast<cISCResExemplar*>(exemplar)->AsISCPropertyHolder();
            if (holder && !holder->GetProperty(0xabe1b5a3)) {
                float lowest = cell->vertexHeights[0];
                for (int i = 1; i != 4; ++i) lowest = std::min(lowest, cell->vertexHeights[i]);
                occupant->SetPassageCeiling(lowest - type.passageClearance);                // vslot 0x110
            }
            static_cast<cISCResExemplar*>(exemplar)->Release();
        }
        found->Release();
    }
    if (piece.supportHeight >= 0.0f) {
        // The support column: the piece's footprint (pylon or ramp, at its rotation) from the piece height less the support
        // height down to the cell's lowest terrain vertex, split at the water surface when there is water over the ground.
        float at[3] = {0, 0, 0};
        occupant->GetPosition(at);
        float ground = FLT_MAX;
        for (int i = 0; i != 4; ++i) ground = std::min(ground, host ? host->TerrainVertexHeight(cell->vertexBaseHeights[i]) : 0.0f);
        const float topY = at[1] - piece.supportHeight;
        if (ground <= topY) {
            const std::vector<float>& footprint = piece.kind == 1 ? type.pylonFootprint : type.rampFootprint;
            float top[4][3], bottom[4][3];
            std::size_t index = static_cast<std::size_t>(piece.rotation) * 8;
            for (int k = 0; k != 4; ++k, index += 2) {
                const float fx = index < footprint.size() ? footprint[index] : 0.0f;
                const float fz = index + 1 < footprint.size() ? footprint[index + 1] : 0.0f;
                top[k][0] = bottom[k][0] = at[0] + fx;
                top[k][1] = topY;
                bottom[k][1] = ground;
                top[k][2] = bottom[k][2] = at[2] + fz;
            }
            const float water = host ? host->WaterLevelAt(at[0], at[2]) : 0.0f;
            const float(*upper)[3] = top;
            float middle[4][3];
            if (water > (bottom[0][1] + bottom[1][1] + bottom[2][1] + bottom[3][1]) * 0.25f) {
                const float line = static_cast<float>(static_cast<double>(water) - 0.01);
                for (int k = 0; k != 4; ++k) {
                    middle[k][0] = bottom[k][0];
                    middle[k][1] = std::min(top[k][1], line);
                    middle[k][2] = bottom[k][2];
                }
                if (water < (top[0][1] + top[1][1] + top[2][1] + top[3][1]) * 0.25f) AddSupportColumnToOccupant(occupant, top, middle, false);
                upper = middle;
            }
            AddSupportColumnToOccupant(occupant, upper, bottom, false);
        }
    }
    if (piece.suspension != 0) AddSuspendedBridgeStructures(occupant, piece, cell, type);
    // The cable stretches: a quad of the stretch's half width above and below it, both faces (_texcoords_207958 /
    // _triIndices_207959, i386 0x008457c0 / 0x00845780).
    for (const tBridgeCablePiece& cable : piece.cables) {
        static constexpr float kCableUv[4][2] = {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
        static constexpr std::uint32_t kCableTris[4][3] = {{0, 1, 2}, {0, 2, 3}, {0, 2, 1}, {0, 3, 2}};
        const float quad[4][3] = {{cable.from[0], cable.from[1] + cable.halfWidth, cable.from[2]},
                                  {cable.from[0], cable.from[1] - cable.halfWidth, cable.from[2]},
                                  {cable.to[0], cable.to[1] - cable.halfWidth, cable.to[2]},
                                  {cable.to[0], cable.to[1] + cable.halfWidth, cable.to[2]}};
        for (const auto& tri : kCableTris) occupant->AddPart(quad, kCableUv, tri, cable.texture);
    }
    if (unknown98_) ClearNetworkOccsFromCell(piece.x, piece.z);
    occupant->SetImmovable(true);                                                          // vslot 0x48
    if (host) cell->occupantId = static_cast<std::uint8_t>(host->InsertNetworkOccupant(occupant, static_cast<std::uint32_t>(unknown458_)));
    if (out) {
        occupant->AddRef();
        out->push_back(occupant);
    }
    occupant->Release();
}

}  // namespace sc4
