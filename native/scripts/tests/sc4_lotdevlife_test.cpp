// The lot developer's life cycle (Mac i386 0x000f1c6c Init, 0x000f5034 Shutdown, 0x000f5d6a DoMessage): the services, agents,
// subscriptions and animation tick Init takes and Shutdown gives back, the boundary rules (0x000ef524), the night-light deque and
// its batches (0x000f2dd0 / 0x000f5ac8 / 0x000f5726 / 0x000f57f0 / 0x000f1398) and the power-changed rectangle (0x000f323e).
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/LotDeveloper.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
class LotHostFake final : public ILotHost {
public:
    std::uint8_t ZoneTypeAt(int, int) override { return 1; }
    std::int32_t SimulatorDay() override { return 100; }
    float TerrainAltitude(float, float) override { return 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t) override { return nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
    std::uint32_t DeveloperType(std::uint32_t, std::int32_t) override { return 0; }
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
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    bool RoadAccessCells(const cSC4Lot&, int) override { return false; }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return nullptr; }
    void WriteSerializable(cIGZOStream&, cIGZUnknown*) override {}
    cIGZUnknown* ReadSerializable(cIGZIStream&, std::uint32_t) override { return nullptr; }
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

// The city as Init and the handlers reach it: every call is recorded.
class CityFake final : public ILotDeveloperCityServices {
public:
    const char* missing = nullptr;
    int tunablesAcquired = 0, visualMode = 0;
    std::vector<std::pair<int, std::string>> agents;   // level, name (flags 2 checked as they come)
    int agentFlagsWrong = 0, agentsRemoved = 0;
    std::vector<std::uint32_t> subscribed, unsubscribed, posted, tickables, untickables;
    std::vector<cSC4Lot*> lots;                          // what the lot manager holds
    cISC4OccupantManager* occupants = nullptr;
    const char* MissingService() override { return missing; }
    void CityCellCounts(int& x, int& z) override { x = 64; z = 64; }
    ILotGateServices* GateServices() override { return nullptr; }
    const cISCPropertyHolder* AcquireTunables() override { ++tunablesAcquired; return nullptr; }
    void ReleaseTunables(const cISCPropertyHolder*) override {}
    bool AddSimulatorAgent(cIGZMessageTarget2*, int level, const char* name, std::uint32_t flags) override {
        agents.emplace_back(level, name);
        if (flags != 2) ++agentFlagsWrong;
        return true;
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2*) override { ++agentsRemoved; }
    bool MessageServerPresent() override { return true; }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t type) override { subscribed.push_back(type); }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override { unsubscribed.push_back(type); }
    void PostMessage(std::uint32_t type) override { posted.push_back(type); }
    void AddTickable(cIGZMessageTarget2*, std::uint32_t priority) override { tickables.push_back(priority); }
    void RemoveTickable(cIGZMessageTarget2*, std::uint32_t priority) override { untickables.push_back(priority); }
    int VisualTimeMode() override { return visualMode; }
    cSC4Lot* LotAt(int x, int z, bool) override {
        for (cSC4Lot* lot : lots) {
            std::int32_t r[4] = {};
            lot->GetBoundingRect(r);
            if (x >= r[0] && x <= r[2] && z >= r[1] && z <= r[3]) return lot;
        }
        return nullptr;
    }
    void LotsInCellRect(int x1, int z1, int x2, int z2, std::vector<cSC4Lot*>& out) override {
        for (cSC4Lot* lot : lots) {
            std::int32_t r[4] = {};
            lot->GetBoundingRect(r);
            if (r[0] <= x2 && x1 <= r[2] && r[1] <= z2 && z1 <= r[3]) out.push_back(lot);
        }
    }
    cISC4OccupantManager* OccupantManager() override { return occupants; }
};

// The power the daily services report for every cell; a lot without a building.
class DailyFake final : public ILotDailyServices {
public:
    bool powered = false;
    int zotsAdded = 0;
    bool DeveloperDesirability(std::uint32_t, std::uint32_t, int, int, std::int32_t&, std::int16_t&) override { return false; }
    bool DeveloperWaterSupplyCap(std::uint32_t, std::uint32_t, std::uint8_t&) override { return false; }
    bool LotGrowthStage(const cSC4Lot&, std::uint8_t&) override { return false; }
    bool CheatIgnorePower() override { return false; }
    bool CheatIgnorePlumbing() override { return false; }
    bool IsPowered(int, int) override { return powered; }
    bool IsCellWatered(int, int) override { return true; }
    std::uint8_t TripLengthAt(int, int) override { return 1; }
    void CellWidths(float& x, float& z) override { x = z = 16.0f; }
    std::int32_t SimDateNumber() override { return 500; }
    bool CityHasJobs() override { return true; }
    bool BuildingBounds(const cSC4Lot&, float[3], float[3]) override { return false; }
    cISC4Occupant* AddZotProp(std::uint32_t, const float[3]) override { ++zotsAdded; return nullptr; }
    bool RemoveLotPropsWithZot(cSC4Lot&, std::uint32_t) override { return true; }
    void PostLotNotification(cSC4Lot&, std::uint32_t) override {}
};

// A prop standing in a box; it records the occupant messages it is told to post.
class Prop final : public cSC4Occupant {
public:
    Prop(float x0, float z0) : lo_{x0, 0.0f, z0}, hi_{x0 + 4.0f, 4.0f, z0 + 4.0f} { type_ = 0xa823821e; }
    void GetPosition(float* xyz) override { xyz[0] = lo_[0] + 2.0f; xyz[1] = 0.0f; xyz[2] = lo_[2] + 2.0f; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* lo, float* hi) override { for (int i = 0; i < 3; ++i) { lo[i] = lo_[i]; hi[i] = hi_[i]; } }
    void PostOccupantMessage(std::uint32_t message, std::uint32_t) override { if (message == 0x09d8e0b6) ++lightMessages; }
    int lightMessages = 0;
private:
    float lo_[3], hi_[3];
};

cSC4BuildingOccupant* MakeBuilding(float x0, float z0) {
    auto* building = new cSC4BuildingOccupant();
    building->AddRef();
    const float lo[3] = {x0, 0.0f, z0}, hi[3] = {x0 + 32.0f, 20.0f, z0 + 32.0f};
    building->SetBoundingBox(lo, hi);
    return building;
}

cSC4Lot* MakeLot(int x, int z, int size) {
    auto* lot = new cSC4Lot(x, z, size, size, 0);
    lot->AddRef();
    lot->Init();
    return lot;
}

// One standard message, delivered and released.
bool Deliver(cSC4LotDeveloper& dev, std::uint32_t type, std::uint64_t d1 = 0, std::uint64_t d2 = 0, std::uint64_t d3 = 0,
             std::uint64_t d4 = 0, void* void1 = nullptr) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(d1);
    message->SetData2(d2);
    message->SetData3(d3);
    message->SetData4(d4);
    if (void1) message->SetVoid1(void1);
    const bool result = dev.DoMessage(message);
    message->Release();
    return result;
}

void Insert(cSC4LotDeveloper& dev, cSC4BuildingOccupant* building) {
    Deliver(dev, 0x99ef1142, 0, 1, 0x278128a0, 0, static_cast<cISC4Occupant*>(static_cast<cSC4Occupant*>(building)));
}
}  // namespace

int main() {
    LotHostFake lotHost;
    SetLotHost(&lotHost);
    CityFake city;
    DailyFake daily;
    cSC4LotDeveloper::SetCityServices(&city);
    cSC4LotDeveloper::SetDailyServices(&daily);
    static std::list<cSC4Lot*> lotList;
    cSC4LotDeveloper::SetLotListProvider(+[]() -> const std::list<cSC4Lot*>* { return &lotList; });
    DeveloperHost host;
    auto* dev = new cSC4LotDeveloper(&host);
    dev->AddRef();

    {   // Init: a missing service fails it with nothing registered (the tunables are read first)
        city.missing = "power";
        CHECK(!dev->Init() && !dev->IsInitialized());
        CHECK(city.agents.empty() && city.subscribed.empty() && city.tickables.empty());
        CHECK(city.tunablesAcquired == 1);
        int sides[4] = {0, 2, 1, 0};
        std::uint32_t texture = 1;
        std::uint8_t rotation = 1;
        bool flag = true;
        dev->GetBoundaryLotTexture(sides, texture, rotation, flag);   // no rules before Init
        CHECK(texture == 0 && rotation == 0 && !flag);
    }
    {   // Init: the three agents (levels 0, 4, 6, flags 2), the nine messages in order, the animation tick at 0xc0
        city.missing = nullptr;
        CHECK(dev->Init() && dev->IsInitialized());
        CHECK(city.agents.size() == 3 && city.agentFlagsWrong == 0);
        CHECK(city.agents.size() == 3 && city.agents[0].first == 0 && city.agents[1].first == 4 && city.agents[2].first == 6);
        CHECK(!city.agents.empty() && city.agents[0].second == "cSC4LotDeveloper");
        const std::vector<std::uint32_t> order = {0x44778234, 0x99ef1142, 0x99ef1143, 0xe8bda2e7, 0xe99b3d4d,
                                                  0x278e6003, 0x8a07ae00, 0x26d31ec4, 0x69247dc7};
        CHECK(city.subscribed == order);
        CHECK(city.tickables == std::vector<std::uint32_t>{0xc0});
        CHECK(!dev->GetForceEmptyLotBaseTextures());                 // flipped and set back
        CHECK(dev->Init());                                           // a second Init registers nothing more
        CHECK(city.agents.size() == 3 && city.subscribed.size() == 9 && city.tickables.size() == 1);
    }
    {   // the lot developer's own boundary rules: the flipped corners (the zone developer's table has other rotations there)
        auto rule = [&](int s0, int s1, int s2, int s3, std::uint32_t texture, int rotation, bool flag) {
            const int sides[4] = {s0, s1, s2, s3};
            std::uint32_t t = 0;
            std::uint8_t r = 9;
            bool f = !flag;
            dev->GetBoundaryLotTexture(sides, t, r, f);
            return t == texture && r == rotation && f == flag;
        };
        CHECK(rule(0, 2, 1, 0, 0x2400, 1, true));
        CHECK(rule(1, 0, 0, 2, 0x2400, 3, true));
        CHECK(rule(2, 1, 0, 0, 0x2400, 0, true));
        CHECK(rule(0, 2, 1, 1, 0x2700, 1, true));
        CHECK(rule(1, 1, 0, 2, 0x2700, 3, true));
        CHECK(rule(2, 0, 1, 1, 0x2700, 0, false));
        CHECK(rule(1, 1, 2, 1, 0x2300, 2, false));
        CHECK(rule(2, 2, 2, 2, 0x3000, 0, false));                   // an absent key takes rule 0
    }

    // The night deque: three buildings on a 2x2 lot at (3, 0), two props on it (one a night light) and one night light off it.
    auto* occupants = new cSC4OccupantManager();
    occupants->AddRef();
    occupants->SetCitySize(1024.0f, 1024.0f);
    occupants->Init();
    city.occupants = occupants;
    cSC4Lot* lot = MakeLot(3, 0, 2);                                  // cells (3, 0)..(4, 1), located at (4, 0)
    city.lots.push_back(lot);
    auto* light = new Prop(70.0f, 10.0f);
    auto* plain = new Prop(50.0f, 20.0f);
    auto* away = new Prop(200.0f, 200.0f);
    for (Prop* prop : {light, plain, away}) {
        prop->AddRef();
        CHECK(occupants->InsertOccupant(prop, 0));
    }
    cSCPropertyHelp::SetPropertyValue(light->AsPropertyHolder(), 0x4a9f188b, true);
    cSCPropertyHelp::SetPropertyValue(away->AsPropertyHolder(), 0x4a9f188b, true);
    cSC4BuildingOccupant* a = MakeBuilding(64.0f, 0.0f);
    cSC4BuildingOccupant* b = MakeBuilding(64.0f, 0.0f);
    cSC4BuildingOccupant* c = MakeBuilding(64.0f, 0.0f);
    {   // inserted buildings join the back, unlit (the remembered lit state); other occupant types do not
        Insert(*dev, a);
        Insert(*dev, b);
        Insert(*dev, c);
        Deliver(*dev, 0x99ef1142, 0, 1, 0xa823821e, 0, static_cast<cISC4Occupant*>(light));
        CHECK(dev->NightBuildingCount() == 3 && !a->IsLit());
    }
    {   // night begins on a clock that runs in real time: a shuffled run in batches of nightBatch every 200 ms of animation
        dev->Tunables().nightBatch = 2;
        dev->Tunables().nightAccumulator = 200;
        CHECK(!Deliver(*dev, 0x86ad10ee, 0, 150000));                // no run: the tick is refused
        CHECK(Deliver(*dev, 0x44778234, 0, 0));
        CHECK(dev->NightMode() == 2 && dev->NightCursor() == 0);
        CHECK(Deliver(*dev, 0x86ad10ee, 0, 150000));                 // 200 + 150 ms > 200: a batch of two
        CHECK(int(a->IsLit()) + int(b->IsLit()) + int(c->IsLit()) == 2 && dev->NightCursor() == 2);
        CHECK(light->lightMessages == 2 && plain->lightMessages == 0 && away->lightMessages == 0);
        CHECK(Deliver(*dev, 0x86ad10ee, 0, 100000));                 // 100 ms: nothing
        CHECK(dev->NightCursor() == 2 && dev->NightMode() == 2);
        CHECK(Deliver(*dev, 0x86ad10ee, 0, 150000));                 // 250 ms: the last building, then the end of the run
        CHECK(a->IsLit() && b->IsLit() && c->IsLit());
        CHECK(dev->NightMode() == 0 && light->lightMessages == 3);
        CHECK(city.posted.empty());
    }
    {   // day begins on a clock held at noon: every building at once, then the lights-changed message
        city.visualMode = 1;
        CHECK(Deliver(*dev, 0x44778234, 0, 1));
        CHECK(!a->IsLit() && !b->IsLit() && !c->IsLit() && dev->NightMode() == 0);
        CHECK(city.posted == std::vector<std::uint32_t>{0xa6b79621});
        city.visualMode = 0;
    }
    {   // a removed building leaves the deque; a cursor past it keeps pointing at the same building
        CHECK(Deliver(*dev, 0x44778234, 0, 0));                      // a new night run (shuffled)
        CHECK(Deliver(*dev, 0x86ad10ee, 0, 250000));                 // the batch of two
        CHECK(dev->NightCursor() == 2);
        cSC4BuildingOccupant* first = a->IsLit() ? a : b->IsLit() ? b : c;   // a building before the cursor
        Deliver(*dev, 0x99ef1143, 0, 1, 0x278128a0, 0, static_cast<cISC4Occupant*>(static_cast<cSC4Occupant*>(first)));
        CHECK(dev->NightBuildingCount() == 2 && dev->NightCursor() == 1);
        CHECK(Deliver(*dev, 0x86ad10ee, 0, 250000));
        CHECK(a->IsLit() && b->IsLit() && c->IsLit() && dev->NightMode() == 0);
    }
    {   // the power of a rectangle changed: the empty lot (zone 1 on a zot cell) loses its power, takes condition 1 and the power
        // zot; power back clears both
        lot->SetHasPower(true);
        lot->SetCondition(0);
        daily.powered = false;
        CHECK(Deliver(*dev, 0x8a07ae00, 0, 0, 10, 10));
        CHECK(!lot->GetHasPower());
        CHECK(lot->GetCondition() == 1);
        CHECK(daily.zotsAdded == 1);
        CHECK(dev->PowerChangedLots() == 1);
        std::uint32_t zot = 0;
        CHECK(cSCPropertyHelp::GetPropertyValue(&lot->Properties(), kLotPropZot, zot) && zot == 4);
        daily.powered = true;
        CHECK(Deliver(*dev, 0x8a07ae00, 3, 0, 4, 1));
        CHECK(lot->GetHasPower() && lot->GetCondition() == 0);
        CHECK(cSCPropertyHelp::GetPropertyValue(&lot->Properties(), kLotPropZot, zot) && zot == 0);
        CHECK(Deliver(*dev, 0x8a07ae00, 20, 20, 30, 30) && dev->PowerChangedLots() == 2);   // no lot there
    }
    {   // every message is counted; the terrain message reads and does nothing
        CHECK(Deliver(*dev, 0x278e6003, 1, 2, 3, 4));
        CHECK(dev->MessageCounts().at(0x278e6003) == 1 && dev->MessageCounts().at(0x8a07ae00) == 3);
        CHECK(Deliver(*dev, 0x66956816) && Deliver(*dev, 0x66956810));   // the month and one-shot agents: nothing
    }
    {   // Shutdown gives everything back: the agents, the tick, the nine messages, the deque (one reference each) and the rules
        CHECK(dev->Shutdown() && !dev->IsInitialized());
        CHECK(city.agentsRemoved == 1 && city.untickables == std::vector<std::uint32_t>{0xc0});
        const std::vector<std::uint32_t> order = {0xe8bda2e7, 0xe99b3d4d, 0x99ef1143, 0x99ef1142, 0x44778234,
                                                  0x278e6003, 0x8a07ae00, 0x26d31ec4, 0x69247dc7};
        CHECK(city.unsubscribed == order);
        CHECK(dev->NightBuildingCount() == 0);
        const int sides[4] = {0, 2, 1, 0};
        std::uint32_t texture = 1;
        std::uint8_t rotation = 0;
        bool flag = false;
        dev->GetBoundaryLotTexture(sides, texture, rotation, flag);
        CHECK(texture == 0);
        CHECK(dev->Shutdown());                                       // a second Shutdown does nothing
        CHECK(city.agentsRemoved == 1 && city.unsubscribed.size() == 9);
        CHECK(dev->Init() && city.subscribed.size() == 18);           // a later Init subscribes again
        dev->Release();                                               // the last reference shuts it down
        CHECK(city.agentsRemoved == 2 && city.unsubscribed.size() == 18);
    }
    a->Release();
    b->Release();
    c->Release();
    for (Prop* prop : {light, plain, away}) {
        occupants->RemoveOccupant(prop, false, 0);
        prop->Release();
    }
    occupants->Shutdown();
    occupants->Release();
    lot->Release();
    if (failures) {
        std::printf("sc4_lotdevlife: %d failures\n", failures);
        return 1;
    }
    std::printf("sc4_lotdevlife: ok\n");
    return 0;
}
