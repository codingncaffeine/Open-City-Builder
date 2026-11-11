// cSC4TrafficAutomataManager — see TrafficAutomataManager.h for the layout, the tables, the seams and the quirks.
// Every function follows the decompilation at the address in its comment; the ones that still depend on an unrecovered
// neighbour are listed under PARTIAL in notes/ledger_pending/cSC4TrafficAutomataManager.txt.
#include "sc4/TrafficAutomataManager.h"
#include "sc4/PathFinder.h"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sc4 {

namespace {

// The float->int "round half away from zero" idiom of the original: n = floor(v - 0.5), n + 1 when v is integral.
int FloorIdiomImpl(float value) {
    const float biased = value - 0.5f + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof bits);
    const int low = static_cast<int>(bits) - 0x4b400000;
    const float roundTrip = static_cast<float>(low + 1);
    if (value != roundTrip || std::isnan(value) || std::isnan(roundTrip)) return low;
    return low + 1;
}

// The binary tables (0x100a18b20 / 0x100a18b40): index layer - 4 and layer - 2.
const std::uint32_t kPathTypeTable[7] = {3, 3, 4, 3, 3, 6, 7};
const std::uint32_t kAutomataGroupTable[9] = {0x4200, 0x4204, 0x4215, 0x4216, 0x4217, 0x4200, 0x4102, 0x422c, 0x422e};
const std::uint32_t kVehicleLayerOrder[cSC4TrafficAutomataManager::kVehicleLayerCount] = {1, 2, 3, 5, 4, 7, 6, 8, 9, 10};

// 0x100a75280 -> 0x100b218d8, 0x100b21950, 0x100b21928, 0x100b21900, 0x100b21928, 0x100b218b0, 0x100b218b0, 0x100b21928,
// 0x100b21928. The port returns the matching AutomataTuning() curves by index below instead of caching pointers (the
// tuning object is a function-local static in this code base).

}  // namespace

// ---- construction / destruction ----------------------------------------------------------------------------------------
cSC4TrafficAutomataManager::cSC4TrafficAutomataManager(ITrafficAutomataManagerHost* host)
    : cSC4AutomataManager(), trafficHost_(host), effectManager_(this) {
    // 0x100111c01..0x100111c47: the four vtables are the base's, the layer list is empty, +0xb30 = null, +0xb38/+0xb3c =
    // -1, +0xb40 = 0 and the response vector is empty; the effect manager is constructed in place at +0xb60.
    currentCellX_ = -1;
    currentCellZ_ = -1;
    crimeLayerMultiplier_ = 0.0f;
}

cSC4TrafficAutomataManager::~cSC4TrafficAutomataManager() {
    // 0x100111c96: destroy the effect manager (its timer goes first), release the response occupants and the layer list.
    responses_.clear();
    layerUpdates_.clear();
    if (trafficSimulator_) {
        trafficSimulator_->Release();
        trafficSimulator_ = nullptr;
    }
}

// ---- lifecycle ---------------------------------------------------------------------------------------------------------
bool cSC4TrafficAutomataManager::Init() {
    // 0x100111db4
    if (Initialized()) return true;
    if (!cSC4AutomataManager::Init()) return false;
    currentCellX_ = -1;
    currentCellZ_ = -1;
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    if (host) {
        trafficSimulator_ = host->TrafficSimulator();
        if (trafficSimulator_) trafficSimulator_->AddRef();
    }
    timer_.Start();
    return true;
}

bool cSC4TrafficAutomataManager::Shutdown() {
    // 0x100111e24
    if (Initialized()) {
        if (trafficSimulator_) {
            trafficSimulator_->Release();
            trafficSimulator_ = nullptr;
        }
        responses_.clear();
        effectManager_.Shutdown();
    }
    return cSC4AutomataManager::Shutdown();
}

// ---- occupant notifications ---------------------------------------------------------------------------------------------
bool cSC4TrafficAutomataManager::HandleNewBuilding(IAutomataBuildingOccupant* building) {
    // 0x10011206a: no traffic-specific behaviour.
    return cSC4AutomataManager::HandleNewBuilding(building);
}

bool cSC4TrafficAutomataManager::HandleRemoveOccupant(IAutomataOccupant* occupant) {
    // 0x100112074: drop every response entry that targets the occupant (each holds one reference), then the base work.
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    cISC4Occupant* cityOccupant = occupant && host ? host->AsCityOccupant(occupant) : nullptr;
    if (cityOccupant) {
        for (auto it = responses_.begin(); it != responses_.end();) {
            if (it->occupant.get() == cityOccupant) {
                it = responses_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return cSC4AutomataManager::HandleRemoveOccupant(occupant);
}

bool cSC4TrafficAutomataManager::HandleRemoveNetworkOccupant(IAutomataNetworkOccupant* rawOccupant) {
    // 0x100111ee2
    const bool result = cSC4AutomataManager::HandleRemoveNetworkOccupant(rawOccupant);
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkOccupant* occupant = rawOccupant && host ? host->AsTrafficNetworkOccupant(rawOccupant) : nullptr;
    SC4CellRect rect{};
    if (occupant) occupant->AsOccupant()->GetBoundingCityCells(rect);
    for (IAutomataAutomaton* automaton : Automata()) {
        ITrafficAutomataAutomaton* traffic = host ? host->AsTrafficAutomaton(automaton) : nullptr;
        if (!traffic) continue;
        if (traffic->GetNetworkOccupant() == static_cast<ITrafficNetworkMapOccupant*>(occupant)) {
            traffic->SetPathToOccupant(nullptr, 0, nullptr, false);
            traffic->HandleNetworkOccupantRemoved();
            continue;
        }
        // 0x100111f49: an onscreen automaton inside the removed occupant's cells that is "alive" (0x2000) and not a
        // special route (0x20000) loses its network occupant too.
        if (!traffic->CheckAutomataFlag(0x2000)) continue;
        if (traffic->CheckAutomataFlag(0x20000)) continue;
        float position[3] = {0.0f, 0.0f, 0.0f};
        traffic->GetPosition(position);
        const int cellX = FloorIdiomImpl(position[0] * 0.0625f);
        const int cellZ = FloorIdiomImpl(position[2] * 0.0625f);
        if (rect.x0 <= cellX && rect.z0 <= cellZ && cellX <= rect.x1 && cellZ <= rect.z1)
            traffic->HandleNetworkOccupantRemoved();
    }
    return result;
}

// ---- response list (dispatch requests) ---------------------------------------------------------------------------------
bool cSC4TrafficAutomataManager::DispatchResponseAutomata(std::uint32_t group, cISC4Occupant* occupant,
                                                          std::uint32_t count, bool forced, std::uint8_t pathType,
                                                          std::uint32_t flags) {
    // 0x100112114
    if (!occupant) return false;
    tResponseAutomataInfo info;
    info.group = group;
    info.occupant = occupant;
    info.count = count;
    info.forced = forced;
    info.pathType = pathType;
    info.flags = flags | 0x02000000u;
    info.timeLeft = 0.0f;
    info.retries = 0;
    responses_.push_back(info);
    return true;
}

bool cSC4TrafficAutomataManager::UpdateResponseList(float deltaMilliseconds) {
    // 0x10011220c PARTIAL: the retire/retry timing and the per-target loop are ported; the actual spawn/dispatch hooks
    // call the two PARTIAL entry points (Dispatch / SpawnOccupantAutomaton), which need the traffic simulator trip
    // object. The lookup itself (cISC4NetworkManager vslot 0xc8) is the host seam.
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkManagerApi* network = host ? host->TrafficNetworkManager() : nullptr;
    for (auto it = responses_.begin(); it != responses_.end();) {
        tResponseAutomataInfo& entry = *it;
        const float previous = entry.timeLeft;
        entry.timeLeft = previous - deltaMilliseconds;
        if (previous - deltaMilliseconds > 0.0f) {
            ++it;
            continue;
        }
        int networkA = static_cast<int>(entry.pathType) + 2;
        int networkB = static_cast<int>(entry.pathType);
        if (entry.forced) {
            networkA = 1;
            networkB = 0;
        }
        std::vector<ITrafficNetworkMapOccupant*> targets;
        const bool found = network && entry.occupant &&
                           network->GetResponseOccupants(entry.occupant.get(), networkA, networkB, 0xc49, targets);
        bool retire = false;
        if (!found) {
            ++entry.retries;
            retire = entry.retries >= 6;   // 0x10011247e: ++retries; keep while the new count is below 6
        } else {
            for (std::uint32_t i = 0; i < entry.count; ++i) {
                if (targets.empty()) break;
                const std::uint32_t pick = Random().RandomUint32Uniform(static_cast<std::uint32_t>(targets.size()));
                ITrafficNetworkMapOccupant* target = targets[pick];
                IAutomataAutomaton* created = nullptr;
                if (entry.forced) {
                    created = SpawnOccupantAutomaton(target, entry.pathType, entry.group, entry.flags, false, 0);
                } else {
                    tAutomataLocation from;
                    from.kind = kLocationOccupant;
                    from.occupant = host ? host->AsAutomataOccupant(target->AsOccupant()) : nullptr;
                    tAutomataLocation to;
                    to.kind = kLocationOccupant;
                    to.occupant = host ? host->AsAutomataOccupant(entry.occupant.get()) : nullptr;
                    created = Dispatch(entry.group, from, to, entry.flags, 3, cGZPersistResourceKey{}, 0);
                }
                if (!created) continue;
                --entry.count;
                target->Release();
                targets.erase(targets.begin() + pick);
            }
            for (ITrafficNetworkMapOccupant* target : targets) target->Release();
            retire = entry.count == 0;
        }
        if (retire) {
            it = responses_.erase(it);
        } else {
            entry.timeLeft = 1000.0f;   // 0x100112462
            ++it;
        }
    }
    return true;
}

// ---- dispatch ----------------------------------------------------------------------------------------------------------
IAutomataAutomaton* cSC4TrafficAutomataManager::Dispatch(std::uint32_t group, const tAutomataLocation& from,
                                                         const tAutomataLocation& to, std::uint32_t flags,
                                                         std::uint8_t type, const cGZPersistResourceKey& key,
                                                         std::uint8_t argument) {
    // 0x100112842 (i386 0x000cb9ae): pick the model, create the automaton, then route it (Dispatch(automaton) below).
    cGZPersistResourceKey modelKey = key;
    tExemplarData* exemplar = nullptr;
    if (modelKey.instance == 0) {
        tCellRect rect{};
        if (TranslateDispatchLocation(from, rect)) {   // no rect or no exemplar: create from the given key anyway
            exemplar = ChooseRandomExemplar(group, PopulateLayer::kLayerNone, rect.x0, rect.z0);
            if (exemplar) modelKey = exemplar->modelKey;
        }
    }
    IAutomataAutomaton* automaton = CreateAutomatonInternal(group, flags, modelKey, exemplar, argument);
    if (!automaton) return nullptr;
    AddAutomaton(automaton);
    const bool dispatched = Dispatch(automaton, from, to, flags, type);
    if (!dispatched) {
        automaton->Release();
        RemoveAutomaton(automaton, true);
        return nullptr;
    }
    if (IsActive()) { automaton->Release(); return automaton; }
    if (automaton->CheckAutomataFlag(0x80)) { automaton->Release(); return automaton; }
    automaton->SetVisible(false);
    automaton->Release();
    return automaton;
}

bool cSC4TrafficAutomataManager::Dispatch(IAutomataAutomaton* automaton, const tAutomataLocation& from,
                                          const tAutomataLocation& to, std::uint32_t flags, std::uint8_t type) {
    // i386 0x000ce2f6 (dispatch.md 4.2.1): route the automaton from `from` (or, when that does not convert, from its own
    // cell) to `to` with a fresh path finder of the traffic simulator, then place it on the first road of the route.
    // `type` is the route's minimum length (0 for a new automaton, 2 for a re-target).
    if (!automaton) return false;
    tCellRect start{};
    if (!TranslateDispatchLocation(from, start)) {
        float position[3] = {0.0f, 0.0f, 0.0f};
        automaton->GetPosition(position);
        const int cellX = FloorIdiomImpl(position[0] * 0.0625f);
        const int cellZ = FloorIdiomImpl(position[2] * 0.0625f);
        IAutomataManagerHost* host = AutomataManagerHost();
        if (!host || cellX < 0) return false;
        const int cells = host->CityCellCount();   // the original asks the city's width (0x174) and depth (0x178): square
        if (cells <= cellX || cellZ < 0 || cells <= cellZ) return false;
        start.x0 = start.x1 = cellX;
        start.z0 = start.z1 = cellZ;
    }
    tCellRect end{};
    if (!TranslateDispatchLocation(to, end)) return false;
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficSimulatorApi* simulator = trafficSimulator_ ? trafficSimulator_ : (host ? host->TrafficSimulator() : nullptr);
    cISC4PathFinder* finder = simulator ? simulator->CreatePathFinder() : nullptr;
    if (!finder) return false;
    ITrafficAutomataAutomaton* traffic = host ? host->AsTrafficAutomaton(automaton) : nullptr;
    if (!IsTrafficAutomaton(automaton) || !traffic) return false;   // quirk: the path finder is not released here
    automaton->SetAutomataFlag(flags, true);
    finder->SetStartRect({start.x0, start.z0, start.x1, start.z1});
    finder->SetTravelTypeFlag(static_cast<std::uint16_t>(1u << (defaultPathType_ & 31)), true);
    finder->SetTrafficDensity(0);
    finder->SetFlag(1);
    finder->SetFlag(2);
    finder->SetFlag(0x10);
    finder->SetTripDestinationFilter(3);
    std::deque<RouteData>* route = traffic->RouteDeque();
    finder->SetRouteInfo(route);
    finder->SetEndRect({end.x0, end.z0, end.x1, end.z1});
    automaton->SetAutomataFlag(4, false);
    bool placed = false;
    if (finder->FindPath() == 1 && route && type <= route->size() && traffic->BeginRoute(false)) {
        automaton->SetAutomataFlag(0x40, true);
        automaton->SetAutomataFlag(0x2000, false);
        automaton->SetAutomataFlag(0x100, false);
        automaton->SetAutomataFlag(0x200, false);
        automaton->SetAutomataFlag(0x20, false);
        traffic->ReturnToDefaultSpeed();
        cS3DVector3 target{};
        if (TranslateDispatchLocation(to, target)) {
            cISC4Occupant* source = from.kind == kLocationOccupant && from.occupant && host ? host->AsCityOccupant(from.occupant) : nullptr;
            traffic->HandleDispatched(target, source);
        }
        placed = true;
    }
    finder->Release();
    return placed;
}

bool cSC4TrafficAutomataManager::GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& origin,
                                                        float minDistance, float maxDistance,
                                                        const cS3DVector3& center, cS3DVector3& direction,
                                                        std::uint32_t flags) {
    // 0x100112566 PARTIAL: the flags & 0x22000 path is the base implementation; the network-occupant path needs
    // cISC4NetworkManager vslot 0xd0 (exemplars in a cell rect) and the lot manager (SL::spLotManager vslot 0x28).
    if ((flags & 0x22000u) != 0)
        return cSC4AutomataManager::GetRandomCreationPoint(group, origin, minDistance, maxDistance, center, direction,
                                                            flags);
    return false;
}

// ---- per-kind queries --------------------------------------------------------------------------------------------------
bool cSC4TrafficAutomataManager::IsTrafficAutomaton(IAutomataAutomaton* automaton) const {
    // 0x100112d36
    if (!automaton) return false;
    IAutomataOccupant* occupant = automaton->Occupant();
    if (!occupant) return false;
    const std::uint32_t type = occupant->GetType();
    return type == kTrafficOccupantType || type == kTrainOccupantType;
}

bool cSC4TrafficAutomataManager::IsTrainGroup(std::uint32_t group) const {
    // 0x100112812: the 64-bit mask at 0x100a163?? covers groups 0x4201..0x4233; 0x4005 is the extra train group.
    constexpr std::uint64_t kTrainGroupMask = 0x0004f80000700085ull;
    if (group >= 0x4201 && group - 0x4201 <= 0x32 && ((kTrainGroupMask >> ((group - 0x4201) & 63)) & 1) != 0)
        return true;
    return group == kTrainGroupFixed;
}

int cSC4TrafficAutomataManager::GetDefaultPathType(ITrafficAutomataAutomaton* automaton) const {
    // 0x100115fa0
    if (!automaton || !automaton->IsTrain()) return static_cast<int>(defaultPathType_);
    IAutomataOccupant* occupant = automaton->Occupant();
    if (!occupant) return 7;
    if (occupant->IsOccupantGroup(0x422f) || occupant->IsOccupantGroup(0x422e)) return 7;
    if (occupant->IsOccupantGroup(0x422d) || occupant->IsOccupantGroup(0x422c)) return 6;
    if (occupant->IsOccupantGroup(0x4208)) return 4;
    return occupant->IsOccupantGroup(0x4217) ? 5 : 3;
}

int cSC4TrafficAutomataManager::GetPathTypeForLayer(PopulateLayer layer) const {
    // 0x100114f92
    const std::uint32_t value = static_cast<std::uint32_t>(layer);
    const std::uint32_t index = value - 4;
    if (index < 7 && ((0x67u >> (index & 31)) & 1) != 0) return static_cast<int>(kPathTypeTable[index]);
    return static_cast<int>(defaultPathType_);
}

std::uint32_t cSC4TrafficAutomataManager::GetAutomataGroupForLayer(PopulateLayer layer) const {
    // 0x100114fbe
    const std::uint32_t value = static_cast<std::uint32_t>(layer);
    const std::uint32_t index = value - 2;
    if (index < 9 && ((0x1dfu >> (index & 31)) & 1) != 0) return kAutomataGroupTable[index];
    return DefaultGroup();
}

bool cSC4TrafficAutomataManager::IsLayerVisible(std::uint32_t layer) {
    // i386 0x000ca21a: the view level (above ground for every layer but the subway's 6, underground for 6)
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    IAutomataViewUtilities* view = host ? host->ViewUtilities() : nullptr;
    if (!view) return false;
    const int level = view->ViewLevel();
    if (layer >= 11) return false;
    if (((0x7beu >> (layer & 31)) & 1) != 0) return level == 1;
    if (layer == 6) return level == 2;
    return false;
}

float cSC4TrafficAutomataManager::GetMaxAutosPerSecond(PopulateLayer layer) {
    // 0x10011548c: the pointer table at 0x100a752d0.
    AutomataTuningGlobals& tuning = AutomataTuning();
    const std::uint32_t index = static_cast<std::uint32_t>(layer) - 2;
    switch (index) {
        case 0: return tuning.f218f8;
        case 1: return tuning.f21970;
        case 2:
        case 4:
        case 7:
        case 8: return tuning.f21948;
        case 3: return tuning.f21920;
        case 5:
        case 6: return tuning.f218d0;
        default: return tuning.f218d0;
    }
}

cSC4ResponseCurve& cSC4TrafficAutomataManager::GetEdgeDensityCurve(PopulateLayer layer) {
    // 0x100115462: the pointer table at 0x100a75280.
    AutomataTuningGlobals& tuning = AutomataTuning();
    switch (static_cast<std::uint32_t>(layer) - 2) {
        case 0: return tuning.f218d8;
        case 1: return tuning.f21950;
        case 2:
        case 4:
        case 7:
        case 8: return tuning.f21928;
        case 3: return tuning.f21900;
        case 5:
        case 6: return tuning.f218b0;
        default: return tuning.f218b0;
    }
}

double cSC4TrafficAutomataManager::GetEdgeDensityMultiplier(PopulateLayer layer) {
    // 0x1001153c6: city vslot 0x220 -> vslot 0x80 (the hour), city vslot 0x2b0 -> vslot 0xa0 (a 0..1 fraction).
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    if (!host) return 0.0;
    const int hour = host->CityHour();
    const float fraction = host->CityTimeFraction();
    cSC4ResponseCurve& curve = GetEdgeDensityCurve(layer);
    const float value = curve.GetOutputValue(static_cast<float>(hour));
    return static_cast<double>(fraction * 0.00027777778f) * static_cast<double>(value);
}

float cSC4TrafficAutomataManager::GetCommotionMultiplier(ITrafficNetworkMapOccupant* occupant, PopulateLayer layer) {
    // 0x10011596c
    (void)occupant;
    (void)layer;
    if (LoadedFlag() == 0) return 1.0f;
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    if (!host) return 1.0f;
    float value = host->CityClockPercent() / -100.0f + 1.0f;
    if (value < 0.0f) value = 0.0f;
    if (value > 0.5f) value = 0.5f;
    return value;
}

void cSC4TrafficAutomataManager::GetEdgeDensity(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                PopulateLayer layer, float clockMultiplier, float (*weights)[4]) {
    // 0x100115196
    if (!weights) return;
    const std::uint32_t value = static_cast<std::uint32_t>(layer);
    std::uint32_t networkType = value;
    switch (value) {
        case 1: networkType = defaultNetworkType_; break;
        case 2:
        case 5:
        case 6: networkType = value; break;
        case 3: networkType = 4; break;
        case 4: networkType = 3; break;
        case 7:
        case 8:
            GetOccupantTripEdgeDensity(occupant, tile, clockMultiplier, weights);
            return;
        case 9: networkType = 7; break;
        case 10: networkType = 8; break;
        default:
            std::memset(weights, 0, 16 * sizeof(float));
            return;
    }
    if (!occupant || !trafficSimulator_) {
        std::memset(weights, 0, 16 * sizeof(float));
        return;
    }
    // i386 0x000cb5da: the scale is GetCommotionMultiplier (vslot 0x174), not the max autos per second.
    const float commotion = GetCommotionMultiplier(occupant, layer);
    SC4CellRect rect{};
    occupant->AsOccupant()->GetBoundingCityCells(rect);
    bool mirrored = false;
    float multiplier = clockMultiplier;
    if (multiplier < 0.0f) {
        multiplier = -multiplier;
        mirrored = true;
    }
    const std::uint16_t* table =
        trafficSimulator_->GetEdgeCountTable(occupant, static_cast<unsigned>(rect.x0), static_cast<unsigned>(rect.z0),
                                             networkType, mirrored);
    if (!table) {
        std::memset(weights, 0, 16 * sizeof(float));
        return;
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            weights[row][column] = static_cast<float>(table[row * 4 + column]) * multiplier * commotion;
        }
    }
}

void cSC4TrafficAutomataManager::GetOccupantTripEdgeDensity(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                            float multiplier, float (*weights)[4]) {
    // 0x100115310 / i386 0x000ca46a: scaled by GetCommotionMultiplier(occupant, layer 7).
    if (!tile || !weights) return;
    const float commotion = GetCommotionMultiplier(occupant, PopulateLayer::kLayer7);
    if (multiplier >= 0.0f) {
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                weights[row][column] =
                    static_cast<float>(tile->distribution[row][column]) * multiplier * commotion;
    } else {
        // The original walks the table transposed with stride 8 bytes (column, row) and negates the product.
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                weights[row][column] =
                    -(static_cast<float>(tile->distribution[column][row]) * multiplier * commotion);
    }
}

int cSC4TrafficAutomataManager::ChooseNetworkTypeForPathType(ITrafficNetworkMapOccupant* occupant,
                                                             std::uint32_t pathType) {
    // 0x100114de4
    if (!occupant) return -1;
    if (pathType - 1 < 2) {
        if (occupant->HasNetworkFlag(2)) return 2;
        if (occupant->HasNetworkFlag(6)) return 6;
        if (occupant->HasNetworkFlag(0)) return 0;
        return occupant->HasNetworkFlag(3) ? 3 : -1;
    }
    if (pathType == 3) return occupant->HasNetworkFlag(1) ? 1 : -1;
    if (pathType == 4) return occupant->HasNetworkFlag(7) ? 7 : -1;
    return -1;
}

// ---- layer population --------------------------------------------------------------------------------------------------
void cSC4TrafficAutomataManager::AdvanceLayer() {
    // 0x100113054..0x100113094: start at the first node, step forward (wrapping) and skip the layers the tuning mask
    // disables; a one-entry list is used unconditionally. The original can loop forever when no bit matches - guard it.
    if (layerUpdates_.empty()) return;
    if (!currentLayerValid_) {
        currentLayer_ = layerUpdates_.begin();
        currentLayerValid_ = true;
    }
    const std::uint32_t mask = AutomataTuning().f21aac;
    for (std::size_t guard = 0; guard <= layerUpdates_.size(); ++guard) {
        ++currentLayer_;
        if (currentLayer_ == layerUpdates_.end()) currentLayer_ = layerUpdates_.begin();
        const std::uint32_t layer = currentLayer_->layer;
        if (layerUpdates_.size() == 1) return;
        if (((mask >> (layer & 31)) & 1) != 0) return;
    }
}

void cSC4TrafficAutomataManager::PopulateVisibleArea(float deltaMilliseconds) {
    // 0x100112f76 PARTIAL: the layer rotation, the visible-rect sweep, the clock gate and the per-layer dispatch are
    // ported; the population entry points it calls are the PARTIAL density/spawn paths below.
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkManagerApi* network = host ? host->TrafficNetworkManager() : nullptr;
    if (!network || layerUpdates_.empty()) return;
    if (AutomataTuning().f21aac == 0) return;
    for (tTrafficLayerUpdate& update : layerUpdates_) update.elapsed += deltaMilliseconds;
    int rect[4] = {0, 0, -1, -1};
    network->GetVisibleCellRect(rect);
    // The sweep stays inside the rectangle the automata are kept by. The original reads both from view state that the same camera
    // messages refresh (the map's visible cells here, the manager's view rectangle in IsAutomatonOnscreen), so both stand still
    // during a scroll; a car spawned outside the manager's rectangle would be removed as off screen on its first tick.
    rect[0] = std::max(rect[0], static_cast<int>(std::floor(ViewRect(0) / 16.0f)));
    rect[1] = std::max(rect[1], static_cast<int>(std::floor(ViewRect(1) / 16.0f)));
    rect[2] = std::min(rect[2], static_cast<int>(std::floor(ViewRect(2) / 16.0f)));
    rect[3] = std::min(rect[3], static_cast<int>(std::floor(ViewRect(3) / 16.0f)));
    if (!currentLayerValid_ || currentCellX_ < 0) {
        AdvanceLayer();
        if (!currentLayerValid_) return;
        currentCellX_ = rect[0];
        currentCellZ_ = rect[1];
    } else {
        currentCellX_ = std::max(currentCellX_, rect[0]);
        currentCellZ_ = std::max(currentCellZ_, rect[1]);
    }
    const std::uint32_t layer = currentLayer_->layer;
    if (layer > 10 || !IsLayerVisible(layer)) {
        currentCellX_ = -1;
        currentCellZ_ = -1;
        currentLayer_->elapsed = 0.0f;
        return;
    }
    const float clockMultiplier = GetClockMultiplier(static_cast<PopulateLayer>(layer));
    if (std::fabs(clockMultiplier) < 0.001f) {
        currentCellX_ = -1;
        currentCellZ_ = -1;
        currentLayer_->elapsed = 0.0f;
        return;
    }
    bool timedOut = false;
    cRZTimer timer(cRZTimer::kMilliseconds);
    timer.Start();
    float elapsedSeconds = 0.0f;
    if ((layer - 1 < 3) || layer == 7) {
        for (int z = currentCellZ_; z <= rect[3] && !timedOut; ++z) {
            for (int x = currentCellX_; x <= rect[2] && !timedOut; ++x) {
                for (TrafficTileInfo* tile = network->GetFirstTileInfoAtCell(x, z); tile; tile = tile->next)
                    PopulateNetworkOccupant(tile->occupant, tile, x, z, layerUpdates_.front().elapsed);   // i386 0x000cc65a: the FRONT layer's time
                timer.GetElapsedTimeFloat(elapsedSeconds);
                if (elapsedSeconds >= 5.0f) timedOut = true;
                currentCellX_ = x;
            }
            currentCellX_ = rect[0];
            currentCellZ_ = z;
        }
        if (timedOut) return;
    } else if (layer == 8) {
        if (!PopulateCrimeLayer(clockMultiplier)) return;
    } else {
        const double densityMultiplier = GetEdgeDensityMultiplier(static_cast<PopulateLayer>(layer));
        for (int z = currentCellZ_; z <= rect[3] && !timedOut; ++z) {
            for (int x = currentCellX_; x <= rect[2] && !timedOut; ++x) {
                for (TrafficTileInfo* tile = network->GetFirstTileInfoAtCell(x, z); tile; tile = tile->next)
                    PopulateNetworkOccupant(tile->occupant, tile, static_cast<PopulateLayer>(layer), x, z,
                                            clockMultiplier, densityMultiplier);
                timer.GetElapsedTimeFloat(elapsedSeconds);
                if (elapsedSeconds >= 5.0f) timedOut = true;
                currentCellX_ = x;
            }
            currentCellX_ = rect[0];
            currentCellZ_ = z;
        }
        if (timedOut) return;
    }
    currentCellX_ = -1;
    currentCellZ_ = -1;
    currentLayer_->elapsed = 0.0f;
}

void cSC4TrafficAutomataManager::PopulateNetworkOccupant(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                         PopulateLayer layer, int cellX, int cellZ,
                                                         float clockMultiplier, double densityMultiplier) {
    // 0x100113eae PARTIAL: the edge-density refresh and the maybe-spawn sweep are ported; MaybeSpawnAutomaton (the
    // actual spawn) is PARTIAL because it needs the network manager's spawn accumulators (vslots 0x98/0xb8) and the
    // traffic automaton's trip API.
    if (!occupant || !tile) return;
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkManagerApi* network = host ? host->TrafficNetworkManager() : nullptr;
    if (!network) return;
    if ((effectManager_.GetPopulateFlags(cellX, cellZ) & 1) != 0) return;
    const float maxAutos = GetMaxAutosPerSecond(layer);
    const int pathType = GetPathTypeForLayer(layer);
    const float elapsed = currentLayerValid_ ? currentLayer_->elapsed * 0.001f : 0.0f;
    float weights[4][4] = {};
    GetEdgeDensity(occupant, tile, layer, clockMultiplier, weights);
    for (std::uint8_t entry = 0; entry < 4; ++entry) {
        const bool offscreen = IsOffscreenEntryEdge(tile, cellX, cellZ, entry);
        for (std::uint8_t exit = 0; exit < 4; ++exit) {
            float& density = network->OccupantPathDensity(occupant, static_cast<std::uint32_t>(layer),
                                                          static_cast<std::uint32_t>(pathType));
            std::uint16_t& count = network->OccupantPathEdgeCount(
                occupant, static_cast<std::uint32_t>(layer), static_cast<std::uint32_t>(pathType), entry, exit);
            density = (static_cast<float>(count) * elapsed + density) / (elapsed + 1.0f);
            if (weights[entry][exit] <= 0.0f) continue;
            if (occupant->PathConnectionExists(static_cast<std::uint32_t>(pathType), entry, exit) && !offscreen)
                continue;
            double spawn = static_cast<double>(maxAutos);
            const double scaled = static_cast<double>(weights[entry][exit]) * densityMultiplier;
            if (scaled <= static_cast<double>(maxAutos)) spawn = scaled;
            MaybeSpawnAutomaton(occupant, tile, layer, elapsed, spawn, entry, exit);
        }
    }
}

// @ i386 0x000cd290: the car layers (1, 2, 3, 7). For path types 1 and 2 (travel types 1 and 0): the occupant's smoothed automata
// count, the summed edge densities of the layers on that path type, and the spawn-and-recall entry of the occupant's network type:
// below the curve's target (total density / the cell's capacity) an automaton is spawned on a layer and edge picked by weight, far
// above it one is recalled.
//   PARTIAL: the layer-2 clock factor (tuning +0x178) and the player-drive path (vslot 0x34 and tuning +0x170) are not
//   ported. The map test at 0x88 is IsIntersection: no car is spawned on an intersection tile.
namespace {
// SC4_SPAWNDEBUG: the car layers' spawn inputs per network type, summed over 5 s of real time: the summed edge density,
// the cell capacity, the curve's target and the smoothed and current automata counts of the occupants passed.
void TraceSpawnInputs(int networkType, float total, float capacity, float desired, float smoothed, std::uint32_t count,
                      float f0, float f4) {
    static const bool trace = std::getenv("SC4_SPAWNDEBUG") != nullptr;
    if (!trace) return;
    struct Sum { double n = 0, total = 0, capacity = 0, desired = 0, smoothed = 0, count = 0, under = 0; float f0 = 0, f4 = 0; };
    static Sum sums[16];
    static auto last = std::chrono::steady_clock::now();
    Sum& s = sums[networkType & 15];
    s.n += 1; s.total += total; s.capacity += capacity; s.desired += desired; s.smoothed += smoothed; s.count += count;
    if (desired > 0.0f && smoothed / desired < f4) s.under += 1;
    s.f0 = f0; s.f4 = f4;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    for (int t = 0; t < 16; ++t) {
        Sum& v = sums[t];
        if (v.n == 0) continue;
        std::fprintf(stderr, "spawn: net %d passes %.0f total %.3f capacity %.2f desired %.3f smoothed %.3f count %.3f under %.0f%% (f0 %.2f f4 %.2f)\n",
                     t, v.n, v.total / v.n, v.capacity / v.n, v.desired / v.n, v.smoothed / v.n, v.count / v.n, 100.0 * v.under / v.n, v.f0, v.f4);
        v = Sum{};
    }
}
}  // namespace

void cSC4TrafficAutomataManager::PopulateNetworkOccupant(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                         int cellX, int cellZ, float elapsedMilliseconds) {
    static constexpr int kTravelTypeForPathType[3] = {-1, 1, 0};   // kTrafficSimNetworkType, path types 1 and 2
    if (!occupant || !tile) return;
    const std::uint32_t flags = effectManager_.GetPopulateFlags(cellX, cellZ);
    if (flags == 3) return;
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkManagerApi* network = host ? host->TrafficNetworkManager() : nullptr;
    if (!network) return;
    const float w = elapsedMilliseconds * 0.001f;
    for (std::uint32_t pathType = 1; pathType < 3; ++pathType) {
        const int travelType = kTravelTypeForPathType[pathType];
        const int networkType = ChooseNetworkTypeForPathType(occupant, pathType);
        if (travelType < 0 || networkType == -1) continue;
        float& smoothed = network->OccupantPathDensity(occupant, 0, pathType);
        smoothed = (static_cast<float>(network->OccupantAutomataCount(occupant, pathType)) * w + smoothed) / (w + 1.0f);
        struct LayerWeights { PopulateLayer layer; float weights[4][4]; float sum; };
        std::vector<LayerWeights> layers;
        float total = 0.0f;
        for (const tTrafficLayerUpdate& update : layerUpdates_) {
            if (update.layer == 8) continue;
            const auto layer = static_cast<PopulateLayer>(update.layer);
            if (static_cast<std::uint32_t>(GetPathTypeForLayer(layer)) != pathType) continue;
            LayerWeights entry{layer, {}, 0.0f};
            GetEdgeDensity(occupant, tile, layer, GetClockMultiplier(layer), entry.weights);
            for (auto& row : entry.weights)
                for (float v : row) entry.sum += v;
            total += entry.sum;
            layers.push_back(entry);
        }
        if (layers.empty() || !trafficSimulator_) continue;
        const float capacity = trafficSimulator_->GetCapacity(travelType, cellX, cellZ);
        if (capacity == 0.0f) continue;
        auto& params = AutomataTuning().spawnRecall[networkType];
        const float desired = params.curve.GetOutputValue(total / capacity);
        TraceSpawnInputs(networkType, total, capacity, desired, smoothed,
                         network->OccupantAutomataCount(occupant, pathType), params.f0, params.f4);
        if (smoothed <= desired || (flags & 2) != 0) {
            if (!(desired > 0.0f && total > 0.01f && (flags & 1) == 0 && smoothed / desired < params.f4 &&
                  !network->OccupantIsIntersection(occupant) &&
                  static_cast<float>(Random().RandomDoubleUniform()) < params.f0))
                continue;
            float pick = static_cast<float>(Random().RandomDoubleRangeUniform(0.009999999776482582, static_cast<double>(total)));
            for (const LayerWeights& entry : layers) {
                pick -= entry.sum;
                if (pick > 0.0f) continue;
                float edge = static_cast<float>(Random().RandomDoubleRangeUniform(0.0, static_cast<double>(entry.sum)));
                bool spawned = false;
                for (std::uint8_t from = 0; from < 4 && !spawned; ++from)
                    for (std::uint8_t to = 0; to < 4 && !spawned; ++to) {
                        edge -= entry.weights[from][to];
                        if (edge <= 0.0f) {
                            SpawnAutomaton(occupant, tile, entry.layer, from, to, entry.weights[from][to]);
                            spawned = true;
                        }
                    }
                break;
            }
        } else if ((desired == 0.0f || params.fc + 1.0f < smoothed / desired) &&
                   static_cast<float>(Random().RandomDoubleUniform()) < params.f8) {
            RecallAutomatonFromNetwork(tile, pathType);
        }
    }
}

// i386 0x000cfa2a / 0x000d0b70's shared body. The entry falls back to the opposite edge, then to any connected entry, then to the
// wildcard path; a random lane of the edge gives the path key. Gated (MaybeSpawnAutomaton) adds the elapsed time to the edge's
// accumulator and spawns only once it reaches the spawn interval (1 / density) and the lane is empty. The automaton starts at
// the path's first point, faded in, on a trip along it.
//   PARTIAL: the "no train within two cells" test of layers 4/5/6/9/10, the camera-follow distance test of SpawnAutomaton, the
//   random priority of layers 7/8 and the cell lifespan of the special groups are not ported; the accumulator is never reset
//   (as in the original's code).
bool cSC4TrafficAutomataManager::SpawnOnEdge(ITrafficNetworkMapOccupant* occupant, PopulateLayer layer, std::uint8_t entry,
                                             std::uint8_t exit, bool gated, float elapsedSeconds, double density) {
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkManagerApi* network = host ? host->TrafficNetworkManager() : nullptr;
    if (!occupant || !network) return false;
    const std::uint32_t pathType = static_cast<std::uint32_t>(GetPathTypeForLayer(layer));
    const std::uint32_t group = GetAutomataGroupForLayer(layer);
    std::uint8_t from = entry;
    if (!occupant->PathConnectionExists(pathType, from, exit)) {
        from = static_cast<std::uint8_t>((exit + 2) & 3);
        if (!occupant->PathConnectionExists(pathType, from, exit)) {
            int found = -1;
            for (std::uint8_t e = 0; e < 4; ++e)
                if (e != exit && occupant->PathConnectionExists(pathType, e, exit)) {
                    found = e;
                    break;
                }
            if (found < 0) {
                cS3DVector3 unused;
                if (!network->PathStart(occupant, (pathType << 24) | 0xff00u | exit, unused)) return false;
                from = 0xff;
            } else {
                from = static_cast<std::uint8_t>(found);
            }
        }
    }
    const int lanes = network->LaneCount(occupant, pathType, from, exit);
    if (lanes <= 0) return false;
    const std::uint32_t lane = lanes >= 2 ? (Random().RandomUint32Uniform(static_cast<std::uint32_t>(lanes)) & 0xffu) : 0u;
    const std::uint32_t key = (pathType << 24) | (lane << 16) | (static_cast<std::uint32_t>(from) << 8) | exit;
    if (gated) {
        if (density <= 0.0) return false;
        float& accumulated = network->SpawnAccumulator(occupant, static_cast<std::uint32_t>(layer), pathType, from, exit);
        accumulated += elapsedSeconds;
        if (network->LaneAutomataCount(occupant, static_cast<std::uint32_t>(layer), key) != 0) return false;
        if (static_cast<double>(accumulated) < 1.0 / density) return false;
    }
    cS3DVector3 start{};
    network->PathStart(occupant, key, start);
    cGZPersistResourceKey model;
    ChooseRandomModel(group, occupant, layer, model);
    if (model.instance == 0) return false;
    IAutomataAutomaton* automaton = CreateAutomaton(group, start, 2, model, nullptr, 0, nullptr);
    ITrafficAutomataAutomaton* traffic = automaton && host ? host->AsTrafficAutomaton(automaton) : nullptr;
    if (!traffic) return false;
    traffic->SetPopulateLayer(static_cast<std::uint32_t>(layer));
    if (!traffic->StartTrip(occupant, key, false)) {
        RemoveAutomaton(automaton, true);
        return false;
    }
    automaton->SetOpacity(0.0f, 0.0f);
    automaton->SetOpacity(1.0f, AutomataTuning().f219b4);
    return true;
}

bool cSC4TrafficAutomataManager::MaybeSpawnAutomaton(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                     PopulateLayer layer, float elapsedSeconds, double density,
                                                     std::uint8_t entry, std::uint8_t exit) {
    // @ i386 0x000cfa2a
    (void)tile;
    return SpawnOnEdge(occupant, layer, entry, exit, true, elapsedSeconds, density);
}

void cSC4TrafficAutomataManager::SpawnAutomaton(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                PopulateLayer layer, std::uint8_t entry, std::uint8_t exit,
                                                float weight) {
    // @ i386 0x000d0b70
    (void)tile;
    (void)weight;
    SpawnOnEdge(occupant, layer, entry, exit, false, 0.0f, 0.0);
}

ITrafficAutomataAutomaton* cSC4TrafficAutomataManager::SpawnOccupantAutomaton(ITrafficNetworkMapOccupant* occupant,
                                                                              int param2, std::uint32_t group,
                                                                              std::uint32_t flags, bool param5,
                                                                              int pathType) {
    // 0x100114fea PARTIAL: needs ITrafficNetworkMapPathInfo slot 0x48 (a random path for the occupant) and the
    // automaton StartTrip slot; the exemplar lookup / creation calls are ready.
    (void)occupant;
    (void)param2;
    (void)group;
    (void)flags;
    (void)param5;
    (void)pathType;
    return nullptr;
}

void cSC4TrafficAutomataManager::RecallAutomatonFromNetwork(TrafficTileInfo* tile, std::uint32_t pathType) {
    // 0x100114c6e
    if (!tile) return;
    std::vector<ITrafficTileAutomaton*> candidates;
    for (ITrafficNetworkMapAutomaton* raw : tile->automata) {
        auto* automaton = static_cast<ITrafficTileAutomaton*>(raw);
        if (automaton->IsRemoved()) continue;
        if (((automaton->GetPathKey() >> 24) & 0xf) != pathType) continue;
        if (!automaton->CheckAutomataFlag(2)) continue;
        if (automaton->CheckAutomataFlag(0x200000)) continue;
        if (automaton->CheckAutomataFlag(0x80)) continue;
        if (automaton->CheckAutomataFlag(0x2000000)) continue;
        if (automaton->GetOpacity() <= 0.95f) continue;
        candidates.push_back(automaton);
    }
    if (candidates.empty()) return;
    const std::uint32_t pick = Random().RandomUint32Uniform(static_cast<std::uint32_t>(candidates.size()));
    candidates[pick]->Remove(false);
}

bool cSC4TrafficAutomataManager::IsOffscreenEntryEdge(TrafficTileInfo* tile, int cellX, int cellZ,
                                                      std::uint8_t entry) {
    // 0x100114162: the four jump-table bodies bump the cell, then the visible-rect and network-flag tests run.
    (void)tile;   // the original does not use its tile argument either
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    ITrafficNetworkManagerApi* network = host ? host->TrafficNetworkManager() : nullptr;
    if (!network) return true;
    int rect[4] = {0, 0, 0, 0};
    network->GetVisibleCellRect(rect);
    switch (entry) {
        case 0: ++cellX; break;
        case 1: ++cellZ; break;
        case 2: ++cellX; break;
        case 3: ++cellZ; break;
        default: break;
    }
    if (cellX < rect[0] || cellZ < rect[1] || cellX > rect[2] || cellZ > rect[3]) return true;
    TrafficTileInfo* info = network->GetTileInfoAtCell(cellX, cellZ);
    if (!info || !info->occupant) return true;
    return !info->occupant->HasNetworkFlag(0x200000);
}

bool cSC4TrafficAutomataManager::PopulateCrimeLayer(float clockMultiplier) {
    // 0x1001154ba PARTIAL: needs SL::spCrimeSim (the crime records + their cell rectangles), which is unrecovered.
    (void)clockMultiplier;
    return true;
}

bool cSC4TrafficAutomataManager::PopulateNetworkOccupantForCrimeLayer(ITrafficNetworkMapOccupant* occupant,
                                                                      TrafficTileInfo* tile, float density,
                                                                      std::uint32_t cellX, std::uint32_t cellZ) {
    // 0x10011574e PARTIAL: ported as far as the effect-manager gate; the rest uses the crime record list (SL::spCrimeSim)
    // and the automaton's path API.
    (void)occupant;
    (void)tile;
    (void)density;
    (void)cellX;
    (void)cellZ;
    return false;
}

// ---- response / creation helpers ---------------------------------------------------------------------------------------
void cSC4TrafficAutomataManager::ChooseRandomModel(std::uint32_t group, ITrafficNetworkMapOccupant* occupant,
                                                   PopulateLayer layer, cGZPersistResourceKey& out) {
    // 0x10011471a
    if (!occupant) return;
    SC4CellRect rect{};
    occupant->AsOccupant()->GetBoundingCityCells(rect);
    tExemplarData* data = ChooseRandomExemplar(group, layer, rect.x0, rect.z0);
    if (data) out = data->modelKey;
}

// ---- automaton registry -------------------------------------------------------------------------------------------------
void cSC4TrafficAutomataManager::RecalculateLeaders(ITrafficAutomataAutomaton* automaton) {
    // 0x100115ac0: every automaton that followed `automaton` re-runs its FindLeader.
    if (!automaton) return;
    for (IAutomataAutomaton* candidate : Automata()) {
        ITrafficAutomataAutomaton* traffic = TrafficHostOrGlobal() ? TrafficHostOrGlobal()->AsTrafficAutomaton(candidate)
                                                                   : nullptr;
        if (!traffic) continue;
        if (traffic->Leader() != automaton) continue;
        traffic->SetLeader(automaton->FindLeader());
    }
}

bool cSC4TrafficAutomataManager::RemoveAutomaton(IAutomataAutomaton* automaton, bool force) {
    // 0x1001159c4
    const bool result = cSC4AutomataManager::RemoveAutomaton(automaton, force);
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    if (host) RecalculateLeaders(host->AsTrafficAutomaton(automaton));
    return result;
}

bool cSC4TrafficAutomataManager::IsAutomatonOnscreen(IAutomataAutomaton* automaton) {
    // 0x1001159fc: the base's +0x24 view rect expanded by 48. The original compares x and z only.
    if (!automaton) return false;
    static const bool keepAll = std::getenv("SC4_AUTOMATANOCULL") != nullptr;   // debug: keep every automaton
    if (keepAll) return true;
    const float minX = ViewRect(0), minZ = ViewRect(1), maxX = ViewRect(2), maxZ = ViewRect(3);
    float position[3] = {0.0f, 0.0f, 0.0f};
    automaton->GetPosition(position);
    return position[0] <= maxX + 48.0f && position[2] <= maxZ + 48.0f && minZ - 48.0f <= position[2] &&
           minX - 48.0f <= position[0];
}

// ---- effect manager ----------------------------------------------------------------------------------------------------
void cSC4TrafficAutomataManager::CreateAccident(cISC4Occupant* occupant, int type, std::uint32_t duration) {
    // 0x100113b98
    effectManager_.CreateAccident(occupant, type, duration);
}

// ---- misc traffic slot -------------------------------------------------------------------------------------------------
bool cSC4TrafficAutomataManager::GetDispatchDestinationPoint(ITrafficAutomataAutomaton* automaton, cS3DVector3& out) {
    // 0x100115ea6 PARTIAL: needs the automaton's route deque (+0x388/+0x390/+0x370) and cISC4PathInfo slot 0xa8.
    (void)automaton;
    (void)out;
    return false;
}

// ---- base delegates ----------------------------------------------------------------------------------------------------
void cSC4TrafficAutomataManager::DoAnimationTick(std::uint32_t data, float deltaMilliseconds) {
    // 0x100112d88: age the effects first, then the visible-area population and the response list while active.
    effectManager_.Update();
    if (MaxCount() > 0 && !cSC4AutomataManager::SimSpeedPaused() && !IsPaused()) {
        if (IsActive()) PopulateVisibleArea(deltaMilliseconds);
        if (!responses_.empty()) UpdateResponseList(deltaMilliseconds);
    }
    cSC4AutomataManager::DoAnimationTick(data, deltaMilliseconds);
}

void cSC4TrafficAutomataManager::ScanExemplars() {
    // 0x10011606e: no traffic-specific behaviour.
    cSC4AutomataManager::ScanExemplars();
}

void cSC4TrafficAutomataManager::HandlePostCityInit() {
    // 0x100114e88 PARTIAL: the base work runs; the per-automaton cached occupant / path info / leader refresh needs the
    // cSC4TrafficAutomaton fields +0x310/+0x318/+0x328/+0xf0, which the recovered traffic automaton keeps private (no
    // setters on its public surface).
    cSC4AutomataManager::HandlePostCityInit();
}

// ---- persistence -------------------------------------------------------------------------------------------------------
bool cSC4TrafficAutomataManager::Write(cIGZOStream& stream) const {
    // 0x100116078: stream version 3, then the base record, the default path type, the current layer (0 when none) and
    // the effect list.
    stream.SetUint16(3);
    if (!cSC4AutomataManager::Write(stream)) return false;
    stream.SetUint32(defaultPathType_);
    const std::uint32_t layer = currentLayerValid_ ? currentLayer_->layer : 0;
    stream.SetUint32(layer);
    return effectManager_.Write(stream);
}

bool cSC4TrafficAutomataManager::Read(cIGZIStream& stream) {
    // 0x1001161b4: versions 2 and 3 only; the first u32 is read and *dropped* (the default path type is not restored,
    // 0x1001161f7), then the current layer selects the list node. Version 3 also reads the effect list.
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if ((version & 0xfffe) != 2) return false;
    if (!cSC4AutomataManager::Read(stream)) return false;
    std::uint32_t discarded = 0;
    stream.GetUint32(discarded);
    currentLayer_ = layerUpdates_.begin();
    currentLayerValid_ = !layerUpdates_.empty();
    std::uint32_t layer = 0;
    stream.GetUint32(layer);
    if (layer != 0 && currentLayerValid_) {
        while (currentLayer_ != layerUpdates_.end() && currentLayer_->layer != layer) ++currentLayer_;
        if (currentLayer_ == layerUpdates_.end()) currentLayer_ = layerUpdates_.begin();
    }
    if (version > 2) return effectManager_.Read(stream);
    return true;
}

// ---- cSC4VehicleManager defaults and the effect-host / test plumbing ------------------------------------------------
void cSC4TrafficAutomataManager::SetDefaultTypes(std::uint32_t pathType, std::uint32_t networkType) {
    defaultPathType_ = pathType;
    defaultNetworkType_ = networkType;
}

void cSC4TrafficAutomataManager::SetCrimeLayerMultiplier(float multiplier) { crimeLayerMultiplier_ = multiplier; }

void cSC4TrafficAutomataManager::AddLayer(std::uint32_t layer) {
    tTrafficLayerUpdate update;
    update.layer = layer;
    update.elapsed = 0.0f;
    layerUpdates_.push_back(update);
    if (!currentLayerValid_) {
        currentLayer_ = layerUpdates_.begin();
        currentLayerValid_ = true;
    }
}

const std::uint32_t* cSC4TrafficAutomataManager::VehicleLayerOrder() { return kVehicleLayerOrder; }
const std::uint32_t* cSC4TrafficAutomataManager::PathTypeTable() { return kPathTypeTable; }
const std::uint32_t* cSC4TrafficAutomataManager::AutomataGroupTable() { return kAutomataGroupTable; }

int cSC4TrafficAutomataManager::FloorIdiom(float value) { return FloorIdiomImpl(value); }

ITrafficAutomataManagerHost* cSC4TrafficAutomataManager::TrafficHostOrGlobal() const {
    if (trafficHost_) return trafficHost_;
    return dynamic_cast<ITrafficAutomataManagerHost*>(AutomataManagerHost());
}

std::uint32_t cSC4TrafficAutomataManager::ElapsedMilliseconds() { return timer_.GetElapsedTimeU32(); }

void cSC4TrafficAutomataManager::PositionToCell(float x, float z, int& cellX, int& cellZ) {
    if (ITrafficAutomataManagerHost* host = TrafficHostOrGlobal()) host->WorldToCell(x, z, cellX, cellZ);
}

bool cSC4TrafficAutomataManager::AttachAccidentController(cISC4Occupant* occupant, const float* position) {
    (void)occupant;
    (void)position;
    // PARTIAL: the traffic-accident controller service (city vslot 0x178, group "kTrafficAccidentControllerGroupName")
    // is unrecovered; no seam carries it yet.
    return false;
}

void cSC4TrafficAutomataManager::DetachController(cISC4Occupant* occupant) {
    (void)occupant;
    // PARTIAL: same service as AttachAccidentController.
}

}  // namespace sc4
