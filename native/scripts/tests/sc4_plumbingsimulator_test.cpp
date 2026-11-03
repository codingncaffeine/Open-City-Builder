// cSC4PlumbingSimulator (Mac i386 port): pipes and buildings on the cell map, the pass (a producer's network along its pipes, the
// cells watered ring by ring around them up to the supply), the producer's share, the statistics, removal and the version 4 stream.
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Occupant.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
constexpr int kCells = 32;

struct SimCity : ISimGridCity {
    std::int32_t CitySizeXInCells() const override { return kCells; }
    std::int32_t CitySizeZInCells() const override { return kCells; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

// An occupant on a cell rectangle (its bounding box spans the cells, its position is the centre).
struct Occ final : cISC4Occupant {
    SC4CellRect cells;
    cSCPropertyHolder properties;
    std::uint32_t type;
    int refs = 1;
    Occ(std::uint32_t t, int x0, int z0, int x1, int z1) : type(t) { cells.x0 = x0; cells.z0 = z0; cells.x1 = x1; cells.z1 = z1; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (type != kPlumbingOccupantBuilding || iid != 0x87dda3a9) return false;   // a building answers its building view
        AddRef();
        *out = static_cast<cIGZUnknown*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &properties; }
    std::uint32_t GetType() const override { return type; }
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

class Host final : public IPlumbingHost {
public:
    std::vector<Occ*> occupants;
    std::map<const cISC4Occupant*, std::uint32_t> pipeFlags;
    std::map<const cISC4Occupant*, bool> watered;
    std::map<std::uint32_t, std::int64_t> history;
    std::vector<std::uint32_t> posted;
    std::int64_t pipeCost = -1;
    std::vector<cSC4SimGrid<std::uint8_t>*> grids;   // the stream's grid references (by index)
    int CityCellsX() override { return kCells; }
    int CityCellsZ() override { return kCells; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = int(x / 16.0f); cz = int(z / 16.0f); }
    void OccupantsAtCell(int x, int z, std::uint32_t type, std::vector<cISC4Occupant*>& out) override {
        for (Occ* o : occupants)
            if (o->type == type && x >= o->cells.x0 && x <= o->cells.x1 && z >= o->cells.z0 && z <= o->cells.z1) out.push_back(o);
    }
    bool IsWaterCell(int, int) override { return false; }
    int WaterPollutionAt(int, int) override { return 0; }
    bool LotRectAt(int x, int z, PlumbingRect& out) override {
        for (Occ* o : occupants)
            if (o->type == kPlumbingOccupantBuilding && x >= o->cells.x0 && x <= o->cells.x1 && z >= o->cells.z0 && z <= o->cells.z1) {
                out = PlumbingRect{o->cells.x0, o->cells.z0, o->cells.x1, o->cells.z1};
                return true;
            }
        return false;
    }
    bool ProducerLotUnpowered(cISC4Occupant*) override { return false; }
    void SetLotConditionAt(int, int, int) override {}
    float DepartmentFundingFraction() override { return 1.0f; }
    void SetPipeCost(std::int64_t cost) override { pipeCost = cost; }
    bool IsPipe(cISC4Occupant* o) override { return o->GetType() == kPlumbingOccupantNetwork; }
    std::uint32_t PipeFlags(cISC4Occupant* pipe) override { return pipeFlags[pipe]; }
    void SetPipeFlag(cISC4Occupant* pipe, std::uint32_t flag, bool on) override { on ? pipeFlags[pipe] |= flag : pipeFlags[pipe] &= ~flag; }
    // a straight pipe along x: it connects west and east (edges 0 / 2)
    bool PipeConnects(cISC4Occupant*, int edge) override { return edge == 0 || edge == 2; }
    void PipeCell(cISC4Occupant* pipe, int& x, int& z) override { SC4CellRect c; pipe->GetBoundingCityCells(c); x = c.x0; z = c.z0; }
    void SetPipeWatered(cISC4Occupant* pipe, bool on) override { watered[pipe] = on; }
    void PostMessage(std::uint32_t type, std::uint32_t) override { posted.push_back(type); }
    void SendUpdated(cIGZUnknown*) override { posted.push_back(kMsgPlumbingUpdated); }
    void AddHistory(std::uint32_t id, std::int64_t value) override { history[id] = value; }
    void WriteGrid(cIGZOStream& s, cSC4SimGrid<std::uint8_t>* grid) override { grids.push_back(grid); s.SetUint32(std::uint32_t(grids.size() - 1)); }
    bool ReadGrid(cIGZIStream& s, cSC4SimGrid<std::uint8_t>** out) override {
        std::uint32_t i = 0;
        s.GetUint32(i);
        *out = grids[i];
        (*out)->AddRef();
        return true;
    }
    void WriteOccupant(cIGZOStream& s, cISC4Occupant* o) override {
        std::uint32_t index = 0xffffffffu;
        for (std::size_t i = 0; i < occupants.size(); ++i) if (occupants[i] == o) index = std::uint32_t(i);
        s.SetUint32(index);
    }
    bool ReadOccupant(cIGZIStream& s, std::uint32_t, cISC4Occupant** out) override {
        std::uint32_t i = 0;
        s.GetUint32(i);
        *out = i < occupants.size() ? occupants[i] : nullptr;
        if (*out) (*out)->AddRef();
        return true;
    }
};

void RunPass(cSC4PlumbingSimulator& water) {
    for (int i = 0; i < 40 && !(water.IdleState() == 9 && !water.RefreshRequested()); ++i) water.DoIdle(5);
}
}  // namespace

int main() {
    SimCity city;
    SetSimGridCity(&city);
    Host host;
    cRZRandom random(7);
    auto* water = new cSC4PlumbingSimulator(&host, &random);
    water->AddRef();
    CHECK(water->Init());
    water->productionFundingCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {100.0f, 100.0f}});   // the exemplar's
    water->pipeCostFactor = 2.0f;
    CHECK(water->IdleState() == 0 && water->RefreshRequested() && !water->IsCellWatered(5, 10));

    // a pump on (2, 10), a pipe from x = 2 to 12 along z = 10, a house two cells off the pipe's end and one far away
    Occ pump(kPlumbingOccupantBuilding, 2, 10, 2, 10), near(kPlumbingOccupantBuilding, 12, 12, 12, 12),
        far(kPlumbingOccupantBuilding, 28, 28, 28, 28);
    cSCPropertyHelp::SetPropertyValue(&pump.properties, kPropWaterProductionCapacity, std::uint32_t(100));
    cSCPropertyHelp::SetPropertyValue(&pump.properties, kPropProducerWaterSource, std::uint8_t(2));   // needs no water nearby
    cSCPropertyHelp::SetPropertyValue(&near.properties, kPropBuildingWaterDemand, std::uint32_t(10));
    cSCPropertyHelp::SetPropertyValue(&far.properties, kPropBuildingWaterDemand, std::uint32_t(5));
    std::vector<Occ*> pipes;
    for (int x = 2; x <= 12; ++x) pipes.push_back(new Occ(kPlumbingOccupantNetwork, x, 10, x, 10));
    host.occupants = {&pump, &near, &far};
    for (Occ* pipe : pipes) host.occupants.push_back(pipe);
    for (Occ* o : host.occupants) water->DoOccupantInserted(o);
    CHECK(water->Producers().size() == 1 && water->Pipes().size() == 11 && water->GetWaterDemand() == 15);
    CHECK(host.pipeCost == 22);
    CHECK((water->CellFlags()[10 * kCells + 5] & 0x15) == 0x15 && (water->CellFlags()[12 * kCells + 12] & 0x40));

    water->DoIdle(5);                                   // not established yet: nothing happens
    CHECK(water->IdleState() == 0);
    auto* established = new cRZMessage2Standard();
    established->AddRef();
    established->SetType(kMsgPlumbingCityEstablished);
    water->DoMessage(established);
    established->Release();
    RunPass(*water);
    CHECK(water->IdleState() == 9);
    CHECK(water->IsCellWatered(5, 10) && water->IsCellWatered(12, 12) && water->IsCellWatered(14, 14) && !water->IsCellWatered(28, 28));
    CHECK(!water->IsCellWatered(20, 10));               // seven rings past the pipe's end
    CHECK(water->AreWateredBuildingsInCell(12, 12) && !water->AreWateredBuildingsInCell(28, 28));
    CHECK(water->GetSupplyValue(5, 10) == 9);           // a watered pipe cell
    CHECK(water->GetNumWateredBuildings() == 1 && water->GetNumUnwateredBuildings() == 1 && water->GetNumPipes() == 11);
    CHECK(water->GetWaterProductionCapacity() == 100 && water->GetWaterConsumed() == 10);
    std::uint32_t current = 0;
    cSCPropertyHelp::GetPropertyValue(&pump.properties, kPropProducerCurrentProduced, current);
    CHECK(current == 10);                               // its share: 10 of 100 used
    CHECK(host.watered[pipes[0]] && host.watered[pipes[10]]);
    CHECK(host.history[0x0a14ebb4] == 100 && host.history[0xca14ebd0] == 10 && host.history[0xca78a955] == 1 && host.history[0x8a78ab06] == 1);
    bool updated = false, dealsPosted = false;
    for (std::uint32_t type : host.posted) {
        updated |= type == kMsgPlumbingUpdated;
        dealsPosted |= type == kMsgPlumbingDealsChanged;
    }
    CHECK(updated && dealsPosted);

    // not enough water: a thirsty house uses up the supply, the cells beyond it stay dry
    Occ thirsty(kPlumbingOccupantBuilding, 4, 11, 4, 11);
    cSCPropertyHelp::SetPropertyValue(&thirsty.properties, kPropBuildingWaterDemand, std::uint32_t(95));
    host.occupants.push_back(&thirsty);
    water->DoOccupantInserted(&thirsty);
    CHECK(!water->RefreshRequested());                  // a plain building does not start a pass
    water->DoMonthlySimulation();
    RunPass(*water);
    CHECK(water->GetWaterConsumed() == 100 && water->IsCellWatered(4, 11) && !water->IsCellWatered(12, 12));
    host.occupants.pop_back();
    water->DoOccupantRemoved(&thirsty);
    water->DoMonthlySimulation();
    RunPass(*water);
    CHECK(water->IsCellWatered(12, 12) && water->GetWaterConsumed() == 10);

    // the stream keeps the network
    PortMemoryOStream out;
    CHECK(water->Write(out));
    auto* copy = new cSC4PlumbingSimulator(&host, &random);
    copy->AddRef();
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(copy->Read(in));
    CHECK(copy->IsCellWatered(12, 12) && !copy->IsCellWatered(28, 28) && copy->GetWaterDemand() == 15);
    CHECK(copy->Producers().size() == 1 && copy->Pipes().size() == 11 && copy->GetWaterConsumed() == 10);
    CHECK(copy->IdleState() == 0 && copy->CellFlags() == water->CellFlags());
    copy->Release();

    // the pump goes: the next pass waters nothing
    const int pumpRefs = pump.refs;
    host.occupants.erase(host.occupants.begin());
    water->DoOccupantRemoved(&pump);
    CHECK(water->Producers().empty() && water->RefreshRequested() && pump.refs == pumpRefs - 1);
    RunPass(*water);
    CHECK(!water->IsCellWatered(12, 12) && !water->IsCellWatered(5, 10) && water->GetNumWateredBuildings() == 0);
    CHECK(!host.watered[pipes[0]]);

    // a pipe removed clears its cell's pipe bits
    water->DoOccupantRemoved(pipes[3]);
    CHECK((water->CellFlags()[10 * kCells + 5] & 0x1f) == 0 && water->Pipes().size() == 10 && host.pipeCost == 20);

    CHECK(water->Shutdown());
    water->Release();
    for (Occ* pipe : pipes) delete pipe;
    std::printf(failures ? "FAILED\n" : "sc4_plumbingsimulator: all passed\n");
    return failures ? 1 : 0;
}
