#include <cstdio>

#include "sc4/AmbienceOccupantFilters.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
class Lot final : public ISC4LotInfo {
public:
    Lot(int s, int w) : state(s), wealth(w) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::int32_t GetState() override { return state; }
    std::int32_t GetWealth() override { return wealth; }
    int state, wealth, refs = 1;
};
class World final : public IAmbienceWorld {
public:
    std::int32_t CityCellsX() override { return 256; }
    std::int32_t CityCellsZ() override { return 256; }
    ISC4LotInfo* LotAtCell(int, int) override { return &lot; }
    ISC4LotInfo* LotOfOccupant(cISC4Occupant*) override { lot.AddRef(); return &lot; }
    Lot lot{4, 3};
};
class Development final : public IBuildingDevelopmentHost {
public:
    const cRZString& ExemplarName(std::uint32_t) override { return name; }
    const cRZString& BuildingName(std::uint32_t) override { return name; }
    const BuildingProfile& Profile(std::uint32_t) override { return profile; }
    cRZString name;
    BuildingProfile profile;
};
Development gDevelopment;
cSC4BuildingOccupant* NewBuilding(int zone) {
    gDevelopment.profile.purpose = zone;
    auto* b = new cSC4BuildingOccupant();
    b->AddRef();
    return b;
}
}  // namespace

int main() {
    World world;
    SetAmbienceWorld(&world);
    auto* o = new cSC4Occupant();
    o->AddRef();
    o->AddOccupantGroup(0x4200);
    auto* bus = cAmbienceOccupantFilterIsBus::Create(); bus->AddRef();
    auto* taxi = cAmbienceOccupantFilterIsTaxi::Create(); taxi->AddRef();
    CHECK(bus->IsTrueOccupant(o) && !taxi->IsTrueOccupant(o));
    auto* traffic = cAmbienceOccupantFilterIsTraffic::Create(); traffic->AddRef();
    CHECK(!traffic->IsTrueOccupant(o));
    o->AddOccupantGroup(0x4002);
    CHECK(!traffic->IsTrueOccupant(o));                       // cached for this occupant (sic)
    auto* fresh = cAmbienceOccupantFilterIsTraffic::Create(); fresh->AddRef();
    CHECK(fresh->IsTrueOccupant(o));
    auto* aband = cAmbienceOccupantFilterIsAbandoned::Create(); aband->AddRef();
    CHECK(aband->IsTrueOccupant(o));
    auto* high = cAmbienceOccupantFilterIsHighWealth::Create(); high->AddRef();
    auto* low = cAmbienceOccupantFilterIsLowWealth::Create(); low->AddRef();
    CHECK(high->IsTrueOccupant(o) && !low->IsTrueOccupant(o));
    auto* powered = cAmbienceOccupantFilterIsPowered::Create(); powered->AddRef();
    CHECK(powered->IsTrueOccupant(o) && world.lot.refs == 1);
    auto* distressed = cAmbienceOccupantFilterIsDistressed::Create(); distressed->AddRef();
    world.lot.state = 5;
    CHECK(distressed->IsTrueOccupant(o));
    SetBuildingDevelopment(&gDevelopment);
    auto* b = NewBuilding(2);
    cISC4Occupant* bo = static_cast<cSC4Occupant*>(b);
    auto* comm = cAmbienceOccupantFilterIsCommercial::Create(); comm->AddRef();
    auto* res = cAmbienceOccupantFilterIsResidential::Create(); res->AddRef();
    auto* building = cAmbienceOccupantFilterIsBuilding::Create(); building->AddRef();
    CHECK(comm->IsTrueOccupant(bo) && !res->IsTrueOccupant(bo) && building->IsTrueOccupant(bo));
    void* raw = nullptr;
    CHECK(bus->QueryInterface(kGZIID_cIAmbienceOccupantFilter, &raw));
    static_cast<cIAmbienceOccupantFilter*>(raw)->Release();
    CHECK(!bus->QueryInterface(0x1234, &raw));
    for (cIAmbienceOccupantFilter* f : std::initializer_list<cIAmbienceOccupantFilter*>{bus, taxi, traffic, fresh, aband, high, low, powered, distressed, comm, res, building}) f->Release();
    b->Release();
    o->Release();
    std::puts(failures ? "sc4_ambiencefilters: FAILED" : "sc4_ambiencefilters: ok");
    return failures != 0;
}
