// cSC4NetworkTool's tunnels, recovered from the Mac i386 build (symbols kept). A straight drag that climbs one step
// steeper than the type's trigger slope (placement +0x18, i386 +0x27c = tan(slope) x cell size) looks for the step where
// the terrain comes back down to within the clearance (placement list +0x6c) of the start: when the two portal heights
// differ by no more than the end slope (placement +0x1c) allows over the length, the start and end steps become portals
// (step kind 2) and the steps between them the tunnel's interior (kind 3, never built on). Placing the drag gives each
// portal cell a pre-built portal model (the placement list +0x54 names the exemplars, +0x60 their rotation), cuts the
// hill to the portal's back edge, and pairs the two portals so the traffic paths run through.
//
// i386 offsets in the comments: +0x44 the terrain, +0x48 the occupant manager, +0x50 the place flag (unknown98_),
// +0x54 the dragged steps, +0x60 the dragged cells, +0x74 the straight step count, +0x78 the valid steps, +0x7c the pen
// direction, +0xe4 the tunnels, +0x218 the preview, +0x230 unknown458_, +0x274 previewUpdateEnabled_, +0x278 the cell
// size, +0x27c the trigger rise, +0x288 / +0x28c the vertex row stride and column count, +0x290 / +0x2b0 the per-direction
// vertex offsets, +0x2d0 the placement parameters (+0x2ec = p+0x1c, +0x324 = p+0x54, +0x330 = p+0x60, +0x33c = p+0x6c).
#include "sc4/NetworkTool.h"

#include <cfloat>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>

#include "sc4/Exemplar.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/NetworkTunnelOccupant.h"
#include "sc4/PathInfo.h"

namespace sc4 {

namespace {
constexpr std::uint32_t Key(std::uint32_t x, std::uint32_t z) { return x + (z << 16); }
// The step kinds of tDraggedStep::direction (network-geometry.md §2.2).
constexpr std::uint32_t kStepRegular = 0, kStepTunnelPortal = 2, kStepTunnelInterior = 3;
// kAltitudeForNetworkType (the file-local table of the i386 network tool): the raised types draw their preview 15.5 up.
constexpr float kAltitudeForNetworkType[13] = {0, 0, 15.5f, 0, 0, 0, 0, 0, 15.5f, 15.5f, 0, 0, 0};
constexpr std::uint32_t kTunnelPreviewColor = 0x8000ff00;   // InsertTunnelPieces' translucent green selection
}  // namespace

float cSC4NetworkTool::StepEdgeMeanHeight(int step, int direction) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (!host) return 0.0f;
    const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
    const SC4PointU& first = cells_[s.firstCell];
    const SC4PointU& last = cells_[s.lastCell];
    const std::int32_t stride = widthPlusOne_;                                   // i386 +0x288
    const std::int32_t a = initScratch_[direction], b = initScratch_[8 + direction];   // i386 +0x290 / +0x2b0
    const std::int32_t v0 = stride * static_cast<std::int32_t>(first.z) + static_cast<std::int32_t>(first.x);
    const std::int32_t v1 = stride * static_cast<std::int32_t>(last.z) + static_cast<std::int32_t>(last.x);
    const float h0 = host->TerrainVertexHeight(v0 + a);                          // terrain vslot 0x34
    const float h1 = host->TerrainVertexHeight(v0 + b);
    const float h2 = host->TerrainVertexHeight(a + v1);
    const float h3 = host->TerrainVertexHeight(b + v1);
    return (h0 + h1 + h2 + h3) * 0.25f;
}

// i386 0x004cc4ee. From the first regular step, each next step's edge mean is compared with the one before; a rise above
// the trigger (strictly, from step 2 on, with this and the two steps before it regular and on the straight part) starts
// a tunnel at the step before the rise when FindTunnelEnd finds its end more than three steps on. The step count is read,
// never cut (network-geometry.md §6.3: the tunnel failure code is never set).
void cSC4NetworkTool::IdentifyTunnelCells(int& steps) {
    if (!AllowTunnels() || steps <= 3) return;                                   // vslot 0x100
    // Guard: FindTunnelEnd reads the clearance list's first entry; the list is as long as the portal list, and a type
    // without portals has none (the original reads past an empty vector there; no shipped type that tunnels lacks it).
    if (placement_.list90.empty()) return;
    const int direction = directionA_;                                           // i386 +0x7c
    for (int first = 0; first != steps; ++first) {
        if (steps_[static_cast<std::size_t>(first)].direction != kStepRegular) continue;
        float height = StepEdgeMeanHeight(first, direction);
        int previous = first;
        for (int next = first + 1; next < steps; ++next) {
            float nextHeight = height;
            const int current = next - 1;
            if (steps_[static_cast<std::size_t>(next)].direction == kStepRegular &&
                steps_[static_cast<std::size_t>(previous)].direction == kStepRegular &&
                (next < 2 || steps_[static_cast<std::size_t>(current - 1)].direction == kStepRegular) && next < straightSteps_) {
                nextHeight = StepEdgeMeanHeight(next, direction);
                if (nextHeight - height > slopeRise_ && next > 1) {                // i386 +0x27c
                    tTunnel tunnel;
                    tunnel.startHeight = height;
                    tunnel.startStep = previous;
                    if (FindTunnelEnd(tunnel) && tunnel.endStep - tunnel.startStep > 3) {
                        tunnels_.push_back(tunnel);
                        steps_[static_cast<std::size_t>(tunnel.startStep)].direction = kStepTunnelPortal;
                        steps_[static_cast<std::size_t>(tunnel.endStep)].direction = kStepTunnelPortal;
                        for (int s = tunnel.startStep + 1; s < tunnel.endStep; ++s) steps_[static_cast<std::size_t>(s)].direction = kStepTunnelInterior;
                    }
                }
            }
            height = nextHeight;
            ++previous;
        }
        return;
    }
}

// i386 0x004cba96. Walks on from the start step while the steps stay regular, before the last valid step and on the
// straight part; a water cell ends the search without a tunnel. Each step's far edge mean ((direction + 4) & 7) that is
// lower than the step before it and nearest the start height is the end candidate; the walk goes on while the terrain is
// above the start height plus the first clearance. The end holds when the height difference fits the end slope over the
// length and the tunnel is longer than two steps.
bool cSC4NetworkTool::FindTunnelEnd(tTunnel& tunnel) {
    INetworkToolHost* host = GetNetworkToolHost();
    const int direction = (directionA_ + 4) & 7;
    tunnel.endStep = -1;
    float best = FLT_MAX;                                                        // 0x00a70730
    float previous = tunnel.startHeight;
    int step = tunnel.startStep + 1;
    while (step < validSteps_ - 1) {
        const tDraggedStep& s = steps_[static_cast<std::size_t>(step)];
        if (s.direction != kStepRegular || step >= straightSteps_) break;
        bool water = false;
        for (std::uint32_t c = s.firstCell; c <= s.lastCell && !water; ++c)
            water = host && host->TerrainAllowsNetwork(cells_[c].x, cells_[c].z);   // terrain vslot 0x88 CellHasWater
        if (water) break;
        const float height = StepEdgeMeanHeight(step, direction);
        if (height < previous) {
            const float off = std::fabs(height - tunnel.startHeight);
            if (off < best) {
                tunnel.endStep = step;
                tunnel.endHeight = height;
                best = off;
            }
        }
        if (!(tunnel.startHeight + placement_.list90[0] < height)) break;       // i386 +0x33c[0], the clearance
        ++step;
        previous = height;
    }
    if (tunnel.endStep < 0) return false;
    const int length = tunnel.endStep - tunnel.startStep;
    float endSlope = 0.0f;
    std::memcpy(&endSlope, &placement_.unknown1c, sizeof endSlope);              // i386 +0x2ec = p+0x1c (radians)
    if (best <= static_cast<float>(length) * cellSize_ * static_cast<float>(std::tan(static_cast<double>(endSlope)))) return length > 2;
    return false;
}

// i386 0x004cbd32: the cells of every tunnel's steps (portals included) leave the set to solve. Stops at the first tunnel
// that ends past the valid steps.
void cSC4NetworkTool::RemoveTunnelPiecesFromSetOfCellsToBeSolved(std::unordered_set<std::uint32_t>& set) const {
    for (const tTunnel& tunnel : tunnels_) {
        if (tunnel.endStep >= validSteps_) break;
        for (int s = tunnel.startStep; s <= tunnel.endStep; ++s) {
            const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
            for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) set.erase(Key(cells_[c].x, cells_[c].z));
        }
    }
}

// i386 0x001256be (SmoothenNetwork, network-geometry.md §9.8): one {start, end, start height, end height} entry per tunnel
// (up to the first that ends past the valid steps); every corner of the start portal's cells is pinned at the start
// height (its allowed window collapsed to it), the end portal's at the end height.
void cSC4NetworkTool::CopyTunnelElevationConstraintsIntoCellInfo(std::vector<tImmovablePortion>& portions) {
    for (const tTunnel& tunnel : tunnels_) {
        if (tunnel.endStep >= validSteps_) break;
        tImmovablePortion portion;
        portion.x = tunnel.startStep;
        portion.step = tunnel.endStep;
        portion.valueA = tunnel.startHeight;
        portion.valueB = tunnel.endHeight;
        portions.push_back(portion);
        const auto pin = [&](int s, float height) {
            const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
            for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
                cSC4NetworkCellInfo* cell = GetCellInfo(Key(cells_[c].x, cells_[c].z));
                if (!cell) continue;
                for (int i = 0; i < 4; ++i) {
                    cell->range70[i].high = height;                              // i386 +0x6c
                    cell->range70[i].low = height;                               // i386 +0x68
                    cell->vertexHeights[i] = height;                             // i386 +0x58
                }
            }
        };
        pin(tunnel.startStep, tunnel.startHeight);
        pin(tunnel.endStep, tunnel.endHeight);
    }
}

// i386 0x00128aee. Previewing, the cells of every tunnel step show the translucent green selection. Placing, a straight
// tunnel (same row or column) gets a portal piece on every cell of its start and end steps: the start portals face the
// drag direction, the end portals the other way, and each end portal is paired with the start portal across from it
// (the start list is used from its back) so the paths of each lead through to the other.
void cSC4NetworkTool::InsertTunnelPieces(std::uint32_t networkType) {
    for (const tTunnel& tunnel : tunnels_) {
        const int endStep = tunnel.endStep;
        if (endStep >= validSteps_) return;
        if (!unknown98_) {
            if (!preview_) continue;
            for (int s = tunnel.startStep; s <= endStep; ++s) {
                const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
                for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
                    const SC4PointU at = cells_[c];
                    preview_->Erase(at);
                    preview_->AddUntexturedSelection(at, kTunnelPreviewColor, nullptr, networkType < 13 ? kAltitudeForNetworkType[networkType] : 0.0f, false);
                }
            }
            continue;
        }
        const tDraggedStep& startStep = steps_[static_cast<std::size_t>(tunnel.startStep)];
        const tDraggedStep& lastStep = steps_[static_cast<std::size_t>(endStep)];
        const SC4PointU a = cells_[startStep.firstCell];
        const SC4PointU b = cells_[lastStep.firstCell];
        std::uint8_t direction = 0;
        bool backwards = false;
        if (a.z == b.z) {
            direction = static_cast<std::uint8_t>(static_cast<int>(b.x) <= static_cast<int>(a.x) ? 3 : 1);
            backwards = static_cast<int>(b.x) < static_cast<int>(a.x);
        } else {
            if (a.x != b.x) return;                                              // only a straight tunnel
            direction = static_cast<std::uint8_t>(static_cast<int>(a.z) < static_cast<int>(b.z) ? 2 : 0);
            backwards = static_cast<int>(a.z) < static_cast<int>(b.z);
        }
        std::list<cSC4NetworkOccupant*> startPortals;
        {
            int from = static_cast<int>(startStep.firstCell), to = static_cast<int>(startStep.lastCell), delta = 1;
            if (backwards && from < to) {
                delta = -1;
                std::swap(from, to);
            }
            std::uint8_t index = 0;
            for (int c = from; c != to + delta; c += delta) {
                cSC4NetworkCellInfo* cell = GetCellInfo(Key(cells_[static_cast<std::size_t>(c)].x, cells_[static_cast<std::size_t>(c)].z));
                if (!cell) continue;
                cell->hasOccupant = true;                                        // i386 cell +0x51
                startPortals.push_back(InsertTunnelPiece(direction, index, cell));
                ++index;
            }
        }
        int from = static_cast<int>(lastStep.firstCell), to = static_cast<int>(lastStep.lastCell), delta = 1;
        if (!backwards && from < to) {
            delta = -1;
            std::swap(from, to);
        }
        std::uint8_t index = 0;
        for (int c = from; c != to + delta; c += delta) {
            cSC4NetworkCellInfo* cell = GetCellInfo(Key(cells_[static_cast<std::size_t>(c)].x, cells_[static_cast<std::size_t>(c)].z));
            if (!cell) continue;
            cell->hasOccupant = true;
            cSC4NetworkOccupant* endPortal = InsertTunnelPiece(static_cast<std::uint8_t>((direction + 2) & 3), index, cell);
            ++index;
            if (startPortals.empty()) continue;
            cSC4NetworkOccupant* startPortal = startPortals.back();
            startPortals.pop_back();
            auto* first = startPortal ? dynamic_cast<cSC4NetworkTunnelOccupant*>(startPortal) : nullptr;   // QI 0x8a4bd52b
            auto* second = endPortal ? dynamic_cast<cSC4NetworkTunnelOccupant*>(endPortal) : nullptr;
            if (!first || !second) continue;
            first->SetOtherEndOccupant(second);
            second->SetOtherEndOccupant(first);
            // Each portal's paths end at the first point of the other portal's matching path (vslot 0xc4 GetPathInfo,
            // path info vslot 0x80 MakeTunnelPaths).
            IPathNetworkOccupant* firstPath = first;
            IPathNetworkOccupant* secondPath = second;
            if (cISC4PathInfo* paths = first->GetPathInfo()) paths->MakeTunnelPaths(firstPath, secondPath);
            if (cISC4PathInfo* paths = second->GetPathInfo()) paths->MakeTunnelPaths(secondPath, firstPath);
        }
    }
}

// i386 0x00128530. One portal: a tunnel occupant over the cell with no quad triangles, opaque and usable at once, the
// portal model of the index-th exemplar of the type's list rotated by (its rotation + direction) & 3, the cell cleared
// and the occupant inserted. The back edge of the portal (the side the tunnel goes on to) rises by the clearance; the four
// corners go into the terrain at the cell's heights, the two vertices one cell further back are raised to the back edge
// when they are lower, and the terrain over the touched vertices redisplays. Returns the inserted occupant (borrowed: the
// occupant manager holds it), or null when the type has no portal exemplars.
cSC4NetworkOccupant* cSC4NetworkTool::InsertTunnelPiece(std::uint8_t direction, std::uint8_t index, cSC4NetworkCellInfo* cell) {
    if (placement_.list60.empty()) return nullptr;                               // i386 +0x324
    INetworkToolHost* host = GetNetworkToolHost();
    float corners[4][3];
    cSC4NetworkOccupant* occupant = nullptr;
    CreateNetworkOccupant(cell, &occupant, corners, kGZCLSID_cSC4NetworkTunnelOccupant);   // vslot 0xe0
    occupant->ClearNetworkFlag(0x2000000);
    occupant->ClearNetworkFlag(0x4000000);
    occupant->ClearNetworkFlag(0x4000);
    occupant->SetNetworkFlag(0x10000000);
    const float offset[3] = {0.0f, 0.0f, 0.0f};
    const std::size_t slot = index;
    const std::uint32_t turn = slot < placement_.list78.size() ? placement_.list78[slot] : 0u;   // i386 +0x330
    const std::uint32_t exemplar = slot < placement_.list60.size() ? placement_.list60[slot] : placement_.list60.back();
    SetExemplarForOccupant(occupant, exemplar, offset, static_cast<std::uint8_t>((static_cast<std::uint8_t>(turn) + direction) & 3), 0);
    ClearNetworkOccsFromCell(cell->x, cell->z);
    if (host) cell->occupantId = static_cast<std::uint8_t>(host->InsertNetworkOccupant(occupant, static_cast<std::uint32_t>(unknown458_)));
    // The back edge: corners (3 - direction) and (4 - direction) & 3 (corner 0 (x, z), 1 (x, z+1), 2 (x+1, z+1), 3 (x+1, z)).
    const std::uint32_t backA = ~static_cast<std::uint32_t>(direction) & 3;
    const std::uint32_t backB = (backA + 1) & 3;
    const float clearance = slot < placement_.list90.size() ? placement_.list90[slot] : placement_.list90.back();   // i386 +0x33c
    cell->vertexHeights[backA] += clearance;
    cell->vertexHeights[backB] += clearance;
    const std::int32_t stride = widthPlusOne_, columns = heightPlusOne_;          // i386 +0x288 / +0x28c
    const auto vx = [&](std::int32_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) % static_cast<std::uint32_t>(stride)); };
    // The row of a vertex: the original divides by the column count (+0x28c), the same as the row stride in a square city.
    const auto vz = [&](std::int32_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) / static_cast<std::uint32_t>(columns)); };
    const std::int32_t idA = cell->vertexBaseHeights[backA], idB = cell->vertexBaseHeights[backB];
    std::int32_t x0 = std::min(vx(idB), vx(idA)), x1 = std::max(vx(idB), vx(idA));
    std::int32_t z0 = std::min(vz(idB), vz(idA)), z1 = std::max(vz(idB), vz(idA));
    if (host) {
        for (int i = 0; i < 4; ++i) host->SetTerrainVertexHeight(cell->vertexBaseHeights[i], cell->vertexHeights[i]);   // terrain vslot 0xc4
        // The vertices one cell beyond the back edge, along the cell's side through each back corner.
        const std::int32_t beyondA = idA * 2 - cell->vertexBaseHeights[(backA - 1) & 3];
        const std::int32_t beyondB = idB * 2 - cell->vertexBaseHeights[(backB + 1) & 3];
        const auto raise = [&](std::int32_t v, float height) {
            if (!(host->TerrainVertexHeight(v) < height)) return;
            host->SetTerrainVertexHeight(v, height);
            x0 = std::min(vx(v), x0);
            x1 = std::max(x1, vx(v));
            z0 = std::min(vz(v), z0);
            z1 = std::max(z1, vz(v));
        };
        raise(beyondA, cell->vertexHeights[backA]);
        raise(beyondB, cell->vertexHeights[backB]);
        host->StartTerrainTextureUpdates();                                      // terrain vslot 0x15c
        host->RedisplayTerrain(x0, z0, x1, z1);                                  // terrain vslot 0x114 (true, true, rect, 6)
        host->StopTerrainTextureUpdates();                                       // terrain vslot 0x160
    }
    cSC4NetworkOccupant* inserted = occupant;
    occupant->Release();
    return inserted;
}

// i386 0x00124496: only a pre-built model occupant (QI 0x49c1a034) takes a model; the exemplar comes from the type's tuning
// group (or `group`).
}  // namespace sc4
