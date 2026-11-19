#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "sc4/RegionalTerrainMap.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

int main() {
    auto* map = new cSC4RegionalTerrainMap(250.0f, 100.0f, 4, 3, 16.0f);
    map->AddRef();
    CHECK(!map->IsInitialized() && map->Init() && map->IsInitialized() && map->Shutdown() && !map->IsInitialized());
    CHECK(map->CellCountX() == 4 && map->CellCountZ() == 3 && map->CellWidth() == 16.0f);
    CHECK(map->GetVertIndex(2, 3) == 3 * 5 + 2);
    std::int32_t x, z;
    map->GetVertex(17, x, z);
    CHECK(x == 2 && z == 3);

    CHECK(map->LocationIsInBounds(0, 0) && map->LocationIsInBounds(63.9f, 47.9f));
    CHECK(!map->LocationIsInBounds(64.0f, 0) && !map->LocationIsInBounds(0, 48.0f) && !map->LocationIsInBounds(-1, 5) && !map->LocationIsInBounds(5, -1));
    CHECK(!map->LocationIsInBounds(NAN, 5));

    CHECK(map->GetAltitude(3.0f, 4.0f) == 250.0f && map->GetAltitude(7) == 250.0f && map->GetAltitudeAtVertex(1, 2) == 250.0f);
    CHECK(map->MinHeight() == 250.0f && map->MaxHeight() == 250.0f && map->MinAllowedHeight() == 0.0f && map->MaxAllowedHeight() == 1023.0f);
    CHECK(map->GetSeaLevel() == 100.0f && map->GetWaterLevel(3) == 0.0f && !map->CellHasWater(1, 1) && map->CellHasLand(1, 1));
    CHECK(map->IsLandVertex(0, 0) && !map->IsWaterVertex(0, 0) && !map->IsSeaVertex(0, 0) && !map->AreSeaVertices(TerrainRect{}));
    float corners[4];
    map->GetCellCornerAltitudes(0, 0, corners);
    CHECK(corners[0] == 250.0f && corners[3] == 250.0f);
    TerrainVector3 n{9, 9, 9};
    map->GetFaceNormal(0, 0, n);
    CHECK(n.x == 0.0f && n.y == 1.0f && n.z == 0.0f);

    // grid index: floor of position / cell width, x + z * vertices per row
    CHECK(map->GetTerrainGridIndex(0.0f, 0.0f) == 0);
    CHECK(map->GetTerrainGridIndex(16.0f, 0.0f) == 1);
    CHECK(map->GetTerrainGridIndex(15.99f, 0.0f) == 0);
    CHECK(map->GetTerrainGridIndex(40.0f, 33.0f) == 2 + 2 * 5);
    CHECK(map->GetTerrainGridIndex(48.0f, 32.0f) == 3 + 2 * 5);       // exact multiples
    CHECK(map->GetTerrainGridIndex(32.0f, 16.0f) == 2 + 1 * 5);

    // rect fills: rows [top, bottom), columns [left, right]
    TerrainRect rect{1, 1, 3, 3};     // left, top, right, bottom
    std::vector<float> buf(5 * 5, -1.0f);
    map->GetAltitudes(rect, buf.data(), 5, 1);
    int filled = 0;
    for (float v : buf) filled += v == 250.0f;
    CHECK(filled == 2 * 3);          // 2 rows x 3 columns
    CHECK(buf[0] == 250.0f && buf[1] == 250.0f && buf[2] == 250.0f && buf[3] == -1.0f && buf[5] == 250.0f && buf[10] == -1.0f);
    std::fill(buf.begin(), buf.end(), -1.0f);
    map->GetWaterDepths(rect, buf.data(), 5, 1);
    int zeros = 0;
    for (float v : buf) zeros += v == 0.0f;
    CHECK(zeros == 6);
    std::fill(buf.begin(), buf.end(), -1.0f);
    map->GetAltitudes(TerrainRect{2, 1, 2, 1}, buf.data(), 5, 1);       // top == bottom: nothing
    CHECK(buf[0] == -1.0f);
    map->GetAltitudes(TerrainRect{2, 1, 2, 2}, buf.data(), 5, 1);       // single column still written inclusively
    CHECK(buf[0] == 250.0f && buf[1] == -1.0f);

    void* iface = nullptr;
    CHECK(map->QueryInterface(kGZIID_cISC4TerrainMap, &iface) && iface == map && map->QueryInterface(1, &iface) && !map->QueryInterface(5, &iface));
    map->Release(); map->Release(); map->Release();
    std::printf(failures ? "FAILED\n" : "sc4_regionalterrainmap ok\n");
    return failures ? 1 : 0;
}
