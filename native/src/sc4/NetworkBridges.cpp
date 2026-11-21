// The bridges of the network tool (the bridge types are read in NetworkBridgeTypes.cpp): the part of cSC4NetworkTool's solve
// that turns a drag across water into a bridge (IdentifyBridgeCells and its helpers, the parametric layout of the pieces) and
// inserts its pieces. Recovered from the Mac
// i386 build with symbols; the i386 addresses and offsets are in the comments (tool: +0x44 terrain, +0x48 occupant manager,
// +0x54 dragged steps, +0x60 dragged cells, +0x74 straight steps, +0x78 valid steps, +0x7c direction, +0xd8 bridges,
// +0x21c possible bridges, +0x228 preferred bridge, +0x22c bridge altitude override, +0x2d0 the placement block,
// +0x37c / +0x388 water / land bridge types, +0x398 the type info, +0x3a4 the network OR filter).
#include "sc4/NetworkTool.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sc4/Exemplar.h"
#include "sc4/NetworkBridgeOccupant.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/SCProperty.h"

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

// i386 0x0014e4a0: the pieces of a bridge without a span-specific layout. Ramp pieces at both ends (mirrored at the far end),
// pylon groups every `spacing` steps between the inner ramp ends (their count clamped to the type's range, the first moved
// inwards by the spacing factor), span pieces elsewhere. Every piece gets its model, rotation (the drag direction added), the
// deck corner heights and the model height.
// NOT PORTED: the suspension wires (PlotBridgeSuspensionWires, when suspensionA / suspensionB are set) and the cables
// (PlotBridgeCables, when hasCables): the pieces keep their default wire heights and no cable pieces.
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
            if ((prevPylon == -1 && nextPylon == -1) || bt.suspensionA == 0 || bt.suspensionB == 0) {
                piece.suspension = 0;
            } else {
                const int r = kRefSpanVert[rot0];
                std::uint8_t flags = static_cast<std::uint8_t>((r + ((r + 3) & 3) == ((r + 1) & 3) + ((r + 2) & 3)) + 1);
                if (prevPylon == step) flags |= 4;
                piece.suspension = flags;
                for (float& w : piece.wire) w = 1.0f;
                piece.wireValue = bt.suspensionValue;
                if (bt.width > 1) {
                    const bool firstColumn = col == 0;
                    const bool side = rot0 == 0 || rot0 == 3;
                    piece.suspension |= side == firstColumn ? 8 : 0x10;
                }
            }
        }
    }
    // PlotBridgeSuspensionWires / PlotBridgeCables: not ported (see above).
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
        // ConstructSpecializedBridge for a span the specialization script lists (none: the parser is not ported).
        ConstructParametricBridge(bridge);
    }
    steps = step;
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

// i386 0x00150814: a bridge occupant over the cell, its model placed at the piece's height, the passage under it below the
// cell's lowest corner by the type's clearance (unless the model exemplar has property 0xabe1b5a3), immovable, inserted.
// NOT PORTED: the support column down from the piece (supportHeight >= 0: AddSupportColumnToOccupant), the suspension
// structures (AddSuspendedBridgeStructures) and the cable parts.
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
