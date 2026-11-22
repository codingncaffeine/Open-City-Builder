// Stream operators for the simulation's small value types (0x1000ce8b3..0x1000cfd0c, 0x100182e4c, 0x100215cb1,
// 0x10030d31a): the random generator, the cell iterators, occupant lists, response curves, dispatch records and rectangles.
// `>>` reads from a cIGZIStream, `<<` writes to a cIGZOStream; field order is the stream order.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/MiscFunctions.h"
#include "rz/cRZRandom.h"
#include "sc4/RandomIterator.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 { class cISC4Occupant; }

// Walks the cells on the border of a rectangle (inclusive edges): the whole first row, the two end cells of each middle row,
// the whole last row. Layout: +0 state (0 fresh, 1 walking, 2 done), +1 direction (0 = rows; the only one the walk uses),
// +4 x, +8 z, +0xc the x step, +0x10 the rectangle. A fresh iterator starts at x = z = 0 with step 1.
struct BoundaryIterator {
    std::int8_t state = 0;
    std::int8_t direction = 0;
    std::int32_t side = 0, step = 0, length = 1;   // x, z, the x step
    cRZRect rect{static_cast<std::int32_t>(0x80000000u), static_cast<std::int32_t>(0x80000000u), 0x7fffffff, 0x7fffffff};

    // i386 0x0060cfe4: the right / bottom edges never fall below the left / top ones.
    void SetClipRect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom) {
        rect.bottom = bottom; rect.right = right; rect.top = top; rect.left = left;
        if (rect.right < left) rect.right = left;
        if (rect.bottom < rect.top) rect.bottom = rect.top;
    }
    // i386 0x006087f6
    bool GetNext(std::int32_t& x, std::int32_t& z) {
        if (state == 2) return false;
        if (state == 0) {
            state = 1;
        } else if (direction == 0) {
            side += length;
            if (side > rect.right) {
                if (rect.bottom <= step) { state = 2; return false; }
                side = rect.left;
                ++step;
                if (rect.bottom <= step) length = 1;
                else length = rect.right - side < 1 ? 1 : rect.right - side;
            }
        }
        x = side;
        z = step;
        return true;
    }
};

// Walks every cell of a rectangle in rows. Layout: +0 state, +4 / +8 position, +0xc count, +0x10 the rectangle.
struct ScanningIterator {
    std::int8_t state = 0;
    std::int32_t x = 0, z = 0;
    std::uint32_t remaining = 0;
    cRZRect rect;
};

// One dispatched vehicle of the dispatch manager (cISC4DispatchManager::DispatchInfo).
struct DispatchInfo {
    std::int32_t id = 0;               // +0
    std::uint32_t source = 0;          // +4
    std::uint32_t destination = 0;     // +8
    bool active = false;               // +0x10
    float values[3] = {0, 0, 0};       // +0x14, +0x18, +0x1c
    cIGZUnknown* vehicle = nullptr;    // +0x20 (class 0x26bb767e)
    cIGZUnknown* target = nullptr;     // +0x28 (class 0xa9b40f05)
    cIGZUnknown* route = nullptr;      // +0x30 (class 0xc9b6d7d6)
};

// SC4Rect<unsigned char> is four bytes, SC4Rect<int> four 32-bit values.
struct SC4RectByte { std::uint8_t v[4] = {0, 0, 0, 0}; };

cIGZIStream& operator>>(cIGZIStream& s, cRZRandom& v);                         // @ 0x1000ce8b3
cIGZOStream& operator<<(cIGZOStream& s, const cRZRandom& v);                   // @ 0x1000ce92a
cIGZIStream& operator>>(cIGZIStream& s, BoundaryIterator& v);                  // @ 0x1000ce99f
cIGZOStream& operator<<(cIGZOStream& s, const BoundaryIterator& v);            // @ 0x1000cea25
cIGZIStream& operator>>(cIGZIStream& s, RandomIterator& v);                    // @ 0x1000cea88
cIGZOStream& operator<<(cIGZOStream& s, const RandomIterator& v);              // @ 0x1000ceb93
cIGZIStream& operator>>(cIGZIStream& s, ScanningIterator& v);                  // @ 0x1000cec88
cIGZOStream& operator<<(cIGZOStream& s, const ScanningIterator& v);            // @ 0x1000ced01
// Occupant lists are saved as a count and each occupant as a class-0x26bb767e serializable; a count of 10,000,000 or more
// (or zero) loads nothing. @ 0x1000cfb37 / 0x1000cfbd7
cIGZIStream& operator>>(cIGZIStream& s, std::list<sc4::cISC4Occupant*>& v);
cIGZOStream& operator<<(cIGZOStream& s, const std::list<sc4::cISC4Occupant*>& v);
cIGZIStream& operator>>(cIGZIStream& s, cSC4ResponseCurve& v);                 // @ 0x1000cfc24
cIGZOStream& operator<<(cIGZOStream& s, const cSC4ResponseCurve& v);           // @ 0x1000cfd0c
cIGZIStream& operator>>(cIGZIStream& s, DispatchInfo& v);                      // @ 0x100215cb1
cIGZOStream& operator<<(cIGZOStream& s, const DispatchInfo& v);                // @ 0x100215d52
cIGZOStream& operator<<(cIGZOStream& s, const SC4RectByte& v);                 // @ 0x100182e4c
cIGZOStream& operator<<(cIGZOStream& s, const SC4RectInt& v);                  // @ 0x10030d31a
