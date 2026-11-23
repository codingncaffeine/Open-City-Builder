// The traffic automata's city services (sc4::ITrafficAutomatonHost: the traffic network map, the city's cells, their automata
// manager, saved network occupant references) and the car's view of its automata manager (ITrafficAutomataManager over
// cSC4TrafficAutomataManager). In the original the car calls its manager directly.
#include <algorithm>
#include "platform/Unwired.h"
#include <map>
#include <memory>

#include "app/NativeLighting.h"
#include "app/NativeAutomataViews.h"
#include "app/NativeDemolition.h"   // CityNeighborsSimulator
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"
#include "app/NativeCity.h"
#include "sc4/LightingManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/DBSegment.h"
#include "sc4/Pedestrian.h"
#include "sc4/PedestrianManager.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/ServiceLocator.h"
#include "sc4audio/AEMSEvents.h"
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
sc4::ITrafficSimulatorApi* NativeTrafficSimulatorApi();   // below
}  // namespace native

namespace native {
const sc4::tAutomataPlayerDriveParams* NativePlayerDriveParams(sc4::cSC4Automaton* automaton);   // app/NativePlayerDrive.cpp
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
void NativeStartArson(sc4::cISC4Occupant* building);                                 // app/NativeDisasters.cpp
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
    cISC4PathFinder* PathFinder() override {   // SL::spTrafficSim CreatePathFinder (one reference)
        ITrafficSimulatorApi* simulator = NativeTrafficSimulatorApi();
        return simulator ? simulator->CreatePathFinder() : nullptr;
    }
    void NeighborConnectionCells(std::vector<std::int32_t>& cells) override {
        cells.clear();
        sc4::cSC4NeighborsSimulator* neighbors = CityNeighborsSimulator();
        if (!neighbors) return;
        for (std::uint32_t kind : {3u, 0xcu, 8u, 2u, 10u}) {
            sc4::ConnectionList connections;
            neighbors->GetConnectionsOfType(kind, connections);
            for (const auto& connection : connections) {
                if (!connection) continue;
                int x = 0, z = 0;
                connection->GetConnectionCell(x, z);
                cells.push_back(x);
                cells.push_back(z);
            }
        }
    }
    void StartAudioEvent(std::uint32_t event) override {
        if (cISC4AEMSEvents* events = AemsEvents()) events->StartSound(static_cast<cISC4AEMSEvents::AEMSEventType>(event));
    }
    void StopAudioEvent(std::uint32_t event) override {
        if (cISC4AEMSEvents* events = AemsEvents()) events->EndSound(static_cast<cISC4AEMSEvents::AEMSEventType>(event));
    }
    static cISC4AEMSEvents* AemsEvents() {   // SL::AEMS(); nothing without an audio event system
        cIGZUnknown* service = SL::AEMS();
        void* raw = nullptr;
        if (!service || !service->QueryInterface(kGZIID_cISC4AEMSEvents, &raw) || !raw) return nullptr;
        auto* events = static_cast<cISC4AEMSEvents*>(raw);
        events->Release();   // SL::AEMS() keeps the service
        return events;
    }
    ITrafficParticleSystem* CreateParticleSystem() override {   // PARTIAL: exhaust / dust particles
        SC4_UNWIRED("cSC4TrafficAutomaton: the dynamic particle system (exhaust / dust) is not created");
        return nullptr;
    }
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
    ITrafficNetworkMapOccupant* FindNetworkOccupant(int x, int z) override { cSC4TrafficNetworkMap* m = Map(); return m ? m->FindNetworkOccupant(x, z) : nullptr; }
    void GetVisibleCellRect(int rect[4]) override {   // the managers' view rectangle in cells (set by NativeAutomataManagerHost)
        for (int i = 0; i < 4; ++i) rect[i] = visible_[i];
    }
    bool GetResponseOccupants(cISC4Occupant* occupant, int maxCells, int minCells, std::uint32_t mask,
                              std::vector<ITrafficNetworkMapOccupant*>& out) override {
        cSC4TrafficNetworkMap* m = Map();
        return m && m->GetSurroundingRoads(out, occupant, maxCells, minCells, mask);
    }
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
    bool RandomPathKey(ITrafficNetworkMapOccupant* o, std::uint32_t type, cRZRandomSuper* random, std::uint32_t& key) override {
        cISC4PathInfo* info = Info(o);
        return info && info->GetRandomPathWildcard(type, 0xff, 0xff, random, &key, nullptr);
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

// The vehicles of a box come from the vehicle manager the same way (the occupant manager's box test: bounding boxes overlap,
// strictly; at most `count`, negative = all); the references go to the caller.
void VehiclesInBox(const float* lo, const float* hi, int count, std::vector<cISC4Occupant*>& out) {
    auto* manager = Component<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
    if (!manager) return;
    for (IAutomataAutomaton* view : manager->Automata()) {
        if (count >= 0 && static_cast<int>(out.size()) >= count) break;
        auto* vehicle = view ? dynamic_cast<cSC4Vehicle*>(AutomatonOf(view)) : nullptr;
        if (!vehicle) continue;
        float a[3] = {0.0f, 0.0f, 0.0f}, b[3] = {0.0f, 0.0f, 0.0f};
        vehicle->GetBoundingBox(a, b);
        if (!(a[0] < hi[0] && lo[0] < b[0] && a[1] < hi[1] && lo[1] < b[1] && a[2] < hi[2] && lo[2] < b[2])) continue;
        vehicle->AddRef();
        out.push_back(static_cast<cISC4Occupant*>(vehicle));
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
    bool SceneLightsOn() override { sc4::cSC4LightingManager* lighting = LightingManager(); return lighting && lighting->IsNight(); }   // SL::spLightingManager slot 0x48
    void ManagerRecalculateLeaders(cSC4Automaton* automaton) override {
        auto* manager = Component<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
        if (manager && automaton) manager->RecalculateLeaders(TrafficAutomatonView(AutomatonView(automaton)));
    }
    bool IsOccupantGroup(cISC4Occupant* occupant, std::uint32_t group) override { return occupant && occupant->IsOccupantGroup(group); }
    // The automata group chain (GetAutomataGroup, then the parents) up to the first group with player-drive parameters.
    bool FindPlayerDriveParams(cSC4Automaton* a, const tAutomataPlayerDriveParams** out) override {
        *out = nullptr;
        if (!a || !a->GetAutomataGroup()) return false;
        *out = NativePlayerDriveParams(a);
        return true;
    }
    int CellAutomatonCount(int travelType, int cellX, int cellZ) override { return stub()->CellAutomatonCount(travelType, cellX, cellZ); }
    // The traffic network map's tile list of a cell (slot 0x70) through the +0x88 next links: each tile's network occupant
    // (the map keeps the references).
    void OccupantsInCell(int cellX, int cellZ, std::vector<ITrafficAutomatonOccupant*>& out) override {
        auto* map = Component<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
        for (TrafficTileInfo* tile = map ? map->SafeGetTileInfoAtCell(cellX, cellZ) : nullptr; tile; tile = tile->next)
            if (tile->occupant) out.push_back(static_cast<ITrafficAutomatonOccupant*>(tile->occupant));
    }
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
    // The pedestrians and vehicles of a box come from their managers (the native automata are not in the occupant manager);
    // the references go to the caller.
    void OccupantsInBox(const float* lo, const float* hi, std::uint32_t type, int count, std::vector<cISC4Occupant*>& out) override {
        if (type == kPedestrianOccupantType) {
            PedestriansInBox(lo, hi, count, out);
        } else if (type == kVehicleOccupantType) {
            VehiclesInBox(lo, hi, count, out);
        } else {
            stub()->OccupantsInBox(lo, hi, type, count, out);
        }
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
    // The arsonist's ignite state (Mac i386 0x000b2670): the city's disaster layer, its fire manager (0x621cda33, interface
    // 0x49b43f57), IgniteOccupant(the controller's building), then SetCause(2), the same call as an arson crime.
    void IgniteOccupant(cISC4Occupant* occupant) override { NativeStartArson(occupant); }
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

// The pedestrians inside a box for the other hosts (the crime simulator's figures are pedestrians); one reference each.
void NativePedestriansInBox(const float* lo, const float* hi, std::vector<cISC4Occupant*>& out) { PedestriansInBox(lo, hi, -1, out); }

sc4::ITrafficAutomatonHost* NativeVehicleTrafficHost() { return &g_vehicles; }
sc4::IVehicleHost* NativeVehicleHost() { return &g_vehicleHost; }
sc4::ITrafficSimulatorApi* NativeTrafficSimulatorApi() { return &g_simulatorApi; }

}  // namespace native
namespace sc4 { class ISituationAutomaton; class ISituationOccupant; }
namespace native {
sc4::cSC4Automaton* SituationEngineAutomaton(sc4::ISituationAutomaton* automaton);   // NativeSituationAutomata.cpp
sc4::ISituationOccupant* SituationViewOfOccupant(void* engineOccupant);

// The escape mission's target: the driven car's best neighbor connection (cSC4TrafficAutomaton::FindBestNeighborConnection) and
// the occupant of that network piece.
sc4::ISituationOccupant* NativeEscapeConnectionTarget(sc4::ISituationAutomaton* automaton) {
    auto* car = dynamic_cast<cSC4TrafficAutomaton*>(SituationEngineAutomaton(automaton));
    ITrafficAutomatonOccupant* piece = car ? car->FindBestNeighborConnection() : nullptr;
    void* raw = nullptr;
    if (!piece || !piece->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
    auto* occupant = static_cast<cISC4Occupant*>(raw);
    sc4::ISituationOccupant* view = SituationViewOfOccupant(occupant);
    occupant->Release();
    return view;
}
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

// A new path finder over the city's traffic simulator (the caller owns the reference); null without a simulator.
sc4::cISC4PathFinder* NativeCreatePathFinder() { return g_simulatorApi.CreatePathFinder(); }

// Kit command `subwayroute X0 Z0 X1 Z1 ALLOWED START` (hex travel-type masks): one search of the traffic simulator's path finder
// (cISC4TrafficSimulator::CreatePathFinder, as the route query uses it) from a cell to a cell, the travel types allowed and the
// ones a trip may start with; prints the trip's cost and the travel types of its legs (6 = subway). It changes nothing.
void NativeSubwayRouteReport(int x0, int z0, int x1, int z1, std::uint32_t allowed, std::uint32_t start) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    cISC4PathFinder* finder = simulator ? simulator->CreatePathFinder() : nullptr;
    if (!finder) {
        std::printf("subway route: no traffic simulator\n");
        return;
    }
    finder->SetTripDestinationFilter(3);
    finder->SetStartRect(SC4RectInt32{x0, z0, x0, z0});
    finder->SetEndRect(SC4RectInt32{x1, z1, x1, z1});
    finder->ClearAllFlags();
    finder->ClearAllTravelTypeFlags();
    finder->SetTravelTypeFlag(static_cast<std::uint16_t>(allowed), false);
    finder->SetTravelTypeFlag(static_cast<std::uint16_t>(start), true);
    finder->SetMaxTripLength(simulator->GetTripScale());   // as SetupTrip does
    const int found = finder->FindPath();
    std::vector<std::uint8_t> route;
    std::uint32_t types = 0;
    if (found == 1) finder->GetCompressedRoute(route, types);
    std::printf("subway route: %d,%d -> %d,%d allowed %x start %x: %s, cost %.2f, travel types %x (subway %s), legs:", x0, z0, x1, z1,
                allowed, start, found == 1 ? "found" : "none", found == 1 ? finder->GetTripLength() : 0.0f, types,
                (types & 0x40) ? "yes" : "no");
    // the compressed route: {type, x, z}, then runs of {type | 0x40, steps, packed directions}
    if (route.size() >= 3) {
        std::printf(" start type %u at %u,%u%s", route[0] & 0x3f, route[1], route[2], (route[0] & 0x80) ? " (ferry)" : "");
        for (std::size_t i = 3; i + 1 < route.size();) {
            const std::uint8_t type = route[i] & 0x3f, steps = route[i + 1];
            std::printf("; %u x%u%s", type, steps, (route[i] & 0x80) ? " (after a ferry or off-map hop)" : "");
            i += 2 + (steps + 3) / 4;
        }
    }
    std::printf(" | raw");
    for (std::uint8_t b : route) std::printf(" %02x", b);
    std::printf("\n");
    finder->Release();
}

}  // namespace native

namespace native {

// Kit command `modesplit [LABEL]`: the commute mode split of the traffic simulator's current trips (CommuteModeSplit). Read-only.
void NativeModeSplitReport(const char* label) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (!simulator) {
        std::printf("modesplit %s: no traffic simulator\n", label);
        return;
    }
    const cSC4TrafficSimulator::ModeSplit s = simulator->CommuteModeSplit();
    const auto pct = [&](std::uint64_t v) { return s.routed ? 100.0 * double(v) / double(s.routed) : 0.0; };
    std::printf("modesplit %s: commuters %llu routed %llu unrouted %llu freight %llu | car %llu (%.1f%%) walk %llu (%.1f%%) walk-only %llu (%.1f%%) "
                "bus %llu (%.1f%%) train %llu (%.1f%%) subway %llu (%.1f%%) el %llu (%.1f%%) mono %llu (%.1f%%) any-transit %llu (%.1f%%)\n",
                label, (unsigned long long)s.commuters, (unsigned long long)s.routed, (unsigned long long)s.unrouted, (unsigned long long)s.freight,
                (unsigned long long)s.byType[1], pct(s.byType[1]), (unsigned long long)s.byType[0], pct(s.byType[0]), (unsigned long long)s.walkOnly,
                pct(s.walkOnly), (unsigned long long)s.byType[2], pct(s.byType[2]), (unsigned long long)s.byType[3], pct(s.byType[3]),
                (unsigned long long)s.byType[6], pct(s.byType[6]), (unsigned long long)s.byType[7], pct(s.byType[7]), (unsigned long long)s.byType[8],
                pct(s.byType[8]), (unsigned long long)s.transit, pct(s.transit));
    static const char* kStrategy[3] = {"transit-pref", "car-pref", "neutral"};
    for (int i = 0; i < 3; ++i)
        std::printf("modesplit %s: strategy %s routed %llu any-transit %llu car %llu\n", label, kStrategy[i], (unsigned long long)s.byStrategy[i][0],
                    (unsigned long long)s.byStrategy[i][1], (unsigned long long)s.byStrategy[i][2]);
}

}  // namespace native

namespace native {

// Kit command `routeparity [N]`: the first N routed commuter trips searched again toward their own destinations
// (cSC4TrafficSimulator::RouteParity); right after loading a city these are the routes the original computed. Changes nothing.
void NativeRouteParityReport(int limit) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (!simulator) {
        std::printf("routeparity: no traffic simulator\n");
        return;
    }
    const cSC4TrafficSimulator::RouteParityResult r = simulator->RouteParity(limit > 0 ? std::uint32_t(limit) : 0xffffffffu);
    std::printf("routeparity: compared %u not found %u same travel types %u same route %u; walk-only stored %u ours %u; mean |length diff| %.4f, mean signed diff (same types) %.4f\n",
                r.compared, r.notFound, r.sameTypes, r.sameRoute, r.savedWalkOnly, r.oursWalkOnly, r.compared ? r.lengthDiff / r.compared : 0.0,
                r.sameTypes ? r.signedDiffSame / r.sameTypes : 0.0);
    for (const std::string& line : r.mismatches) std::printf("routeparity: mismatch %s\n", line.c_str());
}

}  // namespace native

namespace native {

// Kit command `triprect X0 Z0 X1 Z1`: the trips the traffic simulator holds for every lot touching the rectangle. Read-only.
void NativeTripRectReport(int x0, int z0, int x1, int z1) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (!simulator) {
        std::printf("triprect: no traffic simulator\n");
        return;
    }
    simulator->ReportTripsInRect(stdout, x0, z0, x1, z1);
}

// Kit command `tripprobe X0 Z0 X1 Z1 [DESTFLAG]`: commuter searches from a lot rect as SetupTrip configures a first search
// (destination filter 0, destination flag DESTFLAG (7 = any job, as an empty lot's probe trip), travel types 0x1cf, start types
// walk + car, the trip scale as the limit), once per travel strategy's starting costs and once with none, then walk-only. Prints
// where each ends, its length and its route. Changes nothing.
void NativeTripProbeReport(int x0, int z0, int x1, int z1, int destinationFlag) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (!simulator) {
        std::printf("tripprobe: no traffic simulator\n");
        return;
    }
    std::printf("tripprobe: trip scale %.2f, destination bytes around the rect:", simulator->GetTripScale());
    for (int z = z0 - 1; z <= z1 + 1; ++z)
        for (int x = x0 - 1; x <= x1 + 1; ++x) std::printf(" %d,%d=%x", x, z, simulator->DestinationByte(x, z));
    std::printf("\n");
    auto search = [&](const char* label, std::uint16_t allowed, std::uint16_t start, int strategy) {
        cISC4PathFinder* finder = simulator->CreatePathFinder();
        if (!finder) return;
        finder->SetTrafficDensity(1);
        finder->SetTripDestinationFilter(0);
        finder->ClearAllDestinationFlags();
        finder->SetDestinationFlag(static_cast<std::uint8_t>(destinationFlag));
        finder->SetMaxWorkTime(0);
        finder->SetStartRect(SC4RectInt32{x0, z0, x1, z1});
        finder->ClearAllFlags();
        finder->ClearAllTravelTypeFlags();
        finder->SetStartingCostArray(strategy >= 0 ? simulator->StrategyStartingCosts(strategy) : nullptr);
        finder->SetMaxTripLength(simulator->GetTripScale());
        finder->SetTravelTypeFlag(allowed, false);
        finder->SetTravelTypeFlag(start, true);
        const int found = finder->FindPath();
        std::printf("tripprobe: from %d,%d..%d,%d %s (allowed %x start %x strategy %d): %s", x0, z0, x1, z1, label, allowed, start, strategy,
                    found == 1 ? "found" : "none");
        if (found == 1) {
            int ex = 0, ez = 0;
            finder->GetEndCell(ex, ez);
            std::vector<std::uint8_t> route;
            std::uint32_t types = 0;
            finder->GetCompressedRoute(route, types);
            std::printf(" end %d,%d length %.3f arrived %u reversible %d types %x raw", ex, ez, finder->GetTripLength(), finder->GetTrafficArrived(),
                        finder->WasTripReversible() ? 1 : 0, types);
            for (std::size_t i = 0; i < route.size() && i < 48; ++i) std::printf(" %02x", route[i]);
        }
        std::printf("\n");
        finder->Release();
    };
    for (int strategy = -1; strategy < 3; ++strategy) search("commute", 0x1cf, 0x3, strategy);
    search("walk only", 0x1, 0x1, -1);
    search("car only", 0x2, 0x2, -1);
}

// Kit command `cellconn X0 Z0 X1 Z1`: the traffic simulator's connection words of every cell of a rectangle that has any: per
// travel type the exits by entry side (4 bits per entry side, sides west north east south) and the type changes by exit side,
// the wormhole bits and the transit switch id. Read-only.
void NativeCellConnectionReport(int x0, int z0, int x1, int z1) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (!simulator) {
        std::printf("cellconn: no traffic simulator\n");
        return;
    }
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || z < 0 || x > 255 || z > 255) continue;
            const auto& cell = simulator->CellConnections(x, z);
            const std::uint32_t id = simulator->SwitchIdAt(x, z);
            bool any = id != 0 || cell.wormhole != 0;
            for (int t = 0; t < 9; ++t) any = any || cell.masks[std::size_t(t)] != 0;
            if (!any) continue;
            std::printf("cellconn: %d,%d switch %u wormhole %x", x, z, id, cell.wormhole);
            for (int t = 0; t < 9; ++t)
                if (cell.masks[std::size_t(t)] != 0 || cell.switches[std::size_t(t)] != 0xffff)
                    std::printf(" | type %d exits %04x changes %04x", t, cell.masks[std::size_t(t)], cell.switches[std::size_t(t)]);
            std::printf("\n");
        }
}

// Kit command `freightprobe X0 Z0 X1 Z1`: where the traffic simulator's freight trips may end (cells whose destination byte has the
// freight bit 8, border cells included, and the border connections that carry freight types), then one search with SetupTrip's
// freight settings (destination filter 0, destination flag 8, flag 1, travel types 0x30) from the lot rect, and one aimed (filter 3)
// at each freight destination cell. Changes nothing.
void NativeFreightReport(int x0, int z0, int x1, int z1) {
    auto* simulator = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (!simulator) {
        std::printf("freightprobe: no traffic simulator\n");
        return;
    }
    std::vector<std::pair<int, int>> cells;
    const auto& flags = simulator->CellTripFlags();
    for (std::size_t row = 0; row < flags.size(); ++row)
        for (std::size_t column = 0; column < flags[row].size(); ++column)
            if (flags[row][column] & 8) cells.emplace_back(int(row) - 1, int(column) - 1);
    std::printf("freightprobe: %zu freight destination cells:", cells.size());
    for (const auto& [x, z] : cells) std::printf(" %d,%d (byte %x)", x, z, simulator->DestinationByte(x, z));
    std::printf("\n");
    const auto& connections = simulator->NeighborConnections();
    static const char* kSide[4] = {"west", "north", "east", "south"};
    for (int side = 0; side < 4; ++side)
        for (std::size_t i = 0; i < connections[std::size_t(side)].size(); ++i)
            if (connections[std::size_t(side)][i].types)
                std::printf("freightprobe: border %s %zu types %x pool %u cost %.2f\n", kSide[side], i, connections[std::size_t(side)][i].types,
                            connections[std::size_t(side)][i].pool, connections[std::size_t(side)][i].cost);
    auto search = [&](int filter, int ex, int ez) {
        cISC4PathFinder* finder = simulator->CreatePathFinder();
        if (!finder) return;
        finder->SetTrafficDensity(1);
        finder->SetTripDestinationFilter(filter);
        if (filter == 3) {
            finder->SetEndRect(SC4RectInt32{ex, ez, ex, ez});
        } else {
            finder->ClearAllDestinationFlags();
            finder->SetDestinationFlag(8);
        }
        finder->SetMaxWorkTime(0);
        finder->SetStartRect(SC4RectInt32{x0, z0, x1, z1});
        finder->ClearAllFlags();
        finder->ClearAllTravelTypeFlags();
        finder->SetStartingCostArray(nullptr);
        finder->SetMaxTripLength(simulator->GetTripScale());
        finder->SetFlag(1);
        finder->SetTravelTypeFlag(0x30, true);
        const int found = finder->FindPath();
        int cx = 0, cz = 0;
        if (found == 1) finder->GetEndCell(cx, cz);
        std::printf("freightprobe: from %d,%d..%d,%d filter %d%s: %s", x0, z0, x1, z1, filter, filter == 3 ? " to" : "", found == 1 ? "found" : "none");
        if (filter == 3) std::printf(" (target %d,%d)", ex, ez);
        if (found == 1) std::printf(" end %d,%d length %.2f", cx, cz, finder->GetTripLength());
        std::printf("; walk-and-car search to the same target: ");
        finder->ClearAllFlags();
        finder->ClearAllTravelTypeFlags();
        finder->SetTravelTypeFlag(0x3, true);
        std::printf("%s\n", finder->FindPath() == 1 ? "found" : "none");
        finder->Release();
    };
    search(0, 0, 0);
    for (std::size_t i = 0; i < cells.size() && i < 4; ++i) search(3, cells[i].first, cells[i].second);
}

}  // namespace native

namespace native {
// The automata tuning's boolean 0x49be9565 (the original's sTuningParams + 0xf9), read by the regional city's connection pass.
bool NativeDriveSideFlag() { return sc4::AutomataTuning().f219fd; }
// cSC4TrafficSimulator::CheckForAccident's accident: the vehicle manager's (SL::spCity vslot 0xac) CreateAccident on the network
// piece the traffic map holds at the cell (tile occupant, as a city occupant); false when the cell has none.
bool NativeCreateTrafficAccidentAtCell(int x, int z, int type, std::uint32_t milliseconds) {
    auto* map = Component<sc4::cSC4TrafficNetworkMap>(sc4::CityComponent::TrafficNetwork);
    sc4::TrafficTileInfo* tile = map ? map->SafeGetTileInfoAtCell(x, z) : nullptr;
    if (!tile || !tile->occupant) return false;
    auto* vehicles = Component<sc4::cSC4TrafficAutomataManager>(sc4::CityComponent::VehicleManager);
    if (!vehicles) return false;
    vehicles->CreateAccident(tile->occupant->AsOccupant(), type, milliseconds);
    return true;
}

}  // namespace native
