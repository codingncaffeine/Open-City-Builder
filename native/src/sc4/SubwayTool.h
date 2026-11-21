// cSC4SubwayTool — the subway drawing tool: the concrete cSC4UndergroundNetworkTool of network type 7 (Mac i386
// 0x0012c46c..0x0012c86c, vtables 0x00bb6aa8 / 0x00bb6c04). It fills the underground tool's two pure virtuals: the
// network interface id of its pieces (0xca16374f) and the creation of a cSC4SubwayOccupant for a solved cell. The network
// manager's GetNetworkTool(7, true) makes one (operator new 0x698); the destructor traps as the underground tool's does,
// so the tool is never destroyed.
#pragma once
#include <cstdint>

#include "sc4/SubwayOccupant.h"
#include "sc4/UndergroundNetworkTool.h"

namespace sc4 {

class cSC4SubwayTool final : public cSC4UndergroundNetworkTool {
public:
    cSC4SubwayTool();                                                  // 0x0012c86c

    std::uint32_t NetworkGZIID() override;                             // 0x00673742
    using cSC4UndergroundNetworkTool::CreateNetworkOccupant;
    void CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4UndergroundOccupant*& out, float (*corners)[3],
                               std::uint32_t networkType) override;   // 0x0012c46c
};

}  // namespace sc4
