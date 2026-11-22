// cSC4CompressedRouteReader: a hand-traced route (a run of five steps, a crossing jump far along x, a jump whose larger axis is
// z, a one-cell sideways hop, the end), the header-only route and the empty route.
#include <cstdio>
#include <vector>

#include "sc4/CompressedRouteReader.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Expect { int x, z, type, nextType, runStep, entry, exit, crossing; };
void Walk(const std::vector<std::uint8_t>& route, const std::vector<Expect>& cells) {
    cSC4CompressedRouteReader reader;
    reader.SetData(&route);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        CompressedRouteCell c;
        const bool got = reader.GetNext(c);
        CHECK(got);
        if (!got) return;
        const Expect& e = cells[i];
        const bool same = c.x == e.x && c.z == e.z && c.type == e.type && c.nextType == e.nextType && c.runStep == e.runStep &&
                          c.entrySide == e.entry && c.exitSide == e.exit && c.crossing == e.crossing;
        if (!same)
            std::printf("  cell %zu: got (%d,%d) type %u next %u run %u entry %u exit %u crossing %u\n", i, c.x, c.z, c.type, c.nextType,
                        c.runStep, c.entrySide, c.exitSide, c.crossing);
        CHECK(same);
    }
    CompressedRouteCell after;
    CHECK(!reader.GetNext(after));
    CHECK(reader.Done());
}
}  // namespace

int main() {
    // Steps of the run: +x, +x, +z, -x, -z packed low bits first: 2 | 2<<2 | 3<<4 | 0<<6 = 0x3a, then 1.
    const std::vector<std::uint8_t> route = {0x05, 10, 20, 0x45, 5, 0x3a, 0x01, 0x86, 30, 20, 0x07, 31, 22, 0x08, 32, 22};
    Walk(route, {
                    {10, 20, 5, 5, 0, 0xff, 2, 0},
                    {11, 20, 5, 5, 1, 0, 2, 0},
                    {12, 20, 5, 5, 1, 0, 3, 0},
                    {12, 21, 5, 5, 1, 1, 0, 0},
                    {11, 21, 5, 5, 1, 2, 1, 0},
                    {11, 20, 5, 6, 1, 3, 2, 0},       // jump 19 cells along +x: leaves through +x
                    {30, 20, 6, 7, 0, 0, 3, 1},       // reached by the crossing; jump (+1, +2): z is larger, leaves through +z
                    {31, 22, 7, 8, 0, 0xff, 0xff, 0}, // jump (+1, 0): a sideways hop, no exit, entry blanked
                    {32, 22, 8, 8, 0, 1, 0xff, 0},    // the entry carried over the hop; the last cell
                });
    // A jump two cells back along z leaves through -z; one cell back along z through +z (the original's rule).
    Walk({0x01, 5, 5, 0x01, 5, 3, 0x01, 5, 2}, {
                                                   {5, 5, 1, 1, 0, 0xff, 1, 0},
                                                   {5, 3, 1, 1, 0, 3, 3, 0},
                                                   {5, 2, 1, 1, 0, 1, 0xff, 0},
                                               });
    Walk({0x03, 7, 9}, {{7, 9, 3, 3, 0, 0xff, 0xff, 0}});
    // A run of zero steps ends the route.
    Walk({0x02, 4, 4, 0x42, 0}, {{4, 4, 2, 2, 0, 0xff, 0xff, 0}});
    cSC4CompressedRouteReader empty;
    const std::vector<std::uint8_t> none;
    empty.SetData(&none);
    CompressedRouteCell c;
    CHECK(!empty.GetNext(c));
    if (failures == 0) std::printf("sc4_compressedroutereader_test: all checks passed\n");
    else std::printf("sc4_compressedroutereader_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
