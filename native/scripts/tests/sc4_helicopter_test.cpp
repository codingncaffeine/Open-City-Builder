// cSC4Helicopter — construction and the layout constants, Init (drive parameters, effects and the helipad registration
// point), the rotor/HELI_DATA setup, the state machine, the stream round trip, PlaceSelf/GetNewDirection, the heading
// derivation, the flight-model helpers (engine power, hover, weathervane, drag, lift, ApplyForce), the player-drive
// control translation, the collision sweep, the AI idle/arrive behaviours and the dispatch identity. The automaton,
// terrain, lot, effects, renderer, crime and traffic neighbours are fakes behind IAutomatonHost / IAircraftHost /
// IHelicopterHost.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataStates.h"
#include "sc4/Helicopter.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-3f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

// ---- fake created effects -------------------------------------------------------------------------------------------
struct FakeEffect final : IAirplaneEffect {
    std::string name;
    int refs = 0, addRefs = 0, releases = 0, starts = 0, stops = 0, transforms = 0;
    bool playing = false;
    cS3DTransform transform;
    void AddRef() override { ++refs; ++addRefs; }
    void Release() override { --refs; ++releases; }
    void Start(int) override { ++starts; playing = true; }
    void Stop(bool) override { ++stops; playing = false; }
    bool IsPlaying() override { return playing; }
    void SetTransform(const cS3DTransform& value) override { transform = value; ++transforms; }
    void SetPosition(const cS3DVector3&) override {}
};

struct EffectRegistry {
    std::map<std::string, std::unique_ptr<FakeEffect>> effects;
    std::vector<std::string> created;
    FakeEffect* Add(const std::string& name) {
        auto& slot = effects[name];
        if (!slot) { slot = std::make_unique<FakeEffect>(); slot->name = name; }
        return slot.get();
    }
    FakeEffect* Find(const std::string& name) {
        auto it = effects.find(name);
        return it == effects.end() ? nullptr : it->second.get();
    }
    int Releases() const { int total = 0; for (const auto& e : effects) total += e.second->releases; return total; }
};

// ---- automaton-level fake -------------------------------------------------------------------------------------------
struct FakeEffects final : IAutomatonEffects {
    EffectRegistry* registry = nullptr;
    int updates = 0, starts = 0, stops = 0;
    std::vector<std::string> startNamed, stopNamed;
    void SetModel(IAutomatonModel*) override {}
    bool Init() override { return true; }
    void Update(const cS3DVector3&, const cS3DVector3&, float) override { ++updates; }
    void StartAll() override { ++starts; }
    void StopAll(bool) override { ++stops; }
    void StartNamed(const cRZString& effect, int, bool) override { startNamed.push_back(effect.ToChar()); }
    void StopNamed(const cRZString& effect, int) override { stopNamed.push_back(effect.ToChar()); }
    void ToggleNamed(const cRZString&, int, bool) override {}
    void RecreateNamed(const cRZString&, int, bool) override {}
    bool AddRegEffect(const cRZString&, const cRZString&) override { return true; }
    void* FindRegEffect(const cRZString& effect) override {
        return registry ? registry->Find(effect.ToChar()) : nullptr;
    }
    void RemoveRegEffect(const cRZString&) override {}
    void Shutdown() override {}
};

struct FakeModel final : IAutomatonModel {
    int refs = 1;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void GetBounds(float* minOut, float* maxOut) override {
        const float min[3] = {-2, 0, -2}, max[3] = {2, 3, 2};
        std::memcpy(minOut, min, sizeof(min));
        std::memcpy(maxOut, max, sizeof(max));
    }
    void GetTransform(cS3DTransform&) override {}
    void SetTransform(const cS3DTransform&) override {}
    void SetOpacity(float, bool) override {}
    void UpdateAnimation(float) override {}
    int GetFrame() const override { return 0; }
    int GetFrameCount() const override { return 1; }
    void SetZoomAndRotation(int, int) override {}
    void SetPlayMode(s3d::PlayMode) override {}
};

struct FakeGroup final : IAutomatonGroupTemplate {
    std::uint32_t id = 0x4200;
    FakeGroup* parent = nullptr;
    std::uint32_t GroupId() const override { return id; }
    IAutomatonGroupTemplate* Parent() const override { return parent; }
    float SpeedMin() const override { return 1.0f; }
    float SpeedMax() const override { return 2.0f; }
    void GetGroupHierarchyDepth() override {}
};

struct FakeAutomatonHost final : IAutomatonHost {
    FakeEffects effects;
    FakeGroup group;
    FakeModel model;
    int posted = 0;
    void SetRegistry(EffectRegistry* registry) { effects.registry = registry; }
    IAutomatonModel* CreateModel(const cGZPersistResourceKey&) override { return &model; }
    void DestroyModel(IAutomatonModel*) override {}
    IAutomatonEffects* CreateEffects() override { return &effects; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { return true; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { return true; }
    void FadeModelIn(IAutomatonModel*) override {}
    void FadeModelOut(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return true; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return true; }
    bool HasCity() override { return true; }
    float CitySizeX() override { return 4096.0f; }
    float CitySizeZ() override { return 4096.0f; }
    float TerrainHeight(float, float) override { return 0.0f; }
    bool NetworkAt(int, int) override { return false; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, const cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t id) override { return id == group.id ? &group : nullptr; }
    bool AnimationUpdateAllowed() override { return true; }
    float SimSpeedModifier() override { return 1.0f; }
    float MaxSpeedUnits() override { return 3600.0f; }
    float FadeDurationSeconds() override { return 0.5f; }
    void PostMessage(cIGZMessage2* message) override { ++posted; messageTypes.push_back(message->GetType()); }
    std::uint32_t GenerateSoundInstanceId() override { return ++soundId; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t) override { return 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double lo, double) override { return lo; }
    double RandomUnit() override { return 0.0; }
    std::uint32_t soundId = 100;
    std::vector<std::uint32_t> messageTypes;
};

// ---- aircraft-level fake --------------------------------------------------------------------------------------------
struct FakeAircraftHost final : IAircraftHost {
    EffectRegistry* registry = nullptr;
    bool inBounds = true;
    float terrainHeight = 10.0f, waterHeight = 20.0f, defaultAltitude = -7.0f, altitudeClamp = 1000.0f;
    float citySizeX = 4096.0f, citySizeZ = 4096.0f;
    int cellCountX = 64, cellCountZ = 64;
    bool lotFound = false;
    float lotAltitude = 30.0f;
    float effectsWaterHeight = 5.0f;
    bool fireFound = false;
    cS3DVector3 firePosition{111.0f, 0.0f, 222.0f};
    double randomValues[2] = {34.0, 56.0};
    int randomCalls = 0;
    std::vector<float> driveParameters;
    std::vector<float> playerDriveParameters;
    bool regPointFound = false;
    cS3DVector3 regPoint{1.0f, 2.0f, 3.0f};
    bool disasterActive = false;
    float disasterMultiplier = 2.0f;
    bool simulatorPaused = false, simulatorAdvancing = true;
    int viewZoom = 1;
    std::vector<std::uint32_t> audioPlayed, audioStopped;
    int worldToCellCalls = 0;

    bool TerrainPointInBounds(float, float) override { return inBounds; }
    float TerrainHeight(float, float) override { return terrainHeight; }
    float WaterHeight(float, float) override { return waterHeight; }
    float DefaultAltitude() override { return defaultAltitude; }
    float AltitudeClamp() override { return altitudeClamp; }
    float TallestBuildingAltitude() override { return 0.0f; }
    void GetRunwayList(const cS3DVector3&, std::vector<cS3DVector3>&) override {}
    std::uint32_t RandomWeightedChoice(std::uint32_t, const double*) override { return 0; }
    const void* LotAt(const cS3DVector3&) override { return nullptr; }
    float OpacityFadeSeconds() override { return 1.0f; }
    bool CellHasWater(std::uint32_t, std::uint32_t) override { return false; }
    bool FindCollider(AircraftColliderQuery, const float*, const float*, AircraftCollider&) override { return false; }
    bool LotAltitudeAt(const cS3DVector3&, float* altitude) override {
        if (!lotFound) return false;
        *altitude = lotAltitude;
        return true;
    }
    float CitySizeX() override { return citySizeX; }
    float CitySizeZ() override { return citySizeZ; }
    int CityCellCountX() override { return cellCountX; }
    int CityCellCountZ() override { return cellCountZ; }
    void WorldToCell(float, float, std::int32_t& cellX, std::int32_t& cellZ) override {
        ++worldToCellCalls;
        cellX = 3; cellZ = 4;
    }
    bool FindFireOccupant(const cS3DVector3&, const cS3DVector3&, cISC4OccupantFilter*,
                          cS3DVector3& position) override {
        if (!fireFound) return false;
        position = firePosition;
        return true;
    }
    double RandomDoubleRange(double lo, double) override {
        const double value = randomValues[randomCalls < 2 ? randomCalls : 1];
        ++randomCalls;
        (void)lo;
        return value;
    }
    const float* FindDriveParameters(IAutomatonGroupTemplate*) override {
        return driveParameters.size() == 13 ? driveParameters.data() : nullptr;
    }
    const float* FindPlayerDriveParameters(IAutomatonGroupTemplate*) override {
        return playerDriveParameters.size() == 0x28 ? playerDriveParameters.data() : nullptr;
    }
    IAirplaneEffect* CreateEffect(const cRZString& name) override {
        registry->created.push_back(name.ToChar());
        FakeEffect* effect = registry->Add(name.ToChar());
        effect->AddRef();
        return effect;
    }
    bool FindRegPointTransform(const cRZString&, cS3DVector3& out) override {
        if (!regPointFound) return false;
        out = regPoint;
        return true;
    }
    float EffectsWaterHeight(float, float) override { return effectsWaterHeight; }
    bool DisasterActive() override { return disasterActive; }
    float DisasterSpeedMultiplier() override { return disasterMultiplier; }
    bool SimulatorPaused() override { return simulatorPaused; }
    bool SimulatorAdvancing() override { return simulatorAdvancing; }
    int ViewZoom() override { return viewZoom; }
    void PlayAudioEvent(std::uint32_t event) override { audioPlayed.push_back(event); }
    void StopAudioEvent(std::uint32_t event) override { audioStopped.push_back(event); }
};

// ---- helicopter-level fake ------------------------------------------------------------------------------------------
struct FakeHeliEffects final : IHelicopterEffects {
    int modelSet = 0, updates = 0, starts = 0, stops = 0, shutdowns = 0;
    std::vector<int> startTransitions, stopTransitions;
    std::vector<std::string> startNamed, stopNamed, regEffects;
    void SetModel(IAutomatonModel*) override { ++modelSet; }
    bool Init() override { return true; }
    void Update() override { ++updates; }
    void StartAll(int transition) override { ++starts; startTransitions.push_back(transition); }
    void StopAll(int transition) override { ++stops; stopTransitions.push_back(transition); }
    bool StartNamed(const cRZString& effect, int, bool) override { startNamed.push_back(effect.ToChar()); return true; }
    void StopNamed(const cRZString& effect, int) override { stopNamed.push_back(effect.ToChar()); }
    bool AddRegEffect(const cRZString& effect, const cRZString& point) override {
        regEffects.push_back(effect.ToChar());
        regEffects.push_back(point.ToChar());
        return true;
    }
    void Shutdown() override { ++shutdowns; }
};

struct FakeMessageTarget final : cIGZMessageTarget2 {
    int refs = 1;
    std::vector<std::uint32_t> events, data;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZMessage2* message) override {
        auto* standard = dynamic_cast<cRZMessage2Standard*>(message);
        if (!standard) return false;
        events.push_back(static_cast<std::uint32_t>(standard->GetData2()));
        data.push_back(static_cast<std::uint32_t>(standard->GetData3()));
        return true;
    }
};

struct FakeObstacle final : IHelicopterObstacle {
    int refs = 0, releases = 0;
    cS3DVector3 minCorner{-1000.0f, -1000.0f, -1000.0f};
    cS3DVector3 maxCorner{1000.0f, 1000.0f, 1000.0f};
    void AddRef() override { ++refs; }
    void Release() override { --refs; ++releases; }
    void GetBounds(cS3DVector3& minOut, cS3DVector3& maxOut) override { minOut = minCorner; maxOut = maxCorner; }
};

struct FakeHeliHost final : IHelicopterHost {
    FakeHeliEffects effectsA, effectsB;
    int created = 0, destroyed = 0;
    FakeObstacle obstacle;
    bool hasObstacleAt = false, hasObstacleInBox = false;
    int endQueries = 0, lotCalls = 0;
    const void* lotHere = reinterpret_cast<const void*>(0x1);
    const void* lotSpawn = reinterpret_cast<const void*>(0x1);
    float cellSizeX = 64.0f, cellSizeZ = 64.0f;
    bool rendererCone = true, rendererHit = true;
    int coneCalls = 0, hitCalls = 0;
    float centreX = 0.0f, centreZ = 0.0f;
    int dispatchReports = 0;
    int lastDispatchState = -1;
    bool trafficAccident = false;
    cS3DVector3 accident{500.0f, 0.0f, 600.0f};
    bool crimeFound = false;
    cS3DVector3 crime{300.0f, 0.0f, 400.0f};
    IHelicopterEffects* CreateEffects() override { ++created; return (created == 1) ? &effectsA : &effectsB; }
    void DestroyEffects(IHelicopterEffects*) override { ++destroyed; }
    void GetEffectTransform(IAirplaneEffect* effect, cS3DTransform& transform) override {
        auto* fake = static_cast<FakeEffect*>(effect);
        transform = fake->transform;
    }
    IHelicopterObstacle* FindObstacleAt(float, float, std::uint32_t) override {
        return hasObstacleAt ? &obstacle : nullptr;
    }
    IHelicopterObstacle* FindObstacleInBox(const cS3DVector3&, const cS3DVector3&, std::uint32_t,
                                           cISC4OccupantFilter*) override {
        return hasObstacleInBox ? &obstacle : nullptr;
    }
    void EndObstacleQuery() override { ++endQueries; }
    const void* LotAtPoint(const cS3DVector3&) override { ++lotCalls; return lotCalls == 1 ? lotHere : lotSpawn; }
    bool RendererSpotlightCone(const cS3DVector3&, const cS3DVector3&, const cS3DVector3&) override {
        ++coneCalls; return rendererCone;
    }
    bool RendererSpotlightHit(float, float, const cS3DVector3&) override { ++hitCalls; return rendererHit; }
    float CityCellSizeX() override { return cellSizeX; }
    float CityCellSizeZ() override { return cellSizeZ; }
    void CityCellCentre(float, float, float& outX, float& outZ) override { outX = centreX; outZ = centreZ; }
    void DispatchManagerReport(cISC4Occupant*, int state) override { ++dispatchReports; lastDispatchState = state; }
    bool TrafficAccidentLocation(cS3DVector3& position) override {
        if (!trafficAccident) return false;
        position = accident;
        return true;
    }
    bool CrimeAt(const cS3DVector3&, float, cS3DVector3& out) override {
        if (!crimeFound) return false;
        out = crime;
        return true;
    }
};

struct Fixture {
    EffectRegistry registry;
    FakeAutomatonHost automatonHost;
    FakeAircraftHost aircraftHost;
    FakeHeliHost heliHost;
    Fixture() {
        automatonHost.SetRegistry(&registry);
        aircraftHost.registry = &registry;
    }
    cSC4Helicopter* Make() {
        auto* helicopter = new cSC4Helicopter(&automatonHost, &aircraftHost, &heliHost);
        helicopter->AddRef();
        return helicopter;
    }
    FakeEffect* Register(const char* name) { return registry.Add(name); }
};

void SetGroup(cSC4Helicopter* helicopter, Fixture& fixture, std::uint32_t groupId) {
    fixture.automatonHost.group.id = groupId;
    helicopter->SetAutomataGroup(&fixture.automatonHost.group);
}

}  // namespace

int main() {
    // ---- construction and defaults ---------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->GetType() == kAircraftOccupantType);
        CHECK(helicopter->IsFlagSet(kAircraftOccupantFlag));
        CHECK(helicopter->GetGZCLSID() == kGZCLSID_cSC4Helicopter);
        CHECK(helicopter->Physics() == nullptr);
        CHECK(helicopter->Engine() == nullptr);
        CHECK(helicopter->PositionLoaded() == false);
        CHECK(helicopter->SpotOn() == false);
        CHECK(helicopter->ArrivedFlag() == false);
        CHECK(helicopter->EffectSearchlight() == nullptr);
        CHECK(helicopter->EffectSpotlight() == nullptr);
        CHECK(helicopter->EffectCloseToGround() == nullptr);
        CHECK(helicopter->EffectIndicate() == nullptr);
        CHECK(helicopter->RotorStill() != nullptr && helicopter->RotorSpin() != nullptr);
        CHECK(f.heliHost.created == 2);
        CHECK(helicopter->SpotAim().x == 0.0f && helicopter->SpotAim().y == -1.0f && helicopter->SpotAim().z == 1.0f);
        float orientation[3] = {9, 9, 9};
        CHECK(helicopter->GetOrientation(orientation));
        CHECK_NEAR(orientation[0], 0.0f);
        CHECK_NEAR(orientation[1], 0.0f);
        CHECK_NEAR(orientation[2], 1.0f);
        float destination[3] = {0, 0, 0};
        helicopter->GetDestination(destination);
        // kInvalidVector: 0xffffffff / 0x01ffffff / 0x4f5f4090 (kept bit-exactly).
        std::uint32_t bits[3];
        std::memcpy(bits, destination, sizeof(bits));
        CHECK(bits[0] == 0xffffffffu && bits[1] == 0x01ffffffu && bits[2] == 0x4f5f4090u);
        CHECK(helicopter->GetCallNumber() == 0);
        CHECK(helicopter->GetDispatchState() == 0);
        CHECK(helicopter->Release() == 0);
    }

    // ---- Init: drive parameters, effects, helipad registration point -------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        SetGroup(helicopter, f, 0x4200);
        f.aircraftHost.driveParameters.assign(13, 0.0f);
        f.aircraftHost.driveParameters[0] = 250.0f;    // mass
        f.aircraftHost.driveParameters[1] = 3.0f;      // engine power scale
        f.aircraftHost.driveParameters[2] = 0.5f;      // drag
        f.aircraftHost.driveParameters[3] = 0.09f;     // max speed 300
        f.aircraftHost.driveParameters[4] = 2.0f;      // damping
        f.aircraftHost.driveParameters[5] = 1.5f;      // control scale
        f.aircraftHost.driveParameters[6] = 3.0f;      // max pitch rate (deg)
        f.aircraftHost.driveParameters[7] = 0.5f;      // max pitch angle
        f.aircraftHost.driveParameters[8] = 2.0f;      // max roll rate
        f.aircraftHost.driveParameters[9] = 1.0f;      // max roll angle
        f.aircraftHost.driveParameters[10] = 0.25f;    // max yaw rate
        f.aircraftHost.driveParameters[11] = 1.0f;     // rotor tilt pitch
        f.aircraftHost.driveParameters[12] = 0.5f;     // rotor tilt roll
        f.aircraftHost.playerDriveParameters.assign(0x28, 0.0f);
        f.aircraftHost.playerDriveParameters[2] = 0.3f;
        f.aircraftHost.playerDriveParameters[0x18] = 1.5f;
        f.aircraftHost.playerDriveParameters[0x19] = 2.5f;
        f.aircraftHost.playerDriveParameters[0x1a] = 3.5f;
        f.aircraftHost.playerDriveParameters[0x1d] = 0.25f;
        f.aircraftHost.regPointFound = true;
        const float position[3] = {100.0f, 50.0f, 200.0f};
        helicopter->SetPosition(position);
        f.Register("fx_00_searchlight");
        const cGZPersistResourceKey modelKey{0x8df32, 1, 1};
        CHECK(helicopter->AddModelResourceKey(modelKey));
        CHECK(helicopter->Init());
        CHECK(helicopter->IsInitialized());
        CHECK(helicopter->Physics() != nullptr);
        CHECK(helicopter->Engine() != nullptr);
        // The helipad registration point (1,2,3) is subtracted from the spawn position (facing is +Z, so unrotated).
        CHECK_NEAR(helicopter->SpawnPosition().x, 99.0f);
        CHECK_NEAR(helicopter->SpawnPosition().y, 48.0f);
        CHECK_NEAR(helicopter->SpawnPosition().z, 197.0f);
        CHECK_NEAR(helicopter->SpawnAltitude(), 48.0f);
        // The type record's drive block: mass 250, engine scale 3, damping 0.5, max speed 300, control 1.5.
        CHECK_NEAR(helicopter->Physics()->mass, 250.0f);
        CHECK_NEAR(helicopter->PlayerYawRate(), 1.5f);
        CHECK_NEAR(helicopter->PlayerPitchRate(), 2.5f);
        CHECK_NEAR(helicopter->PlayerRudderRate(), 3.5f);
        CHECK_NEAR(helicopter->PlayerSlowScale(), 0.25f);
        // Effects: searchlight from the registered list, the other three created.
        CHECK(helicopter->EffectSearchlight() != nullptr);
        CHECK(std::string(static_cast<FakeEffect*>(helicopter->EffectSearchlight())->name) == "fx_00_searchlight");
        CHECK(helicopter->EffectSpotlight() != nullptr);
        CHECK(std::string(static_cast<FakeEffect*>(helicopter->EffectSpotlight())->name) == "copter_spotlight");
        CHECK(std::string(static_cast<FakeEffect*>(helicopter->EffectCloseToGround())->name) == "heli_closetoground");
        CHECK(std::string(static_cast<FakeEffect*>(helicopter->EffectIndicate())->name) == "aircraftindicate");
        // Spot aim is normalised to (0, -1/sqrt2, 1/sqrt2).
        CHECK_NEAR(helicopter->SpotAim().x, 0.0f);
        CHECK_NEAR(helicopter->SpotAim().y, -0.70710678f);
        CHECK_NEAR(helicopter->SpotAim().z, 0.70710678f);
        CHECK_NEAR(helicopter->SpotTarget().x, 99.0f);
        // HELI_DATA: heading +Z, main rotor at position/0.3, rotors linked, type limits in 0.1-degree units.
        const HELI_DATA* data = helicopter->Physics();
        CHECK(data->rotors[1] != nullptr && data->rotors[0] != nullptr && data->rotors[9] != nullptr);
        CHECK_NEAR(data->rotors[1]->position.x, 99.0f / 0.3f);
        CHECK_NEAR(data->rotors[1]->position.y, 48.0f / 0.3f);
        CHECK_NEAR(data->heading.z, 1.0f);
        CHECK_NEAR(data->startX, 99.0f);
        CHECK_NEAR(data->startZ, 197.0f);
        CHECK(data->rotors[1]->link0 == data->rotors[3]);
        CHECK(data->rotors[3]->link0 == data->rotors[4]);
        CHECK(data->rotors[9]->link0 == nullptr);
        CHECK(data->rotors[0]->axis == 0x21 && data->rotors[1]->axis == 5 && data->rotors[6]->axis == 0);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x60), 250.0f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x64), 3.0f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x68), 0.5f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x6c), 300.0f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x70), 2.0f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x74), 1.5f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x0c), 3.0f * 572.9578f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x24), 0.5f * 572.9578f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x14), 2.0f * 572.9578f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x1c), 1.0f * 572.9578f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x20), 0.25f * 572.9578f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x78), 1.0f);
        CHECK_NEAR(HeliTypeFloat(*data->type, 0x7c), 0.5f);
        CHECK(data->mass == 250.0f && data->inertia == 250.0f);
        // A second Init is a no-op.
        const std::size_t created = f.registry.created.size();
        CHECK(helicopter->Init());
        CHECK(f.registry.created.size() == created);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Physics() == nullptr);
        CHECK(helicopter->Release() == 0);
    }

    // ---- rotor registered effects and the group-0x4307 point names --------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        SetGroup(helicopter, f, kHelicopterGroupSpinning);
        helicopter->AddOccupantGroup(kHelicopterGroupSpinning);
        CHECK(helicopter->AddModelResourceKey(cGZPersistResourceKey{0x8df32, 1, 1}));
        CHECK(helicopter->Init());
        const std::vector<std::string>& reg = f.heliHost.effectsA.regEffects;
        // Both collections receive exactly one registered effect; group 0x4307 selects the "mheli..." points.
        CHECK(reg.size() == 2);
        if (reg.size() == 2) {
            CHECK(reg[0] == "fx_00_rotor" && reg[1] == "mhelibladetsill");
        }
        const std::vector<std::string>& spin = f.heliHost.effectsB.regEffects;
        CHECK(spin.size() == 2);
        if (spin.size() == 2) {
            CHECK(spin[0] == "fx_00_rotor" && spin[1] == "mheliblade");
        }
        CHECK(!f.heliHost.effectsA.regEffects.empty());
        CHECK(!f.heliHost.effectsB.regEffects.empty());
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- SetHelicopterState: effect collections and player audio ------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        helicopter->SetHelicopterState(1);   // hover: engine on
        CHECK(helicopter->Physics()->engineEnabled == 1);
        CHECK(helicopter->Physics()->state == 1);
        CHECK(!f.heliHost.effectsB.startTransitions.empty());
        CHECK(f.heliHost.effectsB.startTransitions.back() == 0);
        CHECK(!f.heliHost.effectsA.stopTransitions.empty());
        CHECK(f.heliHost.effectsA.stopTransitions.back() == 1);
        helicopter->SetHelicopterState(0);   // engine off
        CHECK(helicopter->Physics()->engineEnabled == 0);
        CHECK(f.heliHost.effectsB.stopTransitions.back() == 1);
        CHECK(f.heliHost.effectsA.startTransitions.back() == 0);
        // Player driving adds the audio events.
        const std::size_t before = f.aircraftHost.audioPlayed.size();
        const float pos[3] = {0, 0, 0};
        helicopter->SetPosition(pos);
        helicopter->SetState(kStatePlayerDrive, nullptr, 0.0f, false, 0, 0, nullptr);
        helicopter->SetHelicopterState(4);
        CHECK(f.aircraftHost.audioPlayed.size() > before);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- stream round trip and Read's position-loaded flag -----------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        const float position[3] = {10.0f, 4.0f, 20.0f};
        helicopter->SetPosition(position);
        CHECK(helicopter->Init());
        PortMemoryOStream out;
        CHECK(helicopter->Write(out));
        CHECK(out.bytes().size() >= 6);
        CHECK(out.bytes()[0] == 1 && out.bytes()[1] == 0);   // helicopter version 1
        CHECK(out.bytes()[2] == 1 && out.bytes()[3] == 0);   // aircraft version 1
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        cSC4Helicopter* loaded = f.Make();
        CHECK(loaded->Read(in));
        CHECK(loaded->PositionLoaded());
        CHECK_NEAR(loaded->SpawnPosition().x, 10.0f);
        CHECK_NEAR(loaded->SpawnPosition().z, 20.0f);
        // On the next Init the stored spawn position is restored instead of the helipad lookup.
        f.aircraftHost.regPointFound = false;
        CHECK(loaded->Init());
        CHECK_NEAR(loaded->Position().x, 10.0f);
        CHECK_NEAR(loaded->Position().z, 20.0f);
        CHECK(loaded->Shutdown());
        CHECK(loaded->Release() == 0);
        PortMemoryOStream newer;
        newer.SetUint16(2);
        PortMemoryIStream newerIn(newer.bytes().data(), newer.bytes().size());
        cSC4Helicopter* refused = f.Make();
        CHECK(!refused->Read(newerIn));
        CHECK(refused->Release() == 0);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- PlaceSelf and GetNewDirection ------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        // Not self-controlling: PlaceSelf answers false and does nothing.
        f.aircraftHost.randomCalls = 0;
        CHECK(!helicopter->PlaceSelf());
        CHECK(f.aircraftHost.randomCalls == 0);
        helicopter->SetSelfControlling(true);
        f.aircraftHost.randomValues[0] = 2048.0;   // 0.5 * 4096
        f.aircraftHost.randomValues[1] = 3072.0;   // 0.75 * 4096
        f.aircraftHost.randomCalls = 0;
        CHECK(helicopter->PlaceSelf());
        CHECK_NEAR(helicopter->Position().x, 2048.0f);
        CHECK_NEAR(helicopter->Position().z, 3072.0f);
        CHECK_NEAR(helicopter->Position().y, 220.0f);   // water 20 + 200
        CHECK(f.aircraftHost.randomCalls == 4);   // PlaceSelf draws two, GetNewDirection two more
        // GetNewDirection: a unit vector from the position towards the random point at max(terrain+100, y).
        cS3DVector3 from{0.0f, 0.0f, 0.0f};
        cS3DVector3 direction{0.0f, 0.0f, 0.0f};
        f.aircraftHost.randomValues[0] = 100.0;
        f.aircraftHost.randomValues[1] = 200.0;
        f.aircraftHost.randomCalls = 0;
        helicopter->GetNewDirection(from, direction);
        CHECK_NEAR(std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z), 1.0f);
        CHECK(direction.x > 0.0f && direction.z > 0.0f);
        CHECK_NEAR(direction.y, 120.0f / std::sqrt(100.0f * 100.0f + 120.0f * 120.0f + 200.0f * 200.0f));
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- SetOrientation -> yaw in 0.1-degree units ------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        const float right[3] = {1, 0, 0};
        helicopter->SetOrientation(right);
        CHECK_NEAR(helicopter->Physics()->yawDeg, 900.0f);   // +pi/2 * 572.9578
        const float left[3] = {-1, 0, 0};
        helicopter->SetOrientation(left);
        CHECK_NEAR(helicopter->Physics()->yawDeg, -900.0f);
        const float forward[3] = {0, 0, 1};
        helicopter->SetOrientation(forward);
        CHECK_NEAR(helicopter->Physics()->yawDeg, 0.0f);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- engine power and the hover controller ----------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        HELI_DATA* data = helicopter->Physics();
        HELI_ENGINE* engine = helicopter->Engine();
        helicopter->SetHelicopterState(4);   // engine starting
        data->enginePower = 0.0f;
        engine->liftControl = 0.0f;
        helicopter->CalculateEnginePower(data, 1.0f);
        CHECK_NEAR(data->enginePower, 100.0f);
        helicopter->CalculateEnginePower(data, 4.0f);
        CHECK_NEAR(data->enginePower, 400.0f);
        // With the engine running and the collective at 0 the throttle is 0.5; power = 0.5 * 240 * 1 + vertical term.
        CHECK_NEAR(engine->throttle, 0.5f);
        CHECK_NEAR(engine->power, 0.5f * 240.0f);
        data->velY = 10.0f;
        helicopter->CalculateEnginePower(data, 0.0f);
        // vertical term: 0.018 * 100 * mass * -4 (rising)
        CHECK_NEAR(engine->power, 120.0f - 0.018f * 100.0f * data->mass * 4.0f);
        data->velY = -10.0f;
        helicopter->CalculateEnginePower(data, 0.0f);
        CHECK_NEAR(engine->power, 120.0f + 0.018f * 100.0f * data->mass);
        // Hover: a target far above the current altitude drives the lift control up (bounded by +/-delta*100).
        data->velY = 0.0f;
        engine->liftControl = 0.0f;
        helicopter->AI_ControlHover(helicopter->Position().y + 1000.0f, 0.1f);
        CHECK(engine->liftControl > 0.0f);
        CHECK(engine->liftControl <= 10.0f + 1e-3f);
        for (int i = 0; i < 200; ++i) helicopter->AI_ControlHover(helicopter->Position().y + 1000.0f, 0.1f);
        CHECK(engine->liftControl <= 100.0f && engine->liftControl >= -150.0f);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- flight-model helpers: weathervane, drag, lift, ApplyForce --------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        HELI_DATA* data = helicopter->Physics();
        helicopter->SetHelicopterState(1);
        data->velX = 30.0f;
        data->velY = 0.0f;
        data->velZ = 0.0f;
        cS3DVector3 force{0, 0, 0};
        helicopter->WeathervaneTransverseForce(data, &force);
        // Identity rotor matrix: along = velX = 30 (normalised to 1), transverse = velX = 1.
        CHECK_NEAR(force.x, -(30.0f * 0.011f * 30.0f));   // -speed * 0.011 * speed * along
        CHECK_NEAR(force.z, 30.0f * 0.011f * 30.0f * 0.5f + 30.0f * -0.009f * 30.0f);
        CHECK_NEAR(force.y, -(30.0f * 0.1f * 30.0f * 2.0f));   // |along| + 1 = 2, negative for +transverse
        cS3DVector3 drag{0, 0, 0};
        helicopter->Get3dDrag(1.0f, data, &drag, 200.0f);
        // Identity rotor matrix: x = |m02|*blunt + |m00|*lateral, y = |m12|*blunt + |m10|*lateral + |m11|*front,
        // z = |m22|*blunt + |m20|*lateral.
        CHECK_NEAR(drag.x, 0.0f * 0.0006f + 1.0f * 0.009f);
        CHECK_NEAR(drag.y, 0.0f + 0.0f + 1.0f * 0.043f);
        CHECK_NEAR(drag.z, 1.0f * 0.0006f + 0.0f);
        // Lift with the identity up axis: dot = velY = 0 -> factor 0.5 -> (0.5/0.45*0.5-0.5)*0.015625.
        CHECK_NEAR(helicopter->LiftCoeff(data), (0.5f / 0.45f * 0.5f - 0.5f) * 0.015625f);
        // ApplyForce: mass 250, velocity 0, one second of +Y force.
        float massVelocity[5] = {250.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        cS3DVector3 noDrag{0, 0, 0};
        float speedOut = -1.0f;
        helicopter->ApplyForce(cS3DVector3{0.0f, 2500.0f, 0.0f}, massVelocity, 1.0f, noDrag, &speedOut);
        CHECK_NEAR(massVelocity[3], 10.0f);
        CHECK_NEAR(speedOut, 0.0f);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- UpdatePhysics: the rotor follows the position and the orientation stays normalised -------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        HELI_DATA* data = helicopter->Physics();
        helicopter->SetHelicopterState(1);
        data->enginePower = 400.0f;
        f.heliHost.hasObstacleAt = false;
        for (int i = 0; i < 5; ++i) helicopter->UpdatePhysics(0.1f);
        CHECK(std::isfinite(helicopter->Position().x));
        CHECK(std::isfinite(helicopter->Position().y));
        CHECK(std::isfinite(helicopter->Position().z));
        CHECK_NEAR(data->rotors[1]->position.x * 0.3f, helicopter->Position().x);
        CHECK_NEAR(data->rotors[1]->position.z * 0.3f, helicopter->Position().z);
        const cS3DVector3& orientation = helicopter->Orientation();
        CHECK_NEAR(std::sqrt(orientation.x * orientation.x + orientation.y * orientation.y +
                             orientation.z * orientation.z), 1.0f);
        // The blade transform is fed to the model each frame.
        CHECK(data->rotors[1]->blade.counter > 0);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- DetectCollisions: obstacle sweep + ground clamp ------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        const float position[3] = {100.0f, 1.0f, 100.0f};
        helicopter->SetPosition(position);
        helicopter->SetHelicopterState(1);
        f.heliHost.hasObstacleInBox = true;
        f.heliHost.obstacle.maxCorner = cS3DVector3{200.0f, 10.0f, 200.0f};
        helicopter->DetectCollisions();
        CHECK(f.heliHost.obstacle.releases == 1);
        CHECK(f.heliHost.endQueries == 1);
        CHECK(helicopter->Position().y >= 0.0f);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- player-drive control translation ---------------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        HELI_DATA* data = helicopter->Physics();
        HELI_ENGINE* engine = helicopter->Engine();
        helicopter->SetHelicopterState(1);
        data->enginePower = 400.0f;
        auto* state = new tAutomataStatePlayerDrive();
        state->stateType = kStatePlayerDrive;
        state->history[0] = 1.0f;    // +0xa8 collective up
        state->history[2] = 1.0f;    // +0xb0 forward
        state->history[5] = 1.0f;    // +0xbc yaw right
        state->effectTimerC4 = 1.0f; // +0xc4 (no effect restart)
        CHECK(helicopter->TranslatePlayerDriveCommands(state, 100.0f));
        CHECK(engine->collective == state->steering6c);
        CHECK_NEAR(engine->field04, -state->value38);
        CHECK(engine->liftControl == state->value3c);
        CHECK(state->steering6c <= 100.0f && state->steering6c >= -100.0f);
        CHECK(state->value38 <= 100.0f && state->value38 >= -100.0f);
        CHECK(state->value3c <= 100.0f && state->value3c >= -100.0f);
        CHECK(data->rollDeg <= HeliTypeFloat(*data->type, 0x14) + 1e-3f);
        CHECK(data->pitchDeg <= HeliTypeFloat(*data->type, 0x0c) + 1e-3f);
        // The spawn altitude reference is refreshed when the yaw is trimmed.
        CHECK(helicopter->SpotOn() == false);
        delete state;
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- AI_PursueGoal: idle (state 0), spawn move and arrive ------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        HELI_DATA* data = helicopter->Physics();
        // Behaviour 0x32 with the engine off: resets the controls and clears the dispatch state.
        helicopter->SetDispatchState(6);
        data->state = 0;
        CHECK(helicopter->PushState(kHeliBehaviorIdle, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);
        CHECK(helicopter->GetDispatchState() == 0);
        CHECK(helicopter->GetBehavior() == 0);   // the idle state popped itself
        // Behaviour 0x33 (move to the spawn point) with the helicopter far away: the lift controller takes over.
        helicopter->SetHelicopterState(1);
        data->enginePower = 400.0f;
        const float farDestination[3] = {5000.0f, 0.0f, 5000.0f};
        helicopter->SetDestination(farDestination);
        CHECK(helicopter->PushState(kHeliBehaviorSpawn, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);
        CHECK(helicopter->GetDispatchState() == 3);
        CHECK(helicopter->GetBehavior() == kHeliBehaviorSpawn);   // not at the destination -> no pop
        // Behaviour 0x35 (arrive): aims the spotlight at the destination altitude and holds position.
        CHECK(helicopter->PushState(kHeliBehaviorArrive, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);
        CHECK(helicopter->GetDispatchState() == 5);
        CHECK(helicopter->SpotOn());
        CHECK(helicopter->GetBehavior() == kHeliBehaviorArrive);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- AI_PursueGoal: crime claim -------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        f.heliHost.crimeFound = true;
        f.heliHost.centreX = 333.0f;
        f.heliHost.centreZ = 444.0f;
        CHECK(helicopter->PushState(kHeliBehaviorCrime, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);
        // Crime found -> the cell centre is claimed and arrive + spawn are pushed.
        CHECK_NEAR(helicopter->Destination().x, 333.0f);
        CHECK_NEAR(helicopter->Destination().z, 444.0f);
        CHECK(helicopter->Destination().y == 0.0f);
        CHECK(helicopter->GetBehavior() == kHeliBehaviorSpawn);
        // A second helicopter cannot claim the same position (the list is process-wide).
        Fixture f2;
        cSC4Helicopter* second = f2.Make();
        CHECK(second->Init());
        f2.heliHost.crimeFound = true;
        f2.heliHost.centreX = 333.0f;
        f2.heliHost.centreZ = 444.0f;
        CHECK(second->PushState(kHeliBehaviorCrime, nullptr, 0.0f, false, 0, 0, nullptr));
        second->AI_PursueGoal(0.1f);
        float dest[3] = {0, 0, 0};
        second->GetDestination(dest);
        std::uint32_t bits[3];
        std::memcpy(bits, dest, sizeof(bits));
        CHECK(bits[0] == 0xffffffffu);   // destination cleared: already claimed
        CHECK(second->GetBehavior() == kHeliBehaviorCrime);   // claimed elsewhere: the destination is cleared
        CHECK(second->Shutdown());
        CHECK(second->Release() == 0);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- AI_PursueGoal: traffic accident (behaviour 0x38) ---------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        const float originDestination[3] = {0.0f, 0.0f, 0.0f};
        helicopter->SetDestination(originDestination);
        CHECK(helicopter->PushState(kHeliBehaviorTraffic2, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);
        CHECK(helicopter->GetBehavior() == kHeliBehaviorIdle);   // no accident -> fall back to idle
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        f.heliHost.trafficAccident = true;
        const float originDestination[3] = {0.0f, 0.0f, 0.0f};
        helicopter->SetDestination(originDestination);
        CHECK(helicopter->PushState(kHeliBehaviorTraffic2, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);
        CHECK(helicopter->GetBehavior() == kHeliBehaviorSpawn);
        CHECK_NEAR(helicopter->Destination().x, 500.0f);
        CHECK_NEAR(helicopter->Destination().z, 600.0f);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- dispatch identity ------------------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        helicopter->SetCallNumber(0x1234);
        CHECK(helicopter->GetCallNumber() == 0x1234);
        FakeMessageTarget target;
        target.AddRef();
        helicopter->AddCallback(&target);
        helicopter->SetDispatchState(5);
        CHECK(helicopter->GetDispatchState() == 5);
        CHECK(target.events.size() == 1 && target.events[0] == 7);   // SendCallbackMessage(7, state)
        CHECK(target.data.size() == 1 && target.data[0] == 5);
        helicopter->SetDispatchState(5);      // unchanged: no second message
        CHECK(target.events.size() == 1);
        // The dispatch manager is only reported while a crime behaviour is on the stack.
        f.heliHost.dispatchReports = 0;
        CHECK(helicopter->PushState(kHeliBehaviorCrime, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->SetDispatchState(6);
        CHECK(f.heliHost.dispatchReports == 1);
        CHECK(f.heliHost.lastDispatchState == 6);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- UpdateEffects: the close-to-ground / indicator effects ------------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        helicopter->SetHelicopterState(1);
        f.aircraftHost.viewZoom = 1;
        const float position[3] = {10.0f, 25.0f, 20.0f};   // 5 m above the water
        helicopter->SetPosition(position);
        helicopter->UpdateEffects();
        FakeEffect* closeToGround = static_cast<FakeEffect*>(helicopter->EffectCloseToGround());
        CHECK(closeToGround->starts == 1);
        CHECK(closeToGround->transforms >= 1);
        CHECK_NEAR(closeToGround->transform.translation[0], 10.0f);
        CHECK_NEAR(closeToGround->transform.translation[1], 25.0f);
        // Zoomed out: stopped again.
        f.aircraftHost.viewZoom = 2;
        helicopter->UpdateEffects();
        CHECK(closeToGround->stops >= 1);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- UpdateSpotlight: copter_spotlight / searchlight start-stop ------------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        f.Register("fx_00_searchlight");
        CHECK(helicopter->AddModelResourceKey(cGZPersistResourceKey{0x8df32, 1, 1}));
        CHECK(helicopter->Init());
        helicopter->SetHelicopterState(1);
        f.aircraftHost.viewZoom = 1;
        CHECK(helicopter->PushState(kHeliBehaviorArrive, nullptr, 0.0f, false, 0, 0, nullptr));
        helicopter->AI_PursueGoal(0.1f);   // sets spotOn_
        helicopter->UpdateSpotlight();
        FakeEffect* spot = static_cast<FakeEffect*>(helicopter->EffectSpotlight());
        FakeEffect* search = static_cast<FakeEffect*>(helicopter->EffectSearchlight());
        CHECK(spot->starts >= 1);
        CHECK(search->starts >= 1);
        CHECK(spot->transforms >= 1);
        f.aircraftHost.viewZoom = 2;
        helicopter->UpdateSpotlight();
        CHECK(spot->stops >= 1);
        CHECK(search->stops >= 1);
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    // ---- HandleEnterState / HandleExitState for the player-drive record ---------------------------------------
    {
        Fixture f;
        cSC4Helicopter* helicopter = f.Make();
        CHECK(helicopter->Init());
        auto* state = new tAutomataStatePlayerDrive();
        state->stateType = kStatePlayerDrive;
        helicopter->HandleEnterState(state);
        CHECK(state->value40 == 0xc2c80000u);
        CHECK_NEAR(state->groupSpeed, -100.0f);
        CHECK(state->value170 == 0xf);
        CHECK(state->animStage1a4 == 0);
        CHECK(state->timeout1d8 == 0x44fa0000u);
        // Exiting the player drive reloads the type mass into the physics block.
        helicopter->Physics()->mass = 1.0f;
        helicopter->Physics()->inertia = 1.0f;
        helicopter->HandleExitState(state);
        CHECK_NEAR(helicopter->Physics()->mass, 1.5f);   // no drive block in this fixture: the type default
        delete state;
        CHECK(helicopter->Shutdown());
        CHECK(helicopter->Release() == 0);
    }

    if (failures == 0) std::printf("sc4_helicopter_test: all checks passed\n");
    else std::printf("sc4_helicopter_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
