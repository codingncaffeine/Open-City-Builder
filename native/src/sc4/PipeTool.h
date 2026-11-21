// cSC4PipeTool — the water-pipe drawing tool: the concrete cSC4UndergroundNetworkTool of network type 4 (Mac i386
// 0x0012c8a0..0x0012cd7a, vtables 0x00bb6c88 / 0x00bb6de4). It fills the underground tool's two pure virtuals: the
// network interface id of its pieces (0x49c05b9f) and the creation of a cSC4PipeOccupant for a solved cell. The network
// manager keeps one per city as its tool for type 4; DestroyNetworkOccupants hands it the pipes a demolition removes.
//
// FindPipeOccupant / FindPipeConnectionOccupant (0x0012cca4 / 0x0012cbb8) are the network manager's pipe queries (its host forwards
// them here with the city's occupant manager, which the original reads from SL::spOccupantManager).
// The destructor traps as the underground tool's does: the tool is never destroyed.
#pragma once
#include <cstdint>

#include "sc4/PipeOccupant.h"
#include "sc4/UndergroundNetworkTool.h"

namespace sc4 {

class cISC4OccupantManager;
class cSC4PipeConnectionOccupant;

class cSC4PipeTool final : public cSC4UndergroundNetworkTool {
public:
    cSC4PipeTool();                                                    // 0x0012cb84

    std::uint32_t NetworkGZIID() override;                             // 0x0067386a
    using cSC4UndergroundNetworkTool::CreateNetworkOccupant;
    void CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4UndergroundOccupant*& out, float (*corners)[3],
                               std::uint32_t networkType) override;   // 0x0012c8a0

    // 0x0012cca4 / 0x0012cbb8 (static): the last pipe (pipe connection) among the occupants of city cell (x, z), with one
    // reference, or null; true when one was found.
    static bool FindPipeOccupant(cISC4OccupantManager* occupants, int x, int z, cSC4PipeOccupant*& out);
    static bool FindPipeConnectionOccupant(cISC4OccupantManager* occupants, int x, int z, cSC4PipeConnectionOccupant*& out);
};

}  // namespace sc4
