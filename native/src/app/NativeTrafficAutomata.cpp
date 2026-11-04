// The traffic automata's city services (sc4::ITrafficAutomatonHost: the traffic network map, the city's cells, their automata
// manager, saved network occupant references) and the car's view of its automata manager (ITrafficAutomataManager over
// cSC4TrafficAutomataManager). In the original the car calls its manager directly.
#include <map>
#include <memory>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "sc4/AutomataManager.h"
#include "sc4/DBSegment.h"
#include "sc4/PedestrianManager.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/PathInfo.h"
#include "sc4/Vehicle.h"
#include "sc4/VehicleManager.h"
#include "sc4/cSC4City.h"

namespace sc4 { IVehicleHost* NewStubIVehicleHost(); }   // app/hosts/IfaceIVehicleHost.cpp

namespace native {
sc4::ITrafficAutomatonOccupant* NativeTrafficPiece(sc4::cISC4Occupant* occupant);   // app/NativeTrafficNetworkMap.cpp

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
private:
    cSC4TrafficAutomataManager* manager_;
    std::uint32_t layer_ = 0;
};

template <class T> T* Component(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    return object ? dynamic_cast<T*>(object) : nullptr;
}

class TrafficHost final : public ITrafficAutomatonHost {
public:
    explicit TrafficHost(CityComponent manager) : manager_(manager) {}
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
    cISC4PathFinder* PathFinder() override { return nullptr; }   // PARTIAL: the traffic simulator's path finder (dispatch routes only)
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
    void OccupantsInBox(const float* lo, const float* hi, std::uint32_t type, int count, std::vector<cISC4Occupant*>& out) override {
        stub()->OccupantsInBox(lo, hi, type, count, out);
    }
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

TrafficHost g_vehicles(CityComponent::VehicleManager);
VehicleHost g_vehicleHost;
TrafficHost g_pedestrians(CityComponent::PedestrianManager);

}  // namespace

sc4::ITrafficAutomatonHost* NativeVehicleTrafficHost() { return &g_vehicles; }
sc4::IVehicleHost* NativeVehicleHost() { return &g_vehicleHost; }
sc4::ITrafficSimulatorApi* NativeTrafficSimulatorApi() { return &g_simulatorApi; }
sc4::ITrafficNetworkManagerApi* NativeTrafficNetworkApi() { return &g_networkApi; }
void NativeSetTrafficVisibleCells(int x0, int z0, int x1, int z1) { g_networkApi.SetVisibleCells(x0, z0, x1, z1); }
void NativeClearTrafficNetworkApi() { g_networkApi.Clear(); }
sc4::ITrafficAutomatonHost* NativePedestrianTrafficHost() { return &g_pedestrians; }

}  // namespace native
