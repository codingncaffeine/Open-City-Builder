// Riots in a native city: the riot instance's services (IRiotInstanceHost): the police power map, the road tiles, the
// pedestrian manager that makes the riot leaders, the automata controllers and the animation tick. The leader the riot drives is
// the recovered pedestrian behind a small adapter; a road tile is its occupant behind one kept per tile.
#include <cstdio>
#include <cstdlib>
#include <map>

#include "app/NativeAnimationTicks.h"
#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "rz/cRZRandom.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/Automaton.h"
#include "sc4/DisasterInstanceRiot.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
const sc4::cISCPropertyHolder* NativeHeldExemplar(const cGZPersistResourceKey& key);   // app/NativeDisasters.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_RIOTDEBUG") != nullptr;
    return on;
}

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}

// A riot leader: the pedestrian automaton, by the calls the riot makes on it. Counted by the riot; holds the automaton.
class Leader final : public IRiotLeader {
public:
    explicit Leader(cSC4Automaton* automaton) : automaton_(automaton) { automaton_->AddRef(); }
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ > 0) return;
        automaton_->Release();
        delete this;
    }
    cISC4Occupant* AsOccupant() override { return automaton_->AsOccupant(); }
    void GetPosition(float* xyz) override { automaton_->GetPosition(xyz); }
    void SetVisible(bool visible) override { automaton_->SetVisible(visible); }
    void AddRegEffect(const cRZString& effect, const cRZString& point) override { automaton_->AddRegEffect(effect, point); }
    void RemoveRegEffect(const cRZString& effect) override { automaton_->RemoveRegEffect(effect); }
    void SetState(std::uint32_t state, float timer, bool flag) override {
        if (Trace() && state == kRiotOverState) {
            float p[3] = {};
            automaton_->GetPosition(p);
            const tAutomataState* top = automaton_->GetState();
            std::fprintf(stderr, "riot: leader at (%.0f, %.0f) leaves the riot: in the city %d, automata flags 0x%x, state %d\n", p[0], p[2],
                         automaton_->AsOccupant()->IsFlagSet(1) ? 1 : 0, static_cast<unsigned>(automaton_->GetFlags()),
                         top ? static_cast<int>(top->stateType) : -1);
        }
        automaton_->SetState(state, nullptr, timer, flag, 0, 0, nullptr);
    }

private:
    cSC4Automaton* automaton_;
    int refs_ = 0;
};

// A road tile as the riot manager marks it: the two calls are the occupant's removal flags (Mac i386 occupant slots 0x30 /
// 0x34): a marked tile cannot be removed while riots run. One view per tile, kept for the program; the references are the
// occupant's.
class RoadTile final : public cISC4RiotNetworkOccupant {
public:
    explicit RoadTile(ITrafficNetworkMapOccupant* occupant) : occupant_(occupant) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return occupant_->AddRef(); }
    std::uint32_t Release() override { return occupant_->Release(); }
    void ClearState(std::uint32_t bits) override { occupant_->AsOccupant()->SetRemovalFlags(static_cast<int>(bits)); }
    void SetState(std::uint32_t bits) override { occupant_->AsOccupant()->UnsetRemovalFlags(static_cast<int>(bits)); }
    ITrafficNetworkMapOccupant* Occupant() const { return occupant_; }

private:
    ITrafficNetworkMapOccupant* occupant_;
};

class RiotHost final : public IRiotInstanceHost {
public:
    cISC4DisasterLayer* DisasterLayer() override { return CityAs<cISC4DisasterLayer>(CityComponent::DisasterLayer); }
    int CellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(cellX, cellZ, x, z);
    }
    float TerrainAltitude(float x, float z) override {
        auto* terrain = CityAs<cSTETerrain>(CityComponent::Terrain);
        return terrain ? terrain->GetAltitude(x, z) : 0.0f;
    }
    std::uint8_t PolicePower(int cellX, int cellZ) override {
        cSC4City* city = HostCity();
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        const cSC4SimGrid<std::uint8_t>* grid = police ? police->Protection().AdjustedGrid() : nullptr;
        if (!city || !grid || !city->CellIsInBounds(cellX, cellZ)) return 0;   // port: the original reads the grid unchecked
        return grid->GetCellValue(cellX, cellZ);
    }
    cISC4RiotNetworkOccupant* NetworkOccupantAtCell(int cellX, int cellZ) override {
        auto* map = CityAs<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
        TrafficTileInfo* tile = map ? map->SafeGetTileInfoAtCell(cellX, cellZ) : nullptr;
        if (!tile || !tile->occupant) return nullptr;
        auto found = tiles_.find(tile->occupant);
        if (found == tiles_.end()) found = tiles_.emplace(tile->occupant, new RoadTile(tile->occupant)).first;
        return found->second;
    }
    bool IsRiotRoad(cISC4RiotNetworkOccupant* occupant) override {
        ITrafficNetworkMapOccupant* tile = static_cast<RoadTile*>(occupant)->Occupant();
        return tile->IsOfType(0) || tile->IsOfType(3) || tile->IsOfType(6);
    }
    IRiotLeader* CreateLeader(const float position[3]) override {
        auto* pedestrians = CityAs<cSC4AutomataManager>(CityComponent::PedestrianManager);
        const cS3DVector3 where{position[0], position[1], position[2]};
        IAutomataAutomaton* view =
            pedestrians ? pedestrians->CreateAutomaton(kRiotLeaderGroup, where, 2, cGZPersistResourceKey{}, nullptr, 0xff, nullptr) : nullptr;
        cSC4Automaton* automaton = view ? AutomatonOf(view) : nullptr;
        if (Trace()) std::fprintf(stderr, "riot: leader at (%.0f, %.0f) -> %s\n", position[0], position[2], automaton ? "made" : "refused");
        return automaton ? new Leader(automaton) : nullptr;
    }
    void AddController(cISC4Occupant* occupant, const cRZString& templateName) override {
        auto* controllers = CityAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
        cSC4AutomataController* made = controllers ? controllers->AddControllerFromTemplateName(occupant, templateName, nullptr) : nullptr;
        if (Trace()) std::fprintf(stderr, "riot: controller %s -> %s\n", templateName.ToChar(), made ? "added" : "none");
    }
    void RemoveControllers(cISC4Occupant* occupant) override {
        if (auto* controllers = CityAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager))
            controllers->RemoveControllers(occupant);
    }
    void RecordHistory(std::uint32_t id, std::int32_t value) override {
        if (auto* history = CityAs<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse)) history->RecordHistoryData(id, value);
    }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    double RandomUnit() override { return random_.RandomDoubleUniform(); }
    const cISCPropertyHolder* TuningExemplar() override {
        return NativeHeldExemplar(cGZPersistResourceKey{0x6534284a, kRiotTuningGroup, kRiotTuningInstance});
    }
    bool LoadTuningCurve(cSC4ResponseCurve& curve, std::uint32_t property) override {
        const cISCPropertyHolder* tuning = TuningExemplar();
        std::vector<float> points;
        if (!tuning || !cSCPropertyHelp::GetPropertyValue(tuning, property, points) || points.size() < 2) return false;
        return curve.SetResponseCurveData(points.data(), points.size());
    }
    void ForgetTiles() {
        for (auto& tile : tiles_) delete tile.second;
        tiles_.clear();
    }

private:
    std::map<ITrafficNetworkMapOccupant*, RoadTile*> tiles_;
    cRZRandom random_{0xffffffffu};
} g_riotHost;

cSC4DisasterManagerRiot* RiotManager() {
    cISC4DisasterLayer* layer = g_riotHost.DisasterLayer();
    return layer ? dynamic_cast<cSC4DisasterManagerRiot*>(layer->GetDisasterManager(kDisasterManagerIdRiot)) : nullptr;
}

}  // namespace

void InstallRiotHost() { sc4::SetRiotInstanceHost(&g_riotHost); }
// A closed city's road tiles are gone: so are their views.
void NativeRiotCityClosed() { g_riotHost.ForgetTiles(); }

// The patrol cars' question (IPatrolCarHost::RiotInProgress, the disaster layer's vslot 0x28): is any disaster running.
bool NativeDisasterOngoing() {
    cISC4DisasterLayer* layer = g_riotHost.DisasterLayer();
    return layer && layer->IsADisasterOngoing();
}

// Kit command `riot X Z`: what the mayor rating simulator does at a hotspot (the riot manager's AttemptDisasterStart).
void NativeStartRiot(int cellX, int cellZ) {
    cSC4DisasterManagerRiot* manager = RiotManager();
    if (!manager) {
        std::printf("riot %d,%d -> no riot manager\n", cellX, cellZ);
        return;
    }
    const int before = manager->GetActiveInstanceCount();
    manager->AttemptDisasterStart(cellX, cellZ);
    std::printf("riot %d,%d -> %d riots (was %d), police power %u there\n", cellX, cellZ, manager->GetActiveInstanceCount(), before,
                static_cast<unsigned>(g_riotHost.PolicePower(cellX, cellZ)));
}

// Kit command `riots`: every riot's phase and leaders.
void NativeRiotReport() {
    cSC4DisasterManagerRiot* manager = RiotManager();
    if (!manager) {
        std::printf("riots: no riot manager\n");
        return;
    }
    std::printf("riots: %d\n", manager->GetActiveInstanceCount());
    for (cISC4DisasterInstance* instance : manager->GetInstances()) {
        auto* riot = dynamic_cast<cSC4DisasterInstanceRiot*>(instance);
        if (!riot) continue;
        std::printf("riot: phase %d, %zu leaders\n", riot->GetPhase(), riot->Leaders().size());
        for (const auto& leader : riot->Leaders()) {
            float position[3] = {};
            leader.automaton->GetPosition(position);
            std::printf("riot: leader at (%.0f, %.0f) strength %d level %d highest %d\n", position[0], position[2], leader.strength, leader.level,
                        leader.maxLevel);
        }
    }
}

}  // namespace native
