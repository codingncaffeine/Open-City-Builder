// cSC4PowerSimulator (Mac i386 port): wiring from occupants, the grid flood from a plant, the powered cells and supply values, the
// plant's share of the output, removal, the five-state pass and the version 6 stream.
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Occupant.h"
#include "sc4/PowerSimulator.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
constexpr int kCells = 64;

// A building on a cell rectangle (its bounding box spans the cells, its position is the centre).
struct Building final : cISC4Occupant {
    SC4CellRect cells;
    cSCPropertyHolder properties;
    int refs = 1;
    Building(int x0, int z0, int x1, int z1) { cells.x0 = x0; cells.z0 = z0; cells.x1 = x1; cells.z1 = z1; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &properties; }
    std::uint32_t GetType() const override { return kPowerOccupantBuilding; }
    void GetPosition(float* xyz) override {
        xyz[0] = (float(cells.x0) + float(cells.x1 + 1)) * 8.0f;
        xyz[1] = 0;
        xyz[2] = (float(cells.z0) + float(cells.z1 + 1)) * 8.0f;
    }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* lo, float* hi) override {
        lo[0] = float(cells.x0) * 16.0f + 1.0f; lo[1] = 0; lo[2] = float(cells.z0) * 16.0f + 1.0f;
        hi[0] = float(cells.x1 + 1) * 16.0f - 1.0f; hi[1] = 10; hi[2] = float(cells.z1 + 1) * 16.0f - 1.0f;
    }
    void GetBoundingCityCells(SC4CellRect& out) override { out = cells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

class Host final : public IPowerHost {
public:
    std::vector<cISC4Occupant*> occupants;
    std::map<std::pair<int, int>, std::uint8_t> supply;
    std::set<std::pair<int, int>> poweredLots;
    std::vector<std::uint32_t> posted;
    std::map<std::uint32_t, std::int64_t> history;
    std::int64_t generationCost = -1;
    int CityCellsX() override { return kCells; }
    int CityCellsZ() override { return kCells; }
    float CellSizeX() override { return 16.0f; }
    float CellSizeZ() override { return 16.0f; }
    void OccupantsInRect(const PowerRect& rect, std::vector<cISC4Occupant*>& out) override {
        for (cISC4Occupant* occupant : occupants) {
            SC4CellRect c;
            occupant->GetBoundingCityCells(c);
            if (c.x1 >= rect.left && c.x0 <= rect.right && c.z1 >= rect.top && c.z0 <= rect.bottom) out.push_back(occupant);
        }
    }
    bool WireCell(cISC4Occupant*, int&, int&) override { return false; }
    bool IsZoned(int, int) override { return false; }
    void SetSupplyValue(int x, int z, std::uint8_t value) override { supply[{x, z}] = value; }
    float OrdinancePowerFactor() override { return 1.0f; }
    PowerPlantBudgetEntry GetPlantBudgetEntry(cISC4Occupant*) override { PowerPlantBudgetEntry e; e.tracked = true; e.funding = 1.0f; return e; }
    float DepartmentFundingFraction() override { return 1.0f; }
    void SetGenerationCost(std::int64_t cost) override { generationCost = cost; }
    void SetOccupantLotPowered(cISC4Occupant* building) override {
        SC4CellRect c;
        building->GetBoundingCityCells(c);
        poweredLots.insert({c.x0, c.z0});
    }
    void PostMessage(std::uint32_t type, std::uint32_t, const PowerRect*) override { posted.push_back(type); }
    void AddHistory(std::uint32_t id, std::int64_t value) override { history[id] = value; }
};

void MakeConsumer(Building& b, std::uint32_t draw) { cSCPropertyHelp::SetPropertyValue(&b.properties, kPowerConsumed, draw); }
void MakePlant(Building& b, std::uint32_t output) {
    cSCPropertyHelp::SetPropertyValue(&b.properties, kPowerPlantMaxProduced, output);
    cSCPropertyHelp::SetPropertyValue(&b.properties, kPowerPlantPowerType, std::int32_t(1));
    cSCPropertyHelp::SetPropertyValue(&b.properties, kPowerPlantConditionCurve, std::vector<float>{0.0f, 1.0f, 1.0f, 1.0f});
    cSCPropertyHelp::SetPropertyValue(&b.properties, 0xc910b4ce, 2.0f);   // generation cost per unit
}

void RunPass(cSC4PowerSimulator& power) {
    for (int i = 0; i < 6 && !(power.IdleState() == 4 && !power.RefreshRequested()); ++i) power.DoIdle();
}
}  // namespace

int main() {
    Host host;
    cRZRandom random(7);
    auto* power = new cSC4PowerSimulator(&host, &random);
    power->AddRef();
    CHECK(power->Init());
    CHECK(power->IdleState() == 4 && !power->IsPowered(4, 4));

    Building plant(2, 2, 3, 3), near(4, 4, 4, 4), far(40, 40, 40, 40);
    MakePlant(plant, 100);
    MakeConsumer(near, 10);
    MakeConsumer(far, 5);
    host.occupants = {&plant, &near, &far};
    for (cISC4Occupant* occupant : host.occupants) power->DoOccupantInserted(occupant);
    CHECK(power->Plants().size() == 1 && power->GetPowerPlantCount() == 1 && power->GetPowerPlantCount(1) == 1);
    CHECK(power->consumers == 2 && power->totalConsumption == 15);
    CHECK(power->IsWired(0, 0) && power->IsWired(6, 6) && !power->IsWired(7, 7) && power->IsWired(38, 42));   // radius 2

    power->DoIdle();                                    // not established yet: nothing happens
    CHECK(power->IdleState() == 4);
    auto* established = new cRZMessage2Standard();
    established->AddRef();
    established->SetType(kMsgPowerCityEstablished);
    power->DoMessage(established);
    established->Release();
    RunPass(*power);
    CHECK(power->IdleState() == 4);
    CHECK(power->IsPowered(4, 4) && power->IsPowered(0, 0) && !power->IsPowered(40, 40));
    CHECK(power->GetPoweredConsumerCount() == 1 && power->GetUnpoweredConsumerCount() == 1);
    // the plant supplies 100 * 1.1 units; the grid used 10 of them, and the plant reports its share of what was used
    CHECK(power->GetMonthlyProduction() == 100 && power->GetMonthlyConsumption() == 10 && power->GetMonthlyDemand() == 10);
    std::int32_t current = -1;
    cSCPropertyHelp::GetPropertyValue(&plant.properties, kPowerPlantCurrentProduced, current);
    CHECK(current == 10);
    CHECK(power->MonthlyPower()[std::size_t(power->CurrentMonth())][1] == 10);
    CHECK((host.supply[{4, 4}] == 2 && host.supply.count({40, 40}) == 0));   // only the cells that changed are written
    CHECK(host.generationCost == 20);                   // booked after the flood: 10 units at 2.0 each
    CHECK(host.history[0xaa11f7ce] == 100 && host.history[0xaa11fb59] == 50);   // produced; half the consumers unpowered
    bool announced = false;
    for (std::uint32_t type : host.posted) announced |= type == kMsgPowerGridChanged;
    CHECK(announced);

    // a consumer placed on a powered cell has power at once
    Building late(5, 5, 5, 5);
    MakeConsumer(late, 1);
    host.occupants.push_back(&late);
    power->DoOccupantInserted(&late);
    CHECK((host.poweredLots.count({5, 5}) == 1 && host.poweredLots.count({40, 40}) == 0));

    // the stream keeps the grid
    PortMemoryOStream out;
    CHECK(power->Write(out));
    auto* copy = new cSC4PowerSimulator(&host, &random);
    copy->AddRef();
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(copy->Read(in));
    CHECK(copy->IsPowered(4, 4) && !copy->IsPowered(40, 40) && copy->IsWired(38, 42));
    CHECK(copy->GetMonthlyProduction() == 100 && copy->consumers == 3 && copy->CurrentMonth() == power->CurrentMonth());
    CHECK(copy->Plants().size() == 1 && copy->IdleState() == power->IdleState());
    copy->Release();

    // the plant goes: its cells are rewired from what is left and the next pass powers nothing
    host.occupants = {&near, &far, &late};
    power->DoOccupantRemoved(&plant);
    CHECK(power->Plants().empty() && power->GetPowerPlantCount() == 0 && power->RefreshRequested());
    RunPass(*power);
    CHECK(!power->IsPowered(4, 4) && power->GetPoweredConsumerCount() == 0);
    CHECK((host.supply[{4, 4}] == 1));                    // still wired (the consumers wire their neighbourhood)
    CHECK(plant.refs == 1);                             // the plant list released its reference

    CHECK(power->Shutdown());
    power->Release();
    std::printf(failures ? "FAILED\n" : "sc4_powersimulator: all passed\n");
    return failures ? 1 : 0;
}
