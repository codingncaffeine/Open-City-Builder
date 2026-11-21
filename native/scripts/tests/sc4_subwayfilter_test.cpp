// cSC4SubwayOccupantFilter (the network manager's GetSubwayOccupantFilter / IsOccupantSubwayStation) against the water filter:
// surface network pieces, subway stations by their transit switch rules, the subway bool, elevated trains and the rest.
#include <cstdio>
#include <vector>

#include "sc4/ComplexViewOccupantFilter.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/Occupant.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
// An occupant of a given type (the elevated trains are vehicle occupants, type 0xc7cc75f1).
class TypedOccupant final : public cSC4Occupant {
public:
    explicit TypedOccupant(std::uint32_t type) { type_ = type; }
};
template <class T> T* Make(T* object) { object->AddRef(); return object; }
}  // namespace

int main() {
    cSC4SubwayOccupantFilter subway;
    cSC4WaterOccupantFilter water;
    subway.AddRef();
    water.AddRef();

    // a plain occupant: neither filter keeps it, and it is no station
    auto* plain = Make(new cSC4Occupant());
    CHECK(!subway.IsOccupantIncluded(plain));
    CHECK(!cSC4SubwayOccupantFilter::IsOccupantSubwayStation(plain));
    CHECK(!cSC4SubwayOccupantFilter::IsOccupantSubwayStation(nullptr));
    CHECK(!subway.IsPropertyHolderIncluded(nullptr));

    // transit switch rules without the subway (network 6) do not make a station; with it they do
    auto* depot = Make(new cSC4Occupant());
    cSCPropertyHelp::SetPropertyValue(depot->AsPropertyHolder(), 0xe90e25a1, std::vector<std::uint8_t>{1, 0, 2, 0});
    CHECK(!cSC4SubwayOccupantFilter::IsOccupantSubwayStation(depot));
    CHECK(!subway.IsOccupantIncluded(depot));
    auto* station = Make(new cSC4Occupant());
    cSCPropertyHelp::SetPropertyValue(station->AsPropertyHolder(), 0xe90e25a1, std::vector<std::uint8_t>{1, 0, 6, 3});
    CHECK(cSC4SubwayOccupantFilter::IsOccupantSubwayStation(station));
    CHECK(subway.IsOccupantIncluded(station));
    CHECK(subway.IsPropertyHolderIncluded(station->AsPropertyHolder()));
    CHECK(!water.IsOccupantIncluded(station));

    // the subway bool 0xcaa45671 keeps a holder (the water filter reads 0xcaa45672 instead)
    auto* flagged = Make(new cSC4Occupant());
    cSCPropertyHelp::SetPropertyValue(flagged->AsPropertyHolder(), 0xcaa45671, true);
    CHECK(subway.IsOccupantIncluded(flagged));
    CHECK(!cSC4SubwayOccupantFilter::IsOccupantSubwayStation(flagged));
    CHECK(!water.IsOccupantIncluded(flagged));
    cSCPropertyHelp::SetPropertyValue(flagged->AsPropertyHolder(), 0xcaa45671, false);
    CHECK(!subway.IsOccupantIncluded(flagged));

    // elevated trains: a vehicle occupant in group 0x4217 or 0x4208; another group or another type is not kept
    auto* car = Make(new TypedOccupant(0xc7cc75f1));
    CHECK(!subway.IsOccupantIncluded(car));
    car->AddOccupantGroup(0x4208);
    CHECK(subway.IsOccupantIncluded(car));
    auto* engine = Make(new TypedOccupant(0xc7cc75f1));
    engine->AddOccupantGroup(0x4217);
    CHECK(subway.IsOccupantIncluded(engine));
    auto* bus = Make(new TypedOccupant(0xc7cc75f1));
    bus->AddOccupantGroup(0x4200);
    CHECK(!subway.IsOccupantIncluded(bus));
    auto* notVehicle = Make(new TypedOccupant(0x12345678));
    notVehicle->AddOccupantGroup(0x4217);
    CHECK(!subway.IsOccupantIncluded(notVehicle));

    // a surface network piece (flags 0x1f4f) is kept by both filters; a piece with none of those flags only by its holder
    auto* road = Make(new cSC4NetworkOccupant());
    road->SetNetworkFlag(0x1);
    CHECK(subway.IsOccupantIncluded(road) && water.IsOccupantIncluded(road));
    auto* other = Make(new cSC4NetworkOccupant());
    other->SetNetworkFlag(0x20);
    CHECK(!subway.IsOccupantIncluded(other));

    for (cSC4Occupant* object : std::vector<cSC4Occupant*>{plain, depot, station, flagged, car, engine, bus, notVehicle, road, other})
        object->Release();
    if (failures == 0) std::printf("sc4_subwayfilter: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
