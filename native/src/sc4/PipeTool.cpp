// cSC4PipeTool — see PipeTool.h.
#include "sc4/PipeTool.h"

#include <cstring>

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

}  // namespace sc4
