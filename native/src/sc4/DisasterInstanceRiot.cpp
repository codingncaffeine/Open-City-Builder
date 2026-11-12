#include "sc4/DisasterInstanceRiot.h"

#include <cmath>

#include "rz/cRZMessage2Standard.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IRiotInstanceHost* g_host = nullptr;
constexpr const char* kRiotEffectFormat = "Riot%d";                  // the effect a leader shows at each level (1-based)
constexpr const char* kRiotControllerFormat = "riot_leader_%d";      // 0x005db104: the controller template of a level
constexpr std::uint32_t kOccupantFlagInCity = 1;
}  // namespace

IRiotInstanceHost* RiotInstanceHost() { return g_host; }
void SetRiotInstanceHost(IRiotInstanceHost* host) { g_host = host; }

cSC4DisasterInstanceRiot::cSC4DisasterInstanceRiot() = default;

cSC4DisasterInstanceRiot::~cSC4DisasterInstanceRiot() {
    if (initialised_) Shutdown();
    for (tRiotLeader& leader : leaders_) {
        if (leader.automaton) leader.automaton->Release();
        if (leader.networkOccupant) leader.networkOccupant->Release();
    }
}

bool cSC4DisasterInstanceRiot::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cISC4DisasterInstance || iid == 1) *out = static_cast<cISC4DisasterInstance*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

std::uint32_t cSC4DisasterInstanceRiot::Release() {
    const std::uint32_t left = --refCount_;
    if (left == 0) delete this;
    return left;
}

// @ 0x0033a256
bool cSC4DisasterInstanceRiot::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    if (g_host) {
        if (cISC4DisasterLayer* layer = g_host->DisasterLayer()) layer->UnregisterDisasterLocations(this);
        g_host->RemoveTickable(this, kRiotTickPriority);
    }
    if (manager_) {
        cSC4DisasterManagerRiot* manager = manager_;
        manager_ = nullptr;
        manager->RemoveInstanceFromList(this);
        manager->Release();
    }
    return true;
}

// @ 0x0033aa9c: the defaults, then the tuning exemplar; an exemplar without a police curve gets the built-in one.
void cSC4DisasterInstanceRiot::ReadTuningParameters() {
    levels_ = 6;
    secondsPerLevel_ = 0x1e;
    value50_ = 3;
    leadersPerLevel_ = {0, 1, 0, 1, 0, 0};
    const cISCPropertyHolder* tuning = g_host ? g_host->TuningExemplar() : nullptr;
    if (!tuning) return;
    cSCPropertyHelp::GetPropertyValue(tuning, kPropertyRiotLevels, levels_);
    cSCPropertyHelp::GetPropertyValue(tuning, kPropertyRiotSecondsPerLevel, secondsPerLevel_);
    cSCPropertyHelp::GetPropertyValue(tuning, kPropertyRiotValue50, value50_);
    cSCPropertyHelp::GetPropertyValue(tuning, kPropertyRiotLeadersPerLevel, leadersPerLevel_);
    cSCPropertyHelp::GetPropertyValue(tuning, kPropertyRiotLocationZoom, locationZoom_);
    g_host->LoadTuningCurve(policeCurve_, kPropertyRiotPoliceCurve);
    if (policeCurve_.PointCount() == 0) {
        const std::vector<SC4Point> points = {{0.0f, 0.0f}, {64.0f, 0.0f}, {65.0f, 1.0f}, {255.0f, 3.0f}};
        policeCurve_.SetResponseCurveData(points);
    }
}

// @ 0x0033ad02
bool cSC4DisasterInstanceRiot::Init() {
    if (!g_host) return false;
    ReadTuningParameters();
    initialised_ = true;
    cellCountX_ = g_host->CellCountX();
    cellCountZ_ = g_host->CellCountZ();
    cISC4DisasterLayer* layer = g_host->DisasterLayer();
    cISC4DisasterManager* found = layer ? layer->GetDisasterManager(kDisasterManagerIdRiot) : nullptr;
    if (auto* manager = dynamic_cast<cSC4DisasterManagerRiot*>(found)) {
        manager_ = manager;
        manager_->AddRef();
        manager_->AddInstanceToList(this);
    }
    g_host->AddTickable(this, kRiotTickPriority);
    timer_.SetUnits(cRZTimer::kSeconds);
    timer_.Start();
    return true;
}

// @ 0x0033a742: the nearest cell, spiralling out from (x, z), whose network tile is one a crowd can stand on.
cISC4RiotNetworkOccupant* cSC4DisasterInstanceRiot::FindNearestRoadTile(int& x, int& z) {
    SpiralIterator spiral = SpiralIterator::Over(x, z, g_host->CellCountX(), g_host->CellCountZ());
    int cellX = x, cellZ = z;
    while (spiral.turns <= kRiotRoadSearchTurns) {
        cISC4RiotNetworkOccupant* occupant = g_host->NetworkOccupantAtCell(cellX, cellZ);
        if (occupant && g_host->IsRiotRoad(occupant)) {
            x = cellX;
            z = cellZ;
            return occupant;
        }
        if (!spiral.GetNext(cellX, cellZ)) break;   // port: the original does not test the answer (a start cell inside the city never runs out)
    }
    return nullptr;
}

// @ 0x0033a410: the road tiles of the seven by seven cells around (x, z) go to the manager. The loop counters are unsigned in
// the original: within three cells of the west or north edge the lower bound wraps and nothing is marked.
void cSC4DisasterInstanceRiot::MarkRiotNetworkOccupants(int x, int z) {
    if (!manager_) return;
    for (std::uint32_t cellX = static_cast<std::uint32_t>(x - kRiotMarkRadius); cellX <= static_cast<std::uint32_t>(x + kRiotMarkRadius); ++cellX) {
        for (std::uint32_t cellZ = static_cast<std::uint32_t>(z - kRiotMarkRadius); cellZ <= static_cast<std::uint32_t>(z + kRiotMarkRadius); ++cellZ) {
            cISC4RiotNetworkOccupant* occupant = g_host->NetworkOccupantAtCell(static_cast<int>(cellX), static_cast<int>(cellZ));
            if (occupant) manager_->AddRiotNetworkOccupant(occupant);
        }
    }
}

// @ 0x0033a2e0: every level's effect comes off the leader, then the current level's goes on.
void cSC4DisasterInstanceRiot::UpdateRiotEffects(IRiotLeader* leader, int level) {
    cRZString name;
    for (std::uint32_t i = 0; i < levels_;) {
        ++i;
        name.Sprintf(kRiotEffectFormat, i);
        leader->RemoveRegEffect(name);
    }
    if (-1 < level && static_cast<std::uint32_t>(level) < levels_) {
        name.Sprintf(kRiotEffectFormat, level + 1);
        leader->AddRegEffect(name, name);
    }
}

// @ 0x0033a496
void cSC4DisasterInstanceRiot::UpdatePhase() {
    cISC4DisasterLayer* layer = g_host ? g_host->DisasterLayer() : nullptr;
    if (phase_ == 0) {
        if (leaders_.empty()) return;
        phase_ = 1;
        if (layer) layer->DisasterHasBegun(this);
        g_host->RecordHistory(kHistoryRiotsStarted, 1);
    } else if (phase_ == 1 && leaders_.empty()) {
        phase_ = 3;
        if (layer) {
            layer->UnregisterDisasterLocations(this);
            layer->DisasterHasEnded();
        }
    }
}

// @ 0x0033b26c: a new leader on the road nearest to (x, z), unless the police there are strong enough (the curve answers 2 or
// more) or the riot already has all the leaders it may have. `fromRiot` leaders (brought by another leader) do not move the
// camera.
bool cSC4DisasterInstanceRiot::AddLeader(int x, int z, bool fromRiot) {
    if (!g_host) return false;
    cISC4RiotNetworkOccupant* road = FindNearestRoadTile(x, z);
    if (!road) return false;
    const float held = policeCurve_.GetOutputValue(static_cast<float>(g_host->PolicePower(x, z)));
    if (!(static_cast<int>(held) < 2)) return false;
    if (!(leaders_.size() < static_cast<std::size_t>(cSC4DisasterManagerRiot::snRiotMaxLeaders))) return false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    g_host->CellCornerToPosition(x, z, position[0], position[2]);
    position[0] = static_cast<float>(g_host->RandomUnit() * 16.0 + static_cast<double>(position[0]));
    position[2] = static_cast<float>(g_host->RandomUnit() * 16.0 + static_cast<double>(position[2]));
    position[1] = g_host->TerrainAltitude(position[0], position[2]);
    IRiotLeader* automaton = g_host->CreateLeader(position);
    if (automaton) {
        automaton->AddRef();
        tRiotLeader leader;
        leader.automaton = automaton;
        leader.networkOccupant = road;
        road->AddRef();
        leaders_.push_back(leader);
        automaton->SetState(kRiotLeaderState, -1.0f, true);
        automaton->SetVisible(true);
        if (cISC4DisasterLayer* layer = g_host->DisasterLayer())
            layer->RegisterDisasterLocation(this, 0, 0, automaton->AsOccupant(), locationZoom_, !fromRiot);
        MarkRiotNetworkOccupants(x, z);
    }
    UpdatePhase();
    return automaton != nullptr;
}

// @ 0x0033b5bc: once a second of the instance's timer.
void cSC4DisasterInstanceRiot::Update() {
    const std::uint32_t elapsed = timer_.GetElapsedTimeU32();
    if (elapsed < nextUpdate_) return;
    nextUpdate_ = elapsed + 1;
    UpdateLeaders();
}

void cSC4DisasterInstanceRiot::UpdateLeaders() {
    if (!g_host) return;
    for (auto it = leaders_.begin(); it != leaders_.end();) {
        auto current = it++;
        tRiotLeader& leader = *current;
        cISC4Occupant* occupant = leader.automaton->AsOccupant();
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        int cellX = 0, cellZ = 0;
        g_host->PositionToCell(position[0], position[2], cellX, cellZ);
        const float held = policeCurve_.GetOutputValue(static_cast<float>(g_host->PolicePower(cellX, cellZ)));
        leader.strength += static_cast<int>(1.0f - held);
        std::uint32_t level = 0xffffffffu;
        if (-1 < leader.strength) level = static_cast<std::uint32_t>(leader.strength) / secondsPerLevel_;
        const bool inCity = leader.automaton->AsOccupant()->IsFlagSet(kOccupantFlagInCity);
        const std::uint32_t shown = inCity ? level : 0xffffffffu;
        bool over = !inCity || level == 0xffffffffu;
        if (shown != static_cast<std::uint32_t>(leader.level)) {
            leader.level = static_cast<std::int32_t>(shown);
            UpdateRiotEffects(leader.automaton, static_cast<int>(shown));
            over = levels_ <= shown || over;
            if (leader.maxLevel < static_cast<std::int32_t>(shown) && shown < levels_) {
                leader.maxLevel = static_cast<std::int32_t>(shown);
                if (leaders_.size() < static_cast<std::size_t>(cSC4DisasterManagerRiot::snRiotMaxLeaders)) {
                    const int more = shown < leadersPerLevel_.size() ? static_cast<int>(leadersPerLevel_[shown]) : 0;
                    if (0 < more) {
                        float where[3] = {0.0f, 0.0f, 0.0f};
                        leader.automaton->AsOccupant()->GetPosition(where);
                        int fromX = 0, fromZ = 0;
                        g_host->PositionToCell(where[0], where[2], fromX, fromZ);
                        for (int i = 0; i < more; ++i) AddLeader(fromX, fromZ, true);
                    }
                }
                cRZString controller;
                controller.Sprintf(kRiotControllerFormat, shown);
                g_host->AddController(leader.automaton->AsOccupant(), controller);
            }
        }
        if (over) {
            leader.automaton->SetState(kRiotOverState, -1.0f, false);
            g_host->RemoveControllers(leader.automaton->AsOccupant());
            if (cISC4DisasterLayer* layer = g_host->DisasterLayer())
                layer->UnregisterDisasterLocationByOccupant(this, leader.automaton->AsOccupant());
            leader.automaton->Release();
            if (leader.networkOccupant) leader.networkOccupant->Release();
            leaders_.erase(current);
        } else {
            float where[3] = {0.0f, 0.0f, 0.0f};
            leader.automaton->GetPosition(where);
            const int tileX = static_cast<int>(std::floor(where[0] * 0.0625f));
            const int tileZ = static_cast<int>(std::floor(0.0625f * where[2]));
            cISC4RiotNetworkOccupant* under = g_host->NetworkOccupantAtCell(tileX, tileZ);
            if (under && under != leader.networkOccupant) {
                MarkRiotNetworkOccupants(tileX, tileZ);
                under->AddRef();
                if (leader.networkOccupant) leader.networkOccupant->Release();
                leader.networkOccupant = under;
            }
        }
    }
    UpdatePhase();
}

// @ 0x0033ba88
bool cSC4DisasterInstanceRiot::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kMessageRiotTick) Update();
    return true;
}

}  // namespace sc4
