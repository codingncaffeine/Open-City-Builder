// SpiralIterator — walks the cells of a rectangle in an outward spiral from a start cell (GetNext 0x1004f45e0,
// DoXZEnd 0x1004fe1f0). Used by the disaster managers, the plumbing simulator's water search and the landfill zone.
#pragma once
#include <cstdint>

namespace sc4 {

struct SpiralIterator {
    int x = 0, z = 0;                  // +0, +4 the current cell
    int targetX = 0, targetZ = 0;      // +8, +0xc the end of the current run
    int minX = 0, minZ = 0;            // +0x10, +0x14
    int maxX = 0, maxZ = 0;            // +0x18, +0x1c
    std::uint8_t mode = 1;             // +0x20 (0 or 1: which turn flips the direction)
    int direction = 1;                 // +0x24 signed run length step
    std::uint8_t state = 0;            // +0x28 0 not started, 1 running, 2 finished
    std::uint8_t onZ = 1;              // +0x29 the run is along z
    std::uint8_t edgesHit = 0;         // +0x2a bits 1/2 min/max x edges, 4/8 min/max z edges; 0xf = all four seen
    int turns = 0;                     // +0x2c

    // The constructor the callers inline: a spiral over [0, width) x [0, height) starting at (startX, startZ).
    static SpiralIterator Over(int startX, int startZ, int width, int height);
    // The original's constructor (i386 0x0061d480): an unbounded clip rect, mode 1, started at (0, 0).
    static SpiralIterator Unbounded();
    void Reset(int startX, int startZ);                       // i386 0x0061ca90
    void SetClipRect(int x0, int z0, int x1, int z1);         // i386 0x00620606 (a max below its min is raised to it)
    bool GetNext(int& outX, int& outZ);        // @ 0x1004f45e0
    bool DoXZEnd();                            // @ 0x1004fe1f0
};

}  // namespace sc4
