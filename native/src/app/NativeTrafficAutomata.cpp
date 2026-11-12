// The traffic automata's city services (sc4::ITrafficAutomatonHost: the traffic network map, the city's cells, their automata
// manager, saved network occupant references) and the car's view of its automata manager (ITrafficAutomataManager over
// cSC4TrafficAutomataManager). In the original the car calls its manager directly.
#include <algorithm>
#include <map>
#include <memory>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "sc4/AutomataManager.h"
#include "sc4/DBSegment.h"
#include "sc4/Pedestrian.h"
#include "sc4/PedestrianManager.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/SpecialVehicles.h"
#include "sc4/Train.h"
#include "sc4/PathInfo.h"
#include "sc4/Vehicle.h"
#include "sc4/VehicleManager.h"
#include "sc4/cSC4City.h"

namespace sc4 { IVehicleHost* NewStubIVehicleHost(); }   // app/hosts/IfaceIVehicleHost.cpp

namespace native {
// app/NativeCrime.cpp: the crime simulator's incident list as the crime layer reads it.
bool NativeCrimeIncidents(std::vector<sc4::ITrafficNetworkManagerApi::CrimeIncidentCell>& out);
void NativeSetCrimeVisualsCreated(std::uint32_t index, bool created);
}
namespace native {
sc4::ITrafficAutomatonOccupant* NativeTrafficPiece(sc4::cISC4Occupant* occupant);   // app/NativeTrafficNetworkMap.cpp
sc4::ITrafficNetworkMapLot* NativeTrafficLotAt(int cellX, int cellZ);                 // app/NativeTrafficNetworkMap.cpp
void NativeOccupantsInBox(const float* lo, const float* hi, std::uint32_t type, int count, std::vector<sc4::cISC4Occupant*>& out);
int NativeZipperPieceType(sc4::ITrafficAutomatonOccupant* occupant);                   // app/NativeTrafficNetworkMap.cpp
bool NativeLotBuildingInGroup(void* lot, std::uint32_t group);
void NativePostCityMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2);
bool NativeDisasterOngoing();                                                         // app/NativeRiot.cpp
bool NativeProjectToViewport(const float* xyz, float out[3]);                         // app/NativeAutomataManagerHost.cpp

namespace {
using namespace sc4;

// The car's view of its automata manager (the slots cSC4TrafficAutomaton calls).
class ManagerFace final : public ITrafficAutomataManager {
public:
    explicit ManagerFace(cSC4TrafficAutomataManager* manager) : manager_(manager) {}
    bool IsTrafficAutomaton(cSC4Automaton* automaton) const override { return manager_->IsTrafficAutomaton(AutomatonView(automaton)); }
    void SetCurrentLayer(std::uint32_t layer) override { layer_ = layer; }
    // The original reads the layer's clock (vslot 0x180) and asks for the edge density (vslot 0x270) with it.
    void ComputeEdgeWeights(ITrafficAutomatonOccupant* occupant, TrafficTileInfo* tile, std::uint32_t layer, float* weights) override {
        const auto populate = static_cast<PopulateLayer>(layer);
        manager_->GetEdgeDensity(occupant, tile, populate, manager_->GetClockMultiplier(populate), reinterpret_cast<float(*)[4]>(weights));
    }
    std::uint32_t GetTravelType(cSC4Automaton* automaton) override {   // vslot 0x250
        return std::uint32_t(manager_->GetDefaultPathType(TrafficAutomatonView(AutomatonView(automaton))));
    }
    double GetLayerDensity(std::uint32_t layer) override { return manager_->GetEdgeDensityMultiplier(static_cast<PopulateLayer>(layer)); }
    cSC4TrafficEffectManager* EffectManager() override { return &manager_->EffectManager(); }
    cSC4Automaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags, std::uint8_t priority) override {
        IAutomataAutomaton* made = manager_->CreateAutomaton(group, position, flags, cGZPersistResourceKey{}, nullptr, priority, nullptr);
        return made ? AutomatonOf(made) : nullptr;
    }
private:
    cSC4TrafficAutomataManager* manager_;
    std::uint32_t layer_ = 0;
};

template <class T> T* Component(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    return object ? dynamic_cast<T*>(object) : nullptr;
}

// One host per automata manager kind; Base is the automaton class's host interface (the pedestrian's adds four services).
template <class Base> class TrafficHostOf : public Base {
public:
    explicit TrafficHostOf(CityComponent manager) : manager_(manager) {}
    cSC4TrafficNetworkMap* TrafficNetworkMap() override { return Component<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork); }
    float CitySizeX() override { cSC4City* c = HostCity(); return c ? float(c->CellCountX()) * 16.0f : 0.0f; }
    float CitySizeZ() override { cSC4City* c = HostCity(); return c ? float(c->CellCountZ()) * 16.0f : 0.0f; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = int(x / 16.0f);
        cellZ = int(z / 16.0f);
    }
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    ITrafficAutomataManager* TrafficAutomataManager() override {
        auto* manager = Component<cSC4TrafficAutomataManager>(manager_);
        if (!manager) return nullptr;
        auto& face = faces_[manager];
        if (!face) face = std::make_unique<ManagerFace>(manager);
        return face.get();
    }
    cISC4PathFinder* PathFinder() override { return nullptr; }   // PARTIAL: MySim dispatch routes (the vehicles use the manager's)
    void NeighborConnectionCells(std::vector<std::int32_t>&) override {}   // PARTIAL: neighbor connections as exits
    void StartAudioEvent(std::uint32_t) override {}
    void StopAudioEvent(std::uint32_t) override {}
    ITrafficParticleSystem* CreateParticleSystem() override { return nullptr; }   // PARTIAL: exhaust / dust particles
    // A saved car's network occupant reference, as the map's view of the piece (one reference).
    ITrafficAutomatonOccupant* ReadNetworkOccupant(cIGZIStream& stream) override {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, false);
        if (!raw) return nullptr;
        auto* object = static_cast<cIGZUnknown*>(raw);
        void* occupant = nullptr;
        ITrafficAutomatonOccupant* piece = nullptr;
        if (object->QueryInterface(0x26bb767e, &occupant) && occupant) {   // cISC4Occupant
            piece = NativeTrafficPiece(static_cast<cISC4Occupant*>(occupant));
            static_cast<cISC4Occupant*>(occupant)->Release();
        }
        object->Release();
        return piece;
    }
    // The automata managers' tuning (AutomataTuning(), filled by ReadTuningParameters from the tuning exemplar).
    const TrafficAutomatonTuning* Tuning() override {
        const AutomataTuningGlobals& t = AutomataTuning();
        tuning_.desiredLookaheadCount = t.f21a64;   // 0x100b21a64
        tuning_.lookaheadPenalty = t.f21a68;        // 0x100b21a68
        tuning_.junctionDuration = t.f219a4;        // 0x100b219a4
        tuning_.fadeDuration = t.f219b4;            // 0x100b219b4
        tuning_.dispatchDistance = t.f219f4;        // 0x100b219f4
        return &tuning_;
    }
private:
    CityComponent manager_;
    TrafficAutomatonTuning tuning_;
    std::map<cSC4TrafficAutomataManager*, std::unique_ptr<ManagerFace>> faces_;
};

// cISC4TrafficSimulator as the traffic automata managers read it: a cell's 4x4 edge counts (GetTrafficEdgeDensity's last table;
// the evening direction for a negative clock).
class SimulatorApi final : public ITrafficSimulatorApi {
public:
    std::uint32_t AddRef() override { return 2; }   // the city keeps the simulator
    std::uint32_t Release() override { return 1; }
    const std::uint16_t* GetEdgeCountTable(ITrafficNetworkMapOccupant*, unsigned cellX, unsigned cellZ, unsigned travelType, bool evening) override {
        auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
        return simulator ? simulator->GetTrafficEdgeDensity(int(cellX), int(cellZ), int(travelType), evening).last.data() : nullptr;
    }
    float GetCapacity(int simNetworkType, int cellX, int cellZ) override {
        auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
        return simulator ? simulator->GetCapacity(simNetworkType, cellX, cellZ) : 0.0f;
    }
    cISC4PathFinder* CreatePathFinder() override {
        auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
        return simulator ? simulator->CreatePathFinder() : nullptr;
    }
} g_simulatorApi;

// The traffic network map as the traffic automata managers' population pass reads it, with the per-occupant accumulators.
class NetworkApi final : public ITrafficNetworkManagerApi {
public:
    TrafficTileInfo* GetTileInfoAtCell(int x, int z) override { cSC4TrafficNetworkMap* m = Map(); return m ? m->SafeGetTileInfoAtCell(x, z) : nullptr; }
    TrafficTileInfo* GetFirstTileInfoAtCell(int x, int z) override { return GetTileInfoAtCell(x, z); }
    void GetVisibleCellRect(int rect[4]) override {   // the managers' view rectangle in cells (set by NativeAutomataManagerHost)
        for (int i = 0; i < 4; ++i) rect[i] = visible_[i];
    }
    bool GetResponseOccupants(cISC4Occupant*, int, int, std::uint32_t, std::vector<ITrafficNetworkMapOccupant*>&) override { return false; }
    bool ResponseAllowed(ITrafficNetworkMapOccupant*) override { return false; }
    float& OccupantPathDensity(ITrafficNetworkMapOccupant* o, std::uint32_t layer, std::uint32_t type) override {
        return densities_[{o, (layer << 8) | type}];
    }
    std::uint16_t& OccupantPathEdgeCount(ITrafficNetworkMapOccupant* o, std::uint32_t layer, std::uint32_t type, std::uint8_t entry,
                                         std::uint8_t exit) override {
        return counts_[{o, (layer << 24) | (type << 16) | (std::uint32_t(entry) << 8) | exit}];
    }
    float& SpawnAccumulator(ITrafficNetworkMapOccupant* o, std::uint32_t layer, std::uint32_t type, std::uint8_t entry, std::uint8_t exit) override {
        return accumulators_[{o, (layer << 24) | (type << 16) | (std::uint32_t(entry) << 8) | exit}];
    }
    std::uint32_t LaneAutomataCount(ITrafficNetworkMapOccupant* o, std::uint32_t layer, std::uint32_t key) override {
        cSC4TrafficNetworkMap* m = Map();
        std::uint32_t* count = m ? m->GetAutomataCount(o, layer, key) : nullptr;
        return count ? *count : 0;
    }
    std::uint32_t OccupantAutomataCount(ITrafficNetworkMapOccupant* o, std::uint32_t pathType) override {
        cSC4TrafficNetworkMap* m = Map();
        std::uint32_t* count = m ? m->GetAutomataCount(o, pathType) : nullptr;
        return count ? *count : 0;
    }
    bool OccupantIsIntersection(ITrafficNetworkMapOccupant* o) override {
        cSC4TrafficNetworkMap* m = Map();
        return m && m->IsIntersection(o);
    }
    int LaneCount(ITrafficNetworkMapOccupant* o, std::uint32_t type, std::uint8_t entry, std::uint8_t exit) override {
        cISC4PathInfo* info = Info(o);
        return info ? info->GetLaneCount(type, entry, exit) : 0;
    }
    bool PathStart(ITrafficNetworkMapOccupant* o, std::uint32_t key, cS3DVector3& out) override {
        cISC4PathInfo* info = Info(o);
        const tPath* path = info ? info->GetPath(key, nullptr) : nullptr;
        if (!path || path->points.empty()) return false;
        out = path->points.front();
        return true;
    }
    bool RandomPathPoint(ITrafficNetworkMapOccupant* o, std::uint32_t type, cRZRandomSuper* random, std::uint32_t& key, cS3DVector3& out) override {
        cISC4PathInfo* info = Info(o);
        return info && info->GetRandomPathWildcard(type, 0xff, 0xff, random, &key, nullptr) && info->GetRandomPathPoint(key, out);
    }
    bool CrimeIncidents(std::vector<CrimeIncidentCell>& out) override { return NativeCrimeIncidents(out); }
    void SetCrimeVisualsCreated(std::uint32_t index, bool created) override { NativeSetCrimeVisualsCreated(index, created); }
    void SetVisibleCells(int x0, int z0, int x1, int z1) { visible_[0] = x0; visible_[1] = z0; visible_[2] = x1; visible_[3] = z1; }
    void Clear() { densities_.clear(); counts_.clear(); accumulators_.clear(); }
private:
    static cSC4TrafficNetworkMap* Map() { return Component<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork); }
    static cISC4PathInfo* Info(ITrafficNetworkMapOccupant* o) {
        auto* piece = dynamic_cast<ITrafficAutomatonOccupant*>(o);
        return piece ? piece->GetPathInfoObject() : nullptr;
    }
    using Key = std::pair<ITrafficNetworkMapOccupant*, std::uint32_t>;
    std::map<Key, float> densities_, accumulators_;
    std::map<Key, std::uint16_t> counts_;
    int visible_[4] = {0, 0, -1, -1};
} g_networkApi;

// The pedestrians inside a box, from their manager's list (the native automata are not in the occupant manager); at most
// `count` (a negative count: all of them), one reference each for the caller.
void PedestriansInBox(const float* lo, const float* hi, int count, std::vector<cISC4Occupant*>& out) {
    auto* manager = Component<cSC4AutomataManager>(CityComponent::PedestrianManager);
    if (!manager) return;
    for (IAutomataAutomaton* view : manager->Automata()) {
        if (count >= 0 && static_cast<int>(out.size()) >= count) break;
        auto* pedestrian = view ? dynamic_cast<cSC4Pedestrian*>(AutomatonOf(view)) : nullptr;
        if (!pedestrian) continue;
        float p[3] = {0.0f, 0.0f, 0.0f};
        pedestrian->GetPosition(p);
        if (p[0] < lo[0] || p[0] > hi[0] || p[1] < lo[1] || p[1] > hi[1] || p[2] < lo[2] || p[2] > hi[2]) continue;
        pedestrian->AddRef();
        out.push_back(pedestrian->AsOccupant());
    }
}

// The vehicles' city services (sc4::IVehicleHost). The tuning comes from the automata managers' globals (AutomataTuning(), the
// original's sTuningParams at 0x100b21880: +0x04/+0x08 speed range, +0x1ac/+0x1b0 path offsets, +0x158 network speeds,
// +0x138 curve factor, +0x1b4/+0x1b8 turn limits). The services the native game does not have yet (player drive, accidents,
// effects, disasters) answer as the stub does.
class VehicleHost final : public IVehicleHost {
public:
    const VehicleTuning* Tuning() override {
        const AutomataTuningGlobals& t = AutomataTuning();
        tuning_.defaultSpeedMin = static_cast<std::int32_t>(t.f21884);
        tuning_.defaultSpeedMax = static_cast<std::int32_t>(t.f21888);
        tuning_.pathOffsetMin = t.f21a2c;
        tuning_.pathOffsetMax = t.f21a30;
        for (std::size_t i = 0; i < 13; ++i) tuning_.networkSpeed[i] = i < t.f219d8.size() ? t.f219d8[i] : 1.0f;
        tuning_.pathCurveFactor = t.f219b8;
        tuning_.turnDivisor = t.f21a34;
        tuning_.turnMax = t.f21a38;
        tuning_.freightCarsMin = t.f21a42;
        tuning_.freightCarsMax = t.f21a43;
        tuning_.passengerCarsMin = t.f21a44;
        tuning_.passengerCarsMax = t.f21a45;
        tuning_.trainSpeedMin = t.f21a48;
        tuning_.trainSpeedMax = t.f21a4c;
        static bool printed = false;
        if (!printed && tuning_.defaultSpeedMax > 0 && std::getenv("SC4_TRAFFICSPEED")) {   // the tuning the vehicles drive by, once
            printed = true;
            std::fprintf(stderr, "traffic tuning: speed %d..%d, curve factor %.3f, turn %.3f / %.3f, sim speed x%.2f x%.2f x%.2f (drive x%.2f), network:",
                         tuning_.defaultSpeedMin, tuning_.defaultSpeedMax, tuning_.pathCurveFactor, tuning_.turnDivisor, tuning_.turnMax, t.f21a54,
                         t.f21a58, t.f21a5c, t.f21a60);
            for (float v : t.f219d8) std::fprintf(stderr, " %.2f", v);
            std::fprintf(stderr, "\n");
        }
        return &tuning_;
    }
    // cSC4VehicleManager::sbDefaultCollide (cars follow their leaders) / sbDefaultHeadlights (off until the cheat).
    bool DefaultCollide() override { return true; }
    bool DefaultHeadlights() override { return false; }
    bool SceneLightsOn() override { return false; }   // PARTIAL: the lighting manager's night state
    void ManagerRecalculateLeaders(cSC4Automaton* automaton) override {
        auto* manager = Component<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
        if (manager && automaton) manager->RecalculateLeaders(TrafficAutomatonView(AutomatonView(automaton)));
    }
    bool IsOccupantGroup(cISC4Occupant* occupant, std::uint32_t group) override { return occupant && occupant->IsOccupantGroup(group); }
    bool FindPlayerDriveParams(cSC4Automaton* a, const tAutomataPlayerDriveParams** out) override { return stub()->FindPlayerDriveParams(a, out); }
    int CellAutomatonCount(int travelType, int cellX, int cellZ) override { return stub()->CellAutomatonCount(travelType, cellX, cellZ); }
    void OccupantsInCell(int cellX, int cellZ, std::vector<ITrafficAutomatonOccupant*>& out) override { stub()->OccupantsInCell(cellX, cellZ, out); }
    bool CanDriveOnCell(int cellX, int cellZ, float x, float z) override { return stub()->CanDriveOnCell(cellX, cellZ, x, z); }
    bool IsInsideCity(float x, float z) override {
        cSC4City* c = HostCity();
        return c && x >= 0.0f && z >= 0.0f && x < float(c->CellCountX()) * 16.0f && z < float(c->CellCountZ()) * 16.0f;
    }
    float SurfaceHeight(float x, float z) override { return stub()->SurfaceHeight(x, z); }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override { x = float(cellX) * 16.0f; z = float(cellZ) * 16.0f; }
    bool LotBoundingRectAtCell(int cellX, int cellZ, std::int32_t rect[4]) override { return stub()->LotBoundingRectAtCell(cellX, cellZ, rect); }
    IVehicleEffect* CreateNamedEffect(const char* name) override { return stub()->CreateNamedEffect(name); }
    cIGZUnknown* CreateAccident(cISC4Occupant* o, std::int32_t type, std::uint32_t duration) override { return stub()->CreateAccident(o, type, duration); }
    // The pedestrians of a box come from their manager (the native automata are not in the occupant manager); the references
    // go to the caller. Other types: PARTIAL (the player-drive collision check asks for vehicles).
    void OccupantsInBox(const float* lo, const float* hi, std::uint32_t type, int count, std::vector<cISC4Occupant*>& out) override {
        if (type == kPedestrianOccupantType) PedestriansInBox(lo, hi, count, out);
        else stub()->OccupantsInBox(lo, hi, type, count, out);
    }
    bool DisasterOngoing() override { return NativeDisasterOngoing(); }
    bool CheckRailAccident(int cellX, int cellZ) override {
        auto* sim = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
        return sim && sim->CheckRailAccident(cellX, cellZ);
    }
    void PostRailAccidentMessage(int cellX, int cellZ) override { NativePostCityMessage(0x0c05e4cd, std::uint32_t(cellX), std::uint32_t(cellZ)); }
    int ZipperPieceType(ITrafficAutomatonOccupant* occupant) override { return NativeZipperPieceType(occupant); }
    bool IsLotAtCell(int cellX, int cellZ) override { return NativeTrafficLotAt(cellX, cellZ) != nullptr; }
    bool IsLotBuildingInGroup(void* lot, std::uint32_t group) override { return NativeLotBuildingInGroup(lot, group); }
    float PlayerDriveDisasterDamage(float x, float z) override { return stub()->PlayerDriveDisasterDamage(x, z); }
    cSC4Automaton* CollidingAutomaton(cSC4Automaton* automaton, float distance) override {
        auto* manager = Component<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
        IAutomataAutomaton* hit = manager && automaton ? manager->GetCollidingAutomaton(AutomatonView(automaton), distance) : nullptr;
        return hit ? AutomatonOf(hit) : nullptr;
    }
    void PostCollisionMessage() override {}
private:
    static IVehicleHost* stub() { return NewStubIVehicleHost(); }
    VehicleTuning tuning_;
};

// The pedestrians' host: the traffic services on the pedestrian manager plus the four the pedestrian reaches itself.
class PedestrianHost final : public TrafficHostOf<IPedestrianHost> {
public:
    PedestrianHost() : TrafficHostOf<IPedestrianHost>(CityComponent::PedestrianManager) {}
    // The occupant manager's box query (slot 0x120); the references go to the caller. The native automata are not in the
    // occupant manager, so the pedestrians of a box (the only type the pedestrian asks for) come from their manager's list.
    void OccupantsInBox(const float* lo, const float* hi, std::uint32_t type, int count, std::vector<cISC4Occupant*>& out) override {
        if (type != kPedestrianOccupantType) {
            NativeOccupantsInBox(lo, hi, type, count, out);
            return;
        }
        PedestriansInBox(lo, hi, count, out);
    }
    void IgniteOccupant(cISC4Occupant*) override {}   // PARTIAL: the fire disaster manager (arsonists)
    ITrafficNetworkMapLot* LotAt(int cellX, int cellZ) override { return NativeTrafficLotAt(cellX, cellZ); }
    const PedestrianTuning* PedestrianTuningTable() override {
        const AutomataTuningGlobals& t = AutomataTuning();
        pedestrianTuning_.speedMin = static_cast<std::int32_t>(t.f21898);   // 0x100b21898
        pedestrianTuning_.speedMax = static_cast<std::int32_t>(t.f2189c);
        pedestrianTuning_.pathOffsetMin = t.f21a24;                         // 0x100b21a24
        pedestrianTuning_.pathOffsetMax = t.f21a28;
        pedestrianTuning_.oneShotAnimMin = t.f21a40;                        // 0x100b21a40 (property 0x6a09f630)
        pedestrianTuning_.oneShotAnimMax = t.f21a41;
        return &pedestrianTuning_;
    }
private:
    PedestrianTuning pedestrianTuning_;
};

TrafficHostOf<ITrafficAutomatonHost> g_vehicles(CityComponent::VehicleManager);
VehicleHost g_vehicleHost;
PedestrianHost g_pedestrians;

}  // namespace

sc4::ITrafficAutomatonHost* NativeVehicleTrafficHost() { return &g_vehicles; }
sc4::IVehicleHost* NativeVehicleHost() { return &g_vehicleHost; }
sc4::ITrafficSimulatorApi* NativeTrafficSimulatorApi() { return &g_simulatorApi; }
sc4::ITrafficNetworkManagerApi* NativeTrafficNetworkApi() { return &g_networkApi; }
void NativeSetTrafficVisibleCells(int x0, int z0, int x1, int z1) { g_networkApi.SetVisibleCells(x0, z0, x1, z1); }
void NativeClearTrafficNetworkApi() { g_networkApi.Clear(); }
sc4::IPedestrianHost* NativePedestrianHost() { return &g_pedestrians; }

// Kit command `trains [N]`: where the city has track (network types 1 rail, 8 elevated rail, 9 monorail, 7 subway), the
// vehicle manager's trains and buses, and the first N engines with their cars.
void NativeTrainReport(int limit) {
    auto* map = Component<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
    cSC4City* city = HostCity();
    if (map && city) {
        const std::uint32_t kinds[4] = {1, 8, 9, 7};
        const char* names[4] = {"rail", "elevated", "monorail", "subway"};
        for (int k = 0; k < 4; ++k) {
            int count = 0, x0 = 1 << 30, z0 = 1 << 30, x1 = -1, z1 = -1, firstX = -1, firstZ = -1;
            for (int z = 0; z < city->CellCountZ(); ++z)
                for (int x = 0; x < city->CellCountX(); ++x) {
                    ITrafficNetworkMapOccupant* piece = map->FindNetworkOccupant(x, z);
                    if (!piece || !piece->IsOfType(kinds[k])) continue;
                    if (count++ == 0) { firstX = x; firstZ = z; }
                    x0 = std::min(x0, x); z0 = std::min(z0, z); x1 = std::max(x1, x); z1 = std::max(z1, z);
                }
            if (count) std::printf("trains: %d %s cells, %d,%d..%d,%d, first %d,%d\n", count, names[k], x0, z0, x1, z1, firstX, firstZ);
            else std::printf("trains: no %s cells\n", names[k]);
        }
    }
    // the traffic simulator's edge volumes per travel type (both directions), summed over the city: what the layers spawn from
    if (auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator); simulator && city) {
        std::printf("trains: edge volume by travel type:");
        for (int type = 0; type < 9; ++type) {
            long volume = 0, cells = 0;
            for (int z = 0; z < city->CellCountZ(); ++z)
                for (int x = 0; x < city->CellCountX(); ++x)
                    for (int evening = 0; evening < 2; ++evening) {
                        long sum = 0;
                        for (std::uint16_t v : simulator->GetTrafficEdgeDensity(x, z, type, evening != 0).last) sum += v;
                        volume += sum;
                        if (sum) ++cells;
                    }
            std::printf(" %d: %ld in %ld cells;", type, volume, cells);
        }
        std::printf("\n");
    }
    auto* manager = Component<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
    if (!manager) { std::printf("trains: no manager\n"); return; }
    int engines = 0, transit = 0, loose = 0, cars = 0, buses = 0, shown = 0;
    for (IAutomataAutomaton* view : manager->Automata()) {
        cSC4Automaton* automaton = view ? AutomatonOf(view) : nullptr;
        if (!automaton) continue;
        if (dynamic_cast<cSC4Bus*>(automaton)) { ++buses; continue; }
        auto* engine = dynamic_cast<cSC4Train*>(automaton);
        if (!engine) {
            if (dynamic_cast<cSC4TrainCar*>(automaton)) ++loose;
            continue;
        }
        ++engines;
        if (dynamic_cast<cSC4MassTransitTrain*>(engine)) ++transit;
        cars += static_cast<int>(engine->GetNumCars());
        if (shown >= limit) continue;
        ++shown;
        float p[3] = {0, 0, 0}, s[3] = {0, 0, 0};
        engine->GetPosition(p);
        NativeProjectToViewport(p, s);
        std::printf("trains: engine group %04x on screen (%.0f, %.0f) at (%.1f, %.1f, %.1f) heading (%.2f, %.2f) nose (%.1f, %.1f) back (%.1f, %.1f) radius %.1f opacity %.2f speed %.1f behavior %x flags %08x model %p pulls %u of %u (car group %04x)",
                    engine->AutomataGroupId(), s[0], s[1], p[0], p[1], p[2], engine->Orientation().x, engine->Orientation().z, engine->GetPathPos().x, engine->GetPathPos().z,
                    engine->BackPoint().x, engine->BackPoint().z, engine->GetRadius(), engine->GetOpacity(), engine->GetCurrentSpeed() * 3600.0f,
                    engine->GetBehavior(), engine->AutomataFlags(), static_cast<void*>(engine->GetModelInstance()), engine->GetNumCars(),
                    static_cast<unsigned>(engine->GetDesiredNumCars()), engine->CarGroup());
        for (cSC4TrainCar* car : engine->Cars()) {
            float c[3] = {0, 0, 0}, cs[3] = {0, 0, 0};
            car->GetPosition(c);
            NativeProjectToViewport(c, cs);
            std::printf("; car %04x on screen (%.0f, %.0f) at (%.1f, %.1f, %.1f) radius %.1f opacity %.2f model %p", car->AutomataGroupId(), cs[0], cs[1], c[0], c[1], c[2], car->GetRadius(),
                        car->GetOpacity(), static_cast<void*>(car->GetModelInstance()));
        }
        std::printf("\n");
    }
    std::printf("trains: %d engines (%d mass transit), %d cars in trains, %d cars in the manager, %d buses\n", engines, transit, cars, loose, buses);
}

}  // namespace native
