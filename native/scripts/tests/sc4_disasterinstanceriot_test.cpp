// cSC4DisasterInstanceRiot: tuning defaults and the exemplar, adding leaders (nearest road, the police curve, the leader
// limit), the once-a-second step (strength, levels, effects, controllers, more leaders, ending), the marked road tiles and
// the phases, against a fake city.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/DisasterInstanceRiot.h"
#include "sc4/Occupant.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SpiralIterator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
constexpr int kCells = 16;

class Road final : public cISC4RiotNetworkOccupant {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void ClearState(std::uint32_t bits) override { cleared |= bits; }
    void SetState(std::uint32_t bits) override { set |= bits; }
    std::uint32_t refs = 0, set = 0, cleared = 0;
    bool crowd = true;   // a tile a crowd can stand on
};

class LeaderOccupant final : public cSC4Occupant {
public:
    void GetPosition(float* xyz) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    float position[3] = {0, 0, 0};
};

class Leader final : public IRiotLeader {
public:
    Leader() { occupant.AddRef(); occupant.SetFlag(1); }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    cISC4Occupant* AsOccupant() override { return &occupant; }
    void GetPosition(float* xyz) override { occupant.GetPosition(xyz); }
    void SetVisible(bool v) override { visible = v; }
    void AddRegEffect(const cRZString& effect, const cRZString& point) override { log += "+" + std::string(effect.ToChar()) + "@" + point.ToChar() + " "; }
    void RemoveRegEffect(const cRZString& effect) override { ++removed; lastRemoved = effect.ToChar(); }
    void SetState(std::uint32_t s, float timer, bool flag) override { state = s; stateTimer = timer; stateFlag = flag; }
    LeaderOccupant occupant;
    int refs = 0, removed = 0;
    bool visible = false, stateFlag = false;
    std::uint32_t state = 0;
    float stateTimer = 0;
    std::string log, lastRemoved;
};

class Layer final : public cISC4DisasterLayer {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void EnableDisasters(bool) override {}
    bool DisastersEnabled() const override { return true; }
    cISC4DisasterManager* GetDisasterManager(std::uint32_t id) const override { return id == kDisasterManagerIdRiot ? manager : nullptr; }
    void DisasterHasBegun(cISC4DisasterInstance*) override { ++begun; }
    void DisasterHasEnded() override { ++ended; }
    bool IsADisasterOngoing() const override { return begun > ended; }
    bool GoToNextDisaster() override { return false; }
    bool GetMostRecentDisasterLocation(cISC4DisasterInstance*, int*, int*) override { return false; }
    bool RegisterDisasterLocation(cISC4DisasterInstance*, int x, int z, cISC4Occupant* occupant, int zoom, bool jump) override {
        ++registered; lastOccupant = occupant; lastZoom = zoom; lastJump = jump; lastCell = x * 100 + z; return true;
    }
    bool UnregisterDisasterLocations(cISC4DisasterInstance*) override { ++unregisteredAll; return true; }
    bool UnregisterDisasterLocationByOccupant(cISC4DisasterInstance*, cISC4Occupant* occupant) override { ++unregistered; lastUnregistered = occupant; return true; }
    int GetDisasterLocationCount() const override { return registered - unregistered; }
    bool GetDisasterLocationByIndex(int, cISC4DisasterInstance**, int*, int*) const override { return false; }
    DisasterCost* GetDisasterCost() override { return nullptr; }
    cISC4DisasterManager* manager = nullptr;
    int begun = 0, ended = 0, registered = 0, unregistered = 0, unregisteredAll = 0, lastZoom = -1, lastCell = -1;
    bool lastJump = false;
    cISC4Occupant* lastOccupant = nullptr;
    cISC4Occupant* lastUnregistered = nullptr;
};

class DisasterHostFake final : public IDisasterHost {
public:
    cISC4DisasterLayer* DisasterLayer() override { return nullptr; }
    cISC4OccupantManager* Occupants() override { return nullptr; }
    bool PositionToCell(float, float, int&, int&) override { return false; }
    bool CellCenterToPosition(int, int, float&, float&) override { return false; }
    float TerrainHeight(float, float) override { return 0; }
    int CityCellCountX() override { return kCells; }
    int CityCellCountZ() override { return kCells; }
    cISC4DisasterManager* CreateManager(std::uint32_t) override { return nullptr; }
};

class Host final : public IRiotInstanceHost {
public:
    cISC4DisasterLayer* DisasterLayer() override { return layer; }
    int CellCountX() override { return kCells; }
    int CellCountZ() override { return kCells; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { cellX = static_cast<int>(x / 16.0f); cellZ = static_cast<int>(z / 16.0f); }
    void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) override { x = cellX * 16.0f; z = cellZ * 16.0f; }
    float TerrainAltitude(float x, float z) override { return 100.0f + x * 0.001f + z * 0.0f; }
    std::uint8_t PolicePower(int cellX, int cellZ) override { ++policeAsks; lastPoliceCell = cellX * 100 + cellZ; return police; }
    cISC4RiotNetworkOccupant* NetworkOccupantAtCell(int cellX, int cellZ) override {
        if (cellX < 0 || cellZ < 0 || cellX >= kCells || cellZ >= kCells) { ++outsideAsks; return nullptr; }
        auto it = roads.find(cellX * 100 + cellZ);
        return it == roads.end() ? nullptr : it->second;
    }
    bool IsRiotRoad(cISC4RiotNetworkOccupant* occupant) override { return static_cast<Road*>(occupant)->crowd; }
    IRiotLeader* CreateLeader(const float position[3]) override {
        if (refuse) return nullptr;
        auto* leader = new Leader();
        for (int i = 0; i < 3; ++i) leader->occupant.position[i] = position[i];
        made.push_back(leader);
        return leader;
    }
    void AddController(cISC4Occupant* occupant, const cRZString& name) override { controllers += std::string(name.ToChar()) + (occupant == &made.front()->occupant ? "/first " : "/other "); }
    void RemoveControllers(cISC4Occupant* occupant) override { ++removedControllers; lastRemovedControllers = occupant; }
    void RecordHistory(std::uint32_t id, std::int32_t value) override { if (id == kHistoryRiotsStarted) history += value; }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override { tickable = target; tickPriority = priority; ++ticksAdded; }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override { if (target == tickable && priority == tickPriority) ++ticksRemoved; }
    double RandomUnit() override { return random; }
    const cISCPropertyHolder* TuningExemplar() override { return tuning; }
    bool LoadTuningCurve(cSC4ResponseCurve& curve, std::uint32_t property) override {
        if (property != kPropertyRiotPoliceCurve || tuningCurve.empty()) return false;
        return curve.SetResponseCurveData(tuningCurve);
    }

    Layer* layer = nullptr;
    std::map<int, Road*> roads;
    std::vector<Leader*> made;
    std::uint8_t police = 0;
    int policeAsks = 0, lastPoliceCell = -1, outsideAsks = 0, removedControllers = 0, history = 0, ticksAdded = 0, ticksRemoved = 0;
    std::uint32_t tickPriority = 0;
    cIGZMessageTarget2* tickable = nullptr;
    cISC4Occupant* lastRemovedControllers = nullptr;
    bool refuse = false;
    double random = 0.5;
    const cISCPropertyHolder* tuning = nullptr;
    std::vector<SC4Point> tuningCurve;
    std::string controllers;
};

// The contract of the road search, walked independently: spiral order, crowd tiles only, at most seven turns.
bool Nearest(Host& host, int x, int z, int& outX, int& outZ) {
    SpiralIterator spiral = SpiralIterator::Over(x, z, kCells, kCells);
    int cellX = x, cellZ = z;
    while (spiral.turns <= kRiotRoadSearchTurns) {
        auto it = host.roads.find(cellX * 100 + cellZ);
        if (cellX >= 0 && cellZ >= 0 && cellX < kCells && cellZ < kCells && it != host.roads.end() && it->second->crowd) { outX = cellX; outZ = cellZ; return true; }
        if (!spiral.GetNext(cellX, cellZ)) break;
    }
    return false;
}

int MarkedCount(const std::map<int, Road*>& roads) {
    int count = 0;
    for (const auto& entry : roads) count += entry.second->set == 2 ? 1 : 0;
    return count;
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    DisasterHostFake disasterHost;
    SetDisasterHost(&disasterHost);
    cSC4DisasterManagerRiot::snRiotMaxInstances = 3;
    cSC4DisasterManagerRiot::snRiotMaxLeaders = 5;

    // ---- without a host nothing starts
    {
        SetRiotInstanceHost(nullptr);
        auto* riot = new cSC4DisasterInstanceRiot();
        riot->AddRef();
        CHECK(!riot->Init());
        CHECK(!riot->AddLeader(1, 1, false));
        riot->UpdateLeaders();
        CHECK(riot->GetPhase() == 0 && riot->GetID() == kDisasterManagerIdRiot);
        CHECK(riot->Release() == 0);
    }

    // ---- the city: a road along z = 8 (cells 0..15) and one tile at (10, 3) that crowds cannot use
    Road row[kCells];
    Road rails;
    rails.crowd = false;
    Host host;
    Layer layer;
    host.layer = &layer;
    for (int x = 0; x < kCells; ++x) host.roads[x * 100 + 8] = &row[x];
    host.roads[10 * 100 + 3] = &rails;
    SetRiotInstanceHost(&host);
    CHECK(RiotInstanceHost() == &host);

    auto* manager = new cSC4DisasterManagerRiot();
    manager->AddRef();
    CHECK(manager->Init());
    layer.manager = manager;

    // ---- no tuning exemplar in the data: the defaults and NO police curve (the built-in curve is only for an exemplar
    // that lacks one), so the police neither refuse a leader nor hold one back
    {
        auto* riot = new cSC4DisasterInstanceRiot();
        riot->AddRef();
        CHECK(riot->Init());
        CHECK(riot->LevelCount() == 6 && riot->SecondsPerLevel() == 30);
        host.police = 255;
        CHECK(riot->AddLeader(10, 8, false));
        riot->UpdateLeaders();
        CHECK(riot->Leaders().size() == 1 && riot->Leaders().front().strength == 1);
        CHECK(riot->Release() == 1);
        CHECK(manager->RemoveInstanceFromList(riot));
        CHECK(host.made.size() == 1 && host.made[0]->refs == 0);
        delete host.made[0];
        host.made.clear();
        host = Host();
        layer = Layer();
        layer.manager = manager;
        host.layer = &layer;
        for (int x = 0; x < kCells; ++x) { host.roads[x * 100 + 8] = &row[x]; row[x] = Road(); }
        host.roads[10 * 100 + 3] = &rails;
    }

    // ---- an exemplar without properties: the defaults and the built-in police curve; Init, the first leader
    LeaderOccupant emptyTuning;
    emptyTuning.AddRef();
    host.tuning = emptyTuning.AsPropertyHolder();
    {
        auto* riot = new cSC4DisasterInstanceRiot();
        riot->AddRef();
        CHECK(riot->Init());
        CHECK(riot->LevelCount() == 6 && riot->SecondsPerLevel() == 30);
        CHECK((riot->LeadersPerLevel() == std::vector<std::uint32_t>{0, 1, 0, 1, 0, 0}));
        CHECK(host.ticksAdded == 1 && host.tickPriority == kRiotTickPriority && host.tickable == static_cast<cIGZMessageTarget2*>(riot));
        CHECK(riot->GetManager() == manager && manager->GetInstances().size() == 1);
        CHECK(riot->GetPhase() == 0);
        void* out = nullptr;
        CHECK(riot->QueryInterface(kGZIID_cIGZMessageTarget2, &out) && out == static_cast<cIGZMessageTarget2*>(riot));
        riot->Release();
        CHECK(!riot->QueryInterface(0x12345678, &out) && out == nullptr);

        // the road search: the first tile a crowd can stand on in the spiral's order, while the spiral has turned at most
        // seven times. Police strong enough to refuse the leader show which cell was found without making one.
        host.police = 170;
        int found = 0, missed = 0;
        const int starts[][2] = {{8, 0}, {10, 3}, {0, 0}, {15, 15}, {10, 7}, {3, 8}, {10, 4}, {12, 1}};
        for (const auto& start : starts) {
            int wantX = 0, wantZ = 0;
            const bool want = Nearest(host, start[0], start[1], wantX, wantZ);
            const int asks = host.policeAsks;
            CHECK(!riot->AddLeader(start[0], start[1], false));
            if (want) { ++found; CHECK(host.policeAsks == asks + 1 && host.lastPoliceCell == wantX * 100 + wantZ && wantZ == 8); }
            else { ++missed; CHECK(host.policeAsks == asks); }
        }
        CHECK(found >= 3 && missed >= 1);
        CHECK(host.made.empty() && riot->GetPhase() == 0 && layer.begun == 0);
        host.police = 0;
        CHECK(riot->AddLeader(10, 8, false));
        CHECK(host.made.size() == 1);
        Leader* first = host.made[0];
        CHECK(host.lastPoliceCell == 10 * 100 + 8);
        CHECK(first->occupant.position[0] == 10 * 16.0f + 8.0f && first->occupant.position[2] == 8 * 16.0f + 8.0f);
        CHECK(first->occupant.position[1] > 100.16f && first->occupant.position[1] < 100.17f);   // the terrain under the final point
        CHECK(first->refs == 1 && first->state == kRiotLeaderState && first->stateTimer == -1.0f && first->stateFlag && first->visible);
        CHECK(layer.registered == 1 && layer.lastOccupant == &first->occupant && layer.lastJump && layer.lastCell == 0);
        CHECK(row[10].refs == 2);                                        // the leader's tile + the manager's marked set
        CHECK(MarkedCount(host.roads) == 7 && row[7].set == 2 && row[13].set == 2 && row[6].set == 0 && row[14].set == 0);
        CHECK(rails.set == 0);                                           // row 3 is outside the seven by seven around (10, 8)
        CHECK(riot->GetPhase() == 1 && layer.begun == 1 && host.history == 1);
        CHECK(riot->Leaders().size() == 1 && riot->Leaders().front().level == -1 && riot->Leaders().front().maxLevel == -1);

        // the police curve (built in: 64 -> 0, 65 -> 1, 255 -> 3): an answer of 2 or more refuses a leader
        host.police = 170;
        CHECK(!riot->AddLeader(10, 8, false));
        host.police = 150;
        CHECK(riot->AddLeader(10, 8, true));
        CHECK(host.made.size() == 2 && !layer.lastJump);                 // a leader brought by the riot does not move the camera
        CHECK(layer.begun == 1 && host.history == 1);                    // the riot began once
        // the manager refusing the automaton: no leader, nothing held
        host.police = 0;
        host.refuse = true;
        const std::uint32_t before = row[2].refs;
        CHECK(!riot->AddLeader(2, 8, false));
        CHECK(riot->Leaders().size() == 2 && row[2].refs == before);
        host.refuse = false;

        // within three cells of the west edge the unsigned cell loop wraps: nothing is marked, the leader still stands
        const int marked = MarkedCount(host.roads);
        CHECK(riot->AddLeader(2, 8, false));
        CHECK(MarkedCount(host.roads) == marked && host.outsideAsks == 0);
        CHECK(row[2].refs == 1);                                         // only the leader's tile reference
        // the leader limit
        CHECK(riot->AddLeader(12, 8, false) && riot->AddLeader(12, 8, false));
        CHECK(riot->Leaders().size() == 5);
        CHECK(!riot->AddLeader(12, 8, false));
        CHECK(host.made.size() == 5);

        // ---- the step: no police = one second of strength each; level 0 at once
        host.police = 0;
        riot->UpdateLeaders();
        const auto& lead = riot->Leaders().front();
        CHECK(lead.strength == 1 && lead.level == 0 && lead.maxLevel == 0);
        CHECK(first->removed == 6 && first->lastRemoved == "Riot6" && first->log == "+Riot1@Riot1 ");
        CHECK(host.controllers.find("riot_leader_0/first ") == 0);
        CHECK(riot->Leaders().size() == 5);                              // level 0 brings no leader (and the riot is full)
        for (int i = 0; i < 28; ++i) riot->UpdateLeaders();
        CHECK(lead.strength == 29 && lead.level == 0 && first->removed == 6);   // the effects change only with the level
        // weak police hold a riot still: the curve's 1.37 at power 100 leaves (int)(1 - 1.37) = 0
        host.police = 100;
        riot->UpdateLeaders();
        CHECK(lead.strength == 29);
        // strong police push it back two a second: 29 -> 27
        host.police = 255;
        riot->UpdateLeaders();
        CHECK(lead.strength == 27 && lead.level == 0);
        host.police = 0;
        for (int i = 0; i < 3; ++i) riot->UpdateLeaders();
        CHECK(lead.strength == 30 && lead.level == 1 && lead.maxLevel == 1);
        CHECK(first->removed == 12 && first->log == "+Riot1@Riot1 +Riot2@Riot2 ");
        CHECK(host.controllers.find("riot_leader_1/first") != std::string::npos);
        CHECK(riot->Leaders().size() == 5 && host.made.size() == 5);     // level 1 asks for one more leader, the riot is full

        // a leader that has left the city (occupant flag 1 gone) is over
        Leader* second = host.made[1];
        const std::string secondLog = second->log;
        const int secondRemoved = second->removed;
        second->occupant.ClearFlag(1);
        riot->UpdateLeaders();
        CHECK(riot->Leaders().size() == 4 && second->refs == 0 && second->state == kRiotOverState && !second->stateFlag);
        CHECK(host.removedControllers == 1 && host.lastRemovedControllers == &second->occupant);
        CHECK(layer.unregistered == 1 && layer.lastUnregistered == &second->occupant);
        CHECK(second->removed == secondRemoved + 6 && second->log == secondLog);   // its effects came off, none went on

        // a fallen level does not bring the controller or the leaders again; police power ends a riot below zero
        host.police = 255;
        for (int i = 0; i < 2; ++i) riot->UpdateLeaders();               // 31 -> 27: back at level 0
        CHECK(lead.level == 0 && lead.maxLevel == 1);
        const std::string controllers = host.controllers;
        host.police = 0;
        for (int i = 0; i < 3; ++i) riot->UpdateLeaders();               // 30: level 1 again
        CHECK(lead.level == 1 && host.controllers == controllers && host.made.size() == 5);
        // a leader walking onto another road tile: its tile reference moves and the tiles around the new one are marked
        first->occupant.position[0] = 4 * 16.0f + 1.0f;
        riot->UpdateLeaders();
        CHECK(lead.networkOccupant == &row[4] && row[1].set == 2 && row[7].set == 2);
        const std::uint32_t held = row[10].refs;
        first->occupant.position[0] = 4 * 16.0f + 15.0f;                 // the same tile: nothing changes
        riot->UpdateLeaders();
        CHECK(lead.networkOccupant == &row[4] && row[10].refs == held);
        first->occupant.position[2] = 2 * 16.0f;                         // off the road: the last tile is kept
        riot->UpdateLeaders();
        CHECK(lead.networkOccupant == &row[4]);

        host.police = 255;
        int steps = 0;
        while (!riot->Leaders().empty() && steps < 100) { riot->UpdateLeaders(); ++steps; }
        CHECK(riot->Leaders().empty() && steps < 30);
        CHECK(riot->GetPhase() == 3 && layer.ended == 1 && layer.unregisteredAll == 1);
        for (Leader* leader : host.made) CHECK(leader->refs == 0 && leader->state == kRiotOverState);
        CHECK(host.removedControllers == 5 && layer.unregistered == 5);
        // an over riot stays over
        host.police = 0;
        riot->UpdateLeaders();
        CHECK(riot->GetPhase() == 3 && layer.ended == 1);

        // Shutdown leaves the manager, which lets go of the marked tiles when its last riot goes
        CHECK(riot->Shutdown());
        CHECK(host.ticksRemoved == 1 && layer.unregisteredAll == 2 && manager->GetInstances().empty() && riot->GetManager() == nullptr);
        CHECK(riot->Shutdown() && host.ticksRemoved == 1);
        bool released = true;
        for (Road& road : row) released = released && road.refs == 0 && (road.set == 0 || road.cleared == 2);
        CHECK(released);
        CHECK(riot->Release() == 0);
    }

    // ---- a riot that nobody stops passes the last level and ends; a new top level brings leaders around the leader
    {
        for (Leader* leader : host.made) delete leader;
        host.made.clear();
        host.controllers.clear();
        host.police = 0;
        host.removedControllers = 0;
        auto* riot = new cSC4DisasterInstanceRiot();
        riot->AddRef();
        CHECK(riot->Init());
        CHECK(riot->AddLeader(8, 8, false));
        for (int i = 0; i < 30; ++i) riot->UpdateLeaders();
        CHECK(riot->Leaders().size() == 2 && host.made.size() == 2);     // level 1: one more leader
        CHECK(host.made[1]->occupant.position[0] == 8 * 16.0f + 8.0f);   // on the road nearest to the first leader's cell
        CHECK(riot->Leaders().back().strength == 0 && riot->Leaders().back().level == -1);   // added behind the walk: it steps next second
        for (int i = 0; i < 60; ++i) riot->UpdateLeaders();
        // second 90: the first leader is at level 3 (one more); the second reached level 1 at second 60 (a third), and the
        // third reaches level 1 now (one more): five, the riot is full
        CHECK(riot->Leaders().front().level == 3 && riot->Leaders().size() == 5);
        CHECK(host.controllers.find("riot_leader_3/first") != std::string::npos);
        CHECK(host.made[0]->log == "+Riot1@Riot1 +Riot2@Riot2 +Riot3@Riot3 +Riot4@Riot4 ");
        for (int i = 0; i < 89; ++i) riot->UpdateLeaders();
        CHECK(riot->Leaders().front().automaton == host.made[0] && riot->Leaders().front().level == 5);
        riot->UpdateLeaders();                                           // 180 seconds: level 6 = past the last level
        CHECK(riot->Leaders().front().automaton != host.made[0] && host.made[0]->refs == 0 && host.made[0]->state == kRiotOverState);
        CHECK(host.made[0]->log.find("Riot7") == std::string::npos);     // no effect for the level past the last
        CHECK(host.controllers.find("riot_leader_6") == std::string::npos);
        CHECK(riot->GetPhase() == 1);                                    // the others riot on
        // let go of with leaders still in it (the manager held the last reference): the destructor shuts it down and
        // releases them
        Leader* still = static_cast<Leader*>(riot->Leaders().front().automaton);
        CHECK(still->refs == 1);
        CHECK(riot->Release() == 1);
        const int removedTicks = host.ticksRemoved;
        CHECK(manager->RemoveInstanceFromList(riot));
        CHECK(still->refs == 0 && manager->GetInstances().empty() && host.ticksRemoved == removedTicks + 1);
        bool released = true;
        for (Road& road : row) released = released && road.refs == 0;
        CHECK(released);
    }

    // ---- the tuning exemplar and the animation tick
    {
        for (Leader* leader : host.made) delete leader;
        host.made.clear();
        LeaderOccupant holder;
        holder.AddRef();
        cSCPropertyHelp::SetPropertyValue(holder.AsPropertyHolder(), kPropertyRiotLevels, std::uint32_t{2});
        cSCPropertyHelp::SetPropertyValue(holder.AsPropertyHolder(), kPropertyRiotSecondsPerLevel, std::uint32_t{4});
        cSCPropertyHelp::SetPropertyValue(holder.AsPropertyHolder(), kPropertyRiotLeadersPerLevel, std::vector<std::uint32_t>{2, 0});
        cSCPropertyHelp::SetPropertyValue(holder.AsPropertyHolder(), kPropertyRiotLocationZoom, std::uint8_t{4});
        host.tuning = holder.AsPropertyHolder();
        host.tuningCurve = {{0.0f, 0.0f}, {10.0f, 5.0f}};                // any police at all holds a riot back
        auto* riot = new cSC4DisasterInstanceRiot();
        riot->AddRef();
        CHECK(riot->Init());
        CHECK(riot->LevelCount() == 2 && riot->SecondsPerLevel() == 4 && (riot->LeadersPerLevel() == std::vector<std::uint32_t>{2, 0}));
        host.police = 4;                                                 // the tuned curve answers 2: refused
        CHECK(!riot->AddLeader(8, 8, false));
        host.police = 0;
        CHECK(riot->AddLeader(8, 8, false) && layer.lastZoom == 4);
        // the tick message runs the step at most once a second of the riot's own timer
        cRZMessage2Standard tick;
        tick.SetType(kMessageRiotTick);
        CHECK(riot->DoMessage(&tick));
        CHECK(riot->Leaders().size() == 3 && riot->Leaders().front().strength == 1);   // level 0 brings two leaders here
        CHECK(riot->DoMessage(&tick));
        CHECK(riot->Leaders().front().strength == 1);
        cRZMessage2Standard other;
        other.SetType(0x12345);
        CHECK(riot->DoMessage(&other) && riot->Leaders().front().strength == 1);
        for (int i = 0; i < 6; ++i) riot->UpdateLeaders();               // 7 seconds: level 1, the last
        CHECK(riot->Leaders().front().level == 1 && host.made[0]->removed == 4);   // two effects come off each change
        riot->UpdateLeaders();                                           // 8 seconds: level 2 = past the last
        CHECK(riot->Leaders().size() == 4 && host.made[0]->refs == 0);   // the new leaders brought two more at their level 0
        CHECK(riot->Release() == 1);
        CHECK(manager->RemoveInstanceFromList(riot));
        host.tuning = emptyTuning.AsPropertyHolder();
    }

    for (Leader* leader : host.made) delete leader;
    manager->Shutdown();
    manager->Release();
    SetRiotInstanceHost(nullptr);
    std::printf(failures ? "%d FAILURES\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
