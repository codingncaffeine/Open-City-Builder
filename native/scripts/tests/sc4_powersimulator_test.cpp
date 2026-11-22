#include <algorithm>
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
    bool answersOccupant = false, inManager = true;   // the pole sweep (PostCityInit) asks QI 0x26bb767e then IsFlagSet(1)
    Building(int x0, int z0, int x1, int z1) { cells.x0 = x0; cells.z0 = z0; cells.x1 = x1; cells.z1 = z1; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (!answersOccupant || iid != 0x26bb767e) return false;
        *out = static_cast<cISC4Occupant*>(this);
        ++refs;
        return true;
    }
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
    bool IsFlagSet(std::uint32_t flag) override { return flag == 1 && answersOccupant && inManager; }
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
    // DoTerrainHeightsChanged: a Building listed in `poles` is a pole at its first cell; in `lines` a line joining the two poles
    std::vector<cISC4Occupant*> poles, broken;
    std::map<cISC4Occupant*, std::pair<Building*, Building*>> lines;
    std::vector<std::pair<cIGZUnknown*, cIGZUnknown*>> disconnected;
    PowerRect lastRect{};
    bool PoleCell(cISC4Occupant* o, int& x, int& z) override {
        if (std::find(poles.begin(), poles.end(), o) == poles.end()) return false;
        auto* b = static_cast<Building*>(o);
        x = b->cells.x0;
        z = b->cells.z0;
        return true;
    }
    void BreakPoleAndRemove(cISC4Occupant* pole) override { broken.push_back(pole); }
    bool LinePoles(cISC4Occupant* o, cIGZUnknown*& a, cIGZUnknown*& b) override {
        auto it = lines.find(o);
        if (it == lines.end()) return false;
        a = it->second.first;
        b = it->second.second;
        if (a) a->AddRef();
        if (b) b->AddRef();
        return true;
    }
    void DisconnectPoles(cIGZUnknown* a, cIGZUnknown* b) override { disconnected.emplace_back(a, b); }
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
    // plant failure and the nuclear free zone
    bool hasLot = true;
    std::vector<std::pair<cISC4Occupant*, std::uint32_t>> destroyed;
    std::vector<std::pair<float, float>> radiation;   // amount, radius
    std::vector<std::pair<std::uint32_t, cISC4Occupant*>> plantMessages;
    std::vector<std::vector<cISC4Occupant*>> destroyedGroups;
    std::vector<std::uint32_t> groupFlags, droppedTools;
    int distressed = 0;
    void DistressPlantLot(cISC4Occupant*) override { ++distressed; }
    bool DestroyPlantLot(cISC4Occupant* plant, std::uint32_t flags) override {
        if (!hasLot) return false;
        destroyed.emplace_back(plant, flags);
        return true;
    }
    void AddRadiation(const float*, float amount, float radius) override { radiation.emplace_back(amount, radius); }
    void PostPlantMessage(std::uint32_t type, cISC4Occupant* plant) override { plantMessages.emplace_back(type, plant); }
    void DestroyPlantLots(const std::vector<cISC4Occupant*>& plants, std::uint32_t flags) override {
        destroyedGroups.push_back(plants);
        groupFlags.push_back(flags);
    }
    void DropViewToolOfClass(std::uint32_t clsid) override { droppedTools.push_back(clsid); }
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

    {   // PostCityInit (0x26d31ec1, spec 5.6): a pole out of the occupant manager leaves the list; the cost base is the sum of the
        // remaining poles' upkeep (low 32 bits), and its change asks for a recompute
        Building kept(10, 10, 10, 10), gone(11, 10, 11, 10), plain(12, 10, 12, 10);
        kept.answersOccupant = gone.answersOccupant = true;
        gone.inManager = false;
        cSCPropertyHelp::SetPropertyValue(&kept.properties, kPowerPoleUpkeep, std::int64_t{0x100000005});   // high word dropped
        cSCPropertyHelp::SetPropertyValue(&gone.properties, kPowerPoleUpkeep, std::int64_t{7});
        cSCPropertyHelp::SetPropertyValue(&plain.properties, kPowerPoleUpkeep, std::int64_t{3});
        std::vector<cISC4Occupant*>& poles = power->Poles();
        for (cISC4Occupant* p : poles) if (p) p->Release();
        poles.clear();
        for (Building* b : {&kept, &gone, &plain}) { b->AddRef(); poles.push_back(b); }
        CHECK(!power->RefreshRequested());   // the last pass cleared it
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0x26d31ec1);
        power->DoMessage(message);
        message->Release();
        CHECK(poles.size() == 2 && poles[0] == &kept && poles[1] == &plain);   // a pole that does not answer the QI stays
        CHECK(power->poleUpkeep == 8 && power->RefreshRequested());
        CHECK(gone.refs == 1);   // released when it left
        for (cISC4Occupant* p : poles) p->Release();
        poles.clear();
    }
    {   // 0x278e6003 (spec 5 / 5.4): the rect (d1 >> 16) - 1 .. d1 & 0xffff (x), the same of d2 (z), clamped; poles inside break and go,
        // lines are disconnected once per pole pair; data4 4 or 5 is ignored
        Building inside(5, 5, 5, 5), outside(30, 30, 30, 30), lineA(6, 6, 6, 6), lineB(6, 7, 6, 7);
        host.poles = {&inside, &outside};
        host.lines = {{&lineA, {&inside, &outside}}, {&lineB, {&inside, &outside}}};   // the same pair twice
        host.occupants = {&inside, &outside, &lineA, &lineB};
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0x278e6003);
        message->SetData1((5u << 16) | 9u);   // x 4..9
        message->SetData2((0u << 16) | 9u);   // z -1 -> 0 ..9
        message->SetData4(4);
        power->DoMessage(message);
        CHECK(host.broken.empty() && host.disconnected.empty());   // data4 4: ignored
        message->SetData4(0);
        power->DoMessage(message);
        message->Release();
        CHECK(host.broken.size() == 1 && host.broken[0] == &inside);
        CHECK(host.disconnected.size() == 1 && host.disconnected[0].first == &inside && host.disconnected[0].second == &outside);
        host.poles.clear();
        host.lines.clear();
    }
    {   // AgePowerPlant (i386 0x001821de): past the failure limit with failure flags the plant's lot is demolished with flags | 0x300,
        // its radiation (0xaa5c04c5 amount, 0xaa5c04c8 radius) released, and a nuclear plant (type 5) posts 0xaa552f77; without
        // flags it is only distressed; without a lot nothing happens; no radius: no radiation and no message
        Building nuke(20, 20, 21, 21);
        MakePlant(nuke, 50);
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, kPowerPlantPowerType, std::int32_t(5));
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0xc910b4cf, 1.5f);   // wear
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0xea1cf221, 0.9f);   // distress limit
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0xea1cf222, 1.0f);   // failure limit
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0xaa5c04c5, 40.0f);  // radiation amount
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0xaa5c04c8, 6.0f);   // radiation radius
        power->AgePowerPlant(&nuke);                          // no failure flags: distressed only
        CHECK(host.distressed == 1 && host.destroyed.empty());
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0x6a43150f, std::uint32_t(0x4));
        power->AgePowerPlant(&nuke);
        CHECK(host.distressed == 1);
        CHECK(host.destroyed.size() == 1 && host.destroyed[0].first == &nuke && host.destroyed[0].second == 0x304);
        CHECK(host.radiation.size() == 1 && host.radiation[0].first == 40.0f && host.radiation[0].second == 6.0f);
        CHECK(host.plantMessages.size() == 1 && host.plantMessages[0].first == 0xaa552f77 && host.plantMessages[0].second == &nuke);
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, kPowerPlantPowerType, std::int32_t(1));   // coal: no message
        power->AgePowerPlant(&nuke);
        CHECK(host.destroyed.size() == 2 && host.radiation.size() == 2 && host.plantMessages.size() == 1);
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, 0xaa5c04c8, 0.0f);   // no radius: nothing released
        power->AgePowerPlant(&nuke);
        CHECK(host.destroyed.size() == 3 && host.radiation.size() == 2);
        host.hasLot = false;                                  // no lot under it: nothing
        power->AgePowerPlant(&nuke);
        CHECK(host.destroyed.size() == 3 && host.distressed == 1);
        host.hasLot = true;
    }
    {   // 0xc2c09554 with the nuclear free zone (0xe0d0722e) going on: the nuclear plants' lots go in one call (flags 1) and a
        // current place-lot tool (0x88f154fb) is dropped; another ordinance, or the zone going off, does nothing
        Building nuke(30, 30, 31, 31);
        MakePlant(nuke, 50);
        cSCPropertyHelp::SetPropertyValue(&nuke.properties, kPowerPlantPowerType, std::int32_t(5));
        host.occupants.push_back(&nuke);
        power->DoOccupantInserted(&nuke);
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0xc2c09554);
        message->SetData1(0xe0d0722e);
        message->SetData2(0);
        power->DoMessage(message);
        message->SetData1(0x12345678);
        message->SetData2(1);
        power->DoMessage(message);
        CHECK(host.destroyedGroups.empty() && host.droppedTools.empty());
        message->SetData1(0xe0d0722e);
        power->DoMessage(message);
        message->Release();
        CHECK(host.destroyedGroups.size() == 1 && host.destroyedGroups[0].size() == 1 && host.destroyedGroups[0][0] == &nuke);
        CHECK(host.groupFlags.size() == 1 && host.groupFlags[0] == 1);
        CHECK(host.droppedTools.size() == 1 && host.droppedTools[0] == 0x88f154fb);
        power->DoOccupantRemoved(&nuke);
        host.occupants.pop_back();
    }
    CHECK(power->Shutdown());
    power->Release();
    std::printf(failures ? "FAILED\n" : "sc4_powersimulator: all passed\n");
    return failures ? 1 : 0;
}
