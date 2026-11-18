// cISC4PipeOccupantView — what the pipe-burst disaster asks of a water pipe: its cell, its centre, whether it joins a
// neighbour, and its occupant. The original QueryInterfaces a pipe for 0x49c05b9f and calls the network occupant vtable
// directly (i386 cSC4PipeOccupant table 1: +0xec GetOccupiedCell, +0x148 GetPosition, +0xbc ConnectsToNeighbor(edge, 4));
// the occupant is the pipe + 4. The port's pipe answers 0x49c05b9f with the class itself (the pipe tool casts it), so
// the disaster code asks for this view with the port-only id below; cSC4PipeOccupant answers it with an embedded view.
#pragma once
#include <cstdint>

#include "gz/cIGZUnknown.h"

namespace sc4 {

class cISC4Occupant;

constexpr std::uint32_t kGZIID_PortPipeOccupantView = 0x50506976;   // port-only

class cISC4PipeOccupantView : public cIGZUnknown {
public:
    virtual void GetCell(std::uint32_t& x, std::uint32_t& z) = 0;   // slot 0x1d8
    // What the burst effect needs of a pipe (defaults answer "nothing").
    virtual void GetCenter(float* xyz) { xyz[0] = xyz[1] = xyz[2] = 0.0f; }   // slot 0x290
    // direction: 0 west (x - 1), 1 north (z - 1), 2 east (x + 1), 3 south (z + 1); kind: the network type (4 = pipe)
    virtual bool IsConnected(int direction, int kind) { (void)direction; (void)kind; return false; }   // slot 0x178
    virtual cISC4Occupant* AsOccupant() { return nullptr; }                  // the occupant sub-object (+8)
};

}  // namespace sc4
