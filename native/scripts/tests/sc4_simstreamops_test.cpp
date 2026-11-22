#include <vector>
#include <utility>
#include <cstdio>

#include "rz/PortMemoryStreams.h"
#include "sc4/SimStreamOps.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    {   // BoundaryIterator (i386 0x0060cfe4 / 0x006087f6): a fresh one walks the border of (0,0)-(3,2) inclusive, row by row
        BoundaryIterator bi;
        bi.SetClipRect(0, 0, 3, 2);
        std::vector<std::pair<int, int>> cells;
        std::int32_t x = 0, z = 0;
        while (bi.GetNext(x, z) && cells.size() < 50) cells.emplace_back(x, z);
        const std::vector<std::pair<int, int>> want = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {3, 1}, {0, 2}, {1, 2}, {2, 2}, {3, 2}};
        CHECK(cells == want);
        CHECK(!bi.GetNext(x, z) && bi.state == 2);
        BoundaryIterator flat;                       // right / bottom below left / top are raised to them
        flat.SetClipRect(0, 0, -5, -5);
        CHECK(flat.rect.right == 0 && flat.rect.bottom == 0);
        CHECK(flat.GetNext(x, z) && x == 0 && z == 0 && !flat.GetNext(x, z));
    }
    {   // random generator state round trip
        cRZRandom a(1234);
        a.RandomUint32Uniform(10); a.RandomDoubleGaussian();
        PortMemoryOStream os; os << a;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cRZRandom b(99); is >> b;
        CHECK(a.RandomUint32Uniform() == b.RandomUint32Uniform() && a.RandomDoubleGaussian() == b.RandomDoubleGaussian());
        CHECK(os.bytes().size() == 4 + 4 + 4 + 8);
    }
    {   // iterators
        BoundaryIterator bi; bi.state = 2; bi.direction = -1; bi.side = 3; bi.step = 4; bi.length = 5; bi.rect = {1, 2, 3, 4};
        ScanningIterator si; si.state = 1; si.x = -7; si.z = 8; si.remaining = 99; si.rect = {5, 6, 7, 8};
        RandomIterator ri;
        ri.SetRect(0, 0, 3, 3);
        ri.SetModular(false);
        ri.Reset(true);
        int a, b; ri.Next(a, b);
        PortMemoryOStream os; os << bi << si << ri;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        BoundaryIterator bi2; ScanningIterator si2; RandomIterator ri2;
        is >> bi2 >> si2 >> ri2;
        CHECK(bi2.state == 2 && bi2.direction == -1 && bi2.length == 5 && bi2.rect.bottom == 4);
        CHECK(si2.x == -7 && si2.remaining == 99 && si2.rect.left == 5 && !is.GetError());
        int a2, b2, c, d;
        bool more = true;
        std::vector<std::pair<int, int>> rest1, rest2;
        while (more) { more = ri.Next(c, d); if (more) rest1.push_back({c, d}); }
        more = true;
        while (more) { more = ri2.Next(c, d); if (more) rest2.push_back({c, d}); }
        CHECK(rest1 == rest2 && !rest1.empty());
        (void)a2; (void)b2;
    }
    {   // response curve
        cSC4ResponseCurve curve;
        curve.SetResponseCurveData(std::vector<SC4Point>{{0, 0}, {10, 100}});
        PortMemoryOStream os; os << curve;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4ResponseCurve c2; is >> c2;
        std::vector<SC4Point> pts; c2.GetResponseCurveData(pts);
        CHECK(pts.size() == 2 && pts[1].x == 10 && pts[1].y == 100 && os.bytes().size() == 4 + 16);
    }
    {   // rectangles and empty occupant list
        PortMemoryOStream os;
        os << SC4RectByte{{1, 2, 3, 4}} << SC4RectInt{-1, 2, 3, 4};
        CHECK(os.bytes().size() == 4 + 16 && os.bytes()[2] == 3);
        std::list<sc4::cISC4Occupant*> none;
        PortMemoryOStream o2; o2 << none;
        CHECK(o2.bytes().size() == 4);
        PortMemoryIStream i2(o2.bytes().data(), o2.bytes().size());
        std::list<sc4::cISC4Occupant*> loaded{nullptr, nullptr};
        i2 >> loaded;
        CHECK(loaded.empty());
        PortMemoryOStream big; big << std::uint32_t(10000000);
        PortMemoryIStream i3(big.bytes().data(), big.bytes().size());
        i3 >> loaded;
        CHECK(loaded.empty());   // counts of 10 million or more load nothing
    }
    std::printf(failures ? "FAILED\n" : "sc4_simstreamops ok\n");
    return failures != 0;
}
