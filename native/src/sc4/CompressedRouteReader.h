// cSC4CompressedRouteReader — walks a compressed route cell by cell (Mac i386 SetData 0x000990b0, GetNext 0x00098d40;
// decompile scratch/s85/ghidra/ferry.c). The format (written by the path finders): a header token, x, z; then either a
// jump token (type in bits 0..5, bit 7 = a ferry or off-map crossing) followed by the next cell's x, z, or a run token
// (bit 6 set, type in bits 0..5) followed by a step count and the steps packed two bits each, low bits first
// (0 = x-1, 1 = z-1, 2 = x+1, 3 = z+1).
//
// GetNext hands out the cell the reader stands on and moves to the next one; after the last cell it hands out that cell
// once more with exit direction 0xff and from then on answers false. The reader keeps a pointer into the vector it was
// given: the vector must outlive the walk.
//
// Layout kept from the binary (i386, 0x18 bytes in the original's embedded readers): +0 the vector, +4 the cursor,
// +8 steps left in the run, +9 the step's index in its packed byte, +0xa the packed byte, +0xb done, then the next record:
// +0xc x, +0xe z, +0x10 type, +0x12 reached by a run step, +0x13 entry side, +0x15 reached by a crossing.
#pragma once
#include <cstdint>
#include <vector>

namespace sc4 {

// cSC4CompressedRouteReader::UncompressedData (10 bytes).
struct CompressedRouteCell {
    std::int16_t x = 0;            // +0
    std::int16_t z = 0;            // +2
    std::uint8_t type = 0;         // +4 the cell's travel type
    std::uint8_t nextType = 0;     // +5 the next cell's travel type (the cell's own on the last cell)
    std::uint8_t runStep = 0;      // +6 reached by a step of a run
    std::uint8_t entrySide = 0;    // +7 the side the route came in through (0xff after a diagonal jump)
    std::uint8_t exitSide = 0;     // +8 the side it leaves through (0xff: a diagonal jump or the end)
    std::uint8_t crossing = 0;     // +9 reached by a jump whose token had bit 7
};

class cSC4CompressedRouteReader {
public:
    void SetData(const std::vector<std::uint8_t>* data);   // i386 0x000990b0
    bool GetNext(CompressedRouteCell& out);                 // i386 0x00098d40
    bool Done() const { return done_; }

private:
    const std::vector<std::uint8_t>* data_ = nullptr;   // +0
    std::size_t cursor_ = 0;                            // +4 (an index here, a pointer in the original)
    std::uint8_t stepsLeft_ = 0;                        // +8
    std::uint8_t stepIndex_ = 0;                        // +9
    std::uint8_t packed_ = 0;                           // +0xa
    bool done_ = true;                                  // +0xb
    CompressedRouteCell next_;                          // +0xc..+0x15
};

}  // namespace sc4
