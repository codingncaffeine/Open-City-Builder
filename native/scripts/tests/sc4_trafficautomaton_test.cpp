// cSC4TrafficAutomaton — construction / QueryInterface / accessors, Init / Shutdown / SetPopulateLayer, the path and
// lane selection (SetPath, the path info queries, SnapToPath, FindLeader, the lookahead deque and the route deque), the
// per-tick movement (DoStraightLineTick, DoPathTick, DoAnimationTick), the state transitions and their effect / particle
// wiring, the collision / stoplight logic, the persistence round trip and the debug text.
// The city, the traffic network map, the traffic automata manager, the network occupant, the path info, the effect
// manager and the dynamic particle system are fakes / recovered classes wired through the seams of TrafficAutomaton.h.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "sc4/PathInfo.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficEffectManager.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-3f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

constexpr std::uint32_t kOccupantIid = 0x26bb767e;
constexpr std::uint32_t kNetworkOccupantIid = 0xa821ef94;
constexpr std::uint32_t kSerializableIid = 0xe4fda3d4;
constexpr std::uint32_t kMessageTargetIid = 0x452294aa;

cS3DVector3 Vec(float x, float y, float z) {
    cS3DVector3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

// ---- cISC4Occupant stand-in (the map asks for the bounding cells) ---------------------------------------------------
struct FakeBaseOccupant final : cISC4Occupant {
    SC4CellRect cells{};
    float position[3] = {0, 0, 0};
    std::uint32_t type = 0xc772bf98;
    std::uint32_t flags = 4;
    int refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kOccupantIid) { *out = this; ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { for (int i = 0; i < 3; ++i) xyz[i] = position[i]; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = cells; }
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
    std::uint32_t GetFlags() override { return flags; }
};

// ---- cISC4NetworkOccupant + cISC4PathInfo owner ---------------------------------------------------------------------
struct FakeOccupant final : ITrafficAutomatonOccupant {
    FakeBaseOccupant base;
    cISC4PathInfo* pathInfo = nullptr;   // borrowed (the test owns one heap reference)
    int refs = 1;
    int releases = 0;
    std::uint32_t networkFlags = 0x10000001u;
    std::uint32_t types = 1u << 2;   // IsOfType(2) (road)
    std::uint32_t cellX = 0, cellZ = 0;
    float altitude = 3.0f;
    bool connections[4][4] = {{true, true, true, true}, {true, true, true, true},
                              {true, true, true, true}, {true, true, true, true}};
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kNetworkOccupantIid) { *out = this; AddRef(); return true; }
        if (iid == kSerializableIid) { *out = nullptr; return false; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { ++releases; return static_cast<std::uint32_t>(--refs); }
    bool HasNetworkFlag(std::uint32_t f) const override { return (networkFlags & f) == f; }
    bool HasAnyNetworkFlag(std::uint32_t f) const override { return (networkFlags & f) != 0; }
    void SetNetworkFlag(std::uint32_t f) override { networkFlags |= f; }
    void ClearNetworkFlag(std::uint32_t f) override { networkFlags &= ~f; }
    bool ConnectsToNeighbor(int, std::uint32_t) const override { return true; }
    cISC4Occupant* AsOccupant() override { return &base; }
    ITrafficNetworkMapPathInfo* GetPathInfo() override { return nullptr; }
    void ReleasePathInfo() override {}
    bool PathConnectionExists(std::uint32_t, std::uint8_t entry, std::uint8_t exit) const override {
        if (entry == kPathEdgeWildcard || exit == kPathEdgeWildcard) return true;
        if (entry >= 4 || exit >= 4) return false;
        return connections[entry][exit];
    }
    bool IsIntersection() const override { return false; }
    float GetAltitude(float, float) const override { return altitude; }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { x = cellX; z = cellZ; }
    bool IsOfType(std::uint32_t type) const override { return type < 32 && ((types >> type) & 1u) != 0; }
    bool IsUsable() const override { return true; }
    cISC4PathInfo* GetPathInfoObject() override { return pathInfo; }
};

// ---- traffic network map host ---------------------------------------------------------------------------------------
struct FakeMapHost final : ITrafficNetworkMapHost {
    int cellsX = 8, cellsZ = 4;
    bool active = true, terrain = true;
    float viewPosition[3] = {0, 0, 0};
    float viewTarget[3] = {0, 0, 0};
    int subscribed = 0, unsubscribed = 0;
    int CityCellsX() override { return cellsX; }
    int CityCellsZ() override { return cellsZ; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    bool SimulationActive() override { return active; }
    ITrafficNetworkMapLot* LotAtCell(int, int) override { return nullptr; }
    ITrafficNetworkMapLot* LotOf(cISC4Occupant*) override { return nullptr; }
    std::vector<cISC4Occupant*> NetworkOccupantsInBox(const float*, const float*) override { return {}; }
    void ReleaseOccupants(const std::vector<cISC4Occupant*>&) override {}
    std::vector<ITrafficNetworkMapProp*> PropsAtCell(int, int) override { return {}; }
    bool HasTerrain() override { return terrain; }
    float TerrainHeightAt(float, float) override { return 7.0f; }
    ITrafficNetworkMapBuilding* LotBuilding(ITrafficNetworkMapLot*) override { return nullptr; }
    int ActiveEdgesForBuilding(std::uint32_t, std::uint8_t) override { return 0; }
    void GetViewPosition(float& x, float& y, float& z) override { x = viewPosition[0]; y = viewPosition[1]; z = viewPosition[2]; }
    void GetViewTarget(float& x, float& y, float& z) override { x = viewTarget[0]; y = viewTarget[1]; z = viewTarget[2]; }
    void SubscribeMessage(std::uint32_t) override { ++subscribed; }
    void UnsubscribeMessage(std::uint32_t) override { ++unsubscribed; }
    void MakeTunnelPaths(ITrafficNetworkMapPathInfo*, ITrafficNetworkMapOccupant*, ITrafficNetworkMapOccupant*) override {}
};

// ---- traffic effect host / dynamic particles ------------------------------------------------------------------------
struct FakeEffectHost final : ITrafficEffectHost {
    std::uint32_t elapsed = 0;
    std::uint32_t ElapsedMilliseconds() override { return elapsed; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    bool AttachAccidentController(cISC4Occupant*, const float*) override { return false; }
    void DetachController(cISC4Occupant*) override {}
};

struct FakeParticle final : ITrafficParticle {};

struct FakeParticleSystem final : ITrafficParticleSystem {
    int added = 0, deleted = 0, shutdowns = 0;
    FakeParticle particle;
    ITrafficParticle* AddParticle(const cS3DVector3&) override { ++added; return &particle; }
    void DeleteParticle(ITrafficParticle* p) override { if (p == &particle) ++deleted; }
    void Shutdown() override { ++shutdowns; }
};

// ---- traffic automata manager ---------------------------------------------------------------------------------------
struct FakeManager final : ITrafficAutomataManager {
    float density = 2.0;
    std::uint32_t travelType = 1;
    float weights[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    int layerCalls = 0;
    cSC4TrafficEffectManager* effects = nullptr;
    bool IsTrafficAutomaton(cSC4Automaton* automaton) const override {
        return dynamic_cast<cSC4TrafficAutomaton*>(automaton) != nullptr;
    }
    void SetCurrentLayer(std::uint32_t) override { ++layerCalls; }
    void ComputeEdgeWeights(ITrafficAutomatonOccupant*, TrafficTileInfo*, std::uint32_t, float* out) override {
        std::memcpy(out, weights, sizeof(weights));
    }
    std::uint32_t GetTravelType(cSC4Automaton*) override { return travelType; }
    double GetLayerDensity(std::uint32_t) override { return density; }
    cSC4TrafficEffectManager* EffectManager() override { return effects; }
};

// ---- traffic automaton host -----------------------------------------------------------------------------------------
struct FakeTrafficHost final : ITrafficAutomatonHost {
    cSC4TrafficNetworkMap* map = nullptr;
    FakeManager* manager = nullptr;
    FakeParticleSystem particles;
    int audioStarts = 0, audioStops = 0;
    int particleRequests = 0;
    TrafficAutomatonTuning tuning;
    cSC4TrafficNetworkMap* TrafficNetworkMap() override { return map; }
    float CitySizeX() override { return 128.0f; }
    float CitySizeZ() override { return 64.0f; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    int CityCellsX() override { return 8; }
    int CityCellsZ() override { return 4; }
    ITrafficAutomataManager* TrafficAutomataManager() override { return manager; }
    cISC4PathFinder* PathFinder() override { return nullptr; }
    void NeighborConnectionCells(std::vector<std::int32_t>&) override {}
    void StartAudioEvent(std::uint32_t) override { ++audioStarts; }
    void StopAudioEvent(std::uint32_t) override { ++audioStops; }
    ITrafficParticleSystem* CreateParticleSystem() override { ++particleRequests; return &particles; }
    ITrafficAutomatonOccupant* ReadNetworkOccupant(cIGZIStream&) override { return nullptr; }
    const TrafficAutomatonTuning* Tuning() override { return &tuning; }
};

// ---- IAutomatonHost stand-in (speed modifier, randomness, no models) ------------------------------------------------
struct FakeAutomatonHost final : IAutomatonHost {
    float simSpeedModifier = 1.0f;
    IAutomatonModel* CreateModel(const cGZPersistResourceKey&) override { return nullptr; }
    void DestroyModel(IAutomatonModel* model) override { if (model) model->Release(); }
    IAutomatonEffects* CreateEffects() override { return nullptr; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { return false; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { return false; }
    void FadeModelIn(IAutomatonModel*) override {}
    void FadeModelOut(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return false; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return false; }
    bool HasCity() override { return true; }
    float CitySizeX() override { return 128.0f; }
    float CitySizeZ() override { return 64.0f; }
    float TerrainHeight(float, float) override { return 0.0f; }
    bool NetworkAt(int, int) override { return true; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, const cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t) override { return nullptr; }
    bool AnimationUpdateAllowed() override { return true; }
    float SimSpeedModifier() override { return simSpeedModifier; }
    float MaxSpeedUnits() override { return 3600.0f; }
    float FadeDurationSeconds() override { return 0.2f; }
    void PostMessage(cIGZMessage2*) override {}
    std::uint32_t GenerateSoundInstanceId() override { return 1; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t) override { return 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double lo, double) override { return lo; }
    double RandomUnit() override { return 1.0; }
};

// Counts the automaton's callback messages (the state-entered event is sent twice for state 0x10).
struct CallbackTarget final : cIGZMessageTarget2 {
    int refs = 1;
    int events = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kMessageTargetIid) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool DoMessage(cIGZMessage2* message) override {
        if (message && message->GetType() == kAutomatonMsgAutomaton) ++events;
        return true;
    }
};

// ---- the world ------------------------------------------------------------------------------------------------------
struct World {
    FakeMapHost mapHost;
    cSC4TrafficNetworkMap* map = nullptr;
    FakeEffectHost effectHost;
    cSC4TrafficEffectManager* effects = nullptr;
    FakeManager manager;
    FakeTrafficHost host;
    FakeAutomatonHost automatonHost;

    World() {
        map = new cSC4TrafficNetworkMap(&mapHost);
        map->Init();
        effects = new cSC4TrafficEffectManager(&effectHost);
        manager.effects = effects;
        host.map = map;
        host.manager = &manager;
    }
    ~World() {
        delete effects;
        delete map;
    }
};

cSC4PathInfo* MakePathInfo() {
    auto* info = new cSC4PathInfo();
    info->AddRef();   // the test's own reference; the automaton's references balance around it
    return info;
}

std::uint32_t AddPath(cSC4PathInfo* info, std::uint32_t type, std::uint8_t entry, std::uint8_t exit,
                      const std::vector<cS3DVector3>& points) {
    const std::uint32_t key = MakePathKey(type, entry, exit, 0);
    tPath* path = info->NewPath(key);
    if (!path) return key;
    path->points = points;
    path->length = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const float dx = points[i].x - points[i - 1].x;
        const float dy = points[i].y - points[i - 1].y;
        const float dz = points[i].z - points[i - 1].z;
        path->length += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return key;
}

cSC4TrafficAutomaton* MakeAutomaton(World& world) {
    auto* automaton = new cSC4TrafficAutomaton(&world.host, &world.automatonHost);
    automaton->AddRef();
    return automaton;
}

// Inserts the occupant's tile into the map (its bounding cells are its one cell).
void PlaceOnMap(World& world, FakeOccupant& occupant, int cellX, int cellZ) {
    occupant.cellX = static_cast<std::uint32_t>(cellX);
    occupant.cellZ = static_cast<std::uint32_t>(cellZ);
    occupant.base.cells = SC4CellRect{cellX, cellZ, cellX, cellZ};
    occupant.base.position[0] = static_cast<float>(cellX) * 16.0f + 4.0f;
    occupant.base.position[1] = 3.0f;
    occupant.base.position[2] = static_cast<float>(cellZ) * 16.0f + 4.0f;
    world.map->HandleNetworkOccupantUsable(&occupant, false);
}

}  // namespace

int main() {
    // ---- construction, QueryInterface, accessors -------------------------------------------------------------------
    {
        World world;
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        CHECK(automaton->PopulateLayer() == 7);
        CHECK(automaton->GetPathKey() == 0xffff);
        CHECK(automaton->GetCellLifespan() == 0);
        CHECK(automaton->GetLookaheadDeque().empty());
        CHECK(automaton->GetRouteInfo().empty());
        CHECK_NEAR(automaton->GetPathOffset(), 0.0f);
        CHECK(automaton->GetNetworkOccupant() == nullptr);
        CHECK(automaton->CurrentPath() == nullptr);
        CHECK(automaton->CurrentPathInfo() == nullptr);
        CHECK(!automaton->IsTrain());
        CHECK(!automaton->GetDetectCollisions());
        CHECK(automaton->GetTrafficAutomataManager() == &world.manager);
        void* out = nullptr;
        CHECK(automaton->QueryInterface(kGZIID_cISC4TrafficAutomaton, &out));
        CHECK(out == automaton);
        automaton->Release();
        CHECK(automaton->QueryInterface(1, &out));
        CHECK(out == static_cast<void*>(static_cast<cISC4Occupant*>(automaton)));
        automaton->Release();
        CHECK(automaton->QueryInterface(kOccupantIid, &out));
        CHECK(out == static_cast<void*>(static_cast<cISC4Occupant*>(automaton)));
        automaton->Release();
        CHECK(automaton->QueryInterface(kSerializableIid, &out));
        CHECK(out != nullptr);
        automaton->Release();
        CHECK(automaton->QueryInterface(kMessageTargetIid, &out));
        CHECK(out != nullptr);
        automaton->Release();
        CHECK(!automaton->QueryInterface(0xdeadbeef, &out));
        CHECK(out == nullptr);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
    }

    // ---- Init / Shutdown -------------------------------------------------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        world.host.tuning.desiredLookaheadCount = 2;
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetAutomataFlag(kAutomataFlagIgnorePaths, true);
        automaton->SetAutomataFlag(kAutomataFlagIgnoreAttractors, true);
        CHECK(automaton->Init());
        CHECK(automaton->IsInitialized());
        CHECK(automaton->Init());   // already initialized
        CHECK(automaton->GetDesiredLookaheadCount() == 2);
        CHECK(automaton->Shutdown());
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- SetPath / path queries / map enter / exit -----------------------------------------------------------------
    {
        FakeOccupant occupant;
        FakeOccupant other;
        cSC4PathInfo* info = MakePathInfo();
        cSC4PathInfo* otherInfo = MakePathInfo();
        occupant.pathInfo = info;
        other.pathInfo = otherInfo;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        PlaceOnMap(world, other, 2, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(24.0f, 3.0f, 20.0f),
                                                 Vec(28.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetRadius(1.0f);
        automaton->SetPath(&occupant, key, &points, false);
        CHECK(automaton->GetNetworkOccupant() == &occupant);
        CHECK(automaton->GetPathKey() == key);
        CHECK(automaton->CurrentPath() == &points);
        CHECK(automaton->CurrentPathInfo() == info);
        CHECK(automaton->PathIndex() == 0);
        CHECK_NEAR(automaton->Destination().x, 24.0f);   // points[1]
        CHECK_NEAR(automaton->Destination().z, 20.0f);
        CHECK_NEAR(automaton->Destination().y, 3.0f);    // the occupant's altitude
        TrafficTileInfo* tile = world.map->GetTileInfoByNetwork(&occupant);
        CHECK(tile != nullptr);
        CHECK(tile && tile->automata.size() == 1);
        CHECK(tile && !tile->automata.empty() && tile->automata.front() == automaton);
        // stop point from the path info (a stop-point key with the same type / entry and an exact exit)
        cS3DVector3 stop{};
        CHECK(!automaton->GetStopPoint(stop));
        const std::uint32_t stopKey = (1u << 24) | (0u << 8) | 2u | (0u << 16) | kPathKeyStopFlag;
        CHECK(info->NewStopPoint(Vec(21.0f, 3.5f, 20.0f), stopKey));
        CHECK(automaton->GetStopPoint(stop));
        CHECK_NEAR(stop.y, 3.5f);
        // replacing the path with a new occupant exits the old tile and enters the new one
        const std::vector<cS3DVector3> otherPoints = {Vec(36.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t otherKey = AddPath(otherInfo, 1, 3, 1, otherPoints);
        automaton->SetPath(&other, otherKey, &otherPoints, false);
        CHECK(automaton->GetNetworkOccupant() == &other);
        CHECK(automaton->GetPathKey() == otherKey);
        TrafficTileInfo* oldTile = world.map->GetTileInfoByNetwork(&occupant);
        CHECK(oldTile == nullptr || oldTile->automata.empty());
        TrafficTileInfo* newTile = world.map->GetTileInfoByNetwork(&other);
        CHECK(newTile && newTile->automata.size() == 1);
        // clearing the path drops the occupant, the path info and the map reference
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->GetNetworkOccupant() == nullptr);
        CHECK(automaton->CurrentPath() == nullptr);
        CHECK(automaton->CurrentPathInfo() == nullptr);
        newTile = world.map->GetTileInfoByNetwork(&other);
        CHECK(newTile == nullptr || newTile->automata.empty());
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        otherInfo->Release();
        info->Release();
    }

    // ---- SetPopulateLayer / SetRouteInfo / CloseEnoughToDispatchDestination / cell lifespan -------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(24.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetPath(&occupant, key, &points, true);
        automaton->SetPopulateLayer(3);
        CHECK(automaton->PopulateLayer() == 3);
        CHECK(automaton->GetNetworkOccupant() == &occupant);   // reattached through the new layer
        CHECK(automaton->GetPathKey() == key);
        automaton->SetCellLifespan(5);
        CHECK(automaton->GetCellLifespan() == 5);
        const float nearDestination[3] = {21.0f, 3.0f, 20.0f};
        automaton->SetDestination(nearDestination);
        world.host.tuning.dispatchDistance = 2;   // 2 * 16 = 32 units
        CHECK(automaton->CloseEnoughToDispatchDestination());
        world.host.tuning.dispatchDistance = 0;
        CHECK(!automaton->CloseEnoughToDispatchDestination());
        std::deque<RouteData> route;
        route.push_back(RouteData{1, 1, 1});
        route.push_back(RouteData{2, 1, 1});
        CHECK(automaton->SetRouteInfo(route, false, false));
        CHECK(automaton->GetRouteInfo().size() == 2);
        CHECK(!automaton->SetRouteInfo(std::deque<RouteData>{}, true, true));
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- DoStraightLineTick movement ------------------------------------------------------------------------------
    {
        World world;
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {10.0f, 0.0f, 10.0f};
        automaton->SetPosition(position);
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->SetAutomataFlag(kAutomataFlagIgnoreRoads, true);
        const float destination[3] = {30.0f, 0.0f, 10.0f};
        automaton->SetDestination(destination);
        CHECK(automaton->DoStraightLineTick(10.0f));
        CHECK_NEAR(automaton->Position().x, 20.0f);
        CHECK_NEAR(automaton->Position().z, 10.0f);
        CHECK(!automaton->CheckAutomataFlag(kTrafficFlagStopped));
        // a step past the destination marks it reached
        const float nearDestination[3] = {21.0f, 0.0f, 10.0f};
        automaton->SetDestination(nearDestination);
        CHECK(automaton->DoStraightLineTick(10.0f));
        CHECK(automaton->GetReachedDestination());
        // a negative destination restarts from the position
        automaton->SetAutomataFlag(kAutomataFlagReachedDestination, false);
        const float negative[3] = {-5.0f, 0.0f, -5.0f};
        automaton->SetDestination(negative);
        CHECK(automaton->DoStraightLineTick(1.0f));
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
    }

    // ---- DoPathTick / InterpolateAlongPath / UpdateOrientation -----------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f),
                                                 Vec(40.0f, 3.0f, 40.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->SetAutomataFlag(kAutomataFlagIgnorePaths, false);
        automaton->SetAutomataFlag(kAutomataFlagIgnoreAttractors, true);
        automaton->SetPath(&occupant, key, &points, false);
        cS3DVector3 out{};
        std::int32_t index = 0;
        CHECK(automaton->InterpolateAlongPath(5.0f, Vec(20.0f, 3.0f, 20.0f), out, &index, &points, nullptr));
        CHECK_NEAR(out.x, 25.0f);
        CHECK(index == 0);
        index = 0;
        CHECK(automaton->InterpolateAlongPath(25.0f, Vec(20.0f, 3.0f, 20.0f), out, &index, &points, nullptr));
        CHECK_NEAR(out.x, 40.0f);
        CHECK_NEAR(out.z, 25.0f);
        CHECK(index == 1);
        index = 1;
        float remaining = 0.0f;
        CHECK(!automaton->InterpolateAlongPath(30.0f, Vec(40.0f, 3.0f, 20.0f), out, &index, &points, &remaining));
        CHECK_NEAR(remaining, 10.0f);
        index = 2;
        remaining = 0.0f;
        CHECK(!automaton->InterpolateAlongPath(7.0f, Vec(40.0f, 3.0f, 40.0f), out, &index, &points, &remaining));
        CHECK_NEAR(remaining, 7.0f);
        // DoPathTick: step = 1 * 1 * 10 = 10 units along the first segment
        CHECK(automaton->DoPathTick(10.0f));
        CHECK_NEAR(automaton->Position().x, 30.0f);
        CHECK_NEAR(automaton->Position().z, 20.0f);
        float direction[3] = {0, 0, 0};
        automaton->UpdateOrientation(Vec(40.0f, 3.0f, 20.0f), direction);
        CHECK_NEAR(direction[0], 1.0f);
        CHECK_NEAR(direction[1], 0.0f);
        CHECK_NEAR(direction[2], 0.0f);
        automaton->UpdateOrientation(automaton->Position(), direction);
        CHECK_NEAR(direction[2], 1.0f);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- OffsetPathPoint / GetEdgeFromVector ----------------------------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        automaton->SetPath(&occupant, key, &points, true);
        cS3DVector3 point = points.front();
        automaton->OffsetPathPoint(point);   // offset 0 -> unchanged
        CHECK_NEAR(point.z, 20.0f);
        automaton->SetPathOffset(1.0f);
        CHECK_NEAR(automaton->GetPathOffset(), 1.0f);
        automaton->OffsetPathPoint(point);
        CHECK_NEAR(point.z, 21.0f);   // an east path offsets towards +z
        CHECK_NEAR(point.x, 20.0f);
        automaton->RecalculatePathOffset();
        CHECK_NEAR(automaton->GetPathOffset(), 0.0f);
        std::uint8_t edge = 0xff;
        CHECK(automaton->GetEdgeFromVector(&occupant, Vec(1.0f, 0.0f, 0.0f), edge, true));
        CHECK(edge == 2);
        CHECK(automaton->GetEdgeFromVector(&occupant, Vec(0.0f, 0.0f, -1.0f), edge, true));
        CHECK(edge == 1);
        CHECK(automaton->GetEdgeFromVector(&occupant, Vec(-1.0f, 0.0f, 0.0f), edge, true));
        CHECK(edge == 0);
        CHECK(automaton->GetEdgeFromVector(&occupant, Vec(0.0f, 0.0f, 1.0f), edge, true));
        CHECK(edge == 3);
        CHECK(automaton->GetEdgeFromVector(&occupant, Vec(0.8f, 0.0f, 0.6f), edge, true));
        CHECK(!automaton->GetEdgeFromVector(&occupant, Vec(0.3f, 0.0f, 0.3f), edge, false));
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- state entry / exit: effects, particles, the double base enter ---------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        occupant.networkFlags = 0x403c1004u;   // the tunnel-occupant flag group (the tunnel flag itself stays clear)
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetPath(&occupant, key, &points, true);
        CallbackTarget* callback = new CallbackTarget();
        automaton->AddCallback(callback);
        callback->Release();   // the automaton holds the reference now
        CHECK(automaton->PushState(kTrafficStateDrive, nullptr, 0.0f, false, 0, 0, nullptr));
        CHECK(world.effects->Effects().size() == 1);
        CHECK(world.host.particleRequests == 1);
        CHECK(world.host.particles.added == 1);
        CHECK(automaton->FindState(kTrafficStateDrive) != nullptr);
        CHECK(callback->events >= 2);   // the base HandleEnterState runs twice for state 0x10
        CHECK(automaton->GetSelfControlling());
        // pushing the station stop exits the driving state: the effect and the particle are torn down
        CHECK(automaton->PushState(kTrafficStateStopAtStation, nullptr, 0.0f, false, 0, 0, nullptr));
        CHECK(world.effects->Effects().empty());
        CHECK(world.host.particles.deleted == 1);
        CHECK(world.host.particles.shutdowns == 1);
        // PushState stacks the station state on top; the drive state stays below it but has been exited.
        CHECK(automaton->FindState(kTrafficStateStopAtStation) != nullptr);
        CHECK_NEAR(automaton->SpeedTween().desired, 0.0f);
        CHECK_NEAR(automaton->SpeedTween().duration, 0.5f);
        automaton->ClearStateStack();
        CHECK(automaton->StateStack().empty());
        automaton->SetPath(nullptr, 0, nullptr, true);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        delete callback;
        info->Release();
    }

    // ---- DoStopSignTick / DoStopAtStationTick / DoAnimationTick ----------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(80.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->SetPath(&occupant, key, &points, true);
        automaton->SetSelfControlling(true);
        CHECK(automaton->DoAnimationTick(0, 10.0f));   // no state -> DoPathTick
        CHECK(automaton->Position().x > 20.0f);
        CHECK(automaton->Position().x < 30.0f);   // the speed tween is still easing from 1 to the default 1/36
        // the stoplight wait: the timer drops by the speed modifier * milliseconds
        CHECK(automaton->PushState(kTrafficStateStopSign, nullptr, 1000.0f, false, 0, 0, nullptr));
        automaton->SetAutomataFlag(kTrafficFlagStopped, true);
        CHECK(automaton->DoStateTick(250.0f));
        CHECK(automaton->FindState(kTrafficStateStopSign) != nullptr);
        CHECK_NEAR(automaton->GetState()->timer, 750.0f);
        automaton->SetAutomataFlag(kTrafficFlagStopped, false);
        CHECK(!automaton->DoStateTick(10.0f));
        // the station stop keeps driving while the speed tween's current value is positive
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->ClearStateStack();
        CHECK(automaton->PushState(kTrafficStateStopAtStation, nullptr, 0.0f, false, 0, 0, nullptr));
        const float beforeStation = automaton->Position().x;
        CHECK(automaton->DoStateTick(10.0f));
        CHECK(automaton->Position().x > beforeStation);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- UpdateCollision / CheckIntersectionDensity ----------------------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->SetPath(&occupant, key, &points, true);
        automaton->PushState(kTrafficStateStopSign, nullptr, 1000.0f, false, 0, 0, nullptr);
        automaton->UpdateCollision();   // behaviour 0xd -> returns early
        // type 1 has paths, so the path info answers IsIntersection; the stopped flag is clear, so the speed returns
        automaton->ClearStateStack();
        automaton->UpdateCollision();
        CHECK_NEAR(automaton->SpeedTween().desired, automaton->GetDefaultSpeed());
        // a lone automaton never trips the density check (it needs three followers)
        automaton->SetAutomataFlag(kTrafficFlagWaiting, true);
        automaton->SetAutomataFlag(kTrafficFlagStopped, true);
        CHECK(automaton->CheckIntersectionDensity());
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- FindLeader / GetSafeDistance / RemoveSurroundingAutomata --------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(60.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        // The leader enters the tile first and the follower after it. An arrival goes in front of the tile's automata
        // list, so the list is [follower, leader] and the follower's scan (from itself towards the end) reaches the
        // automaton ahead of it; the leader's own scan finds nobody.
        cSC4TrafficAutomaton* leader = MakeAutomaton(world);
        const float leaderPosition[3] = {30.0f, 3.0f, 20.0f};
        leader->SetPosition(leaderPosition);
        leader->SetAutomataFlag(kAutomataFlagCollideCandidate, true);
        leader->SetAutomataFlag(kAutomataFlagSelfControlling, true);
        leader->SetPath(&occupant, key, &points, true);
        cSC4TrafficAutomaton* follower = MakeAutomaton(world);
        const float followerPosition[3] = {20.0f, 3.0f, 20.0f};
        follower->SetPosition(followerPosition);
        follower->SetRadius(1.0f);
        follower->SetPath(&occupant, key, &points, true);
        CHECK(follower->FindLeaderInTile(&occupant, key, 0xfff00ffu, 0xfffff00u) == leader);
        CHECK(leader->FindLeaderInTile(&occupant, key, 0xfff00ffu, 0xfffff00u) == nullptr);
        // safe distance: |path end - position| - radius = 40 - 1 = 39, floored at the 16 default
        follower->SetAutomataFlag(kTrafficFlagWaiting, true);
        CHECK_NEAR(follower->GetSafeDistance(), 16.0f);
        // with a leader and collision detection: |collide position (30) - position (20)| - radius 1 - 3 = 6
        follower->SetDetectCollisions(true);
        CHECK_NEAR(follower->GetSafeDistance(), 6.0f);
        follower->RemoveSurroundingAutomata(&occupant, -1);
        CHECK(leader->GetMarkedForRemove());
        follower->SetPath(nullptr, 0, nullptr, true);
        CHECK(follower->Release() == 0);
        leader->SetPath(nullptr, 0, nullptr, true);
        CHECK(leader->Release() == 0);
        info->Release();
    }

    // ---- lookahead selection / route / ChooseBestPath / ChooseNewDestination ---------------------------------------
    {
        FakeOccupant occupantA;
        FakeOccupant occupantB;
        cSC4PathInfo* infoA = MakePathInfo();
        cSC4PathInfo* infoB = MakePathInfo();
        occupantA.pathInfo = infoA;
        occupantB.pathInfo = infoB;
        World world;
        PlaceOnMap(world, occupantA, 1, 1);
        PlaceOnMap(world, occupantB, 2, 1);
        const std::vector<cS3DVector3> pointsA = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t keyA = AddPath(infoA, 1, 0, 2, pointsA);
        const std::vector<cS3DVector3> pointsB = {Vec(36.0f, 3.0f, 20.0f), Vec(48.0f, 3.0f, 20.0f)};
        const std::uint32_t keyB = AddPath(infoB, 1, 0, 2, pointsB);
        world.host.tuning.desiredLookaheadCount = 1;
        world.host.tuning.lookaheadPenalty = 0.5f;
        // The candidate exits come from ChooseWeightedEdge: make exit 2 (the path B owns) the only weight so the
        // lookahead key is deterministic.
        world.manager.weights[0] = 0.0f;
        world.manager.weights[1] = 0.0f;
        world.manager.weights[2] = 1.0f;
        world.manager.weights[3] = 0.0f;
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->SetPath(&occupantA, keyA, &pointsA, true);
        CHECK(automaton->ChooseLookaheadTiles());
        CHECK(automaton->GetLookaheadDeque().size() == 2);
        CHECK(automaton->GetLookaheadDeque().front().key == keyA);
        const tLookaheadNode& nodeB = automaton->GetLookaheadDeque().back();
        CHECK(nodeB.occupant.get() == &occupantB);
        CHECK(nodeB.key != 0);
        CHECK(nodeB.deadEnd == 0);
        std::uint32_t chosenKey = 0;
        float chosenLength = 0.0f;
        CHECK(automaton->ChooseBestPath(std::next(automaton->GetLookaheadDeque().begin()), nodeB, pointsA.back(),
                                        chosenKey, chosenLength));
        CHECK(chosenKey == keyB);
        CHECK_NEAR(chosenLength, 4.0f);
        // ChooseNewDestination: the front node starts a trip on B's occupant
        CHECK(automaton->ChooseNewDestination());
        CHECK(automaton->GetNetworkOccupant() == &occupantB);
        CHECK(automaton->GetPathKey() == keyB);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        infoB->Release();
        infoA->Release();
    }

    // ---- ApplyAttractionWeight (chase state) ----------------------------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        cSC4TrafficAutomaton* target = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        const float targetPosition[3] = {36.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        target->SetPosition(targetPosition);
        automaton->SetPath(&occupant, key, &points, true);
        auto* chase = static_cast<tAutomataStateChase*>(automaton->CreateAutomataState(kStatePursuit));
        chase->target = static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(target));
        target->AddRef();
        chase->stateType = kStatePursuit;
        automaton->StateStack().push_back(chase);
        float weights[16] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};
        CHECK(automaton->ApplyAttractionWeight(&occupant, weights));
        // attraction towards +x -> exit edge 2; max(weight, 100) * 100 * 0.01 = 100 moves from edge 0 onto edge 2
        CHECK(weights[2] > 10.0f);
        CHECK(weights[0] < 10.0f);
        automaton->StateStack().clear();
        delete chase;
        target->Release();
        CHECK(!automaton->ApplyAttractionWeight(&occupant, weights));   // no attraction at all
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        target->Release();
        info->Release();
    }

    // ---- BeginRoute / SetRouteInfo / StartTrip / ChooseRandomPath --------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {20.0f, 3.0f, 20.0f};
        automaton->SetPosition(position);
        // a random wildcard path for the type / entry exists (exit wildcard)
        CHECK(automaton->ChooseRandomPath(&occupant, (1u << 24) | (0u << 8) | 0u));
        CHECK(automaton->GetNetworkOccupant() == &occupant);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->StartTrip(&occupant, key, false));
        CHECK(automaton->GetPathKey() == key);
        // StartTrip refuses a path with fewer than two points
        const std::uint32_t stopKey = (1u << 24) | (0u << 8) | 2u | kPathKeyStopFlag;
        CHECK(info->NewStopPoint(Vec(21.0f, 3.0f, 20.0f), stopKey));
        CHECK(!automaton->StartTrip(&occupant, stopKey, false));
        // BeginRoute from a route deque whose cell holds the occupant
        std::deque<RouteData> route;
        route.push_back(RouteData{1, 1, 1});
        route.push_back(RouteData{2, 1, 1});
        CHECK(automaton->SetRouteInfo(route, true, false));
        CHECK(automaton->GetLookaheadDeque().empty());
        CHECK(automaton->GetSpecialRouteFlag());
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- Write / Read round trip -----------------------------------------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        const float position[3] = {12.0f, 3.0f, 12.0f};
        automaton->SetPosition(position);
        automaton->SetCellLifespan(9);
        automaton->SetPopulateLayer(4);
        automaton->SetPath(&occupant, key, &points, true);
        PortMemoryOStream os;
        CHECK(automaton->Write(os));
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4TrafficAutomaton* loaded = MakeAutomaton(world);
        CHECK(loaded->Read(is));
        CHECK(loaded->GetCellLifespan() == 9);
        CHECK(loaded->PopulateLayer() == 4);
        CHECK(loaded->GetPathKey() == key);
        CHECK(loaded->PathIndex() == 0);
        CHECK(loaded->IsSavedFlagSet());
        CHECK(loaded->Release() == 0);
        // a version other than 2 is refused
        PortMemoryOStream newer;
        newer.SetUint16(3);
        PortMemoryIStream newerIn(newer.bytes().data(), newer.bytes().size());
        cSC4TrafficAutomaton* refused = MakeAutomaton(world);
        CHECK(!refused->Read(newerIn));
        CHECK(refused->Release() == 0);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    // ---- debug text ------------------------------------------------------------------------------------------------
    {
        FakeOccupant occupant;
        cSC4PathInfo* info = MakePathInfo();
        occupant.pathInfo = info;
        World world;
        PlaceOnMap(world, occupant, 1, 1);
        const std::vector<cS3DVector3> points = {Vec(20.0f, 3.0f, 20.0f), Vec(40.0f, 3.0f, 20.0f)};
        const std::uint32_t key = AddPath(info, 1, 0, 2, points);
        cSC4TrafficAutomaton* automaton = MakeAutomaton(world);
        automaton->SetPath(&occupant, key, &points, true);
        automaton->SetCellLifespan(2);
        const char* text = automaton->GetDebugExtraText();
        CHECK(std::strstr(text, "layer: 7") != nullptr);
        CHECK(std::strstr(text, "lifespan: 2") != nullptr);
        CHECK(std::strstr(text, "edge: 0-2") != nullptr);
        automaton->SetPath(nullptr, 0, nullptr, true);
        CHECK(automaton->Release() == 0);
        info->Release();
    }

    if (failures == 0) std::printf("sc4_trafficautomaton: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
