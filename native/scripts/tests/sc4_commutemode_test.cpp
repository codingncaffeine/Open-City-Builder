// Commute mode choice: cSC4PathFinder configured as SetupTrip configures a commuter trip (travel types 0x1cf, starting types
// walk + car, the strategy's starting cost array), over a street with sidewalks and an optional subway line, with the shipped
// tuning (speeds of the Traffic Simulator exemplar, start costs, max trip time 6, nearest destination attractiveness 0.09).
#include <cstdio>
#include <vector>

#include "sc4/PathFinder.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

constexpr int kW = 40, kD = 9;
constexpr int kStreetZ = 4, kSubwayZ = 5, kLotZ = 3;
constexpr std::uint8_t kStreet = 3, kSubway = 5;   // network piece types
// 1 / (km/h x 1000 / 60 / 16 m cells): walk 3.5 and car 21 on a street, subway 150
constexpr float kWalkStreet = 1.0f / (3.5f * 1000.0f / 60.0f / 16.0f), kCarStreet = 1.0f / (21.0f * 1000.0f / 60.0f / 16.0f),
                kSubwayTube = 1.0f / (150.0f * 1000.0f / 60.0f / 16.0f);

// A street on row 4 from x = 1 to 38 (walk and car, both directions), lots on row 3 at the start (1) and the destination, and
// with `subway` a subway line on row 5 from x = 2 to 30 with a walk-to-subway change from (2, 4) and back to walk onto (30, 4).
struct City : IPathFinderHost {
    bool subway = false;
    int destX = 6;
    int CityWidthInCells() override { return kW; }
    int CityDepthInCells() override { return kD; }
    float TripScaleForDisplay() override { return 1.0f; }
    std::uint32_t TripCapacityLimit() override { return 0; }
    std::uint32_t UsedTripCapacity(cISCPropertyHolder*, std::uint32_t) override { return 0; }
    bool NeighborConnectionCost(int, int, float& c) override { c = 0; return true; }
    bool Street(int x, int z) const { return z == kStreetZ && x >= 1 && x <= 38; }
    bool Tube(int x, int z) const { return subway && z == kSubwayZ && x >= 2 && x <= 30; }
    std::uint16_t Connections(int t, int x, int z) override {
        // east-west: entered from the west (side 0) leave east (2), entered from the east (2) leave west (0)
        if ((t == 0 || t == 1) && Street(x, z)) return std::uint16_t((1u << 2) | (1u << 8));
        if (t == 6 && Tube(x, z)) return std::uint16_t((1u << 2) | (1u << 8) | (1u << (2 + 4 * 1)));   // also on from the north
        return 0;
    }
    std::uint16_t Switches(int t, int x, int z) override {
        std::uint16_t s = 0xffff;
        if (subway && t == 0 && x == 2 && z == kStreetZ) s = std::uint16_t((s & ~(0xfu << 12)) | (6u << 12));   // south: walk -> subway
        if (subway && t == 6 && x == 30 && z == kSubwayZ) s = std::uint16_t((s & ~(0xfu << 4)) | (0u << 4));    // north: subway -> walk
        return s;
    }
    bool CellNetwork(int n, int x, int z, float& inverse, std::uint8_t& type, float& extra) override {
        inverse = 0.001f;
        extra = 0.0f;
        if ((n == 0 || n == 1) && Street(x, z)) { type = kStreet; return true; }
        if (n == 3 && Tube(x, z)) { type = kSubway; return true; }
        return false;
    }
    bool LotAtCell(int x, int z, SC4RectInt32& rect, cISCPropertyHolder*& holder) override {
        holder = nullptr;
        if (z != kLotZ || (x != 1 && x != destX)) return false;
        rect = {x, z, x, z};
        return true;
    }
    float InverseMaxSpeed(int t, int type) override {
        if (type == kStreet && t == 0) return kWalkStreet;
        if (type == kStreet && t == 1) return kCarStreet;
        if (type == kSubway && t == 6) return kSubwayTube;
        return 3.4028235e38f;
    }
    bool AffectedByTraffic(int) override { return false; }
    float NearestDestinationAttractiveness() override { return 0.09f; }
};

// the shipped Trip Starting Cost arrays: neutral (0xA92356BC), mass transit (0xEA8C3CDB), car preference (0xCAD64136)
float gNeutral[9] = {0.0f, 0.4f}, gTransit[9] = {0.0f, 1.95f}, gCar[9] = {0.1f, 0.0f};

// One commuter search from the lot at (1, 3) to the lot at (destX, 3); returns the travel types of the route (0 = not found).
std::uint32_t Commute(City& city, int destX, float* startCosts) {
    city.destX = destX;
    cSC4PathFinder finder(&city);
    finder.AddRef();
    finder.Init();
    finder.SetTripDestinationFilter(3);
    finder.SetStartRect({1, kLotZ, 1, kLotZ});
    finder.SetEndRect({destX, kLotZ, destX, kLotZ});
    finder.ClearAllFlags();
    finder.ClearAllTravelTypeFlags();
    finder.SetTravelTypeFlag(0x1cf, false);
    finder.SetTravelTypeFlag(0x3, true);
    finder.SetStartingCostArray(startCosts);
    finder.SetMaxTripLength(6.0f);
    finder.SetTrafficDensity(1);
    if (finder.FindPath() != 1) return 0;
    std::vector<std::uint8_t> route;
    std::uint32_t types = 0;
    finder.GetCompressedRoute(route, types);
    return types;
}

}  // namespace

int main() {
    City city;
    // mass-transit preference: a short trip walks (5 cells x 0.274 = 1.37 < 1.95 + 0.24), a long one drives
    CHECK(Commute(city, 6, gTransit) == 0x1);
    CHECK(Commute(city, 21, gTransit) == 0x2);
    // car preference drives even a short trip; neutral walks one cell and drives five
    CHECK(Commute(city, 6, gCar) == 0x2);
    CHECK(Commute(city, 2, gNeutral) == 0x1);
    CHECK(Commute(city, 6, gNeutral) == 0x2);
    // beyond the max trip time on foot (29 cells x 0.274 > 6) the mass-transit commuter drives, unless a subway runs there:
    // one cell on foot, 28 by subway, one on foot (about 0.73 against 1.95 + 1.38 by car)
    CHECK(Commute(city, 30, gTransit) == 0x2);
    city.subway = true;
    CHECK(Commute(city, 30, gTransit) == 0x41);
    // the car-preference commuter drives although the subway trip costs less (0.83 against 1.38): beyond the 4-cell search rect
    // the heuristic adds 0.09 per cell, more than a car pays per street cell, so the car frontier (ahead at the rect edge, 2.49
    // against 2.75) only gets cheaper and reaches the destination before the subway frontier is expanded again (weighted A*)
    CHECK(Commute(city, 30, gCar) == 0x2);
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("sc4_commutemode: ok\n");
    return 0;
}
