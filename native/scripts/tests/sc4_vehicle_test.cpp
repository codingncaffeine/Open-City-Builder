// cSC4Vehicle — construction / QueryInterface / the headlight and ghost-model wiring, the headlight timer, the path
// position / previous orientation / path history bookkeeping, the path step, the vehicle geometry helpers
// (LineIntersectsSphere, FindPathIntersection, Check2DIntersection), the player-drive speed and steering translation,
// the pull-over / plummet / collision state entry, the damage thresholds, the persistence round trip and the off-road
// lot crossing. The host seams of Vehicle.h are faked; the traffic network map, the path info and the occupants are the
// recovered classes / traffic-test style fakes.
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <set>
#include <list>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "sc4/Occupant.h"
#include "sc4/PathInfo.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/Ambulance.h"
#include "sc4/EmergencyVehicle.h"
#include "sc4/PatrolCar.h"
#include "sc4/FireTruck.h"
#include "sc4/Pedestrian.h"
#include "sc4/SpecialVehicles.h"
#include "sc4/TrailerVehicle.h"
#include "sc4/Vehicle.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-3f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); \
    ++failures; } } while (0)

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

float TestDistance(const cS3DVector3& a, const cS3DVector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- cISC4Occupant stand-in -----------------------------------------------------------------------------------------
struct FakeBaseOccupant final : cISC4Occupant {
    SC4CellRect cells{};
    float position[3] = {0, 0, 0};
    std::uint32_t type = kVehicleOccupantType;
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

// The tunnel face a tunnel piece answers 0x8a4bd52b with (the map asks it for the far end; the fake has none).
struct FakeTunnel final : ITrafficNetworkMapTunnel {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    ITrafficNetworkMapOccupant* GetOtherEndOccupant() override { return nullptr; }
};

struct FakeOccupant final : ITrafficAutomatonOccupant {
    FakeBaseOccupant base;
    FakeTunnel tunnelFace;
    cISC4PathInfo* pathInfo = nullptr;   // borrowed; the test owns the reference
    int refs = 1;
    std::uint32_t networkFlags = 0x10000001u;
    std::uint32_t types = 1u << 2;   // IsOfType(2) = road
    std::uint32_t cellX = 0, cellZ = 0;
    float altitude = 3.0f;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kNetworkOccupantIid) { *out = this; AddRef(); return true; }
        if (iid == 0x8a4bd52b && (networkFlags & kVehicleNetworkFlagTunnelInterface)) {
            *out = &tunnelFace;
            tunnelFace.AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool HasNetworkFlag(std::uint32_t f) const override { return (networkFlags & f) == f; }
    bool HasAnyNetworkFlag(std::uint32_t f) const override { return (networkFlags & f) != 0; }
    void SetNetworkFlag(std::uint32_t f) override { networkFlags |= f; }
    void ClearNetworkFlag(std::uint32_t f) override { networkFlags &= ~f; }
    bool ConnectsToNeighbor(int, std::uint32_t) const override { return true; }
    cISC4Occupant* AsOccupant() override { return &base; }
    ITrafficNetworkMapPathInfo* GetPathInfo() override { return nullptr; }
    void ReleasePathInfo() override {}
    bool PathConnectionExists(std::uint32_t, std::uint8_t, std::uint8_t) const override { return true; }
    bool IsIntersection() const override { return false; }
    float GetAltitude(float, float) const override { return altitude; }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { x = cellX; z = cellZ; }
    bool IsOfType(std::uint32_t type) const override { return type < 32 && ((types >> type) & 1u) != 0; }
    bool IsUsable() const override { return true; }
    cISC4PathInfo* GetPathInfoObject() override { return pathInfo; }
};

// ---- model / effect stand-ins ---------------------------------------------------------------------------------------
struct FakeModel final : IAutomatonModel {
    int refs = 1;
    int opacityCalls = 0;
    float lastOpacity = 0.0f;
    bool transparent = false;
    bool QI(std::uint32_t, void** out) { *out = this; ++refs; return true; }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void GetBounds(float* minXyz, float* maxXyz) override {
        minXyz[0] = -2.0f; minXyz[1] = 0.0f; minXyz[2] = -1.0f;
        maxXyz[0] = 2.0f; maxXyz[1] = 1.5f; maxXyz[2] = 1.0f;
    }
    void GetTransform(cS3DTransform& out) override { out = cS3DTransform{}; }
    void SetTransform(const cS3DTransform&) override {}
    void SetOpacity(float opacity, bool translucent) override {
        ++opacityCalls;
        lastOpacity = opacity;
        transparent = translucent;
    }
    void UpdateAnimation(float) override { ++animationUpdates; }
    int GetFrame() const override { return frame; }
    int GetFrameCount() const override { return frameCount; }
    void SetZoomAndRotation(int, int) override {}
    void SetPlayMode(s3d::PlayMode) override {}
    void SetAutoFrameRate(float rate) override { autoRate = rate; }
    int frame = 0, frameCount = 1, animationUpdates = 0;
    float autoRate = 0.0f;
};

struct FakeEffect final : IVehicleEffect {
    int refs = 1;
    int transforms = 0;
    int flags = 0;
    int detaches = 0;
    float matrix[16] = {0};
    bool QueryInterface(std::uint32_t, void** out) override { *out = this; ++refs; return true; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    void SetTransform(const float* value) override {
        ++transforms;
        std::memcpy(matrix, value, sizeof(matrix));
    }
    void SetFlag(bool) override { ++flags; }
    void Detach(bool) override { ++detaches; }
};

struct FakeUnknown final : cIGZUnknown {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void** out) override { *out = this; ++refs; return true; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
};

// ---- IAutomatonHost -------------------------------------------------------------------------------------------------
struct FakeAutomatonHost final : IAutomatonHost {
    float simSpeedModifier = 1.0f;
    float maxSpeedUnits = 3600.0f;
    float terrainHeight = 0.0f;
    int modelsCreated = 0;
    int createdRefs = 0;
    std::vector<std::uint32_t> createdInstances;
    FakeModel* lastModel = nullptr;
    IAutomatonModel* CreateModel(const cGZPersistResourceKey& key) override {
        createdInstances.push_back(key.instance);
        ++modelsCreated;
        auto* model = new FakeModel();
        createdRefs = model->refs;
        lastModel = model;
        return model;
    }
    void DestroyModel(IAutomatonModel* model) override { if (model) model->Release(); }
    IAutomatonEffects* CreateEffects() override { return nullptr; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { return true; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { return true; }
    void FadeModelIn(IAutomatonModel*) override {}
    void FadeModelOut(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return true; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return true; }
    bool HasCity() override { return true; }
    float CitySizeX() override { return 128.0f; }
    float CitySizeZ() override { return 64.0f; }
    float TerrainHeight(float, float) override { return terrainHeight; }
    bool NetworkAt(int, int) override { return true; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, const cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t) override { return nullptr; }
    bool AnimationUpdateAllowed() override { return true; }
    float SimSpeedModifier() override { return simSpeedModifier; }
    float MaxSpeedUnits() override { return maxSpeedUnits; }
    float FadeDurationSeconds() override { return 0.2f; }
    void PostMessage(cIGZMessage2*) override {}
    std::uint32_t GenerateSoundInstanceId() override { return 1; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t range) override { return range ? range - 1 : 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double lo, double) override { return lo; }
    double RandomUnit() override { return 0.5; }
};

// ---- traffic automaton host -----------------------------------------------------------------------------------------
struct FakeMapHost final : ITrafficNetworkMapHost {
    int CityCellsX() override { return 8; }
    int CityCellsZ() override { return 4; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    bool SimulationActive() override { return true; }
    ITrafficNetworkMapLot* LotAtCell(int, int) override { return nullptr; }
    ITrafficNetworkMapLot* LotOf(cISC4Occupant*) override { return nullptr; }
    std::vector<cISC4Occupant*> NetworkOccupantsInBox(const float*, const float*) override { return {}; }
    void ReleaseOccupants(const std::vector<cISC4Occupant*>&) override {}
    std::vector<ITrafficNetworkMapProp*> PropsAtCell(int, int) override { return {}; }
    bool HasTerrain() override { return true; }
    float TerrainHeightAt(float, float) override { return 0.0f; }
    ITrafficNetworkMapBuilding* LotBuilding(ITrafficNetworkMapLot*) override { return nullptr; }
    int ActiveEdgesForBuilding(std::uint32_t, std::uint8_t) override { return 0; }
    void GetViewPosition(float& x, float& y, float& z) override { x = y = z = 0.0f; }
    void GetViewTarget(float& x, float& y, float& z) override { x = y = z = 0.0f; }
    void SubscribeMessage(std::uint32_t) override {}
    void UnsubscribeMessage(std::uint32_t) override {}
    void MakeTunnelPaths(ITrafficNetworkMapPathInfo*, ITrafficNetworkMapOccupant*, ITrafficNetworkMapOccupant*) override {}
};

struct FakeManager final : ITrafficAutomataManager {
    bool IsTrafficAutomaton(cSC4Automaton*) const override { return true; }
    void SetCurrentLayer(std::uint32_t) override {}
    void ComputeEdgeWeights(ITrafficAutomatonOccupant*, TrafficTileInfo*, std::uint32_t, float* weights) override {
        for (int i = 0; i < 16; ++i) weights[i] = 1.0f;
    }
    std::uint32_t GetTravelType(cSC4Automaton*) override { return 1; }
    double GetLayerDensity(std::uint32_t) override { return 1.0; }
    cSC4TrafficEffectManager* EffectManager() override { return nullptr; }
    // the trailer tests: what was asked for, and the automaton to hand out
    cSC4Automaton* nextCreated = nullptr;
    int created = 0;
    std::uint32_t createdGroup = 0, createdFlags = 0;
    std::uint8_t createdPriority = 0;
    cS3DVector3 createdAt{0, 0, 0};
    cSC4Automaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags, std::uint8_t priority) override {
        ++created;
        createdGroup = group;
        createdAt = position;
        createdFlags = flags;
        createdPriority = priority;
        cSC4Automaton* made = nextCreated;
        nextCreated = nullptr;
        return made;
    }
};

struct FakeTrafficHost final : ITrafficAutomatonHost {
    cSC4TrafficNetworkMap* map = nullptr;
    FakeManager* manager = nullptr;
    TrafficAutomatonTuning tuning;
    int audioStarts = 0;
    int audioStops = 0;
    std::uint32_t lastEvent = 0;
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
    void StartAudioEvent(std::uint32_t event) override { ++audioStarts; lastEvent = event; }
    void StopAudioEvent(std::uint32_t event) override { ++audioStops; lastEvent = event; }
    ITrafficParticleSystem* CreateParticleSystem() override { return nullptr; }
    ITrafficAutomatonOccupant* ReadNetworkOccupant(cIGZIStream&) override { return nullptr; }
    const TrafficAutomatonTuning* Tuning() override { return &tuning; }
};

// ---- the vehicle host -----------------------------------------------------------------------------------------------
struct FakeVehicleHost final : IVehicleHost {
    VehicleTuning tuning;
    bool defaultCollide = true;
    bool defaultHeadlights = true;
    bool lightsOn = false;
    bool insideCity = true;
    float terrain = 0.0f;
    float surface = 0.0f;
    bool hasGroup = true;
    bool inOccupantGroup = false;
    bool hasLot = false;
    std::int32_t lotRect[4] = {0, 0, 0, 0};
    std::vector<ITrafficAutomatonOccupant*> cellOccupants;
    std::vector<cISC4Occupant*> boxOccupants;
    std::function<bool(int, int)> canDrive = [](int, int) { return true; };
    tAutomataPlayerDriveParams params;
    int effectCreations = 0;
    int accidents = 0;
    int collisionMessages = 0;
    int leaderCalls = 0;
    int density = 0;
    float disasterDamage = 0.0f;
    std::vector<FakeEffect*> effects;
    std::vector<FakeUnknown*> madeAccidents;
    // Tuning / manager
    const VehicleTuning* Tuning() override { return &tuning; }
    bool DefaultCollide() override { return defaultCollide; }
    bool DefaultHeadlights() override { return defaultHeadlights; }
    bool SceneLightsOn() override { return lightsOn; }
    void ManagerRecalculateLeaders(cSC4Automaton*) override { ++leaderCalls; }
    bool IsOccupantGroup(cISC4Occupant*, std::uint32_t group) override {
        return inOccupantGroup && group == kVehicleGroupPlayerDrive;
    }
    bool FindPlayerDriveParams(cSC4Automaton*, const tAutomataPlayerDriveParams** out) override {
        *out = hasGroup ? &params : nullptr;
        return true;
    }
    int CellAutomatonCount(int, int, int) override { return density; }
    bool IsInsideCity(float, float) override { return insideCity; }
    float SurfaceHeight(float, float) override { return surface; }
    void OccupantsInCell(int, int, std::vector<ITrafficAutomatonOccupant*>& out) override { out = cellOccupants; }
    bool CanDriveOnCell(int cellX, int cellZ, float, float) override { return canDrive(cellX, cellZ); }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override {
        x = static_cast<float>(cellX) * 16.0f;
        z = static_cast<float>(cellZ) * 16.0f;
    }
    bool LotBoundingRectAtCell(int, int, std::int32_t rect[4]) override {
        if (!hasLot) return false;
        for (int i = 0; i < 4; ++i) rect[i] = lotRect[i];
        return true;
    }
    IVehicleEffect* CreateNamedEffect(const char*) override {
        ++effectCreations;
        auto* effect = new FakeEffect();
        effects.push_back(effect);
        return effect;
    }
    cIGZUnknown* CreateAccident(cISC4Occupant*, std::int32_t, std::uint32_t) override {
        ++accidents;
        auto* unknown = new FakeUnknown();
        madeAccidents.push_back(unknown);
        return unknown;
    }
    void OccupantsInBox(const float*, const float*, std::uint32_t, int, std::vector<cISC4Occupant*>& out) override {
        out = boxOccupants;
    }
    float PlayerDriveDisasterDamage(float, float) override { return disasterDamage; }
    void PostCollisionMessage() override { ++collisionMessages; }
    cSC4Automaton* CollidingAutomaton(cSC4Automaton*, float) override { return nullptr; }
    void ResetEffects() {
        for (FakeEffect* effect : effects) delete effect;
        effects.clear();
        for (FakeUnknown* unknown : madeAccidents) delete unknown;
        madeAccidents.clear();
    }
};

// ---- the world ------------------------------------------------------------------------------------------------------
struct World {
    FakeMapHost mapHost;
    cSC4TrafficNetworkMap* map = nullptr;
    FakeManager manager;
    FakeTrafficHost host;
    FakeAutomatonHost automatonHost;
    FakeVehicleHost vehicleHost;
    World() {
        map = new cSC4TrafficNetworkMap(&mapHost);
        map->Init();
        host.map = map;
        host.manager = &manager;
        SetVehicleHost(&vehicleHost);
    }
    ~World() {
        SetVehicleHost(nullptr);
        vehicleHost.ResetEffects();
        delete map;
    }
};

cSC4PathInfo* MakePathInfo() {
    auto* info = new cSC4PathInfo();
    info->AddRef();   // the test's own reference
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

FakeOccupant* MakeOccupant(cISC4PathInfo* info) {
    auto* occupant = new FakeOccupant();
    occupant->pathInfo = info;
    occupant->AddRef();   // the test's own reference
    return occupant;
}

cSC4Vehicle* MakeVehicle(World& world) {
    auto* vehicle = new cSC4Vehicle(&world.host, &world.automatonHost);
    vehicle->AddRef();
    return vehicle;
}

// ---- the trailer vehicle (i386 0x00391d80 .. 0x003929ca) -------------------------------------------------------------
struct TrailerGroup final : IAutomatonGroupTemplate {
    std::list<std::uint32_t> trailers;
    std::uint32_t GroupId() const override { return 0x4222; }
    IAutomatonGroupTemplate* Parent() const override { return nullptr; }
    float SpeedMin() const override { return 0.0f; }
    float SpeedMax() const override { return 0.0f; }
    void GetGroupHierarchyDepth() override {}
    const std::list<std::uint32_t>* TrailerGroupIds() const override { return &trailers; }
};
struct TestTrailer final : cSC4TrailerVehicle {
    using cSC4TrailerVehicle::cSC4TrailerVehicle;
    void SetRadiusForTest(float radius) { radius_ = radius; }
};
struct TestCar final : cSC4Vehicle {
    using cSC4Vehicle::cSC4Vehicle;
    void SetRadiusForTest(float radius) { radius_ = radius; }
};

void TestTrailerVehicle() {
    World world;
    auto* lead = new TestTrailer(&world.host, &world.automatonHost);
    lead->AddRef();
    lead->SetRadiusForTest(4.0f);

    // the point of a segment on the perpendicular through a point; the height goes by the SQUARED length ratio
    cS3DVector3 out{9, 9, 9};
    CHECK(lead->FindPointOnPathSegment(Vec(4, 0, 3), Vec(0, 0, 0), Vec(10, 2, 0), out));
    CHECK_NEAR(out.x, 4.0f);
    CHECK_NEAR(out.z, 0.0f);
    CHECK_NEAR(out.y, 0.32f);   // 16 / 100 of the rise of 2 (a plain ratio would give 0.8)
    CHECK(!lead->FindPointOnPathSegment(Vec(12, 0, 3), Vec(0, 0, 0), Vec(10, 2, 0), out));   // past the end
    CHECK(!lead->FindPointOnPathSegment(Vec(4, 0, 3), Vec(5, 1, 5), Vec(5, 1, 5), out));     // no segment

    // the group's trailer list names the cars to pull
    TrailerGroup group;
    group.trailers = {0x4223, 0x4224};
    lead->SetAutomataGroup(&group);
    CHECK(lead->GetDesiredNumCars() == 2 && lead->DesiredCars()[0] == 0x4223 && lead->DesiredCars()[1] == 0x4224);
    CHECK(lead->GetNumCars() == 0);

    // a road along x; the lead stands at x = 60 heading +x
    cSC4PathInfo* info = MakePathInfo();
    const std::uint32_t key = AddPath(info, 1, 0, 2, {Vec(0, 0, 50), Vec(100, 0, 50)});
    FakeOccupant* road = MakeOccupant(info);
    const tPath* stored = info->GetPath(key, nullptr);
    CHECK(stored != nullptr);
    if (stored) {
        const float heading[3] = {1.0f, 0.0f, 0.0f};
        lead->SetPath(road, key, &stored->points, true);
        lead->SetPathIndex(1);
        lead->SetPathPos(Vec(60, 0, 50));
        const float at60[3] = {60.0f, 0.0f, 50.0f};
        lead->MoveTo(at60, heading);
        lead->DoAnimationTick(0, 0.0f);   // remembers the tail: position, occupant, path key
        CHECK_NEAR(lead->TailPosition().x, 60.0f);
        CHECK(lead->TailOccupant() == road && lead->TailPathKey() == key);
        CHECK(lead->GetNumCars() == 0 && world.manager.created == 0);   // the tail has not moved two radii yet

        // 10 further on (two radii = 8): the first car is made at the remembered tail position
        auto* car = new TestCar(&world.host, &world.automatonHost);
        car->AddRef();
        car->SetRadiusForTest(3.0f);
        car->SetAutomataFlag(2, true);
        world.manager.nextCreated = car;
        lead->SetPathPos(Vec(70, 0, 50));
        const float at70[3] = {70.0f, 0.0f, 50.0f};
        lead->MoveTo(at70, heading);
        lead->SetPathIndex(1);
        CHECK(lead->AddCarAtEnd());
        CHECK(lead->GetNumCars() == 1 && world.manager.created == 1 && world.manager.createdGroup == 0x4223);
        CHECK_NEAR(world.manager.createdAt.x, 60.0f);
        CHECK(!car->CheckAutomataFlag(2));
        CHECK(car->GetNetworkOccupant() == road && car->GetPathKey() == key);
        float p[3] = {0, 0, 0};
        car->GetPosition(p);
        CHECK_NEAR(p[0], 60.0f);
        CHECK_NEAR(p[2], 50.0f);
        CHECK(!lead->AddCarAtEnd() && world.manager.created == 1);   // the new tail stands on the remembered position

        // following on the road: the car's path position is the lead's rear (70 - 4) on the path, the car half the
        // lead's radius behind it; a car without a previous heading takes the segment's
        lead->TickFollowingCars(16.0f);
        CHECK_NEAR(car->GetPathPos().x, 66.0f);
        CHECK_NEAR(car->GetPathPos().z, 50.0f);
        car->GetPosition(p);
        CHECK_NEAR(p[0], 64.0f);
        CHECK_NEAR(p[2], 50.0f);
        CHECK_NEAR(car->GetPrevOrient().x, 1.0f);
        CHECK(car->CurrentPath() == &stored->points);
        // the heading turns halfway from the previous one
        car->SetPrevOrient(Vec(0, 0, 1));
        lead->TickFollowingCars(16.0f);
        CHECK_NEAR(car->GetPrevOrient().x, 0.5f);
        CHECK_NEAR(car->GetPrevOrient().z, 0.5f);
        car->GetPosition(p);
        CHECK_NEAR(p[0], 65.0f);   // 66 - 2 * 0.5
        CHECK_NEAR(p[2], 49.0f);   // 50 - 2 * 0.5

        // the lead on the next road, its rear still over the first: the search goes back through the path history
        cSC4PathInfo* nextInfo = MakePathInfo();
        const std::uint32_t nextKey = AddPath(nextInfo, 1, 2, 0, {Vec(100, 0, 50), Vec(200, 0, 50)});
        CHECK(nextKey != key);
        FakeOccupant* nextRoad = MakeOccupant(nextInfo);
        const tPath* nextStored = nextInfo->GetPath(nextKey, nullptr);
        CHECK(nextStored != nullptr);
        if (nextStored) {
            lead->SetPath(nextRoad, nextKey, &nextStored->points, true);
            lead->PathHistory().clear();
            tLookaheadNode node;
            node.occupant = road;
            node.key = key;
            lead->PathHistory().push_back(node);
            lead->SetPathIndex(1);
            lead->SetPathPos(Vec(103, 0, 50));
            const float at103[3] = {103.0f, 0.0f, 50.0f};
            lead->MoveTo(at103, heading);
            car->SetPrevOrient(Vec(1, 0, 0));
            lead->TickFollowingCars(16.0f);
            CHECK_NEAR(car->GetPathPos().x, 99.0f);
            CHECK(car->GetNetworkOccupant() == road && car->GetPathKey() == key && car->CurrentPath() == &stored->points);
            car->GetPosition(p);
            CHECK_NEAR(p[0], 97.0f);
        }

        // off the road the car is set one of its radii behind the lead's rear, facing it
        const float off[3] = {110.0f, 0.0f, 60.0f};
        const float diagonal[3] = {0.0f, 0.0f, 1.0f};
        lead->MoveTo(off, diagonal);
        lead->TickFollowingCarsOffRoad(16.0f);
        CHECK_NEAR(car->GetPathPos().x, 110.0f);   // the hitch: the lead's rear
        CHECK_NEAR(car->GetPathPos().z, 56.0f);
        car->GetPosition(p);
        const float dx = 110.0f - p[0], dz = 56.0f - p[2];
        CHECK_NEAR(std::sqrt(dx * dx + dz * dz), 3.0f);

        lead->Shutdown();
        car->Shutdown();
        car->Release();
        nextRoad->Release();
        nextInfo->Release();
    }
    lead->Release();
    road->Release();
    info->Release();
}

void TestConstructionAndQueries() {
    World world;
    world.vehicleHost.tuning.defaultSpeedMin = 3600;
    world.vehicleHost.tuning.defaultSpeedMax = 3600;
    world.vehicleHost.tuning.pathOffsetMin = 2.0f;
    world.vehicleHost.tuning.pathOffsetMax = 2.0f;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    CHECK(vehicle->GetType() == kVehicleOccupantType);
    CHECK_NEAR(vehicle->GetDefaultSpeed(), 1.0f);   // 3600 units/hour * 1/3600
    CHECK(!vehicle->IsInitialized());
    CHECK(vehicle->AutomataFlags() == 0);
    CHECK_NEAR(vehicle->HeadlightTimer(), 0.0f);
    CHECK_NEAR(vehicle->PathRemaining(), 0.0f);
    CHECK(vehicle->HeadlightModel() == nullptr);
    CHECK(vehicle->MotionBlurModel(0) == nullptr);
    CHECK(vehicle->GetPathHistory().empty());
    CHECK(vehicle->GetGZCLSID() == kVehicleIID);

    void* out = reinterpret_cast<void*>(1);
    CHECK(vehicle->QueryInterface(kVehicleIID, &out) && out == vehicle);
    CHECK(vehicle->QueryInterface(kGZIID_cISC4TrafficAutomaton, &out) && out == vehicle);
    CHECK(vehicle->QueryInterface(1, &out) && out != nullptr);
    CHECK(vehicle->QueryInterface(0xa9b40f05, &out) && out != nullptr);
    CHECK(vehicle->QueryInterface(kOccupantIid, &out) && out != nullptr);
    CHECK(vehicle->QueryInterface(kSerializableIid, &out) && out != nullptr);
    CHECK(vehicle->QueryInterface(kMessageTargetIid, &out) && out != nullptr);
    CHECK(!vehicle->QueryInterface(0x12345678, &out) && out == nullptr);

    vehicle->SetPathPos(Vec(4.0f, 5.0f, 6.0f));
    CHECK_NEAR(vehicle->GetPathPos().x, 4.0f);
    CHECK_NEAR(vehicle->GetPathPos().z, 6.0f);
    vehicle->SetPrevOrient(Vec(0.0f, 0.0f, 1.0f));
    CHECK_NEAR(vehicle->GetPrevOrient().z, 1.0f);
    vehicle->SetHeadlightTimer(2.5f);
    CHECK_NEAR(vehicle->HeadlightTimer(), 2.5f);

    // Init: the collision flag, the random path offset and the headlights (the port has no auxiliary model).
    CHECK(vehicle->Init());
    CHECK(vehicle->IsInitialized());
    CHECK(vehicle->CheckAutomataFlag(kAutomataFlagCollideCandidate));
    CHECK_NEAR(vehicle->GetPathOffset(), 2.0f);
    CHECK(vehicle->Shutdown());
    CHECK(!vehicle->IsInitialized());
    vehicle->Release();
}

void TestGeometry() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);

    cS3DVector3 first;
    cS3DVector3 second;
    // Segment through the sphere: the larger parameter lands in `first`, the smaller in `second`.
    CHECK(vehicle->LineIntersectsSphere(Vec(0, 0, 0), Vec(10, 0, 0), Vec(5, 0, 0), 1.0f, first, second) == 2);
    CHECK_NEAR(first.x, 6.0f);
    CHECK_NEAR(second.x, 4.0f);
    // A miss.
    CHECK(vehicle->LineIntersectsSphere(Vec(0, 0, 0), Vec(10, 0, 0), Vec(5, 5, 0), 1.0f, first, second) == 0);
    // One intersection: the start point is inside the sphere.
    CHECK(vehicle->LineIntersectsSphere(Vec(5, 0, 0), Vec(10, 0, 0), Vec(5.5f, 0, 0), 1.0f, first, second) == 1);
    CHECK_NEAR(first.x, 6.5f);

    // FindPathIntersection prefers the second point when the centre faces the direction of travel.
    cS3DVector3 hit;
    CHECK(vehicle->FindPathIntersection(Vec(0, 0, 0), Vec(10, 0, 0), Vec(4.5f, 0.5f, 0), 1.0f, hit, 0.1f));
    CHECK_NEAR(hit.x, 3.633975f);   // the point facing the direction of travel (the centre->point dot is >= 0.1)
    // A degenerate segment is refused.
    CHECK(!vehicle->FindPathIntersection(Vec(1, 1, 1), Vec(1, 1, 1), Vec(0, 0, 0), 5.0f, hit, 0.1f));

    // The lot edges: an edge crossing a circle of radius 6 around (5, 5).
    float best = FLT_MAX;
    cS3DVector3 out = Vec(0, 0, 0);
    vehicle->Check2DIntersection(0.0f, 0.0f, 10.0f, 0.0f, Vec(5, 0, 5), 6.0f, out, best);
    CHECK_NEAR(best, 6.0f);                    // every intersection is exactly the radius from the centre
    CHECK_NEAR(std::fabs(out.x - 5.0f), 3.3166f);   // x = 5 +- sqrt(36 - 25)
    CHECK_NEAR(out.z, 0.0f);
    CHECK_NEAR(TestDistance(out, Vec(5, 0, 5)), 6.0f);
    // A parallel edge that misses leaves the answer alone.
    float untouched = FLT_MAX;
    cS3DVector3 untouchedPoint = Vec(0, 0, 0);
    vehicle->Check2DIntersection(0.0f, 100.0f, 10.0f, 100.0f, Vec(5, 0, 5), 6.0f, untouchedPoint, untouched);
    CHECK(untouched == FLT_MAX);

    vehicle->Release();
}

void TestAnimationAndHeadlights() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    // Motion blur: one ghost model is created from the primary key and the cursor advances.
    vehicle->SetAutomataFlag(kAutomataFlagMotionBlur, true);
    vehicle->UpdateMotionBlur(0.0f);
    CHECK(world.automatonHost.modelsCreated == 1);
    CHECK(vehicle->MotionBlurModel(0) != nullptr);
    // The cursor advanced to slot 3; the next call fills it with a second ghost.
    vehicle->UpdateMotionBlur(0.0f);
    CHECK(world.automatonHost.modelsCreated == 2);
    CHECK(vehicle->MotionBlurModel(1) != nullptr);
    // The headlight timer crossing zero toggles the lights; the ghost update runs at the end of the tick.
    vehicle->SetHeadlightTimer(0.05f);
    world.automatonHost.simSpeedModifier = 1.0f;
    vehicle->DoAnimationTick(0, 100.0f);
    CHECK_NEAR(vehicle->HeadlightTimer(), 0.0f);
    // A negative timer crossing zero asks for the lights off (nothing to release here).
    vehicle->SetHeadlightTimer(-0.05f);
    vehicle->DoAnimationTick(0, 100.0f);
    CHECK_NEAR(vehicle->HeadlightTimer(), 0.0f);
    // ShowMotionBlur(false) clears the ghosts.
    vehicle->ShowMotionBlur(false);
    CHECK(vehicle->MotionBlurModel(0) == nullptr);
    CHECK(!vehicle->CheckAutomataFlag(kAutomataFlagMotionBlur));
    vehicle->ShowMotionBlur(true);
    CHECK(vehicle->CheckAutomataFlag(kAutomataFlagMotionBlur));
    vehicle->ShowMotionBlur(true);
    CHECK(vehicle->CheckAutomataFlag(kAutomataFlagMotionBlur));
    vehicle->Shutdown();
    vehicle->Release();
}

void TestTweensAndSpeed() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    tAutomataStatePlayerDrive state;
    tAutomataPlayerDriveParams& params = world.vehicleHost.params;
    params.SetF(0x08, 10.0f);    // maximum speed
    params.SetF(0x0c, -5.0f);    // minimum speed (reverse)
    params.SetF(0x18, 1.0f);     // off-network scale
    params.SetF(0x1c, 0.0f);     // reverse drag
    params.SetF(0x48, 10.0f);    // collision speed divisor

    // Forward acceleration is clamped to the maximum.
    state.value3c = 0.0f;
    state.value38 = 1.0f;
    state.onRoad254 = 1;
    CHECK_NEAR(vehicle->PlayerDriveUpdateSpeed(&state, &params, true, false, 100.0f), 10.0f);
    CHECK_NEAR(state.value3c, 10.0f);

    // Reversing from a standstill is clamped to the minimum.
    state.value3c = 0.0f;
    state.value38 = -1.0f;
    CHECK_NEAR(vehicle->PlayerDriveUpdateSpeed(&state, &params, true, false, 100.0f), -5.0f);
    CHECK_NEAR(state.value3c, -5.0f);

    // A negative speed only survives off the network.
    state.value3c = -3.0f;
    state.value38 = 0.0f;
    state.onRoad254 = 0;
    CHECK_NEAR(vehicle->PlayerDriveUpdateSpeed(&state, &params, true, true, 10.0f), 0.0f);
    state.onRoad254 = 1;
    state.value3c = -3.0f;
    CHECK(vehicle->PlayerDriveUpdateSpeed(&state, &params, true, true, 10.0f) < 0.0f);

    // TranslatePlayerDriveCommands: forward command adds the parameter speed at +0x10; the steering clamps at +-pi/2.
    params.SetF(0x10, 50.0f);
    params.SetF(0x14, 10.0f);
    params.SetF(0x20, 0.7f);
    params.SetF(0x24, 0.5f);
    params.SetF(0x28, 1.5f);
    params.SetF(0x2c, 1.0f);
    params.SetF(0x30, 2.0f);
    state.history[0] = 1.0f;
    state.history[1] = -1.0f;
    state.history[2] = -1.0f;
    state.history[3] = -1.0f;
    state.value38 = 0.0f;
    state.steering6c = 0.0f;
    vehicle->SetSpeed(1.0f, 0.0f);   // the eased speed decides between the +0x10 / +0x14 command speeds
    vehicle->TranslatePlayerDriveCommands(&state, 100.0f);
    CHECK_NEAR(state.value38, 5.0f);   // 50 * 0.1
    // Turn right for a long time: the steering stops at +pi/2 (paths not ignored).
    state.history[3] = 1.0f;
    for (int i = 0; i < 100; ++i) vehicle->TranslatePlayerDriveCommands(&state, 100.0f);
    CHECK_NEAR(state.steering6c, -1.5707964f);
    // Turn left: the opposite clamp.
    state.history[3] = -1.0f;
    state.history[2] = 1.0f;
    for (int i = 0; i < 100; ++i) vehicle->TranslatePlayerDriveCommands(&state, 100.0f);
    CHECK_NEAR(state.steering6c, 1.5707964f);
    // No group: the translation answers false.
    world.vehicleHost.hasGroup = false;
    CHECK(!vehicle->TranslatePlayerDriveCommands(&state, 16.0f));
    world.vehicleHost.hasGroup = true;
    vehicle->Shutdown();
    vehicle->Release();
}

void TestStateMachine() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    // HandleEnterState for the drive state copies the group's parameter word +8 into the group speed.
    world.vehicleHost.params.SetF(0x08, 3.5f);
    CHECK(vehicle->PushState(kTrafficStateDrive, nullptr, 0.0f, true, 0, 0, nullptr));
    tAutomataState* drive = vehicle->GetState();
    CHECK(drive && drive->stateType == kTrafficStateDrive);
    auto* playerDrive = static_cast<tAutomataStatePlayerDrive*>(drive);
    vehicle->HandleEnterState(drive);
    CHECK_NEAR(playerDrive->value40, 0.0f);
    CHECK_NEAR(playerDrive->groupSpeed, 3.5f);
    CHECK(vehicle->FindState(kTrafficStateDrive) == drive);

    // DoStateTick dispatches the plummet state and its timer counts down.
    vehicle->ClearStateStack();
    CHECK(vehicle->PushState(kStatePlummet, nullptr, 0.0f, true, 0, 0, nullptr));
    auto* plummet = static_cast<tAutomataStatePlummet*>(vehicle->GetState());
    plummet->value44 = 100.0f;
    plummet->value40 = 0.0f;
    plummet->value3c = 1.0f;
    CHECK(vehicle->DoStateTick(20.0f));
    CHECK_NEAR(plummet->value44, 80.0f);
    CHECK(plummet->value3c < 0.0f);   // the spin bounced at +0.05

    // The pull-over state: value38 grows by the frame time each tick.
    vehicle->ClearStateStack();
    CHECK(vehicle->PushState(kStatePullOver, nullptr, 0.0f, true, 0, 0, nullptr));
    auto* pullOver = static_cast<tAutomataStatePullOver*>(vehicle->GetState());
    CHECK(vehicle->DoStateTick(16.0f));
    CHECK_NEAR(pullOver->value38, 16.0f);
    CHECK(vehicle->DoStateTick(16.0f));
    CHECK_NEAR(pullOver->value38, 32.0f);
    // SkipTown rolls a cell lifespan in 0..49 and answers nothing.
    vehicle->SkipTown();
    CHECK(vehicle->CheckAutomataFlag(0x2000000));
    CHECK(vehicle->GetCellLifespan() <= 49);
    CHECK(!vehicle->DoBumpedTick(16.0f));
    vehicle->Shutdown();
    vehicle->Release();
}

void TestPaths() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    cSC4PathInfo* info = MakePathInfo();
    const std::vector<cS3DVector3> points = {Vec(0, 0, 0), Vec(0, 0, 16), Vec(16, 0, 16)};
    const std::uint32_t key = AddPath(info, 1, 0, 2, points);
    FakeOccupant* occupant = MakeOccupant(info);

    vehicle->SetPath(occupant, key, nullptr, false);
    CHECK(vehicle->CurrentPath() == &info->GetPath(key, nullptr)->points);
    CHECK(vehicle->GetPathKey() == key);
    CHECK_NEAR(vehicle->GetPathPos().x, 0.0f);
    CHECK_NEAR(vehicle->GetPathPos().z, 0.0f);
    CHECK(vehicle->GetNetworkOccupant() == occupant);

    // The safe distance measures the path end from the path position (the 0x10 flag gates the path term).
    vehicle->SetAutomataFlag(0x10, true);
    vehicle->SetPathPos(Vec(16, 0, 15.0f));   // the last path point is (16, 0, 16)
    CHECK_NEAR(vehicle->GetSafeDistance(), 1.0f);
    // Far from the end the path term is 16; without the 0x10 flag it stays at the 16 default.
    vehicle->SetPathPos(Vec(0, 0, 0));
    CHECK_NEAR(vehicle->GetSafeDistance(), 16.0f);
    vehicle->SetAutomataFlag(0x10, false);
    vehicle->SetPathPos(Vec(16, 0, 15.0f));
    CHECK_NEAR(vehicle->GetSafeDistance(), 16.0f);

    // SavePathHistory stores the occupant / key pair and trims to `keep`.
    std::deque<tLookaheadNode> history;
    vehicle->SavePathHistory(history, 4);
    CHECK(history.size() == 1);
    CHECK(history.back().occupant.get() == occupant);
    CHECK(history.back().key == key);
    vehicle->SavePathHistory(history, 4);
    CHECK(history.size() == 1);   // the same occupant replaces the last entry
    vehicle->SavePathHistory(history, 1);
    CHECK(history.size() == 1);

    // RecalculatePathOffset uses the tuning range.
    world.vehicleHost.tuning.pathOffsetMin = 1.5f;
    world.vehicleHost.tuning.pathOffsetMax = 1.5f;
    vehicle->RecalculatePathOffset();
    CHECK_NEAR(vehicle->GetPathOffset(), 1.5f);

    // A transition to a tunnel occupant restarts the tunnel audio event (only with the driving state active).
    CHECK(vehicle->PushState(kTrafficStateDrive, nullptr, 0.0f, true, 0, 0, nullptr));
    const int startsBefore = world.host.audioStarts;
    FakeOccupant* tunnel = MakeOccupant(info);
    tunnel->networkFlags |= kVehicleNetworkFlagTunnelInterface;
    vehicle->SetPath(tunnel, key, nullptr, true);
    CHECK(world.host.audioStarts == startsBefore);   // the old occupant is not a tunnel: the vehicle stops the event
    vehicle->SetPath(occupant, key, nullptr, true);
    CHECK(world.host.audioStarts == startsBefore + 1);

    vehicle->ClearStateStack();
    vehicle->SetPath(nullptr, 0, nullptr, false);
    CHECK(vehicle->CurrentPath() == nullptr);
    CHECK(vehicle->GetNetworkOccupant() == nullptr);

    history.clear();   // the saved entries hold references to the occupants
    tunnel->Release();
    occupant->Release();
    delete tunnel;
    delete occupant;
    info->Release();
    vehicle->Shutdown();
    vehicle->Release();
}

void TestStartTripAndPathTick() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    cSC4PathInfo* info = MakePathInfo();
    const std::vector<cS3DVector3> points = {Vec(0, 0, 0), Vec(0, 0, 16), Vec(16, 0, 16)};
    const std::uint32_t key = AddPath(info, 1, 0, 2, points);
    FakeOccupant* occupant = MakeOccupant(info);
    vehicle->SetRadius(1.0f);
    vehicle->SetAutomataFlag(kAutomataFlagSelfControlling, true);
    vehicle->SetSpeed(1.0f, 0.0f);
    vehicle->SetDefaultSpeed(1.0f);
    CHECK(vehicle->StartTrip(occupant, key, false));
    CHECK(vehicle->CurrentPath() != nullptr);
    // StartTrip with no remaining distance snaps onto the path.
    CHECK(vehicle->PathRemaining() == 0.0f);

    // DoPathTick advances along the path and stores the actual speed.
    const cS3DVector3 before = vehicle->GetPathPos();
    vehicle->SetPathPos(Vec(0, 0, 0));
    vehicle->DoPathTick(100.0f);
    CHECK(vehicle->GetPathPos().z > 0.0f);
    CHECK(vehicle->GetActualSpeed() >= 0.0f);
    (void)before;

    vehicle->Shutdown();   // drops the occupant reference before the fake is deleted
    occupant->Release();
    delete occupant;
    info->Release();
    vehicle->Release();
}

void TestCollisionAndDamage() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    tAutomataStatePlayerDrive state;
    world.vehicleHost.params.SetF(0x48, 10.0f);
    world.vehicleHost.params.SetF(0x5c, 20.0f);

    // HandleCollision posts the message, turns the heading, marks the automaton removed and pushes the collide state.
    vehicle->SetSpeed(1.0f, 0.0f);
    CHECK(vehicle->HandleCollision(nullptr, Vec(0, 0, 0)));
    CHECK(world.vehicleHost.collisionMessages == 1);
    CHECK(vehicle->CheckAutomataFlag(kAutomataFlagRemoved));
    CHECK(vehicle->GetState() && vehicle->GetState()->stateType == kStateCollide);
    CHECK_NEAR(vehicle->GetState()->timer, 5000.0f);
    // A second collision while in the collide state answers true without pushing another state.
    CHECK(vehicle->HandleCollision(nullptr, Vec(0, 0, 0)));
    CHECK(vehicle->GetState() && vehicle->GetState()->stateType == kStateCollide);

    // ResolveCollision: a hard impact at the damage threshold creates the accident, disables the car and updates the
    // crash effect.
    vehicle->ClearStateStack();
    state.damage70 = 0;
    vehicle->ResolveCollision(Vec(0.0f, 0.0f, -1.0f), 30.0f, &state, &world.vehicleHost.params);
    CHECK(state.damage70 > 0);
    CHECK(world.vehicleHost.accidents == 1);
    CHECK(world.vehicleHost.effectCreations >= 1);
    CHECK(world.vehicleHost.effects.size() == 1);
    CHECK(world.vehicleHost.effects[0]->transforms >= 1);
    CHECK(world.vehicleHost.effects[0]->flags == 1);
    CHECK(vehicle->DriveEffect() == world.vehicleHost.effects[0]);
    CHECK(vehicle->Accident() != nullptr);
    CHECK(vehicle->CheckAutomataFlag(kAutomataFlagDisabled));

    // A light impact only creates the injury effect (half the threshold).
    World world2;
    FakeVehicleHost& host2 = world2.vehicleHost;
    cSC4Vehicle* vehicle2 = MakeVehicle(world2);
    vehicle2->Init();
    host2.params.SetF(0x48, 10.0f);
    host2.params.SetF(0x5c, 20.0f);
    tAutomataStatePlayerDrive state2;
    state2.damage70 = 11;
    vehicle2->ResolveCollision(Vec(0.0f, 0.0f, -0.001f), 1.0f, &state2, &host2.params);
    CHECK(host2.accidents == 0);
    CHECK(host2.effects.size() == 1);   // the injury effect
    vehicle2->Shutdown();
    vehicle2->Release();

    vehicle->Shutdown();
    vehicle->Release();
}

// The leader term of the vehicle's safe distance (Mac i386 0x000a5310): behind the leader the gap is measured to the
// leader's rear (position - radius * orientation) minus 1; when the leader's position is the nearer point, to that
// position minus 3; half the vehicle's own radius comes off either. Without the collision flag the leader does not count.
void TestSafeDistanceBehindALeader() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    cSC4Vehicle* leader = MakeVehicle(world);
    const cS3DVector3 leaderPosition = Vec(0, 0, 20), heading = Vec(0, 0, 1);
    leader->SetPosition(&leaderPosition.x);
    leader->SetOrientation(&heading.x);
    leader->SetRadius(2.0f);
    vehicle->SetRadius(2.0f);
    vehicle->SetLeader(leader);
    vehicle->SetAutomataFlag(kAutomataFlagCollideCandidate, true);
    vehicle->SetPathPos(Vec(0, 0, 10));          // rear at z 18: 8 away, the position 10 away
    CHECK_NEAR(vehicle->GetSafeDistance(), 8.0f - 1.0f - 1.0f);
    vehicle->SetPathPos(Vec(0, 0, 17.5f));       // half a unit behind the rear
    CHECK_NEAR(vehicle->GetSafeDistance(), 0.5f - 1.0f - 1.0f);
    vehicle->SetPathPos(Vec(0, 0, 21));          // past the leader: its position (1 away) is nearer than its rear (3)
    CHECK_NEAR(vehicle->GetSafeDistance(), 1.0f - 3.0f - 1.0f);
    vehicle->SetAutomataFlag(kAutomataFlagCollideCandidate, false);
    CHECK_NEAR(vehicle->GetSafeDistance(), 16.0f);
    vehicle->SetLeader(nullptr);
    leader->Shutdown();
    leader->Release();
    vehicle->Shutdown();
    vehicle->Release();
}

// A new vehicle drives at the default speed it rolled, not at the manager's maximum (Mac i386 0x000a4034).
void TestConstructionSpeed() {
    World world;
    world.vehicleHost.tuning.defaultSpeedMin = 1800;
    world.vehicleHost.tuning.defaultSpeedMax = 1800;
    world.automatonHost.maxSpeedUnits = 3600.0f;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    CHECK_NEAR(vehicle->GetDefaultSpeed(), 0.5f);
    CHECK_NEAR(vehicle->GetCurrentSpeed(), 0.5f);
    CHECK_NEAR(vehicle->GetMaxSpeed(), 1.0f);
    vehicle->Shutdown();
    vehicle->Release();
}

// The port's rule for saved vehicles (not in the original): a loaded vehicle whose default speed is more than twice the
// top of the tuning range takes a default inside the range on its first drive step, with its tween scaled alike; a loaded
// vehicle inside that bound and a vehicle that was never loaded keep their speeds.
void TestLoadedSpeedIsBroughtIntoRange() {
    World world;
    world.vehicleHost.tuning.defaultSpeedMin = 90;
    world.vehicleHost.tuning.defaultSpeedMax = 90;
    const float unit = 1.0f / 3600.0f;
    cSC4PathInfo* info = MakePathInfo();
    const std::vector<cS3DVector3> points = {Vec(0, 0, 0), Vec(0, 0, 16), Vec(16, 0, 16)};
    const std::uint32_t key = AddPath(info, 1, 0, 2, points);
    FakeOccupant* occupant = MakeOccupant(info);
    struct Case { float savedDefault, savedSpeed; bool loaded; float wantDefault, wantSpeed; };
    const Case cases[] = {
        {550.0f, 500.0f, true, 90.0f, 500.0f * 90.0f / 550.0f},   // the tutorial cities' cars
        {550.0f, 0.0f, true, 90.0f, 0.0f},                        // stopped: stays stopped
        {150.0f, 150.0f, true, 150.0f, 150.0f},                   // inside twice the top (180): untouched
        {550.0f, 500.0f, false, 550.0f, 500.0f},                  // never loaded: untouched
    };
    for (const Case& c : cases) {
        cSC4Vehicle* source = MakeVehicle(world);
        source->Init();
        source->SetDefaultSpeed(c.savedDefault * unit);
        source->SetSpeed(c.savedSpeed * unit, 0.0f);
        cSC4Vehicle* vehicle = source;
        if (c.loaded) {
            PortMemoryOStream out;
            CHECK(source->Write(out));
            PortMemoryIStream in(out.bytes().data(), out.bytes().size());
            vehicle = new cSC4Vehicle(&world.host, &world.automatonHost);
            vehicle->AddRef();
            CHECK(vehicle->Read(in));
            CHECK_NEAR(vehicle->GetDefaultSpeed(), c.savedDefault * unit);   // Read itself keeps the saved speed
            vehicle->Init();
        }
        vehicle->SetRadius(1.0f);
        vehicle->SetAutomataFlag(kAutomataFlagSelfControlling, true);
        CHECK(vehicle->StartTrip(occupant, key, false));
        vehicle->SetPathPos(Vec(0, 0, 0));
        vehicle->DoPathTick(10.0f);
        CHECK_NEAR(vehicle->GetDefaultSpeed(), c.wantDefault * unit);
        CHECK_NEAR(vehicle->GetCurrentSpeed(), c.wantSpeed * unit);
        vehicle->Shutdown();
        if (vehicle != source) {
            vehicle->Release();
            source->Shutdown();
        }
        source->Release();
    }
    occupant->Release();
    delete occupant;
    info->Release();
}

void TestPersistence() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    const cS3DVector3 startPosition = Vec(1, 2, 3);
    const cS3DVector3 heading = Vec(0, 0, 1);
    vehicle->SetPosition(&startPosition.x);
    vehicle->SetPathPos(Vec(4, 5, 6));
    vehicle->SetPathRemaining(7.5f);
    vehicle->SetOrientation(&heading.x);
    vehicle->SetDefaultSpeed(0.5f);
    PortMemoryOStream out;
    CHECK(vehicle->Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* copy = new cSC4Vehicle(&world.host, &world.automatonHost);
    copy->AddRef();
    CHECK(copy->Read(in));
    CHECK(copy->Write(out));
    CHECK_NEAR(copy->PathRemaining(), 7.5f);
    CHECK_NEAR(copy->GetPathPos().x, 4.0f);
    CHECK_NEAR(copy->GetPathPos().z, 6.0f);
    // A wrong version is refused.
    const std::uint8_t bad[2] = {9, 0};
    PortMemoryIStream badStream(bad, sizeof(bad));
    CHECK(!copy->Read(badStream));
    copy->Release();
    vehicle->Shutdown();
    vehicle->Release();
}

void TestLotIntersection() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    // An empty lot crossing from cell 0 to cell 1: only the start cell is drivable.
    world.vehicleHost.canDrive = [](int cellX, int) { return cellX == 0; };
    cS3DVector3 position = Vec(20.0f, 0.0f, 8.0f);
    cS3DVector2 point;
    cS3DVector2 direction;
    CHECK(vehicle->GetPlayerDriveLotIntersection(Vec(8.0f, 0.0f, 8.0f), position, &point, &direction));
    CHECK_NEAR(point.x, 15.9f);
    CHECK_NEAR(point.y, 8.0f);
    CHECK_NEAR(direction.x, -1.0f);
    CHECK_NEAR(direction.y, 0.0f);
    // The same cell answers false.
    cS3DVector3 same = Vec(8.0f, 0.0f, 8.0f);
    CHECK(!vehicle->GetPlayerDriveLotIntersection(Vec(8.0f, 0.0f, 8.0f), same, &point, &direction));

    // The position is clamped to the lot border: from (1,1) to (20,20) over the lot cells 0..1.
    world.vehicleHost.hasLot = true;
    world.vehicleHost.lotRect[0] = 0;
    world.vehicleHost.lotRect[1] = 0;
    world.vehicleHost.lotRect[2] = 1;
    world.vehicleHost.lotRect[3] = 1;
    cS3DVector3 target = Vec(20.0f, 0.0f, 20.0f);
    vehicle->RestrictPlayerDrivePosition(Vec(1.0f, 0.0f, 1.0f), target);
    CHECK(target.z == 0.0f || target.z == 32.0f || target.x == 0.0f || target.x == 32.0f);
    CHECK(target.x >= 0.0f && target.x <= 32.0f && target.z >= 0.0f && target.z <= 32.0f);
    vehicle->Shutdown();
    vehicle->Release();
}

void TestPlayerDriveTickAndOffRoad() {
    World world;
    cSC4Vehicle* vehicle = MakeVehicle(world);
    vehicle->Init();
    cSC4PathInfo* info = MakePathInfo();
    const std::vector<cS3DVector3> points = {Vec(0, 0, 0), Vec(0, 0, 16)};
    AddPath(info, 1, 0, 2, points);
    FakeOccupant* occupant = MakeOccupant(info);
    vehicle->SetRadius(1.0f);
    vehicle->SetSpeed(0.5f, 0.0f);
    vehicle->SetDefaultSpeed(0.5f);
    const cS3DVector3 driveStart = Vec(4, 0, 4);
    vehicle->SetPosition(&driveStart.x);
    vehicle->SetPathPos(driveStart);
    world.vehicleHost.params.SetF(0x08, 10.0f);
    world.vehicleHost.params.SetF(0x0c, -5.0f);
    world.vehicleHost.params.SetF(0x18, 1.0f);
    world.vehicleHost.params.SetF(0x1c, 0.0f);
    world.vehicleHost.params.SetF(0x20, 1.0f);
    world.vehicleHost.params.SetF(0x2c, 1.0f);
    world.vehicleHost.params.SetF(0x30, 2.0f);
    world.vehicleHost.params.SetF(0x34, 0.5f);
    world.vehicleHost.params.SetF(0x38, 0.5f);
    world.vehicleHost.params.SetF(0x48, 10.0f);
    world.vehicleHost.params.SetF(0x5c, 100.0f);
    CHECK(vehicle->PushState(kTrafficStateDrive, nullptr, 0.0f, true, 0, 0, nullptr));
    vehicle->HandleEnterState(vehicle->GetState());
    // No occupant in the cell: the off-road branch runs, keeps the vehicle alive and sets the ignore-paths flag.
    world.vehicleHost.cellOccupants.clear();
    CHECK(vehicle->DoPlayerDriveTick(50.0f));
    CHECK(vehicle->CheckAutomataFlag(kAutomataFlagIgnorePaths));
    CHECK(vehicle->GetState() != nullptr);

    // With a drivable occupant the network branch runs and the callback reports the on-road state.
    FakeOccupant* other = MakeOccupant(info);
    other->networkFlags = kVehicleNetworkFlagDriveable | kVehicleNetworkFlagOnRoad;
    other->cellX = 0;
    other->cellZ = 0;
    world.vehicleHost.cellOccupants.push_back(other);
    vehicle->SetPathPos(Vec(4, 0, 4));
    CHECK(vehicle->DoPlayerDriveTick(33.0f));
    CHECK(vehicle->GetState() != nullptr);

    // The off-road tick moves the state's driving position.
    tAutomataStatePlayerDrive* drive = static_cast<tAutomataStatePlayerDrive*>(vehicle->GetState());
    drive->drivePos54 = Vec(4, 0, 4);
    drive->driveDir60 = Vec(0, 0, 1);
    drive->steering6c = 0.5f;
    drive->value3c = 1.0f;
    drive->value38 = 0.0f;
    CHECK(vehicle->DoPlayerDriveOffRoadTick(0.0f, drive, 2.0f, &world.vehicleHost.params, other));
    CHECK(drive->drivePos54.x != 4.0f || drive->drivePos54.z != 4.0f);

    vehicle->ClearStateStack();
    vehicle->Shutdown();
    other->Release();
    delete other;
    occupant->Release();
    delete occupant;
    info->Release();
    vehicle->Release();
}

}  // namespace

// ---- cSC4EmergencyVehicle (the recovered part) ----------------------------------------------------------------------------
struct TestEmergencyCar final : cSC4EmergencyVehicle {
    using cSC4EmergencyVehicle::cSC4EmergencyVehicle;
    std::uint32_t GetDispatchType() const override { return 7; }
    float GetEffectiveRadius() const override { return 80.0f; }
};

struct FakeEmergencyHost final : IEmergencyVehicleHost {
    int stopped = 0, moved = 0, lightsOn = 0, lightsOff = 0, started = 0, blocked = 0, lookups = 0, routes = 0, returnRoutes = 0;
    std::uint32_t lastSound = 0, lastType = 0;
    std::uint8_t lastRepath = 0;
    bool routeFound = false;
    EmergencyVehicleTuning tuning;
    ITrafficAutomatonOccupant* occupantAt = nullptr;
    int lastX = 0, lastY = 0, lastZ = 0;
    void SirenStopped(const cSC4EmergencyVehicle&) override { ++stopped; }
    void SirenMoved(const cSC4EmergencyVehicle&, int x, int y, int z) override { ++moved; lastX = x; lastY = y; lastZ = z; }
    void SetEmergencyLights(cSC4EmergencyVehicle&, bool on) override { on ? ++lightsOn : ++lightsOff; }
    const EmergencyVehicleTuning& Tuning() override { return tuning; }
    void SirenStarted(const cSC4EmergencyVehicle&, std::uint32_t sound) override { ++started; lastSound = sound; }
    void PostPathBlocked(cSC4EmergencyVehicle&, std::uint32_t type, std::uint8_t repaths) override { ++blocked; lastType = type; lastRepath = repaths; }
    ITrafficAutomatonOccupant* NetworkOccupantAt(float, float) override { ++lookups; return occupantAt; }
    bool FindRoute(cSC4EmergencyVehicle&, const cS3DVector3&) override { ++routes; return routeFound; }
    bool FindReturnRoute(cSC4EmergencyVehicle&, const cS3DVector3&, cIGZUnknown*) override { ++returnRoutes; return routeFound; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = static_cast<int>(x * 0.0625f); cz = static_cast<int>(z * 0.0625f); }
};

void TestEmergencyVehicle() {
    World world;
    FakeEmergencyHost emergencyHost;
    auto* ev = new TestEmergencyCar(&world.host, &world.automatonHost, nullptr, &emergencyHost);
    ev->AddRef();
    CHECK(ev->GetCallNumber() == 0 && ev->GetDispatchState() == 0 && ev->RepathCount() == 0);
    ev->SetCallNumber(0x1234);
    ev->SetDispatchDestination(Vec(1, 2, 3));
    ev->SetDispatchStateRaw(3);
    ev->SetRepathCount(2);
    CHECK(ev->GetCallNumber() == 0x1234 && ev->GetDispatchState() == 3 && ev->GetDispatchDestination().z == 3.0f);

    // the stream: version 2, the vehicle record, destination, the (null) dispatch source, repath count, call number, state
    PortMemoryOStream out;
    CHECK(ev->Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* copy = new TestEmergencyCar(&world.host, &world.automatonHost, nullptr, &emergencyHost);
    copy->AddRef();
    CHECK(copy->Read(in));
    CHECK(copy->GetCallNumber() == 0x1234 && copy->GetDispatchState() == 3 && copy->RepathCount() == 2 &&
          copy->GetDispatchDestination().x == 1.0f && copy->GetDispatchDestination().y == 2.0f && copy->DispatchSource() == nullptr);
    const std::uint8_t badVersion[2] = {9, 0};
    PortMemoryIStream bad(badVersion, sizeof(badVersion));
    CHECK(!copy->Read(bad));

    // lights key: released toggles the lights (on while there is no light model), pressed goes to the vehicle
    CHECK(ev->HandlePlayerDriveCommand(kEmergencyPlayerDriveLightsCommand, false));
    CHECK(emergencyHost.lightsOn == 1 && emergencyHost.lightsOff == 0);
    ev->ShowEmergencyLights(false);                                  // already off: nothing to do
    CHECK(emergencyHost.lightsOff == 0);

    // collisions: ordinary vehicles use the base rule; emergency mode ignores everything that is not also an emergency car
    cSC4Vehicle* other = MakeVehicle(world);
    CHECK(ev->ShouldCollide(other) == static_cast<const cSC4Automaton*>(ev)->cSC4Automaton::ShouldCollide(other));
    ev->SetAutomataFlag(0x80, true);
    CHECK(!ev->ShouldCollide(nullptr) && !ev->ShouldCollide(ev) && !ev->ShouldCollide(other));
    other->SetAutomataFlag(0x80, true);
    CHECK(ev->ShouldCollide(other));                                   // two emergency cars that are not queueing collide
    other->SetAutomataFlag(0x20, true);
    CHECK(!ev->ShouldCollide(other) || ev->Route().empty());           // a queueing one only when this car has nothing routed
    other->Release();

    // dispatch state: 3 turns the emergency mode on, 6 turns it off; 1 reports a blocked path; the same state again is ignored
    emergencyHost.tuning.maxRepaths = 2;
    emergencyHost.tuning.retrySeconds = 3.0f;
    ev->SetDispatchStateRaw(0);
    ev->SetDispatchState(3);
    CHECK(ev->GetDispatchState() == 3 && ev->CheckAutomataFlag(0x80) && ev->CheckAutomataFlag(0x800) && ev->CheckAutomataFlag(0x1000));
    CHECK(ev->GetState() != nullptr && ev->GetState()->stateType == kEmergencyStateEnRoute);
    CHECK(emergencyHost.started == 0);                                   // no sound group: no siren message
    ev->SetDispatchState(3);                                             // (same state: ignored)
    ev->AddOccupantGroup(0x4228);                                         // a police car
    ev->SetDispatchStateRaw(0);
    ev->SetDispatchState(3);
    CHECK(emergencyHost.started == 1 && emergencyHost.lastSound == 0x29de9021);
    ev->SetDispatchState(1);
    CHECK(emergencyHost.blocked == 1 && emergencyHost.lastType == 7);
    ev->SetDispatchState(6);
    CHECK(!ev->CheckAutomataFlag(0x80) && !ev->CheckAutomataFlag(0x800) && ev->CheckAutomataFlag(0x10000));

    // repathing: a found route restores the state and clears the counter; failures retry up to the limit while the car still
    // has a road under it, and a car without one is removed at once
    ev->SetRepathCount(0);
    emergencyHost.routeFound = true;
    CHECK(ev->DoRepathingTick(16.0f) && ev->RepathCount() == 0 && ev->GetDispatchState() == 3);
    emergencyHost.routeFound = false;
    emergencyHost.lookups = 0;
    CHECK(!ev->ValidateNetworkOccupant() && emergencyHost.lookups == 1);   // nothing under the car yet
    cSC4PathInfo* info = MakePathInfo();
    FakeOccupant* road = MakeOccupant(info);
    emergencyHost.occupantAt = road;
    CHECK(ev->ValidateNetworkOccupant());                                    // attached now
    const int blockedBefore = emergencyHost.blocked;
    CHECK(ev->DoRepathingTick(16.0f) && ev->RepathCount() == 1 && emergencyHost.blocked == blockedBefore + 1);
    CHECK(ev->GetState() != nullptr && ev->GetState()->stateType == 7);     // the retry pause
    CHECK(ev->DoRepathingTick(16.0f) && ev->RepathCount() == 2 && !ev->GetMarkedForRemove());
    CHECK(ev->DoRepathingTick(16.0f) && ev->RepathCount() == 3 && ev->GetMarkedForRemove());   // past the limit: removed
    road->Release();
    info->Release();

    // reaching the call: close enough hands over to the event handler, too far reports blocked and repaths after a pause
    {
        ev->SetAutomataFlag(0x80, true);
        ev->SetDispatchStateRaw(3);
        emergencyHost.tuning.teleportCells = 4;                                  // 64 metres
        const float here[3] = {100.0f, 0.0f, 20.0f};                             // cell (6, 1) of the 8 x 4 test map
        ev->SetPosition(here);
        ev->SetDispatchDestination(Vec(100.0f, 0.0f, 60.0f));                    // 40 away: close
        CHECK(ev->CloseEnoughToDispatchDestination());
        ev->SetDispatchDestination(Vec(100.0f, 0.0f, 120.0f));                   // 100 away: far
        CHECK(!ev->CloseEnoughToDispatchDestination());
        ev->SetDispatchDestination(Vec(100.0f, 0.0f, 60.0f));
        ev->HandleReachedRouteDestination();
        CHECK(ev->GetDispatchState() == 5 && ev->GetState() != nullptr && ev->GetState()->stateType == kEmergencyStateHandlingEvent);
        CHECK(ev->CheckAutomataFlag(0x4000) && !ev->CheckAutomataFlag(4));
        ev->SetDispatchStateRaw(3);
        ev->SetAutomataFlag(0x80, true);                                            // entering a state resets the mode flags
        ev->SetDispatchDestination(Vec(100.0f, 0.0f, 200.0f));
        ev->HandleReachedRouteDestination();
        CHECK(ev->GetDispatchState() == 1 && ev->GetState() != nullptr && ev->GetState()->stateType == 7);   // waiting to repath
        ev->SetDispatchStateRaw(1);
        const std::uint32_t before = ev->GetState() ? ev->GetState()->stateType : 0;
        ev->HandleReachedRouteDestination();                                       // a blocked call waits
        CHECK(ev->GetState() != nullptr && ev->GetState()->stateType == before);
        // going home: not while already home, not while a player drives; the teleport state follows
        ev->SetDispatchStateRaw(6);
        ev->ReturnToSourceOccupant();
        CHECK(ev->GetDispatchState() == 6);
        ev->SetDispatchStateRaw(3);
        ev->ReturnToSourceOccupant();
        CHECK(ev->GetDispatchState() == 6 && ev->GetState() != nullptr && ev->GetState()->stateType == kEmergencyStateTeleport);
        // the teleport tick: no source -> leave; source close -> leave; source far with a route -> drive home
        ev->OpacityTween() = AutomatonTween{};                                      // opacity 0: faded out
        CHECK(ev->DoTeleportTick(16.0f) && ev->GetMarkedForRemove());

        auto makeHomeBound = [&](float sourceX) {
            auto* car = new TestEmergencyCar(&world.host, &world.automatonHost, nullptr, &emergencyHost);
            car->AddRef();
            auto* source = new FakeBaseOccupant();
            source->position[0] = sourceX;
            car->SetDispatchSource(source);
            source->refs--;                                                       // the car holds the only reference
            const float spot[3] = {0.0f, 0.0f, 0.0f};
            car->SetPosition(spot);
            car->OpacityTween() = AutomatonTween{};
            return car;
        };
        emergencyHost.tuning.teleportCells = 4;
        emergencyHost.routeFound = true;
        emergencyHost.returnRoutes = 0;
        TestEmergencyCar* far = makeHomeBound(500.0f);                                // 500 away >= 64: ask for the way home
        CHECK(far->DoTeleportTick(16.0f) && !far->GetMarkedForRemove() && emergencyHost.returnRoutes == 1);
        CHECK(far->GetDispatchState() == 6 && far->RepathCount() == 0);
        far->Release();
        emergencyHost.routeFound = false;
        TestEmergencyCar* noRoute = makeHomeBound(500.0f);                            // no route: leave the map
        CHECK(noRoute->DoTeleportTick(16.0f) && noRoute->GetMarkedForRemove());
        noRoute->Release();
        TestEmergencyCar* near = makeHomeBound(10.0f);                                // already at the source: leave
        CHECK(near->DoTeleportTick(16.0f) && near->GetMarkedForRemove());
        near->Release();
    }

    // a new call is stored with its sender and clears the arrived look
    {
        auto* car = new TestEmergencyCar(&world.host, &world.automatonHost, nullptr, &emergencyHost);
        car->AddRef();
        auto* sender = new FakeBaseOccupant();
        const int sendersBefore = sender->refs;
        car->SetAutomataFlag(0x400, true);
        car->HandleDispatched(Vec(7.0f, 8.0f, 9.0f), sender);
        CHECK(car->GetDispatchDestination().x == 7.0f && car->GetDispatchDestination().z == 9.0f && car->DispatchSource() == sender);
        CHECK(sender->refs == sendersBefore + 1 && !car->CheckAutomataFlag(0x400));
        car->HandleDispatched(Vec(1.0f, 2.0f, 3.0f), nullptr);                       // no sender: the old one stays
        CHECK(car->DispatchSource() == sender && car->GetDispatchDestination().x == 1.0f);
        car->Release();
    }

    // the ambulance: type 7, radius 80, a version-1 wrapper around the emergency record
    {
        auto* ambulance = new cSC4Ambulance(&world.host, &world.automatonHost, nullptr, &emergencyHost);
        ambulance->AddRef();
        ambulance->SetCallNumber(77);
        CHECK(ambulance->GetDispatchType() == 7 && ambulance->GetEffectiveRadius() == 80.0f && ambulance->GetGZCLSID() == 0x6c1acde1);
        PortMemoryOStream stream;
        CHECK(ambulance->Write(stream));
        PortMemoryIStream back(stream.bytes().data(), stream.bytes().size());
        auto* twin = new cSC4Ambulance(&world.host, &world.automatonHost, nullptr, &emergencyHost);
        twin->AddRef();
        CHECK(twin->Read(back) && twin->GetCallNumber() == 77);
        const std::uint8_t wrong[2] = {2, 0};
        PortMemoryIStream wrongStream(wrong, sizeof(wrong));
        CHECK(!twin->Read(wrongStream));
        twin->Release();
        ambulance->Release();
    }

    // the three small vehicles: ids, versions and the limo's initial fields
    {
        auto roundTrip = [&](cSC4Vehicle* source, cSC4Vehicle* target, std::uint16_t version) {
            PortMemoryOStream stream;
            CHECK(source->Write(stream));
            CHECK(stream.bytes().size() >= 2 && stream.bytes()[0] == version);
            PortMemoryIStream back(stream.bytes().data(), stream.bytes().size());
            CHECK(target->Read(back));
            std::uint8_t wrong[2] = {9, 0};
            PortMemoryIStream wrongStream(wrong, sizeof(wrong));
            CHECK(!target->Read(wrongStream));
        };
        cSC4CarjackedVehicle jacked(&world.host, &world.automatonHost);
        cSC4CarjackedVehicle jackedTwin(&world.host, &world.automatonHost);
        cSC4GetawayVan van(&world.host, &world.automatonHost);
        cSC4GetawayVan vanTwin(&world.host, &world.automatonHost);
        cSC4MayorLimo limo(&world.host, &world.automatonHost);
        cSC4MayorLimo limoTwin(&world.host, &world.automatonHost);
        CHECK(jacked.GetGZCLSID() == 0xcbb32b0a && van.GetGZCLSID() == 0xea863423 && limo.GetGZCLSID() == 0x4c0dd224);
        roundTrip(&jacked, &jackedTwin, 1);
        roundTrip(&van, &vanTwin, 3);
        roundTrip(&limo, &limoTwin, 1);
        CHECK(limo.Value424() == 50 && limo.Value428() == 48.0f && limo.Value42c() == 10.0f);
    }

    // ---- the crime layer's two vehicles (PostInit: i386 0x0049d9a8 carjacked, 0x0039189c getaway van) ------------------------
    {
        struct FakeCity final : IGetawayVanHost {
            cISC4Occupant* target = nullptr;
            bool grids = true;
            std::uint8_t police = 0, crime = 0;
            int askedX = -1, askedZ = -1;
            bool IsSituationTarget(cISC4Occupant* occupant) override { return occupant == target; }
            bool PoliceAndCrimeAt(int x, int z, std::uint8_t& p, std::uint8_t& c) override {
                askedX = x;
                askedZ = z;
                p = police;
                c = crime;
                return grids;
            }
        } city;
        constexpr std::uint32_t kLeavingTown = 0x201180a;

        auto* jacked = new cSC4CarjackedVehicle(&world.host, &world.automatonHost);
        jacked->AddRef();
        CHECK(!jacked->CheckAutomataFlag(kLeavingTown));
        cSC4Automaton* asAutomaton = jacked;
        CHECK(asAutomaton->PostInit());                                             // reached through the base (the manager's call)
        CHECK(jacked->CheckAutomataFlag(kLeavingTown));
        CHECK(jacked->GetState() && jacked->GetState()->stateType == kStateAttachAnim && jacked->GetState()->timer == 5.0f);
        CHECK(!world.automatonHost.createdInstances.empty() && world.automatonHost.createdInstances.back() == 0x112a0000);
        jacked->Release();

        const auto vanModel = [&](bool withCity, cS3DVector3 at, bool target) {
            SetGetawayVanHost(withCity ? &city : nullptr);
            auto* van = new cSC4GetawayVan(&world.host, &world.automatonHost);
            van->AddRef();
            van->SetPosition(&at.x);
            city.target = target ? van->AsOccupant() : nullptr;
            CHECK(van->PostInit());
            CHECK(van->CheckAutomataFlag(kLeavingTown));
            CHECK(van->GetState() && van->GetState()->stateType == kStateAttachAnim);
            const std::uint32_t instance = world.automatonHost.createdInstances.back();
            CHECK(van->Escapes() == (instance == 0x117c0000));
            van->Release();
            SetGetawayVanHost(nullptr);
            return instance;
        };
        constexpr std::uint32_t kEscape = 0x117c0000, kCaught = 0x117b0000;
        // the police outweigh the crime at the van's cell (world / 16): caught
        city.police = 5;
        city.crime = 3;
        CHECK(vanModel(true, cS3DVector3{40.0f, 0.0f, 72.0f}, false) == kCaught);
        CHECK(city.askedX == 2 && city.askedZ == 4);
        city.crime = 5;                                                             // as much crime as police: away
        CHECK(vanModel(true, cS3DVector3{40.0f, 0.0f, 72.0f}, false) == kEscape);
        city.police = 0;                                                            // no police at all: away
        city.crime = 0;
        CHECK(vanModel(true, cS3DVector3{40.0f, 0.0f, 72.0f}, false) == kEscape);
        city.police = 5;                                                            // the mission's target always gets away
        city.crime = 3;
        city.askedX = -1;
        CHECK(vanModel(true, cS3DVector3{40.0f, 0.0f, 72.0f}, true) == kEscape);
        CHECK(city.askedX == -1);
        city.grids = false;                                                         // no grids: the coin (this host rolls range - 1)
        CHECK(vanModel(true, cS3DVector3{40.0f, 0.0f, 72.0f}, false) == kCaught);
        CHECK(vanModel(false, cS3DVector3{40.0f, 0.0f, 72.0f}, false) == kCaught);
    }

    // attraction: emergency mode drops the attraction controller instead of updating it
    ev->UpdateAttraction();

    // shutdown silences the siren and releases the light model
    const int stoppedBefore = emergencyHost.stopped;
    ev->Shutdown();
    CHECK(emergencyHost.stopped == stoppedBefore + 1 && ev->LightModel() == nullptr);
    copy->Release();
    ev->Release();
}

// ---- cSC4PatrolCar ------------------------------------------------------------------------------------------------------------
struct FakeCrew final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

struct FakeParticipant final : ICrimeParticipant {
    int* remaining = nullptr;
    int arrests = 0;
    void Arrest(bool on) override { if (on) { ++arrests; --*remaining; } }
};

struct FakePatrolHost final : IPatrolCarHost {
    bool riot = false, stationKnown = true;
    float competence = 0.5f, radius = 40.0f;
    cSC4Automaton* criminal = nullptr;
    int participantsLeft = 0;
    FakeParticipant participant;
    FakeCrew crew;
    int created = 0, setups = 0, removed = 0, replaced = 0;
    float setupCompetence = -1.0f;
    std::uint32_t replacedInstance = 0;
    FakePatrolHost() { participant.remaining = &participantsLeft; }
    bool RiotInProgress() override { return riot; }
    bool PoliceCompetence(cIGZUnknown*, float& value) override { value = competence; return stationKnown; }
    bool StationRadius(cIGZUnknown*, float& value) override { value = radius; return stationKnown; }
    cSC4Automaton* FindCrimeAutomaton(const cS3DVector3&, float) override { return criminal; }
    ICrimeParticipant* FindCrimeParticipant(const cS3DVector3&, float) override { return participantsLeft > 0 ? &participant : nullptr; }
    cIGZUnknown* NewRiotPolice(cSC4PatrolCar&) override { ++created; return &crew; }
    void SetupRiotPolice(cIGZUnknown*, float c) override { ++setups; setupCompetence = c; }
    void RemoveRiotPolice(cIGZUnknown*) override { ++removed; }
    void ReplaceArrestModel(cSC4PatrolCar&, const cGZPersistResourceKey& key) override { ++replaced; replacedInstance = key.instance; }
};

void TestPatrolCar() {
    World world;
    FakeEmergencyHost emergencyHost;
    FakePatrolHost patrol;
    auto* car = new cSC4PatrolCar(&world.host, &world.automatonHost, nullptr, &emergencyHost, &patrol);
    car->AddRef();
    CHECK(car->GetGZCLSID() == 0x4a222b98 && car->GetDispatchType() == 0);
    CHECK(car->GetEffectiveRadius() == 16.0f);                       // no dispatch source: the default reach
    auto* source = new FakeBaseOccupant();
    car->SetDispatchSource(source);
    source->refs--;
    CHECK(car->GetEffectiveRadius() == 40.0f);
    patrol.stationKnown = false;
    CHECK(car->GetEffectiveRadius() == 16.0f);
    patrol.stationKnown = true;

    // a blocked call (1) and a call being handled during a riot bring the crew; the crew is counted by the car
    car->CreateRiotPolice();
    CHECK(patrol.created == 1 && patrol.setups == 1 && patrol.setupCompetence == 0.5f && car->RiotPolice() == &patrol.crew && patrol.crew.refs == 1);
    car->CreateRiotPolice();
    CHECK(patrol.created == 1);                                       // one crew per car
    car->ReleaseRiotPolice();
    CHECK(patrol.removed == 1 && car->RiotPolice() == nullptr && patrol.crew.refs == 0);
    car->HandleCrewFinished();
    CHECK(car->GetDispatchState() == 2 && patrol.removed == 1);       // nothing to remove: the manager is not asked

    patrol.stationKnown = false;
    car->SetDispatchState(1);                                          // blocked: crew with the default competence
    CHECK(car->RiotPolice() == &patrol.crew && patrol.setupCompetence == 1.0f);
    patrol.stationKnown = true;
    car->SetDispatchState(3);                                          // driving to the call: the crew is released
    CHECK(car->RiotPolice() == nullptr);

    // at the call (5): during a riot the crew comes; otherwise a criminal in reach starts the arrest state, none the idle state
    patrol.riot = true;
    car->SetDispatchStateRaw(0);
    car->SetDispatchState(5);
    CHECK(car->RiotPolice() == &patrol.crew);
    patrol.riot = false;
    car->SetDispatchStateRaw(0);
    car->SetDispatchState(5);
    CHECK(car->RiotPolice() == nullptr && car->GetState() && car->GetState()->stateType == kStateIdleTick && car->GetState()->timer == 2000.0f);
    car->PopState();
    patrol.participantsLeft = 2;
    car->SetDispatchStateRaw(0);
    car->SetDispatchState(5);
    CHECK(car->GetState() && car->GetState()->stateType == kPatrolStateArrest);

    // the arrest state: the timer counts down, then everybody in reach is arrested and the competence picks the animation
    car->GetState()->timer = 1000.0f;
    patrol.participantsLeft = 3;
    CHECK(car->DoStateTick(400.0f) && car->GetState()->timer == 600.0f && patrol.participant.arrests == 0 && patrol.replaced == 0);
    CHECK(car->DoStateTick(700.0f));
    CHECK(patrol.participant.arrests == 3 && patrol.participantsLeft == 0);
    patrol.participantsLeft = 50;
    patrol.participant.arrests = 0;
    car->GetState()->timer = 1.0f;
    car->DoStateTick(2.0f);
    CHECK(patrol.participant.arrests == 5);                           // at most five arrests per tick
    patrol.participantsLeft = 0;

    // a lone criminal automaton fades out and walks away when nobody can be arrested
    cSC4Vehicle* thief = MakeVehicle(world);
    patrol.criminal = thief;
    car->GetState()->timer = 1.0f;
    car->DoStateTick(2.0f);
    CHECK(thief->GetState() && thief->GetState()->stateType == kStateIdleTick && thief->GetState()->timer == 5000.0f);
    patrol.criminal = nullptr;
    thief->ClearStateStack();
    thief->Release();

    // the exit of the arrest state restarts the first animation; the player taking the wheel drops the crew
    CHECK(!car->HandleExitState(nullptr));
    car->CreateRiotPolice();
    tAutomataStatePlayerDrive drive;
    drive.stateType = kStatePlayerDrive;
    CHECK(car->HandleEnterState(&drive) && car->RiotPolice() == nullptr);

    // the stream: version 2, the emergency vehicle's record, the crew (null here)
    PortMemoryOStream out;
    CHECK(car->Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* copy = new cSC4PatrolCar(&world.host, &world.automatonHost, nullptr, &emergencyHost, &patrol);
    copy->AddRef();
    CHECK(copy->Read(in) && copy->RiotPolice() == nullptr);
    const std::uint8_t badVersion[2] = {9, 0};
    PortMemoryIStream bad(badVersion, sizeof(badVersion));
    CHECK(!copy->Read(bad));
    copy->Release();

    // shutting down releases the crew first
    car->CreateRiotPolice();
    const int before = patrol.removed;
    car->ClearStateStack();
    car->Shutdown();
    CHECK(patrol.removed == before + 1 && car->RiotPolice() == nullptr);
    car->Release();
}

// ---- cSC4FireTruck ------------------------------------------------------------------------------------------------------------
struct FakeFireHost final : IFireTruckHost {
    FakeCrew crew, fireFilter, spillFilter;
    bool stationKnown = true;
    float competence = 0.75f, radius = 32.0f, maxStream = 10.0f;
    int fires = 0, nearbyFires = 0, spills = 0, nearbySpills = 0;
    int created = 0, registered = 0, setups = 0, removed = 0;
    float crewCompetence = -1.0f;
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
    int rect[4] = {0, 0, 0, 0};
    bool pickHit = true;
    cS3DVector3 picked{0, 0, 0};
    int cityW = 8, cityH = 8;
    bool hasCity = true;
    cIGZUnknown* NewFireFilter() override { return &fireFilter; }
    cIGZUnknown* NewSpillFilter() override { return &spillFilter; }
    bool FireCompetence(cIGZUnknown*, float& value) override { value = competence; return stationKnown; }
    bool StationRadius(cIGZUnknown*, float& value) override { value = radius; return stationKnown; }
    int FireCount() override { return fires; }
    bool OccupantsInBox(const float min[3], const float max[3], cIGZUnknown*, int& count) override {
        for (int i = 0; i < 3; ++i) { boxMin[i] = min[i]; boxMax[i] = max[i]; }
        count = nearbyFires;
        return nearbyFires > 0;
    }
    int SpillTotal() override { return spills; }
    int SpillsInRect(const int r[4]) override { for (int i = 0; i < 4; ++i) rect[i] = r[i]; return nearbySpills; }
    cIGZUnknown* NewFireCrew(cSC4FireTruck&) override { ++created; return &crew; }
    void SetFireCrewCompetence(cIGZUnknown*, float value) override { crewCompetence = value; }
    void RegisterFireCrew(cIGZUnknown*) override { ++registered; }
    void SetupFireCrew(cIGZUnknown*) override { ++setups; }
    void RemoveFireCrew(cIGZUnknown*) override { ++removed; }
    bool PickGroundPoint(float, float, cS3DVector3& out) override { out = picked; return pickHit; }
    bool CityCells(int& w, int& h) override { w = cityW; h = cityH; return hasCity; }
    float MaxStreamDistance() override { return maxStream; }
};

void TestFireTruck() {
    World world;
    FakeEmergencyHost emergencyHost;
    FakeFireHost fire;
    auto* truck = new cSC4FireTruck(&world.host, &world.automatonHost, nullptr, &emergencyHost, &fire);
    truck->AddRef();
    CHECK(truck->GetGZCLSID() == 0xaa222b83 && truck->GetDispatchType() == 1);
    CHECK(truck->FireFilter() == &fire.fireFilter && truck->SpillFilter() == &fire.spillFilter && fire.fireFilter.refs == 1);
    CHECK(truck->GetEffectiveRadius() == 16.0f);
    auto* source = new FakeBaseOccupant();
    truck->SetDispatchSource(source);
    source->refs--;
    CHECK(truck->GetEffectiveRadius() == 32.0f);

    // the counts: a box of the reach around the truck, a square of cells for the spills (radius * 3 / 16 cells)
    const float where[3] = {100.0f, 5.0f, 200.0f};
    truck->SetPosition(where);
    fire.fires = 3; fire.nearbyFires = 2; fire.spills = 4; fire.nearbySpills = 1;
    int cityFires = 0, nearFires = 0, citySpills = 0, nearSpills = 0;
    truck->GetFireCount(cityFires, nearFires);
    CHECK(cityFires == 3 && nearFires == 2);
    CHECK(fire.boxMin[0] == 68.0f && fire.boxMax[0] == 132.0f && fire.boxMin[2] == 168.0f && fire.boxMax[2] == 232.0f);
    CHECK(fire.boxMin[1] < -3.0e38f && fire.boxMax[1] > 3.0e38f);
    truck->GetToxicSpillCount(citySpills, nearSpills);
    CHECK(citySpills == 4 && nearSpills == 1 && fire.rect[2] - fire.rect[0] == 12 && fire.rect[3] - fire.rect[1] == 12);   // (int)(32 * 3 / 16) = 6

    // the crew: competence from the station (1.0 when unknown), one per truck
    truck->CreateFireCrew();
    CHECK(fire.created == 1 && fire.crewCompetence == 0.75f && fire.registered == 1 && fire.setups == 1 && truck->FireCrew() == &fire.crew);
    truck->CreateFireCrew();
    CHECK(fire.created == 1);
    truck->ReleaseFireCrew();
    CHECK(fire.removed == 1 && truck->FireCrew() == nullptr && fire.crew.refs == 0);

    // dispatch states: a blocked call (1) brings an unconditioned crew, at the call (5) only something to fight does
    truck->SetDispatchState(1);
    CHECK(truck->FireCrew() == &fire.crew && fire.crewCompetence == 0.75f);
    fire.crewCompetence = -1.0f;
    truck->SetDispatchState(3);
    CHECK(truck->FireCrew() == nullptr);
    fire.nearbyFires = 0; fire.nearbySpills = 0;
    truck->SetDispatchStateRaw(0);
    truck->SetDispatchState(5);
    CHECK(truck->FireCrew() == nullptr);                               // nothing near: no crew
    fire.nearbyFires = 1;
    truck->SetDispatchStateRaw(0);
    truck->SetDispatchState(5);
    CHECK(truck->FireCrew() == &fire.crew);

    // handling the event: a finished call with something burning near goes back to work; none near and nothing anywhere goes home
    truck->ReleaseFireCrew();
    truck->SetDispatchStateRaw(2);
    CHECK(truck->DoHandlingEventTick(1.0f) && truck->GetDispatchState() == 5 && truck->FireCrew() == &fire.crew);
    fire.nearbyFires = 0;
    CHECK(truck->DoHandlingEventTick(1.0f) && truck->GetDispatchState() == 2 && truck->FireCrew() == nullptr);   // 5, nothing near: done
    truck->HandleCrewFinished();
    CHECK(truck->GetDispatchState() == 2);

    // the keys: the stop key with the truck stopped starts the crew, the fire key with a crew out sends it home
    truck->SetDispatchStateRaw(3);
    truck->HandlePlayerDriveCommand(kFireTruckCommandStop, false);
    CHECK(truck->FireCrew() == &fire.crew && truck->GetDispatchState() == 5);
    truck->HandlePlayerDriveCommand(kFireTruckCommandFire, true);
    CHECK(truck->FireCrew() == nullptr && truck->GetDispatchState() == 2);
    truck->HandlePlayerDriveCommand(0, true);                           // keys 0 and 1 drop a crew; none out here

    // the stream target and its clamps
    tAutomataStatePlayerDrive drive;
    drive.history[8] = -1.0f;
    fire.picked = Vec(500.0f, 7.0f, -3.0f);
    truck->TranslatePlayerDriveCommands(&drive, 1.0f);
    CHECK(drive.effectPosition248.x == 0.0f && drive.effectPosition248.y == 0.0f);      // key not held: untouched
    drive.history[8] = 0.0f;
    fire.maxStream = 1000.0f;
    truck->TranslatePlayerDriveCommands(&drive, 1.0f);
    CHECK(drive.effectPosition248.x == 127.0f && drive.effectPosition248.z == 0.0f && drive.effectPosition248.y == 7.0f);   // clamped to 8 cells * 16 - 1
    fire.maxStream = 10.0f;
    fire.cityW = fire.cityH = 32;
    fire.picked = Vec(100.0f, 5.0f, 230.0f);
    truck->TranslatePlayerDriveCommands(&drive, 1.0f);
    CHECK(std::fabs(drive.effectPosition248.z - 210.0f) < 1e-3f && drive.effectPosition248.x == 100.0f);   // 30 away, reach 10
    CHECK(!truck->GetStreamTargetPoint(*new cS3DVector3()));            // no player-drive state on the truck

    // the stream: version 2, the emergency vehicle's record, the crew (null here)
    PortMemoryOStream out;
    CHECK(truck->Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* copy = new cSC4FireTruck(&world.host, &world.automatonHost, nullptr, &emergencyHost, &fire);
    copy->AddRef();
    CHECK(copy->Read(in) && copy->FireCrew() == nullptr);
    const std::uint8_t badVersion[2] = {9, 0};
    PortMemoryIStream bad(badVersion, sizeof(badVersion));
    CHECK(!copy->Read(bad));
    copy->Release();

    truck->CreateFireCrew();
    truck->ClearStateStack();
    truck->Shutdown();
    CHECK(truck->FireCrew() == nullptr && truck->FireFilter() == nullptr && truck->SpillFilter() == nullptr);
    truck->Release();
}

// ---- cSC4Pedestrian ----------------------------------------------------------------------------------------------------
struct FakePedestrianHost final : IPedestrianHost {
    FakeTrafficHost traffic;
    PedestrianTuning tuning;
    std::vector<cISC4Occupant*> boxOccupants;   // each is handed out with one extra reference, as the occupant manager does
    int boxQueries = 0;
    cISC4Occupant* ignited = nullptr;
    cSC4TrafficNetworkMap* TrafficNetworkMap() override { return traffic.TrafficNetworkMap(); }
    float CitySizeX() override { return traffic.CitySizeX(); }
    float CitySizeZ() override { return traffic.CitySizeZ(); }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { traffic.PositionToCell(x, z, cellX, cellZ); }
    int CityCellsX() override { return traffic.CityCellsX(); }
    int CityCellsZ() override { return traffic.CityCellsZ(); }
    ITrafficAutomataManager* TrafficAutomataManager() override { return traffic.TrafficAutomataManager(); }
    cISC4PathFinder* PathFinder() override { return nullptr; }
    void NeighborConnectionCells(std::vector<std::int32_t>&) override {}
    void StartAudioEvent(std::uint32_t) override {}
    void StopAudioEvent(std::uint32_t) override {}
    ITrafficParticleSystem* CreateParticleSystem() override { return nullptr; }
    ITrafficAutomatonOccupant* ReadNetworkOccupant(cIGZIStream&) override { return nullptr; }
    const TrafficAutomatonTuning* Tuning() override { return traffic.Tuning(); }
    void OccupantsInBox(const float*, const float*, std::uint32_t type, int, std::vector<cISC4Occupant*>& out) override {
        ++boxQueries;
        CHECK(type == kPedestrianOccupantType);
        for (cISC4Occupant* occupant : boxOccupants) { occupant->AddRef(); out.push_back(occupant); }
    }
    void IgniteOccupant(cISC4Occupant* occupant) override { ignited = occupant; }
    ITrafficNetworkMapLot* LotAt(int, int) override { return nullptr; }
    const PedestrianTuning* PedestrianTuningTable() override { return &tuning; }
};

struct PedestrianController final : cSC4AutomataController {
    int ticks = 0;
    cISC4Occupant* occupant = nullptr;
    float position[3] = {20.0f, 0.0f, 0.0f};
    bool CheckFlag(std::uint32_t) const override { return false; }
    void GetPosition(float* xyz) const override { for (int i = 0; i < 3; ++i) xyz[i] = position[i]; }
    cISC4Occupant* GetOccupant() const override { return occupant; }
    bool HandleStateTick(IControllerAutomaton*, std::uint32_t, float) override { ++ticks; return false; }
    bool HandleEnterState(IControllerAutomaton*, std::uint32_t) override { return false; }
    bool HandleExitState(IControllerAutomaton*, std::uint32_t) override { return false; }
};

cSC4Pedestrian* MakePedestrian(World& world, FakePedestrianHost& host) {
    host.traffic.map = world.map;
    host.traffic.manager = &world.manager;
    SetAutomatonHost(&world.automatonHost);   // the pedestrian reaches the automaton host through the global, like the original
    auto* pedestrian = new cSC4Pedestrian(&host);
    pedestrian->AddRef();
    return pedestrian;
}

void TestPedestrian() {
    World world;
    FakePedestrianHost host;
    cSC4Pedestrian* pedestrian = MakePedestrian(world, host);
    CHECK(pedestrian->GetType() == kPedestrianOccupantType);
    CHECK(pedestrian->GetGZCLSID() == kGZCLSID_cSC4Pedestrian);
    CHECK_NEAR(pedestrian->GetRadius(), 0.5f);
    CHECK_NEAR(pedestrian->AnimationSpeed(), 0.01f);
    void* out = nullptr;
    CHECK(pedestrian->QueryInterface(kGZIID_cISC4Pedestrian, &out) && out == pedestrian);
    CHECK(pedestrian->QueryInterface(kGZIID_cISC4TrafficAutomaton, &out) && out == pedestrian);

    // UpdateOrientation: a unit ground-plane heading to the target; on the position the heading is (0, 0, 1) (z lane).
    const float at[3] = {10.0f, 0.0f, 20.0f};
    pedestrian->SetPosition(at);
    float heading[3] = {9, 9, 9};
    pedestrian->UpdateOrientation(Vec(13.0f, 50.0f, 24.0f), heading);
    CHECK_NEAR(heading[0], 0.6f);
    CHECK_NEAR(heading[1], 0.0f);
    CHECK_NEAR(heading[2], 0.8f);
    pedestrian->UpdateOrientation(Vec(10.0f, 3.0f, 20.0f), heading);
    CHECK_NEAR(heading[0], 0.0f);
    CHECK_NEAR(heading[1], 0.0f);
    CHECK_NEAR(heading[2], 1.0f);

    // HandleCrowdReactionEvent: event 0 -> state 1, anything else -> 2; the automaton turns towards the event on the ground
    // plane (the decompile loses the direction argument); flag 0x4000 refuses the reaction.
    CHECK(pedestrian->HandleCrowdReactionEvent(Vec(10.0f, 0.0f, 30.0f), 0));
    CHECK(pedestrian->CrowdReactionState() == 1);
    float orientation[3] = {0, 0, 0};
    pedestrian->GetOrientation(orientation);
    CHECK_NEAR(orientation[0], 0.0f);
    CHECK_NEAR(orientation[2], 1.0f);
    CHECK(pedestrian->HandleCrowdReactionEvent(Vec(10.0f, 0.0f, 30.0f), 7));
    CHECK(pedestrian->CrowdReactionState() == 2);
    pedestrian->SetAutomataFlag(kAutomataFlagIgnoreAttractors, true);
    pedestrian->SetCrowdReactionState(0);
    CHECK(!pedestrian->HandleCrowdReactionEvent(Vec(0.0f, 0.0f, 0.0f), 0));
    CHECK(pedestrian->CrowdReactionState() == 0);
    pedestrian->SetAutomataFlag(kAutomataFlagIgnoreAttractors, false);

    // TestCollisionExpensive: nothing happens until 500 ms have accumulated, then the box query runs once and resets the timer.
    cSC4Pedestrian* other = MakePedestrian(world, host);
    const float otherAt[3] = {10.0f, 0.0f, 20.5f};
    other->SetPosition(otherAt);
    host.boxOccupants = {other->AsOccupant()};
    CHECK(!pedestrian->TestCollisionExpensive(Vec(10.0f, 0.0f, 20.0f), 250.0f));
    CHECK(host.boxQueries == 0);
    CHECK_NEAR(pedestrian->CollisionTimer(), 250.0f);
    CHECK(pedestrian->TestCollisionExpensive(Vec(10.0f, 0.0f, 20.0f), 250.0f));
    CHECK(host.boxQueries == 1);
    CHECK_NEAR(pedestrian->CollisionTimer(), 0.0f);
    // The other pedestrian is pushed along the vector from the target to it; the extra reference taken by the query is released.
    CHECK(other->CrowdReactionDirectionSet());
    CHECK_NEAR(other->CrowdReactionDirection().x, 0.0f);
    CHECK_NEAR(other->CrowdReactionDirection().z, 0.5f);
    // Nearly on top of the target: a random ground-plane direction (the fake random returns the lower bound -0.5).
    other->SetCrowdReactionDirectionSet(false);
    const float onTop[3] = {10.0f, 0.0f, 20.001f};
    other->SetPosition(onTop);
    CHECK(pedestrian->TestCollisionExpensive(Vec(10.0f, 0.0f, 20.0f), 600.0f));
    CHECK(other->CrowdReactionDirectionSet());
    CHECK_NEAR(other->CrowdReactionDirection().x, -0.5f);
    CHECK_NEAR(other->CrowdReactionDirection().y, 0.0f);
    CHECK_NEAR(other->CrowdReactionDirection().z, -0.5f);
    // Itself is skipped, and a pedestrian a metre or more away does not count.
    host.boxOccupants = {pedestrian->AsOccupant()};
    CHECK(!pedestrian->TestCollisionExpensive(Vec(10.0f, 0.0f, 20.0f), 600.0f));
    const float far[3] = {10.0f, 0.0f, 25.0f};
    other->SetPosition(far);
    host.boxOccupants = {other->AsOccupant()};
    CHECK(!pedestrian->TestCollisionExpensive(Vec(10.0f, 0.0f, 20.0f), 600.0f));
    host.boxOccupants.clear();

    // VerifyNetworkOccupant: no network tile under the pedestrian.
    CHECK(!pedestrian->VerifyNetworkOccupant());

    // MoveForwardWithCollision: a free move walks speed * ms along the direction and clears the pending push.
    pedestrian->SetPosition(at);
    pedestrian->SetSpeed(0.01f, 0.0f);
    pedestrian->SetCrowdReactionDirectionSet(true);
    CHECK(pedestrian->MoveForwardWithCollision(Vec(1.0f, 0.0f, 0.0f), 100.0f));
    CHECK(!pedestrian->CrowdReactionDirectionSet());
    float moved[3] = {0, 0, 0};
    pedestrian->GetPosition(moved);
    CHECK(moved[0] > at[0]);
    CHECK_NEAR(moved[2], at[2]);

    // The queue state with no controller pops itself; DoStateTick has nothing to run on an empty stack.
    CHECK(!pedestrian->DoStateTick(16.0f));
    CHECK(pedestrian->PushState(kStateIdleTick, nullptr, 0.0f, false, 0, 0, nullptr));
    CHECK(pedestrian->DoQueueTick(16.0f));
    CHECK(pedestrian->StateStack().empty());

    // DoIgniteTick: the controller's occupant is handed to the disaster layer, then the pedestrian goes idle (state 1).
    auto* controller = new PedestrianController();
    controller->AddRef();
    controller->occupant = other->AsOccupant();
    CHECK(pedestrian->PushState(kPedestrianStateIgnite, controller, 0.0f, false, 0, 0, nullptr));
    CHECK(pedestrian->DoIgniteTick(16.0f));
    CHECK(host.ignited == other->AsOccupant());
    CHECK(pedestrian->GetState() != nullptr && pedestrian->GetState()->stateType == kStateIdle);
    controller->Release();

    // Persistence: version 5, the base record, then the push flag and direction, the animation speed and the reaction state.
    pedestrian->SetCrowdReactionDirectionSet(true);
    pedestrian->SetCrowdReactionDirection(Vec(1.0f, 2.0f, 3.0f));
    pedestrian->SetAnimationSpeed(0.25f);
    pedestrian->SetCrowdReactionState(2);
    PortMemoryOStream stream;
    CHECK(pedestrian->Write(stream));
    PortMemoryIStream in(stream.bytes().data(), stream.bytes().size());
    cSC4Pedestrian* copy = MakePedestrian(world, host);
    CHECK(copy->Read(in));
    CHECK(copy->CrowdReactionDirectionSet());
    CHECK_NEAR(copy->CrowdReactionDirection().y, 2.0f);
    CHECK_NEAR(copy->CrowdReactionDirection().z, 3.0f);
    CHECK_NEAR(copy->AnimationSpeed(), 0.25f);
    CHECK(copy->CrowdReactionState() == 2);
    const std::uint8_t bad[2] = {4, 0};
    PortMemoryIStream badStream(bad, sizeof(bad));
    CHECK(!copy->Read(badStream));
    copy->Release();

    // ---- cSC4PedestrianOneShot (i386 0x000b1c0a..0x000b28cc): stands, plays its animation a rolled number of times, leaves ----
    {
        host.tuning.oneShotAnimMin = 2;
        host.tuning.oneShotAnimMax = 4;
        auto* shot = new cSC4PedestrianOneShot(&host);
        shot->AddRef();
        CHECK(shot->GetGZCLSID() == 0xaa09dc28 && shot->GetAnimCount() == 5);
        void* answer = nullptr;
        CHECK(shot->QueryInterface(kGZCLSID_cSC4PedestrianOneShot, &answer) && answer == shot);
        shot->Release();
        CHECK(shot->QueryInterface(kGZIID_cISC4Pedestrian, &answer) && answer == shot);
        shot->Release();
        shot->AddModelResourceKey(cGZPersistResourceKey{0x5ad0e817, 0xbadb57f1, 0x0b400000});
        CHECK(shot->Init());
        CHECK(shot->GetAnimCount() == 4);                      // 2 + roll(4 - 2 + 1), this host rolls range - 1
        FakeModel* model = world.automatonHost.lastModel;
        CHECK(model && model->autoRate == kPedestrianAnimationSpeed);
        if (model) {
            model->frameCount = 3;
            const float before[3] = {shot->Position().x, shot->Position().y, shot->Position().z};
            CHECK(shot->DoAnimationTick(0, 16.0f));            // mid animation: nothing is counted
            CHECK(model->animationUpdates == 1 && shot->GetAnimCount() == 4);
            model->frame = 2;                                  // the last frame uses one play up per tick
            CHECK(shot->DoAnimationTick(0, 16.0f) && shot->GetAnimCount() == 3);
            CHECK(shot->DoAnimationTick(0, 16.0f) && shot->GetAnimCount() == 2);
            CHECK(shot->DoAnimationTick(0, 16.0f) && shot->GetAnimCount() == 1 && !shot->GetMarkedForRemove());
            CHECK(!shot->DoAnimationTick(0, 16.0f) && shot->GetMarkedForRemove());   // the last one: it leaves
            CHECK(shot->Position().x == before[0] && shot->Position().z == before[2]);   // and never walked
        }
        shot->Release();

        // the exemplar's own range is not set here, so an equal low and high gives exactly that count; a count of 0 leaves at once
        host.tuning.oneShotAnimMin = 1;
        host.tuning.oneShotAnimMax = 1;
        auto* once = new cSC4PedestrianOneShot(&host);
        once->AddRef();
        once->AddModelResourceKey(cGZPersistResourceKey{0x5ad0e817, 0xbadb57f1, 0x0b300000});
        CHECK(once->Init() && once->GetAnimCount() == 1);
        CHECK(!once->DoAnimationTick(0, 16.0f) && once->GetMarkedForRemove());
        once->Release();
    }

    other->Release();
    pedestrian->Release();
    SetAutomatonHost(nullptr);
}

int main() {
    TestConstructionAndQueries();
    TestGeometry();
    TestAnimationAndHeadlights();
    TestTweensAndSpeed();
    TestStateMachine();
    TestPaths();
    TestStartTripAndPathTick();
    TestCollisionAndDamage();
    TestPersistence();
    TestConstructionSpeed();
    TestSafeDistanceBehindALeader();
    TestLoadedSpeedIsBroughtIntoRange();
    TestLotIntersection();
    TestPlayerDriveTickAndOffRoad();
    TestEmergencyVehicle();
    TestPatrolCar();
    TestFireTruck();
    TestPedestrian();
    TestTrailerVehicle();
    if (failures == 0) {
        std::printf("sc4_vehicle: ok\n");
        return 0;
    }
    std::printf("sc4_vehicle: %d failure(s)\n", failures);
    return 1;
}
