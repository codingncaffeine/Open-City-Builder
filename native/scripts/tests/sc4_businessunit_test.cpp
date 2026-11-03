// Business units: cSC4BusinessUnit with real lots, and the lot developer's PickUnitToJoin / UpdateTravelDesignates /
// UpdateTravelPopulations / RemoveLotFromItsBusinessUnit (Mac i386 0x000f3dbc / 0x000f4dec / 0x000ee72e / 0x0064ff94).
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "sc4/BusinessFeature.h"
#include "sc4/Lot.h"
#include "sc4/LotDeveloper.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
constexpr std::uint32_t kFactory = 0x100, kOffice = 0x200, kFarm = 0x500;
constexpr std::uint32_t kWorkers = 0x7100;

struct Building final : cIGZUnknown {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
} g_building;

class LotHostFake final : public ILotHost {
public:
    LotBuildingProfile factory, farm;
    std::map<const cSC4Lot*, std::map<int, std::vector<cSC4Lot*>>> neighbours;   // by edge
    std::set<const cSC4Lot*> road, built;
    std::uint8_t ZoneTypeAt(int, int) override { return 8; }
    std::int32_t SimulatorDay() override { return 100; }
    float TerrainAltitude(float, float) override { return 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t t) override { return t == kFarm ? &farm : t ? &factory : nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override { return purpose * 0x100 + wealth; }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int, int) override { return 0; }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t) override { return nullptr; }
    void PostLotMessage(std::uint32_t, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) override {}
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 1.0f; }
    float TravelVolumeFactor() override { return 1.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = out[1] = out[2] = 0; }
    void JobsPerOccupant(float out[4], int) override { out[0] = out[1] = out[2] = out[3] = 0; }
    void ExemplarJobs(const cSC4Lot&, float*) override {}
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override { available = false; return false; }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void NeighborLots(const cSC4Lot& lot, int edge, std::vector<cSC4Lot*>& out) override {
        for (cSC4Lot* n : neighbours[&lot][edge]) out.push_back(n);
    }
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    bool RoadAccessCells(const cSC4Lot& lot, int) override { return road.count(&lot) != 0; }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot& lot) override { return built.count(&lot) ? &g_building : nullptr; }
    void WriteSerializable(cIGZOStream&, cIGZUnknown*) override {}
    cIGZUnknown* ReadSerializable(cIGZIStream&, std::uint32_t) override { return nullptr; }
};

class Construction final : public ILotConstructionServices {
public:
    std::uint32_t pick = 0;
    cISC4LotConfigurationRef* ConfigurationRef(cSC4LotConfiguration*) override { return nullptr; }
    cSC4LotConfiguration* Configuration(cISC4LotConfigurationRef*) override { return nullptr; }
    std::int32_t BuildingPurpose(std::uint32_t type) override { return type == kFactory ? 7 : type == kFarm ? 5 : 3; }
    std::uint32_t BuildingWealth(std::uint32_t) override { return 1; }
    cISC4BuildingOccupant* AddNewBuildingOccupant(std::uint32_t, float[3], int) override { return nullptr; }
    void CellCornerToPosition(int, int, float&, float&) override {}
    float TerrainAltitude(float, float) override { return 0.0f; }
    void DemolishBlockingOccupants(const float[3], const float[3]) override {}
    void StartConstructionOccupant(cSC4Lot*) override {}
    cSC4Lot* LotAt(int, int) override { return nullptr; }
    std::uint32_t RandomBelow(std::uint32_t range) override { return pick < range ? pick : 0; }
    bool LotTextureExists(std::uint32_t) override { return false; }
    void ObjectPropTypes(const cSC4LotConfigurationObject&, std::vector<std::uint32_t>&) override {}
    cISC4Occupant* AddLotProp(std::uint32_t, const float[3], std::int32_t) override { return nullptr; }
    bool FloraTypeValid(std::uint32_t) override { return false; }
    void FloraTypesInFamily(std::vector<std::uint32_t>&, std::uint32_t) override {}
    cISC4Occupant* AddFlora(std::uint32_t, float[3]) override { return nullptr; }
};

class DeveloperHost final : public IBaseLotDeveloperHost {
public:
    float VertexHeight(int) override { return 0; }
    float VertexWaterHeight(int) override { return 0; }
    float HeightAt(float, float) override { return 0; }
    float WaterHeightAt(float, float) override { return 0; }
    bool VertexIsLevelled(int, int) override { return false; }
    int VerticesPerRow() override { return 65; }
    float CityWidthMetres() override { return 1024; }
    float CityDepthMetres() override { return 1024; }
    int LevelVertices(float, bool, bool, int, int, int, int, void*, int, int) override { return 0; }
    void CellOfPosition(float, float, int&, int&) override {}
    void OccupantBounds(void*, float[3], float[3]) override {}
};

cSC4Lot* MakeLot(int x, std::uint32_t type, std::uint16_t workers) {
    auto* lot = new cSC4Lot(x, 0, 1, 1, 0);
    lot->AddRef();
    lot->Init();
    lot->SetBuildingType(type);
    lot->SetState(3, 0, 0);
    if (workers) lot->SetPopulation(kWorkers, workers);
    return lot;
}
}  // namespace

int main() {
    LotHostFake host;
    host.factory.capacities = {{{kWorkers, 100}}};
    host.farm.capacities = {{{kWorkers, 100}}};
    SetLotHost(&host);
    Construction construction;
    cSC4LotDeveloper::SetConstructionServices(&construction);
    DeveloperHost devHost;
    cSC4LotDeveloper dev(&devHost);

    {   // the unit's own bookkeeping: one membership per lot, the lot's unit in step, removal drops the designate
        cSC4Lot* a = MakeLot(0, kFactory, 0);
        cSC4Lot* b = MakeLot(1, kFactory, 0);
        auto* unit = new cSC4BusinessUnit();
        unit->AddRef();
        unit->Add(a);
        unit->Add(a);
        unit->Add(b);
        CHECK(unit->GetMembers().size() == 2 && a->GetUnit() == unit && b->GetUnit() == unit);
        b->SetTravelDesignate(a);
        unit->Remove(b);
        CHECK(unit->GetMembers().size() == 1 && !b->GetUnit() && !b->GetTravelDesignate());
        CHECK(unit->GetGZCLSID() == 0x4a232da8);
        void* raw = nullptr;
        CHECK(unit->QueryInterface(0x6a233069, &raw) && unit->QueryInterface(0xe4fda3d4, &raw) && !unit->QueryInterface(3, &raw));
        unit->Release();
        unit->Release();
        unit->Release();                                   // the member a still holds the unit
        CHECK(a->GetUnit() == unit && unit->GetMembers().size() == 1);
        a->Shutdown();                                     // shutting the lot down leaves the unit, which goes with it
        CHECK(!a->GetUnit());
        a->Release();
        b->Release();
    }

    // A (road, building) - B (no road) - C (no road), all factories; O an office beside B.
    cSC4Lot* a = MakeLot(0, kFactory, 10);
    cSC4Lot* b = MakeLot(1, kFactory, 20);
    cSC4Lot* c = MakeLot(2, kFactory, 30);
    cSC4Lot* o = MakeLot(5, kOffice, 0);
    host.road = {a, o};
    host.built = {a, b, c, o};
    host.neighbours[b] = {{1, {a}}, {4, {c, o}}};
    host.neighbours[c] = {{1, {b}}};
    host.neighbours[a] = {{4, {b}}};

    dev.PickUnitToJoin(o);                                 // an office never joins
    CHECK(!o->GetUnit());
    dev.PickUnitToJoin(c);                                 // its only neighbour B reaches no road
    CHECK(!c->GetUnit());
    dev.PickUnitToJoin(b);                                 // A reaches a road: a new unit of A then B
    cISC4BusinessUnit* unit = b->GetUnit();
    CHECK(unit && a->GetUnit() == unit && unit->GetMembers().size() == 2 && unit->GetMembers().front() == a);
    CHECK(!b->GetTravelDesignate());                       // not a farm: the designate waits for the unit refresh
    dev.PickUnitToJoin(b);                                 // already in a unit: nothing
    CHECK(unit->GetMembers().size() == 2);

    dev.UpdateTravelDesignates(unit);
    CHECK(unit == a->GetUnit() && b->GetTravelDesignate() == a && !a->GetTravelDesignate());
    dev.PickUnitToJoin(c);                                 // B reaches a road through its designate now
    CHECK(c->GetUnit() == unit && unit->GetMembers().size() == 3);
    dev.UpdateTravelDesignates(unit);
    CHECK(c->GetTravelDesignate() == a);

    dev.UpdateTravelPopulations(unit);                     // the workers of the unit travel from A
    CHECK(a->GetTravelPopulation(kWorkers) == 60 && b->GetTravelPopulation(kWorkers) == 0 && c->GetTravelPopulation(kWorkers) == 0);

    dev.RemoveLotFromItsBusinessUnit(c);
    CHECK(!c->GetUnit() && !c->GetTravelDesignate() && unit->GetMembers().size() == 2);

    {   // the road goes: no member can be the designate, the unit dissolves and nobody finds another
        host.road.clear();
        a->UpdateRoadAccess();                             // the daily pass refreshes road access after a network change
        b->UpdateRoadAccess();
        b->SetTravelDesignate(nullptr);
        cISC4BusinessUnit* old = unit;
        dev.UpdateTravelDesignates(old);
        CHECK(!old && !a->GetUnit() && !b->GetUnit());
    }

    {   // a farm field joins the farm's lot (it needs the building, not the road) and travels from the unit's first member
        cSC4Lot* farm = MakeLot(10, kFarm, 5);
        cSC4Lot* field = MakeLot(11, kFarm, 0);
        cSC4Lot* bare = MakeLot(12, kFarm, 0);
        host.built.insert(farm);
        host.neighbours[field] = {{2, {bare, farm}}};
        dev.PickUnitToJoin(field);
        CHECK(field->GetUnit() && field->GetUnit() == farm->GetUnit() && field->GetTravelDesignate() == farm);
        CHECK(!bare->GetUnit());
        farm->Shutdown();
        field->Shutdown();
        CHECK(!field->GetUnit() && !field->GetTravelDesignate());
        farm->Release();
        field->Release();
        bare->Release();
    }

    a->Release();
    b->Release();
    c->Release();
    o->Release();
    std::printf(failures ? "FAILED\n" : "sc4_businessunit: all passed\n");
    return failures ? 1 : 0;
}
