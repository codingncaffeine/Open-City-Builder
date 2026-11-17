#include "sc4/MapUtilities.h"

namespace sc4 {

bool cSC4MapUtilities::Expand(Rows& grid, int countX, int countZ, std::uint8_t distance) {
    const std::uint8_t next = static_cast<std::uint8_t>(distance + 1);
    bool changed = false;
    const auto lower = [&](int x, int z) {
        std::uint8_t& cell = grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
        if (next < cell) {
            cell = next;
            changed = true;
        }
    };
    for (int x = 0; x < countX; ++x) {
        for (int z = 0; z < countZ; ++z) {
            if (grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] != distance) continue;
            lower(x - 1 < 0 ? 0 : x - 1, z);
            lower(x + 1 >= countX ? countX - 1 : x + 1, z);
            lower(x, z - 1 < 0 ? 0 : z - 1);
            lower(x, z + 1 >= countZ ? countZ - 1 : z + 1);
        }
    }
    return changed;
}

void cSC4MapUtilities::FloodFillDistanceSetup(cSC4SimGrid<std::uint8_t>* grid, const cSC4SimGrid<std::uint8_t>* source,
                                              std::uint8_t key) {
    if (!source || !grid) return;
    Rows& rows = grid->GetGridData();
    const Rows& from = source->GetGridData();
    const int countX = grid->GetTractCountX();
    const int countZ = grid->GetTractCountZ();
    const int shift = grid->GetTractShift() - source->GetTractShift();
    for (int x = 0; x < countX; ++x) {
        for (int z = 0; z < countZ; ++z) {
            std::uint8_t& out = rows[static_cast<std::size_t>(x)][0];   // the defect: element 0 of the column
            if (shift == 0) {
                out = from[static_cast<std::size_t>(x)][0] == key ? 0 : 0xff;
            } else if (shift < 0) {
                out = from[static_cast<std::size_t>(x >> -shift)][static_cast<std::size_t>(z >> -shift)] == key ? 0 : 0xff;
            } else {
                std::uint8_t value = 0xff;
                const int x1 = (x + 1) << shift, z0 = z << shift, z1 = (z + 1) << shift;
                for (int sx = x << shift; sx < x1 && value; ++sx)
                    for (int sz = z0; sz < z1; ++sz)
                        if (from[static_cast<std::size_t>(sx)][static_cast<std::size_t>(sz)] == key) {
                            value = 0;
                            break;
                        }
                out = value;
            }
        }
    }
}

void cSC4MapUtilities::FloodFillDistance(cSC4SimGrid<std::uint8_t>* grid, const cSC4SimGrid<std::uint8_t>* source,
                                         std::uint8_t key) {
    if (!source || !grid) return;
    FloodFillDistanceSetup(grid, source, key);
    const int countZ = grid->GetTractCountZ();
    const int countX = grid->GetTractCountX();
    Rows& rows = grid->GetGridData();
    // One step of a sweep: the running distance grows by one per cell and is lowered by any smaller cell; the bits that
    // differ between a lowered cell's old and new value say whether the pass changed anything.
    const auto step = [](std::uint8_t& cell, unsigned& run, unsigned& changed) {
        ++run;
        const unsigned value = cell;
        if (static_cast<int>(run) < static_cast<int>(value)) {
            changed |= value ^ run;
        } else {
            run = value;
        }
        cell = static_cast<std::uint8_t>(run);
    };
    unsigned changed = 0;
    do {
        changed = 0;
        for (int x = 0; x < countX; ++x) {
            std::vector<std::uint8_t>& column = rows[static_cast<std::size_t>(x)];
            unsigned run = 0xff;
            for (int z = 0; z < countZ; ++z) step(column[static_cast<std::size_t>(z)], run, changed);
            run = 0xff;
            for (int z = countZ - 1; z >= 0; --z) step(column[static_cast<std::size_t>(z)], run, changed);
        }
        for (int z = 0; z < countZ; ++z) {
            unsigned run = 0xff;
            for (int x = 0; x < countX; ++x) step(rows[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)], run, changed);
            run = 0xff;
            for (int x = countX - 1; x >= 0; --x) step(rows[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)], run, changed);
        }
    } while (changed != 0);
}

std::uint8_t cSC4MapUtilities::FloodFillDistance(Rows& grid, std::uint8_t start, std::uint8_t count) {
    const int countX = static_cast<int>(grid.size());
    if (countX < 1) return 0;
    const int countZ = static_cast<int>(grid[0].size());
    const std::uint8_t end = static_cast<std::uint8_t>(start + count);
    for (std::uint8_t distance = start; distance < end; ++distance)
        if (!Expand(grid, countX, countZ, distance)) return distance;
    return end;
}

}  // namespace sc4
