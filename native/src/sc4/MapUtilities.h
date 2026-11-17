// cSC4MapUtilities — the static grid helpers the simulators share (i386 0x0009df98..0x0009f556). This file holds the
// distance floods: Expand, FloodFillDistanceSetup / FloodFillDistance on two tract grids (the commercial simulator's
// proximity maps), and the vector form the terrain's water proximity uses. The radius and distance effects live with
// the simulators that call them.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/cSC4SimGrid.h"

namespace sc4 {

class cSC4MapUtilities {
public:
    using Rows = std::vector<std::vector<std::uint8_t>>;

    // i386 0x0009de12: every cell holding `distance` lowers its four neighbours (clamped to the grid's edges) to
    // distance + 1; true when a cell changed.
    static bool Expand(Rows& grid, int countX, int countZ, std::uint8_t distance);

    // i386 0x0009e71e: seed the distance grid from the source grid: 0 where the source holds `key`, 0xff elsewhere.
    // The tract shifts of the two grids decide how cells match: the same shift compares cell to cell, a finer
    // destination reads its source cell, a coarser one scans the source cells it covers.
    // DEFECT KEPT (the original's, read in the Mac build's instructions and in the PC game's saved maps, which hold
    // its result): every write goes to element 0 of the destination column and, at the same shift, the comparison
    // reads element 0 of the source column; the rest of the grid keeps its old values.
    static void FloodFillDistanceSetup(cSC4SimGrid<std::uint8_t>* grid, const cSC4SimGrid<std::uint8_t>* source,
                                       std::uint8_t key);
    // i386 0x0009e8e0: the setup, then forward and backward sweeps along every column and every row (the running
    // distance restarts from 255 at each sweep and is lowered by any smaller cell) until a whole pass changes nothing.
    static void FloodFillDistance(cSC4SimGrid<std::uint8_t>* grid, const cSC4SimGrid<std::uint8_t>* source,
                                  std::uint8_t key);
    // i386 0x0009f376: Expand outward from `start` for up to `count` distances (the byte sum wraps as in the
    // original: a start of 0xff expands nothing); answers the first distance that changed nothing, else start + count.
    static std::uint8_t FloodFillDistance(Rows& grid, std::uint8_t start, std::uint8_t count);
};

}  // namespace sc4
