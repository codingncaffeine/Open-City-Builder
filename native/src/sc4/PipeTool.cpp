// cSC4PipeTool — see PipeTool.h.
#include "sc4/PipeTool.h"

#include <cstring>

#include "sc4/OccupantManager.h"
#include "sc4/PipeConnectionOccupant.h"
#include "sc4/PipeOccupant.h"

namespace sc4 {

// 0x0012cb84: the underground tool of network type 4; byte +0x3d4 cleared (no reader in the recovered code).
cSC4PipeTool::cSC4PipeTool() : cSC4UndergroundNetworkTool(kPipeNetworkType) {}

// 0x0067386a
std::uint32_t cSC4PipeTool::NetworkGZIID() { return kGZCLSID_cSC4PipeOccupant; }

// 0x0012c8a0: the cell's four corners (cell size from the terrain, heights from the solved vertices), a new pipe made
// the cell's occupant with the cell's network flags, its corners, the tool's tuning, the terrain heights under its
// vertices, its edges for this network and the type's demolition cost. The network type argument is unused.
void cSC4PipeTool::CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4UndergroundOccupant*& out,
                                         float (*corners)[3], std::uint32_t networkType) {
    (void)networkType;
    INetworkToolHost* toolHost = GetNetworkToolHost();
    const float size = toolHost != nullptr ? toolHost->TerrainCellSize() : 16.0f;   // world +0x44 vslot 0x18
    const float x0 = static_cast<float>(cell->x) * size;
    const float z0 = static_cast<float>(cell->z) * size;
    const float c[4][3] = {{x0, cell->vertexHeights[0], z0},
                           {x0, cell->vertexHeights[1], size + z0},
                           {size + x0, cell->vertexHeights[2], size + z0},
                           {size + x0, cell->vertexHeights[3], z0}};
    std::memcpy(corners, c, sizeof c);

    auto* pipe = new cSC4PipeOccupant();
    pipe->AddRef();                                    // vslot 0x4: the reference handed back through `out`
    cell->occupant = pipe;                             // cell +0xc (the port's cell keeps it uncounted)
    pipe->SetNetworkFlag(cell->networkFlags);          // vslot 0x28
    pipe->Init(c);                                     // vslot 0xc
    cSC4UndergroundOccupant::SetTuningParams(TuningA(), TuningFlag(), TuningB());   // vslot 0x1d0: +0x3b0, +0x3b4, +0x3b8
    float terrain[4] = {};
    IUndergroundNetworkToolHost* host = GetUndergroundNetworkToolHost();
    for (int v = 0; v < 4; ++v)                        // world +0x44 vslot 0x34
        terrain[v] = host != nullptr ? host->TerrainHeight(static_cast<std::uint32_t>(cell->vertexBaseHeights[v])) : 0.0f;
    pipe->SetTerrainHeights(terrain);                  // vslot 0x1c8
    pipe->SetEdgeTypesFlag(static_cast<std::uint8_t>(networkType_), cell->tileNeighborhoods[networkType_]);   // vslot 0x94
    pipe->SetCostOfDemolition(static_cast<std::int64_t>(placement_.unknownA8[9]));   // vslot 0x100 (i386 +0x36c, high 0)
    out = pipe;
}

namespace {
// The shared body of the two queries: every occupant of the cell (spOccupantManager vslot 0xa4, any occupant, unlimited) is
// asked for `iid`; each answer replaces (and releases) the previous one, so the last match wins. The list's references are
// dropped as it is walked.
template <class T>
bool FindCellOccupant(cISC4OccupantManager* occupants, int x, int z, std::uint32_t iid, T*& out) {
    out = nullptr;
    if (occupants == nullptr) return false;
    const int cell[2] = {x, z};
    OccupantList list;
    occupants->GetOccupantsByStandardCityCells(list, cell, cell, static_cast<cISC4OccupantFilter*>(nullptr), 0xffffffffu);
    bool found = false;
    for (cISC4Occupant* occupant : list) {
        void* raw = nullptr;
        if (occupant->QueryInterface(iid, &raw) && raw) {
            if (out != nullptr) out->Release();
            out = static_cast<T*>(raw);
            found = true;
        }
        occupant->Release();
    }
    return found;
}
}  // namespace

// 0x0012cca4
bool cSC4PipeTool::FindPipeOccupant(cISC4OccupantManager* occupants, int x, int z, cSC4PipeOccupant*& out) {
    return FindCellOccupant(occupants, x, z, kGZCLSID_cSC4PipeOccupant, out);
}

// 0x0012cbb8
bool cSC4PipeTool::FindPipeConnectionOccupant(cISC4OccupantManager* occupants, int x, int z,
                                              cSC4PipeConnectionOccupant*& out) {
    return FindCellOccupant(occupants, x, z, kGZCLSID_cSC4PipeConnectionOccupant, out);
}

}  // namespace sc4
