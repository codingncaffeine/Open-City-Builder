// cSC4PowerLineTool — see PowerLineTool.h for the layout, the vtable slots and the seams. Hand-written from
// the Ghidra decompilation of the Aspyr macOS build (0x1002e9c30..0x1002ed8e0); every address below is the
// original function / call site. UpdateOnZoneChange is marked PARTIAL: the reduced implementation keeps exactly
// what its comment says (also listed under PARTIAL: in notes/ledger_pending/cSC4PowerLineTool.txt).
#include "sc4/PowerLineTool.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <vector>

#include "rz/RZConvert.h"

#pragma STDC FP_CONTRACT OFF

namespace sc4 {

namespace {

// Occupant types CellIsOKForPlacement distinguishes (cSC4NetworkOccupant::GetType / cSC4BuildingOccupant's
// type 0x278128a0) and the interface id of cISC4UndergroundOccupant3.
constexpr std::uint32_t kNetworkOccupantType = 0xc772bf98;
constexpr std::uint32_t kBuildingOccupantType = 0x278128a0;
constexpr std::uint32_t kIIDUndergroundOccupant3 = 0x4a10afc2;

IPowerLineToolHost* g_powerLineToolHost = nullptr;

// One occupant query for a single cell (SL::spOccupantManager +0x148 with both corners on the cell). The
// entries carry one reference each; every caller below releases them.
void QueryCellOccupants(IPowerLineToolHost* host, int x, int z, std::vector<ICityOccupant*>& out) {
    out.clear();
    if (host != nullptr) host->GetOccupantsInCellRect(out, x, z, x, z);
}

}  // namespace

void SetPowerLineToolHost(IPowerLineToolHost* host) { g_powerLineToolHost = host; }

IPowerLineToolHost* GetPowerLineToolHost() { return g_powerLineToolHost; }

bool cSC4PowerLineTool::sbInited_ = false;
std::uint32_t cSC4PowerLineTool::sPowerPoleForDirectionsFlag[16] = {};

// ============================================================================================================
// construction
// ============================================================================================================

// 0x1002ea12c (the 0x1002ea216 symbol is the compiler-generated C2 alias: SKIPPED in the ledger)
cSC4PowerLineTool::cSC4PowerLineTool() : cSC4NetworkTool(kNetworkPowerLine) {}

// 0x1002ed8e0 (0x1002ed6a4 / 0x1002ed6ae are the generated complete / deleting destructor aliases: SKIPPED).
// The original destructor only runs the container destructors and the base destructor: it does NOT release
// the pole occupants still referenced from createdPoles_ / previewPoles_ / connectionList_ (those are dropped
// by PlacePoles / ClearCellPreviews). Kept as-is, so destroying a tool mid-drag leaks those references.
cSC4PowerLineTool::~cSC4PowerLineTool() = default;

// ============================================================================================================
// cursors
// ============================================================================================================

// 0x1002ea220
std::uint32_t cSC4PowerLineTool::CursorId() { return 0xa16f1463; }

// 0x1002ea22c
std::uint32_t cSC4PowerLineTool::DirectionalCursorBase() { return 0x69e13770; }

// ============================================================================================================
// pole infos
// ============================================================================================================

// 0x1002e9c30: record the connection on both ends and put its direction bit into both masks. The direction
// comes from cSC4NetworkRoutines::Get0To3Direction (0x1002ee7ca), not from the cell deltas themselves.
void cSC4PowerLineTool::AddConnection(tPoleInfo& a, tPoleInfo& b, int value) {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    a.connectionList.push_back(tConnectionInfo{static_cast<std::uint32_t>(b.index), value});
    b.connectionList.push_back(tConnectionInfo{static_cast<std::uint32_t>(a.index), value});
    const std::uint8_t direction = static_cast<std::uint8_t>(host != nullptr
                                                                  ? host->Get0To3Direction(static_cast<int>(a.x),
                                                                                           static_cast<int>(a.z),
                                                                                           static_cast<int>(b.x),
                                                                                           static_cast<int>(b.z))
                                                                  : 0);
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (direction & 0x1f));
    a.connections = static_cast<std::uint8_t>(a.connections | bit);
    b.connections = static_cast<std::uint8_t>(b.connections | bit);
}

// 0x1002e9cd2: cRZIniFile callback of the "PowerPoles" section, key = direction mask, value = exemplar id.
// The original indexes the 16-entry array without a range check; a malformed ini writes past it (quirk kept).
void cSC4PowerLineTool::AddPowerPoleInfo(const cRZString& key, const cRZString& value, void* context) {
    (void)context;
    const std::uint32_t directionMask = RZ::ConvertToUint(key.ToChar());
    const std::uint32_t exemplarId = RZ::ConvertToUint(value.ToChar());
    sPowerPoleForDirectionsFlag[directionMask] = exemplarId;
}

// 0x1002ea238: append one pole (index = current size), copy the intersection tail, optionally connect it to
// the previous pole. Note that the connection value the original passes is the number of per-pole line-info
// lists (a "generation" tag), not the new pole's index.
void cSC4PowerLineTool::PlacePole(int cellIndex, bool connect, int value, const tIntersectionInfo& info) {
    tPoleInfo pole;
    pole.index = static_cast<std::int32_t>(poleInfos_.size());
    pole.x = cells_[static_cast<std::size_t>(cellIndex)].x;
    pole.z = cells_[static_cast<std::size_t>(cellIndex)].z;
    pole.cellIndex = cellIndex;
    pole.poleA = info.first;
    pole.poleB = info.second;
    pole.flagA = info.firstNew;
    pole.flagB = info.secondNew;
    poleInfos_.push_back(std::move(pole));
    if (connect && poleInfos_.back().index != 0) {
        tPoleInfo& previous = poleInfos_[poleInfos_.size() - 2];
        AddConnection(previous, poleInfos_.back(), value);
    }
}

// 0x1002ea4a2: the four fields of a tIntersectionInfo are the last four fields of a tPoleInfo.
bool cSC4PowerLineTool::IntersectionIsInList(const tIntersectionInfo& info) {
    for (const tPoleInfo& pole : poleInfos_) {
        if (pole.poleA == info.first && pole.poleB == info.second && pole.flagA == info.firstNew &&
            pole.flagB == info.secondNew) {
            return true;
        }
    }
    return false;
}

// 0x1002ea4f2: drop the pole occupant references of every pole and empty the vector.
void cSC4PowerLineTool::ReleasePoleInfos() {
    for (tPoleInfo& pole : poleInfos_) {
        if (pole.poleA != nullptr) {
            pole.poleA->Release();
            pole.poleA = nullptr;
        }
        if (pole.poleB != nullptr) {
            pole.poleB->Release();
            pole.poleB = nullptr;
        }
    }
    poleInfos_.clear();
}

// 0x1002ea594: the cell of a drag step that sits on the same diagonal as the given anchor (|dx| == |dz|).
// Steps before the straight part (straightSteps_) and steps with no such cell return their first cell / -1.
int cSC4PowerLineTool::GetPrimaryCell(int step, int anchorIndex) {
    const int first = static_cast<int>(steps_[static_cast<std::size_t>(step)].firstCell);
    const int last = static_cast<int>(steps_[static_cast<std::size_t>(step)].lastCell);
    if (straightSteps_ != 0 && straightSteps_ > step) return first;
    if (first > last) return -1;
    // The original reads anchors_[anchorIndex] unchecked (the solver walks past the last anchor on the drag's last
    // cell); the port keeps the step's first cell there.
    if (anchorIndex < 0 || anchorIndex >= static_cast<int>(anchors_.size())) return first;
    for (int cell = first; cell <= last; ++cell) {
        const int dx = static_cast<int>(anchors_[static_cast<std::size_t>(anchorIndex)].x) -
                       static_cast<int>(cells_[static_cast<std::size_t>(cell)].x);
        const int dz = static_cast<int>(anchors_[static_cast<std::size_t>(anchorIndex)].z) -
                       static_cast<int>(cells_[static_cast<std::size_t>(cell)].z);
        if (std::abs(dx) == std::abs(dz)) return cell;
    }
    return -1;
}

// 0x1002ea616. Quirk kept: the original returns true when the pole's x matches a pole's x OR its z matches a
// pole's z (two separate compares, not an &&), so a pole that merely shares a row or a column is "in the list".
bool cSC4PowerLineTool::PoleIsInList(IPowerPoleOccupant* pole) {
    if (pole == nullptr) return false;
    std::uint32_t x = 0, z = 0;
    pole->GetCell(x, z);
    for (const tPoleInfo& info : poleInfos_) {
        if (info.x == x) return true;
        if (info.z == z) return true;
    }
    return false;
}

// 0x1002e9f9e: the first pole occupant whose position maps to (x, z). The pole occupant query is a
// QueryInterface for 0xa8c86142 on every occupant of the cell; the position is the occupant's float3 and the
// cell comes from SL::spOccupantManager +0x98 PositionToStandardCityCell. The result carries one reference.
bool cSC4PowerLineTool::FindPoleOccupant(int x, int z, IPowerPoleOccupant*& out) {
    out = nullptr;
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (host == nullptr) return false;
    std::vector<ICityOccupant*> occupants;
    QueryCellOccupants(host, x, z, occupants);
    bool found = false;
    for (ICityOccupant* occupant : occupants) {
        IPowerPoleOccupant* pole = occupant->QueryPole();
        if (pole != nullptr) {
            float position[3] = {0.0f, 0.0f, 0.0f};
            occupant->GetPosition(position);
            int cellX = 0, cellZ = 0;
            host->PositionToCityCell(position[0], position[2], cellX, cellZ);
            if (cellX == x && cellZ == z) {
                if (out != nullptr) out->Release();
                out = pole;
                found = true;
            } else {
                pole->Release();
            }
        }
        occupant->Release();
    }
    occupants.clear();
    return found;
}

// 0x1002ea38c: the first cSC4PowerLineOccupant of the cell (iid 0xc98f3b24), with one reference.
bool cSC4PowerLineTool::CellIsOccupiedByPowerLine(std::uint32_t x, std::uint32_t z,
                                                  IPowerLineOccupant*& out) {
    out = nullptr;
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (host == nullptr) return false;
    std::vector<ICityOccupant*> occupants;
    QueryCellOccupants(host, static_cast<int>(x), static_cast<int>(z), occupants);
    bool found = false;
    for (ICityOccupant* occupant : occupants) {
        IPowerLineOccupant* wire = occupant->QueryPowerLine();
        if (wire != nullptr) {
            if (out != nullptr) out->Release();
            out = wire;
            found = true;
        }
        occupant->Release();
    }
    occupants.clear();
    return found;
}

// ============================================================================================================
// placement tests
// ============================================================================================================

// 0x1002eb380. Return values (the callers below use them as a small enum):
//   0 = no blocking occupant and no lot, 1 = the cell is zoned (the zone test failed),
//   2 = a building / lot occupant or a lot is there, 3 = another (non-underground) network occupant is there.
// Quirks kept: the zone query is made as zoneAt(x, z), zoneAt(x, z) again and then zoneAt(z, z) (the third
// call passes z for both coordinates); 0xc772bf98 (network occupant) is ignored when it answers
// 0x4a10afc2 (cISC4UndergroundOccupant3), and a lot turns 0 into 2.
std::uint8_t cSC4PowerLineTool::CellIsOKForPlacement(int x, int z) {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (host == nullptr) return 1;
    const int zone = host->ZoneAt(x, z);
    if (zone == 0 || host->ZoneAt(x, z) == 0x0f || host->ZoneAt(z, z) == 0x0e) {
        std::vector<ICityOccupant*> occupants;
        QueryCellOccupants(host, x, z, occupants);
        std::uint8_t result = 0;
        for (ICityOccupant* occupant : occupants) {
            const std::uint32_t type = occupant->GetType();
            if (type == kNetworkOccupantType) {
                // The original only ignores an underground-network piece; any other network occupant blocks.
                if (!occupant->QueriesAs(kIIDUndergroundOccupant3)) result = 3;
            } else if (type == kBuildingOccupantType) {
                result = 2;
            }
            occupant->Release();
        }
        occupants.clear();
        if (result != 0) return result;
        return host->HasLotAt(x, z) ? static_cast<std::uint8_t>(2) : static_cast<std::uint8_t>(0);
    }
    return 1;
}

// ============================================================================================================
// pole creation
// ============================================================================================================

// 0x1002e9d28: the centre of the cell in world units, half a cell above the terrain (or above the highest
// corner when the cell is not a valid network cell). The height offset +0x6dc is added by the caller.
void cSC4PowerLineTool::ComputePolePosition(int x, int z, float* out) {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    const float cellSize = host != nullptr ? host->TerrainCellSize() : 0.0f;
    out[0] = static_cast<float>(x) * cellSize + cellSize * 0.5f;
    out[2] = static_cast<float>(z) * cellSize + cellSize * 0.5f;
    float height;
    if (host != nullptr && !host->TerrainAllowsNetwork(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z))) {
        float corners[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        host->TerrainCornerHeights(x, z, corners);
        height = -FLT_MAX;
        for (float corner : corners) {
            // minss/maxss semantics: the second operand wins when the comparison is false (NaN propagates).
            if (!(corner <= height)) height = corner;
        }
    } else {
        height = host != nullptr ? host->TerrainHeightAt(out[0], out[2]) : 0.0f;
    }
    out[1] = height + 0.5f;
}

// 0x1002e9e2c: take the existing pole of the cell or create one, give it the exemplar for the pole's
// direction mask, store its position and mark it as a preview. The mask is OR'ed with the direction flags the
// existing pole already reports, which is what lets a new line pick the right junction model.
void cSC4PowerLineTool::CreatePowerPole(tPoleInfo& info) {
    std::uint32_t directionMask = info.connections;
    IPowerPoleOccupant* existing = nullptr;
    const bool found = FindPoleOccupant(static_cast<int>(info.x), static_cast<int>(info.z), existing);
    IPowerPoleOccupant* pole = nullptr;
    if (!found) {
        pole = GetPowerLineToolHost() != nullptr ? GetPowerLineToolHost()->CreatePoleOccupant() : nullptr;
        info.occupant = pole;
        if (pole != nullptr) pole->AddRef();
        info.created = true;
        cIGZUnknown* exemplar = GetOccupantExemplar(sPowerPoleForDirectionsFlag[directionMask & 0x0f], 0);
        if (exemplar != nullptr) {
            if (pole != nullptr) pole->InitFromExemplar(exemplar, directionMask);
            exemplar->Release();
        }
    } else {
        info.occupant = existing;
        // The existing pole's direction flags are read before they are overwritten.
        directionMask = directionMask | (existing != nullptr ? existing->DirectionFlags() : 0u);
        cIGZUnknown* exemplar = GetOccupantExemplar(sPowerPoleForDirectionsFlag[directionMask & 0x0f], 0);
        if (exemplar != nullptr) {
            if (existing != nullptr) existing->SetExemplar(exemplar);
            exemplar->Release();
        }
        info.created = false;
    }
    pole = info.occupant;
    if (pole != nullptr) {
        pole->SetDirectionFlags(directionMask);
        float position[3] = {0.0f, 0.0f, 0.0f};
        ComputePolePosition(static_cast<int>(info.x), static_cast<int>(info.z), position);   // the pole stands on the cell
        pole->SetPosition(position);
        pole->SetPreview(unknown98_ == 0);
    }
}

// ============================================================================================================
// solving
// ============================================================================================================

// i386 0x0022b798: the sign of each axis of the line between two cells, measured from the cell with the larger x.
void cSC4PowerLineTool::GetLineSlope(int x1, int z1, int x2, int z2, int& dx, int& dz) {
    if (x1 < x2) {
        dx = x1 - x2;
        dz = z1 - z2;
    } else {
        dx = x2 - x1;
        dz = z2 - z1;
    }
    if (dx != 0) dx = dx / std::abs(dx);
    if (dz != 0) dz = dz / std::abs(dz);
}

namespace {

// The intersection data a pole carries when it is placed on (or right after) the wire of an existing line: the two
// poles of that wire and their "connect to the new pole" flags (both start true).
void ResetIntersection(tIntersectionInfo& info) {
    info.first = nullptr;
    info.second = nullptr;
    info.firstNew = true;
    info.secondNew = true;
}

void ReleaseIntersection(tIntersectionInfo& info) {
    if (info.first != nullptr) info.first->Release();
    if (info.second != nullptr) info.second->Release();
    ResetIntersection(info);
}

}  // namespace

// 0x1002ea67e (i386 0x0022c98a). Walks every cell of every valid drag step. A placeable cell (CellIsOKForPlacement 0)
// becomes a pole when it is forced (an anchor, the drag's last cell, the first cell of the diagonal part that leaves the
// first anchor's row and column, or the primary cell of a step after poleSpacing steps without a pole), when it sits on
// an existing line whose wire the drag crosses at a right angle, or when the terrain hides the next pole from the last
// one; otherwise it is a wire cell of the current segment. A zoned cell (1) places a pole at the last placeable cell
// and breaks the chain; a lot or another network (2, 3) sets the failure reason. validSteps ends after the last step
// that placed a pole or a wire cell.
void cSC4PowerLineTool::DeterminePolePositions() {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    std::vector<tLineInfo> segment;
    const int anchorCount = static_cast<int>(anchors_.size());
    wireCellCount_ = 0;
    tIntersectionInfo intersection;
    ResetIntersection(intersection);
    ReleasePoleInfos();
    lineInfos_.clear();

    // The first cell of the diagonal part that leaves the first anchor's row and column is a forced pole.
    int forcedCell = -1;
    if (straightSteps_ < validSteps_ && straightSteps_ > 0) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(straightSteps_)];
        bool found = false;
        for (int cell = static_cast<int>(step.firstCell); cell <= static_cast<int>(step.lastCell); ++cell) {
            if (anchors_.empty()) { forcedCell = cell; found = true; break; }
            const SC4PointU& c = cells_[static_cast<std::size_t>(cell)];
            if (c.x == anchors_[0].x || c.z == anchors_[0].z) { forcedCell = cell; found = true; break; }
        }
        if (!found) forcedCell = -1;
    }

    int lastPoleStep = -1;      // the step of the last pole placed (-1: none yet)
    int candidateCell = -1;     // the last placeable cell since the last pole (-1: none)
    int lastStep = 0;           // the last step that placed something
    bool force = false;
    bool connect = false;       // the next pole connects to the previous one
    int polesInChain = 0;
    int anchorIndex = 0;
    float lastPolePosition[3] = {0.0f, 0.0f, 0.0f};

    for (int s = 0; s < validSteps_; ++s) {
        const int first = static_cast<int>(steps_[static_cast<std::size_t>(s)].firstCell);
        const int last = static_cast<int>(steps_[static_cast<std::size_t>(s)].lastCell);
        const int primary = GetPrimaryCell(s, anchorIndex);
        for (int cell = first; cell <= last; ++cell) {
            const std::uint32_t x = cells_[static_cast<std::size_t>(cell)].x;
            const std::uint32_t z = cells_[static_cast<std::size_t>(cell)].z;
            const std::uint8_t result = CellIsOKForPlacement(static_cast<int>(x), static_cast<int>(z));
            const bool isPrimary = cell == primary;
            bool forced = forcedCell == cell;
            if (forced) {
                forcedCell = -1;
                force = true;
            }
            if (anchorIndex < anchorCount &&
                ((anchors_[static_cast<std::size_t>(anchorIndex)].x == x && anchors_[static_cast<std::size_t>(anchorIndex)].z == z) ||
                 (s == validSteps_ - 1 && cell == last))) {
                ++anchorIndex;
                force = true;
                forced = true;
            }
            if (isPrimary && connect && lastPoleStep >= 0 &&
                static_cast<std::uint32_t>(poleSpacing_) <= static_cast<std::uint32_t>(s - lastPoleStep)) {
                force = true;
                forced = true;
            }
            const int lastValidCell = static_cast<int>(steps_[static_cast<std::size_t>(validSteps_ - 1)].lastCell);
            bool placePole = false;
            bool wireCell = false;
            if (result == 0) {
                failureReason_ = 0;
                IPowerPoleOccupant* existing = nullptr;
                const bool foundPole = FindPoleOccupant(static_cast<int>(x), static_cast<int>(z), existing);
                candidateCell = cell;
                if (!foundPole) {
                    IPowerLineOccupant* wire = nullptr;
                    if (CellIsOccupiedByPowerLine(x, z, wire)) {
                        IPowerPoleOccupant* a = nullptr;
                        IPowerPoleOccupant* b = nullptr;
                        bool connected = false;
                        wire->GetConnectedPoles(a, b, connected);
                        intersection.first = a;
                        intersection.second = b;
                        intersection.firstNew = true;
                        intersection.secondNew = true;
                        std::uint32_t ax = 0, az = 0, bx = 0, bz = 0;
                        if (a != nullptr) a->GetCell(ax, az);
                        if (b != nullptr) b->GetCell(bx, bz);
                        int dx = 0, dz = 0;
                        GetLineSlope(static_cast<int>(ax), static_cast<int>(az), static_cast<int>(bx), static_cast<int>(bz), dx, dz);
                        if (s < straightSteps_ && (dx == 0 || dz == 0)) {
                            // The wire runs along an axis and the drag crosses it: a pole here joins the two lines,
                            // unless the crossing was already taken by this drag.
                            if ((IntersectionIsInList(intersection) || PoleIsInList(a) || PoleIsInList(b)) && !forced) {
                                force = false;
                                ReleaseIntersection(intersection);
                            } else {
                                force = true;
                            }
                        } else {
                            ReleaseIntersection(intersection);
                        }
                        wire->Release();
                    }
                    if (!isPrimary) {
                        if (force) placePole = true;
                        else { lastStep = s; wireCell = true; }
                    } else if (force) {
                        placePole = true;
                    } else {
                        // Line of sight from the last pole's top to the next cell's pole top (SL::spRenderer +0x1e0):
                        // a hill in between needs a pole here.
                        if (cell < lastValidCell && !poleInfos_.empty() && host != nullptr) {
                            float next[3] = {0.0f, 0.0f, 0.0f};
                            const SC4PointU& n = cells_[static_cast<std::size_t>(cell + 1)];
                            ComputePolePosition(static_cast<int>(n.x), static_cast<int>(n.z), next);
                            next[1] += poleHeightOffset_;
                            float direction[3] = {next[0] - lastPolePosition[0], next[1] - lastPolePosition[1],
                                                  next[2] - lastPolePosition[2]};
                            const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                                           direction[2] * direction[2]);
                            if (length > 0.0f) {
                                direction[0] /= length;
                                direction[1] /= length;
                                direction[2] /= length;
                            }
                            float hit[3] = {0.0f, 0.0f, 0.0f};
                            if (host->RendererRayTest(lastPolePosition, direction, hit)) {
                                const float hx = hit[0] - lastPolePosition[0], hy = hit[1] - lastPolePosition[1],
                                            hz = hit[2] - lastPolePosition[2];
                                if (std::sqrt(hx * hx + hy * hy + hz * hz) <= length) placePole = true;
                            }
                        }
                        if (!placePole) {
                            lastStep = s;
                            force = false;
                            wireCell = true;
                        }
                    }
                } else {
                    // An existing pole on the cell. The original drops its reference only on the primary cell; the
                    // port drops it in every case (the pole stays referenced by the city).
                    if (existing != nullptr) existing->Release();
                    if (isPrimary || force) placePole = true;
                    else { lastStep = s; wireCell = true; }
                }
                if (placePole) {
                    PlacePole(cell, connect, static_cast<int>(lineInfos_.size()), intersection);
                    lineInfos_.push_back(segment);
                    segment.clear();
                    ResetIntersection(intersection);
                    ++polesInChain;
                    ComputePolePosition(static_cast<int>(x), static_cast<int>(z), lastPolePosition);
                    lastPolePosition[1] += poleHeightOffset_;
                    lastPoleStep = s;
                    lastStep = s;
                    candidateCell = -1;
                    force = false;
                    connect = true;
                }
            } else if (result == 1) {
                // A zoned cell ends the chain; the last placeable cell gets the pole that closes it.
                if (polesInChain != 1 || candidateCell != -1) {
                    if (candidateCell > 0 && connect) {
                        PlacePole(candidateCell, true, static_cast<int>(lineInfos_.size()), intersection);
                        lineInfos_.push_back(segment);
                        segment.clear();
                        ResetIntersection(intersection);
                        lastPoleStep = s;
                        candidateCell = -1;
                        force = true;
                        connect = false;
                        polesInChain = 0;
                    } else {
                        force = true;
                        connect = false;
                    }
                    continue;
                }
                // A chain of one unconnected pole is dropped (its intersection references stay with it, as the original).
                if (!poleInfos_.empty() && poleInfos_.back().connectionList.empty()) poleInfos_.pop_back();
                candidateCell = -1;
                force = true;
                connect = false;
                polesInChain = 0;
            } else {
                if ((!connect || lastPoleStep < 0) && !forced) {
                    wireCell = true;
                } else {
                    bool stop = true;
                    if (result == 2) {
                        failureReason_ = 0xf0000012;
                    } else {
                        if (result == 3) failureReason_ = 0xf0000013;
                        if (!forced) { wireCell = true; stop = false; }
                    }
                    if (stop) {
                        if (polesInChain != 1 || candidateCell != -1) {
                            candidateCell = -1;
                            continue;
                        }
                        if (!poleInfos_.empty() && poleInfos_.back().connectionList.empty()) {
                            tPoleInfo& back = poleInfos_.back();
                            if (back.poleA != nullptr) back.poleA->Release();
                            if (back.poleB != nullptr) back.poleB->Release();
                            poleInfos_.pop_back();
                        } else {
                            candidateCell = -1;
                        }
                        polesInChain = 0;
                    }
                }
            }
            if (wireCell && cell < lastValidCell) {
                tLineInfo info;
                info.x = x;
                info.z = z;
                info.line = nullptr;
                info.first = isPrimary;
                segment.push_back(info);
                ++wireCellCount_;
            }
        }
    }
    if (polesInChain == 1 && !poleInfos_.empty() && poleInfos_.back().connectionList.empty()) {
        tPoleInfo& back = poleInfos_.back();
        if (back.poleA != nullptr) back.poleA->Release();
        if (back.poleB != nullptr) back.poleB->Release();
        poleInfos_.pop_back();
    }
    ReleaseIntersection(intersection);   // the original leaves a pending crossing's references alive
    validSteps_ = lastStep + 1;
    poleCount_ = static_cast<int>(poleInfos_.size());
}


// 0x1002eb536: walk the cells from startIndex to endIndex by step and take the first one outside `rect` that is
// placeable (CellIsOKForPlacement == 0), create a pole there, connect it to `existing` and re-solve the drag.
// `other` is never read by the original (the decompiler shows the second pole argument unused).
void cSC4PowerLineTool::FindNewPolePosition(SC4CellRect rect, int startIndex, int endIndex, int step,
                                            std::vector<SC4PointU>& cells, IPowerPoleOccupant* existing,
                                            IPowerPoleOccupant* other) {
    (void)other;
    int remaining = endIndex - startIndex;
    for (int index = startIndex; remaining != 0; index += step, remaining += step) {
        if (index < 0 || index >= static_cast<int>(cells.size())) return;
        const SC4PointU cell = cells[static_cast<std::size_t>(index)];
        const bool outside = static_cast<std::int64_t>(cell.x) < rect.x0 ||
                             static_cast<std::int64_t>(cell.x) > rect.x1 ||
                             static_cast<std::int64_t>(cell.z) < rect.z0 ||
                             static_cast<std::int64_t>(cell.z) > rect.z1;
        if (!outside) continue;
        if (CellIsOKForPlacement(static_cast<int>(cell.x), static_cast<int>(cell.z)) != 0) continue;
        tPoleInfo info;
        info.x = cell.x;
        info.z = cell.z;
        info.cellIndex = index;
        if (existing != nullptr && GetPowerLineToolHost() != nullptr) {
            std::uint32_t poleX = 0, poleZ = 0;
            existing->GetCell(poleX, poleZ);
            const int direction = GetPowerLineToolHost()->Get0To3Direction(
                static_cast<int>(cell.x), static_cast<int>(cell.z), static_cast<int>(poleX),
                static_cast<int>(poleZ));
            info.connections = static_cast<std::uint8_t>(1u << (direction & 0x1f));
        }
        CreatePowerPole(info);
        IPowerPoleOccupant* pole = info.occupant;
        if (pole != nullptr) {
            if (info.created && GetPowerLineToolHost() != nullptr) GetPowerLineToolHost()->InsertPoleOccupant(pole);
            pole->Release();
            info.occupant = nullptr;
        }
        if (existing != nullptr && GetPowerLineToolHost() != nullptr) {
            std::uint32_t poleX = 0, poleZ = 0;
            existing->GetCell(poleX, poleZ);
            SC4PointU from{cell.x, cell.z};
            SC4PointU to{poleX, poleZ};
            anchors_.clear();
            anchors_.push_back(from);
            anchors_.push_back(to);
            GetPowerLineToolHost()->DrawNetworkLine(from, to, false, networkType_);
            validSteps_ = totalSteps_;
            DeterminePolePositions();
            PlacePoles(true);
        }
        return;
    }
}

// ============================================================================================================
// placement
// ============================================================================================================

// 0x1002eb7de (i386 0x0022d4c2). Pass A joins the poles of each connection group whose cells are one apart. Every pole
// info then gets its occupant (CreatePowerPole). Per pole: the wire of an existing line it splits is hidden (preview) or
// removed (placement), the poles of that wire that connect to the new pole are queued, the pair goes to the preview
// list, and, when placing, every new connection gets its wire occupants (one per drag cell of the segment, owned by the
// pole with the lower cell) and is added on both poles. Then the created poles go into the city (as highlighted
// previews or for good, with their wire occupants), and the queued poles are connected to the new ones: directly for a
// preview, through a re-solve of the drag between them when placing.
void cSC4PowerLineTool::PlacePoles(bool resolve) {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    std::vector<tIntersectionInfo> pending;

    // --- pass A: connection groups ------------------------------------------------------------------------------
    for (const std::vector<int>& group : connectionGroups_) {
        std::vector<int> indices;
        for (int groupCell : group) {
            int counter = 0;
            for (const tPoleInfo& info : poleInfos_) {
                if ((unknown98_ == 0 && info.occupant != nullptr) || info.cellIndex != groupCell) {
                    ++counter;
                    continue;
                }
                indices.push_back(counter);
                break;
            }
        }
        for (int a : indices) {
            for (int b : indices) {
                tPoleInfo& pa = poleInfos_[static_cast<std::size_t>(a)];
                tPoleInfo& pb = poleInfos_[static_cast<std::size_t>(b)];
                const float distance = std::fabs(static_cast<float>(pa.z) - static_cast<float>(pb.z)) +
                                       std::fabs(static_cast<float>(pa.x) - static_cast<float>(pb.x));
                if (distance == 1.0f) AddConnection(pa, pb, -1);
            }
        }
    }
    connectionGroups_.clear();

    // --- pass B: every pole info gets its occupant --------------------------------------------------------------
    for (tPoleInfo& info : poleInfos_) CreatePowerPole(info);

    // --- pass C: split wires, queue the joining poles, build the wires of the new connections ---------------------
    for (tPoleInfo& info : poleInfos_) {
        IPowerPoleOccupant* first = info.poleA;
        IPowerPoleOccupant* second = info.poleB;
        if (first != nullptr) {
            if (second != nullptr && host != nullptr) {
                if (unknown98_ == 0) host->DrawPoleConnection(first, second, false);
                else host->RemovePoleConnection(first, second, false);
            }
            if (info.flagA) {
                first->AddRef();
                if (info.occupant != nullptr) info.occupant->AddRef();
                pending.push_back(tIntersectionInfo{first, info.occupant, false, false, {}});
            }
        }
        if (second != nullptr) {
            if (first != nullptr && host != nullptr) {
                if (unknown98_ == 0) host->DrawPoleConnection(second, first, false);
                else host->RemovePoleConnection(second, first, false);
            }
            if (info.flagB) {
                second->AddRef();
                if (info.occupant != nullptr) info.occupant->AddRef();
                pending.push_back(tIntersectionInfo{second, info.occupant, false, false, {}});
            }
        }
        if (unknown98_ == 0 && first != nullptr && second != nullptr) {
            connectionList_.push_back(tIntersectionInfo{first, second, info.flagA, info.flagB, {}});
        } else {
            if (first != nullptr) first->Release();
            if (second != nullptr) second->Release();
        }
        info.poleA = nullptr;
        info.poleB = nullptr;

        IPowerPoleOccupant* pole = info.occupant;
        if (pole == nullptr || host == nullptr) continue;
        const int existingCount = pole->GetNumberOfConnections();
        for (const tConnectionInfo& connection : info.connectionList) {
            tPoleInfo& other = poleInfos_[connection.poleIndex];
            bool alreadyConnected = false;
            for (int i = 0; i < existingCount; ++i) {
                if (pole->GetConnectionOccupant(i) == other.occupant) {
                    alreadyConnected = true;
                    break;
                }
            }
            if (alreadyConnected || unknown98_ == 0) continue;
            std::vector<tLineInfo>* mine = nullptr;
            std::vector<tLineInfo>* theirs = nullptr;
            if (connection.value >= 0) {
                // The wire belongs to the pole with the lower cell (z first, then x).
                const bool lower = info.z < other.z || (info.z == other.z && info.x < other.x);
                std::vector<tLineInfo>& lines = lineInfos_[static_cast<std::size_t>(connection.value)];
                (lower ? mine : theirs) = &lines;
                for (tLineInfo& line : lines) {
                    if (line.line != nullptr) continue;
                    auto* wire = new cSC4PowerLineOccupant();
                    wire->AddRef();
                    wire->Init();
                    wire->SetPreview(unknown98_ == 0);
                    if (lower) wire->SetConnectedPoles(info.occupant, other.occupant, line.first);
                    else wire->SetConnectedPoles(other.occupant, info.occupant, line.first);
                    float position[3] = {0.0f, 0.0f, 0.0f};
                    ComputePolePosition(static_cast<int>(line.x), static_cast<int>(line.z), position);
                    wire->SetPosition(position);
                    line.line = wire;
                }
            }
            host->ConnectPoles(info.occupant, other.occupant, mine);
            host->ConnectPoles(other.occupant, info.occupant, theirs);
        }
    }

    // --- the segments' wire references: the poles hold their own now ---------------------------------------------
    if (unknown98_ != 0) {
        for (tPoleInfo& info : poleInfos_) {
            for (const tConnectionInfo& connection : info.connectionList) {
                if (connection.value <= 0) continue;   // quirk kept: the original skips segment 0
                for (tLineInfo& line : lineInfos_[static_cast<std::size_t>(connection.value)]) {
                    if (line.line != nullptr) {
                        line.line->Release();
                        line.line = nullptr;
                    }
                }
            }
        }
    }

    // --- the poles go into the city --------------------------------------------------------------------------------
    for (tPoleInfo& info : poleInfos_) {
        IPowerPoleOccupant* pole = info.occupant;
        if (pole == nullptr) continue;
        if (info.created && host != nullptr) host->InsertPoleOccupant(pole);
        if (unknown98_ == 0) {
            pole->SetModelState(3, false);
            if (info.created) createdPoles_.push_back(pole);
            else previewPoles_.push_back(pole);
            pole->AddRef();
        } else {
            pole->OnInserted();
        }
        pole->Release();
        info.occupant = nullptr;
    }

    // --- the poles of split wires join the new poles -----------------------------------------------------------------
    for (tIntersectionInfo& pair : pending) {
        IPowerPoleOccupant* first = pair.first;
        IPowerPoleOccupant* second = pair.second;
        if (host != nullptr && first != nullptr && second != nullptr) {
            if (unknown98_ == 0) {
                host->ConnectPoles(first, second, nullptr);
                host->ConnectPoles(second, first, nullptr);
            } else if (resolve) {
                ReleasePoleInfos();
                lineInfos_.clear();
                SC4PointU from{}, to{};
                first->GetCell(from.x, from.z);
                second->GetCell(to.x, to.z);
                DrawNetworkLine(from, to, false, networkType_);
                validSteps_ = totalSteps_;
                DeterminePolePositions();
                PlacePoles(false);
            }
        }
        if (first != nullptr) first->Release();
        if (second != nullptr) second->Release();
    }
}


// 0x1002ec3c2, PARTIAL. The original (1739 bytes) sets the +0x98 update flag, then walks every pole and wire
// occupant of the changed rectangle: poles outside the rectangle are destroyed, poles inside it have their
// connections broken (GetWireCells / RemoveConnection), wires whose endpoints survive are re-routed through
// FindNewPolePosition in both directions, and the destroyed poles are removed from the city. None of that
// occupant-level work is reproduced here; only the update flag the rest of the tool reads is recovered.
void cSC4PowerLineTool::UpdateOnZoneChange(SC4RectU rect, std::uint32_t network) {
    (void)rect;
    if (network != 0) unknown98_ = true;
}

// ============================================================================================================
// drag checks
// ============================================================================================================

// 0x1002ecaec: scan the steps and trim the drag to the last placeable one. Result 1 only sets the failure
// reason and continues; 2 stops and trims to the previous step; 3 trims to the previous step when the previous
// cell was placeable and this is the last step of the drag. Quirk kept: the original's "previous cell" result
// starts from an uninitialised register; the port starts it at 0.
void cSC4PowerLineTool::CheckForReservedTiles(int& step) {
    const int total = step;
    std::uint32_t valid = static_cast<std::uint32_t>(total - 1);
    std::uint8_t previousResult = 0;
    bool keep = total > 0;
    for (int s = 0; keep && s < total; ++s) {
        const std::uint32_t first = steps_[static_cast<std::size_t>(s)].firstCell;
        const std::uint32_t last = steps_[static_cast<std::size_t>(s)].lastCell;
        if (first > last) continue;
        for (std::uint32_t cell = first; keep && cell <= last; ++cell) {
            const std::uint8_t result =
                CellIsOKForPlacement(static_cast<int>(cells_[cell].x), static_cast<int>(cells_[cell].z));
            if (result < 4) {
                switch (result) {
                    case 1:
                        failureReason_ = 0xf0000011;
                        break;
                    case 2:
                        failureReason_ = 0xf0000012;
                        keep = false;
                        valid = static_cast<std::uint32_t>(s - 1);
                        break;
                    case 3:
                        failureReason_ = 0xf0000013;
                        if (previousResult == 0 && s == total - 1) {
                            keep = false;
                            valid = static_cast<std::uint32_t>(s - 1);
                        }
                        break;
                    default:
                        valid = static_cast<std::uint32_t>(s);
                        break;
                }
            }
            previousResult = result;
        }
    }
    step = static_cast<int>(valid + 1);
}

// 0x1002ecc80: highlight the cells of the accepted part of the drag in blue (0x800000ff) unless a pole already
// stands there, and the cells of the rejected part in red (0x80ff0000).
void cSC4PowerLineTool::SetTerrainHilight(int step) {
    if (preview_ == nullptr) return;
    int valid = validSteps_;
    if (step < valid) {
        for (int s = step; s < valid; ++s) {
            const std::uint32_t first = steps_[static_cast<std::size_t>(s)].firstCell;
            const std::uint32_t last = steps_[static_cast<std::size_t>(s)].lastCell;
            for (std::uint32_t cell = first; cell <= last; ++cell) {
                SC4PointU point{cells_[cell].x, cells_[cell].z};
                bool hasPole = false;
                for (const tPoleInfo& info : poleInfos_) {
                    if (info.x == point.x && info.z == point.z) {
                        hasPole = true;
                        break;
                    }
                }
                if (!hasPole) preview_->AddUntexturedSelection(point, 0x800000ffu, nullptr, 0.0f, false);
            }
            valid = validSteps_;
        }
    }
    const int total = totalSteps_;
    if (valid < total) {
        for (int s = valid; s < total; ++s) {
            const std::uint32_t first = steps_[static_cast<std::size_t>(s)].firstCell;
            const std::uint32_t last = steps_[static_cast<std::size_t>(s)].lastCell;
            for (std::uint32_t cell = first; cell <= last; ++cell) {
                SC4PointU point{cells_[cell].x, cells_[cell].z};
                preview_->AddUntexturedSelection(point, 0x80ff0000u, nullptr, 0.0f, false);
            }
        }
    }
}

// ============================================================================================================
// placement entry point
// ============================================================================================================

// 0x1002ecde4: solve the drag, place the poles, optionally auto-connect to the neighbours, then drop the pole
// infos again. The base calls AttemptNeighborConnections / PlaceNeighborConnections (0x10018bdc6 / 0x10018c640)
// which the base port does not define, so they go through the host seam.
bool cSC4PowerLineTool::DoNetworkSpecificPlacement(bool first, bool checkNeighbors) {
    wireCellCount_ = 0;
    ReleasePoleInfos();
    lineInfos_.clear();
    validSteps_ = totalSteps_;
    if (totalSteps_ > 1) {
        DeterminePolePositions();
        PlacePoles(true);
    }
    if (poleInfos_.empty()) validSteps_ = 0;
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (checkNeighbors && totalSteps_ <= validSteps_ && anchors_.size() > 1 && failureReason_ == 0 &&
        host != nullptr && host->AttemptNeighborConnections(first, networkType_) && unknown98_ != 0) {
        host->PlaceNeighborConnections();
    }
    if (unknown98_ == 0) SetTerrainHilight(0);
    ReleasePoleInfos();
    return true;
}

// ============================================================================================================
// lifecycle
// ============================================================================================================

// 0x1002ecf2a. Returns true even when the base Init fails (the original ignores its result); the tuning values
// come from the power-line exemplar {0x6534284a, 0xe7e2c2db, 0xc911e35b} property 0xa52160f5.
bool cSC4PowerLineTool::Init() {
    if (powerLineInited_) return true;
    if (!sbInited_) StaticInit();
    powerLineInited_ = true;
    cSC4NetworkTool::Init();
    poleSpacing_ = 0x14;
    poleHeightOffset_ = 7.0f;
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (host != nullptr) {
        const cISCPropertyHolder* holder = host->TuningPropertyHolder();
        if (holder != nullptr) {
            cSCPropertyHelp::GetPropertyValue(holder, 0x098b25c8, poleSpacing_);
            cSCPropertyHelp::GetPropertyValue(holder, 0x098b27d0, poleHeightOffset_);
            cSCPropertyHelp::GetPropertyValue(holder, 0x4a5f5953, poleCost_);
            cSCPropertyHelp::GetPropertyValue(holder, 0x0a5f596b, wireCost_);
        }
    }
    // The original drops the previous drag's pole and line-info vectors here (no Release of the occupants).
    poleInfos_.clear();
    lineInfos_.clear();
    return true;
}

// 0x1002ed0a4
bool cSC4PowerLineTool::StaticInit() {
    if (!sbInited_) {
        sbInited_ = true;
        cSC4NetworkTool::StaticInit();
        ReadPowerPoleInfos();
    }
    return true;
}

// 0x1002ed208: the "PowerPoles" section of the resource ini {0x8a5971c5, 0, 0x8a5993b9}, one key/value pair per
// direction mask. SC4GetResourceAsFile + cRZIniFile are behind the host seam.
void cSC4PowerLineTool::ReadPowerPoleInfos() {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (host == nullptr) return;
    cRZIniFile* ini = host->OpenPowerPoleIni();
    if (ini == nullptr) return;
    ini->EnumEntries(cRZString("PowerPoles"), &cSC4PowerLineTool::AddPowerPoleInfo, nullptr);
    delete ini;
}

// 0x1002ed32a
bool cSC4PowerLineTool::Shutdown() {
    if (powerLineInited_) {
        powerLineInited_ = false;
        return cSC4NetworkTool::Shutdown();
    }
    return true;
}

// ============================================================================================================
// cost / failure reason
// ============================================================================================================

// 0x1002ed348: the original only prices a solved solution (more than one pole).
int cSC4PowerLineTool::GetCostOfSolution() {
    if (static_cast<std::uint32_t>(poleCount_) > 1) {
        return static_cast<int>(static_cast<float>(wireCellCount_) * wireCost_) + poleCount_ * poleCost_;
    }
    return 0;
}

// 0x1002ed37c: reason 0 has no base string; when the last anchor is on a zoned cell the tool reports
// "you cannot build here" (SC4LoadString group 0x2a592fd1, instance 0x8aadd2b5).
void cSC4PowerLineTool::GetFailureReasonString(std::uint32_t reason, cRZString& out) {
    if (reason != 0) {
        cSC4NetworkTool::GetFailureReasonString(reason, out);
        return;
    }
    IPowerLineToolHost* host = GetPowerLineToolHost();
    if (host == nullptr || anchors_.empty()) return;
    const SC4PointU& anchor = anchors_.back();
    if (host->ZoneAt(static_cast<int>(anchor.x), static_cast<int>(anchor.z)) > 0) {
        host->LoadString(0x2a592fd1, 0x8aadd2b5, out);
    }
}

// 0x1002ed0c6: destroy the preview poles, remove them from the city and clear the intersection list. The original
// tests the base tool's occupant manager (this+0x88, always there in a city) before it removes and releases the created
// poles; the port removes them through the host, so it tests the host.
void cSC4PowerLineTool::ClearCellPreviews() {
    IPowerLineToolHost* host = GetPowerLineToolHost();
    for (IPowerPoleOccupant* pole : createdPoles_) {
        if (pole == nullptr) continue;
        pole->SetModelState(0, false);
        if (host != nullptr) {
            host->RemovePoleOccupant(pole, true);
            pole->Release();
        }
    }
    createdPoles_.clear();
    for (IPowerPoleOccupant* pole : previewPoles_) {
        if (pole == nullptr) continue;
        pole->SetModelState(0, false);
        pole->SetPreview(true);
        pole->Release();
    }
    previewPoles_.clear();
    for (tIntersectionInfo& info : connectionList_) {
        if (info.first != nullptr) {
            if (host != nullptr) host->DrawPoleConnection(info.first, info.second, true);
            info.first->Release();
            info.first = nullptr;
        }
        if (info.second != nullptr) {
            if (host != nullptr) host->DrawPoleConnection(info.second, info.second, true);
            info.second->Release();
            info.second = nullptr;
        }
    }
    connectionList_.clear();
    if (host != nullptr) host->ClearBaseCellPreviews();
}

}  // namespace sc4
