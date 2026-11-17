// cSC4MapUtilities distance floods: the tract-grid FloodFillDistance (with the original's setup defect, whose result is
// what the saved cities hold: column element 0 = 1, the rest 0), its finer / coarser source cases, and the vector form
// the terrain's water proximity uses (Expand, the byte wrap of a 0xff start).
#include <cstdio>
#include <vector>

#include "sc4/MapUtilities.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

class City final : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 8; }
    std::int32_t CitySizeZInCells() const override { return 8; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

cSC4SimGrid<std::uint8_t>* Grid(int tractSize) {
    auto* grid = new cSC4SimGrid<std::uint8_t>();
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(tractSize);
    return grid;
}

int CountValue(const cSC4SimGrid<std::uint8_t>& grid, std::uint8_t value) {
    int count = 0;
    for (const auto& column : grid.GetGridData())
        for (std::uint8_t v : column) count += v == value;
    return count;
}

void TestSameShift() {
    cSC4SimGrid<std::uint8_t>* grid = Grid(1);
    cSC4SimGrid<std::uint8_t>* source = Grid(1);
    CHECK(grid->GetTractCountX() == 8 && grid->GetTractCountZ() == 8);
    // No source cell holds the key: a new grid ends as the saved maps hold it, column element 0 = 1, the rest 0.
    cSC4MapUtilities::FloodFillDistance(grid, source, 7);
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 8; ++z) CHECK(grid->GetTractValue(x, z) == (z == 0 ? 1 : 0));
    // The key at element 0 of column 3 seeds 0 there; a key anywhere else in a column is never read.
    source->SetTractValue(3, 0, 7);
    source->SetTractValue(5, 4, 7);
    cSC4MapUtilities::FloodFillDistance(grid, source, 7);
    CHECK(grid->GetTractValue(3, 0) == 0 && grid->GetTractValue(2, 0) == 1 && grid->GetTractValue(5, 0) == 1);
    CHECK(CountValue(*grid, 1) == 7 && CountValue(*grid, 0) == 57);
    // Only element 0 of each column is seeded (0xff); the backward sweep brings it down to its neighbour + 1 and the other
    // cells keep their old values.
    grid->SetTractValues(200);
    source->SetTractValues(0);
    cSC4MapUtilities::FloodFillDistance(grid, source, 7);
    CHECK(grid->GetTractValue(0, 0) == 201 && grid->GetTractValue(4, 7) == 200 && CountValue(*grid, 200) == 56);
    // The sweeps alone: one zero at element 0 spreads along its column and then across the rows.
    grid->SetTractValues(200);
    source->SetTractValue(2, 0, 7);
    cSC4MapUtilities::FloodFillDistance(grid, source, 7);
    CHECK(grid->GetTractValue(2, 0) == 0 && grid->GetTractValue(2, 5) == 5 && grid->GetTractValue(6, 0) == 4 &&
          grid->GetTractValue(6, 7) == 11 && grid->GetTractValue(0, 7) == 9);
    // A null grid or source does nothing.
    cSC4MapUtilities::FloodFillDistance(grid, nullptr, 7);
    cSC4MapUtilities::FloodFillDistance(nullptr, source, 7);
    grid->Release();
    source->Release();
}

void TestOtherShifts() {
    // A finer destination (tract 1) reads its source tract (tract 2) at (x >> 1, z >> 1).
    cSC4SimGrid<std::uint8_t>* fine = Grid(1);
    cSC4SimGrid<std::uint8_t>* coarse = Grid(2);
    CHECK(coarse->GetTractCountX() == 4);
    coarse->SetTractValue(1, 0, 9);
    fine->SetTractValues(50);
    cSC4MapUtilities::FloodFillDistanceSetup(fine, coarse, 9);
    // element 0 of every column gets the answer of the LAST z (z = 7 -> source z 3): never the key, so 0xff
    CHECK(fine->GetTractValue(2, 0) == 0xff && fine->GetTractValue(2, 1) == 50);
    coarse->SetTractValue(1, 3, 9);
    cSC4MapUtilities::FloodFillDistanceSetup(fine, coarse, 9);
    CHECK(fine->GetTractValue(2, 0) == 0 && fine->GetTractValue(3, 0) == 0 && fine->GetTractValue(4, 0) == 0xff);
    // A coarser destination scans the source cells it covers.
    cSC4SimGrid<std::uint8_t>* source = Grid(1);
    source->SetTractValue(5, 7, 4);   // inside coarse tract (2, 3): the last z of column 2
    cSC4MapUtilities::FloodFillDistanceSetup(coarse, source, 4);
    CHECK(coarse->GetTractValue(2, 0) == 0 && coarse->GetTractValue(1, 0) == 0xff && coarse->GetTractValue(3, 0) == 0xff);
    fine->Release();
    coarse->Release();
    source->Release();
}

void TestVectorForm() {
    std::vector<std::vector<std::uint8_t>> grid(5, std::vector<std::uint8_t>(5, 9));
    grid[2][2] = 0;
    CHECK(cSC4MapUtilities::FloodFillDistance(grid, 0, 3) == 3);
    CHECK(grid[2][2] == 0 && grid[2][3] == 1 && grid[3][3] == 2 && grid[0][2] == 2 && grid[0][0] == 9 && grid[4][3] == 3);
    // Every cell reached before the count runs out: the first distance that changed nothing.
    std::vector<std::vector<std::uint8_t>> small(2, std::vector<std::uint8_t>(2, 9));
    small[0][0] = 0;
    CHECK(cSC4MapUtilities::FloodFillDistance(small, 0, 6) == 2);
    CHECK(small[1][1] == 2);
    // A 0xff start wraps (0xff + 3 = 2): nothing expands and the answer is the wrapped end.
    std::vector<std::vector<std::uint8_t>> none(3, std::vector<std::uint8_t>(3, 0xff));
    CHECK(cSC4MapUtilities::FloodFillDistance(none, 0xff, 3) == 2 && none[1][1] == 0xff);
    std::vector<std::vector<std::uint8_t>> empty;
    CHECK(cSC4MapUtilities::FloodFillDistance(empty, 0, 3) == 0);
    // Expand clamps at the edges: a corner cell lowers itself through the clamp only when it is larger.
    std::vector<std::vector<std::uint8_t>> edge(2, std::vector<std::uint8_t>(3, 9));
    edge[0][0] = 4;
    CHECK(cSC4MapUtilities::Expand(edge, 2, 3, 4));
    CHECK(edge[0][0] == 4 && edge[1][0] == 5 && edge[0][1] == 5 && edge[1][1] == 9);
    CHECK(!cSC4MapUtilities::Expand(edge, 2, 3, 7));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash keeps the failures printed before it
    City city;
    SetSimGridCity(&city);
    TestSameShift();
    TestOtherShifts();
    TestVectorForm();
    SetSimGridCity(nullptr);
    if (failures == 0) {
        std::printf("map utilities ok\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
