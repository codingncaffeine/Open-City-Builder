// cSC4Airplane / cSC4Aircraft — construction, QueryInterface, Init (drive parameters + effects), the stream round trip,
// the heading->yaw derivation, the state/player-drive handshakes, the animation tick, UpdateEffects, the altitude and
// world-clip helpers, the clearance/fire queries and AI_TurnToVector. The automaton, terrain, lot, city, effects and
// aircraft-manager neighbours are fakes behind IAutomatonHost / IAircraftHost.
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
#include "sc4/Airplane.h"
#include "sc4/Blimp.h"
#include "sc4/AutomataStates.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-4f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

// ---- fake registered effects ----------------------------------------------------------------------------------------
struct FakeEffect final : IAirplaneEffect {
    std::string name;
    int refs = 0, addRefs = 0, releases = 0, starts = 0, stops = 0, transforms = 0, positions = 0;
    bool playing = false;
    std::vector<int> startTransitions;
    std::vector<bool> stopFades;
    cS3DTransform transform;
    cS3DVector3 position;

    void AddRef() override { ++refs; ++addRefs; }
    void Release() override { --refs; ++releases; }
    void Start(int transition) override { ++starts; playing = true; startTransitions.push_back(transition); }
    void Stop(bool fade) override { ++stops; playing = false; stopFades.push_back(fade); }
    bool IsPlaying() override { return playing; }
    void SetTransform(const cS3DTransform& value) override { transform = value; ++transforms; }
    void SetPosition(const cS3DVector3& value) override { position = value; ++positions; }
};

// Owns every fake effect so a Release never dangles; also the registry the fake effects collection searches.
struct EffectRegistry {
    std::map<std::string, std::unique_ptr<FakeEffect>> effects;
    std::vector<std::string> created;

    FakeEffect* Add(const std::string& name) {
        auto& slot = effects[name];
        if (!slot) {
            slot = std::make_unique<FakeEffect>();
            slot->name = name;
        }
        return slot.get();
    }
    FakeEffect* Find(const std::string& name) {
        auto it = effects.find(name);
        return it == effects.end() ? nullptr : it->second.get();
    }
    int TotalStarts() const {
        int total = 0;
        for (const auto& entry : effects) total += entry.second->starts;
        return total;
    }
    int TotalReleases() const {
        int total = 0;
        for (const auto& entry : effects) total += entry.second->releases;
        return total;
    }
};

// ---- automaton-level fake -------------------------------------------------------------------------------------------
FakeEffect* AsFake(IAirplaneEffect* effect) { return static_cast<FakeEffect*>(effect); }

struct FakeGroup final : IAutomatonGroupTemplate {
    std::uint32_t id = 0x4200;
    FakeGroup* parent = nullptr;
    float speedMin = 1.0f, speedMax = 2.0f;
    std::uint32_t GroupId() const override { return id; }
    IAutomatonGroupTemplate* Parent() const override { return parent; }
    float SpeedMin() const override { return speedMin; }
    float SpeedMax() const override { return speedMax; }
    void GetGroupHierarchyDepth() override {}
};

struct FakeModel final : IAutomatonModel {
    int refs = 1;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void GetBounds(float* minOut, float* maxOut) override {
        const float min[3] = {-1, 0, -1}, max[3] = {1, 2, 1};
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

struct FakeEffects final : IAutomatonEffects {
    EffectRegistry* registry = nullptr;
    int inits = 0, starts = 0, stops = 0;
    std::vector<std::string> stoppedNames;
    void SetModel(IAutomatonModel*) override {}
    bool Init() override { ++inits; return true; }
    void Update(const cS3DVector3&, const cS3DVector3&, float) override {}
    void StartAll() override { ++starts; }
    void StopAll(bool) override { ++stops; }
    void StartNamed(const cRZString&, int, bool) override {}
    void StopNamed(const cRZString& effect, int) override { stoppedNames.push_back(effect.ToChar()); }
    void ToggleNamed(const cRZString&, int, bool) override {}
    void RecreateNamed(const cRZString&, int, bool) override {}
    bool AddRegEffect(const cRZString&, const cRZString&) override { return true; }
    void* FindRegEffect(const cRZString& effect) override {
        if (!registry) return nullptr;
        return registry->Find(effect.ToChar());
    }
    void RemoveRegEffect(const cRZString&) override {}
    void Shutdown() override {}
};

struct FakeHost final : IAutomatonHost {
    FakeEffects effects;
    FakeGroup group;
    std::uint32_t soundId = 100;
    int posted = 0;
    std::vector<std::uint32_t> messageTypes;
    void SetRegistry(EffectRegistry* registry) { effects.registry = registry; }

    IAutomatonModel* CreateModel(const cGZPersistResourceKey&) override { return nullptr; }
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
    void PlaceOnNetwork(const cS3DVector3&, cS3DVector3&, std::uint32_t) override {}
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
    std::vector<std::array<float, 3>> fireRects;
    cISC4OccupantFilter* lastFilter = nullptr;
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
    std::int32_t lastCellX = 0, lastCellZ = 0;

    bool TerrainPointInBounds(float, float) override { return inBounds; }
    float TerrainHeight(float, float) override { return terrainHeight; }
    float WaterHeight(float, float) override { return waterHeight; }
    float DefaultAltitude() override { return defaultAltitude; }
    float AltitudeClamp() override { return altitudeClamp; }
    float TallestBuildingAltitude() override { return 0.0f; }
    std::vector<cS3DVector3> runways;
    void GetRunwayList(const cS3DVector3&, std::vector<cS3DVector3>& out) override { out = runways; }
    std::uint32_t RandomWeightedChoice(std::uint32_t, const double*) override { return 0; }
    const void* lotHere = nullptr;
    const void* lotSaved = nullptr;
    const void* LotAt(const cS3DVector3& p) override { return p.x == savedLotX ? lotSaved : lotHere; }
    float savedLotX = -1.0e9f;
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
        cellX = 3;
        cellZ = 4;
        lastCellX = cellX;
        lastCellZ = cellZ;
    }
    bool FindFireOccupant(const cS3DVector3& minCorner, const cS3DVector3& maxCorner, cISC4OccupantFilter* filter,
                          cS3DVector3& position) override {
        lastFilter = filter;
        fireRects.push_back({minCorner.x, minCorner.z, maxCorner.x});
        if (!fireFound) return false;
        position = firePosition;
        return true;
    }
    double RandomDoubleRange(double, double) override {
        const double value = randomValues[randomCalls < 2 ? randomCalls : 1];
        ++randomCalls;
        return value;
    }
    const float* FindDriveParameters(IAutomatonGroupTemplate*) override {
        return driveParameters.size() == 18 ? driveParameters.data() : nullptr;
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

struct Fixture {
    EffectRegistry registry;
    FakeHost automatonHost;
    FakeAircraftHost aircraftHost;
    Fixture() {
        automatonHost.SetRegistry(&registry);
        aircraftHost.registry = &registry;
    }
    cSC4Airplane* Make() {
        auto* airplane = new cSC4Airplane(&automatonHost, &aircraftHost);
        airplane->AddRef();
        return airplane;
    }
    FakeEffect* Register(const char* name) { return registry.Add(name); }
};

void SetGroup(cSC4Airplane* airplane, Fixture& fixture, std::uint32_t groupId) {
    fixture.automatonHost.group.id = groupId;
    airplane->SetAutomataGroup(&fixture.automatonHost.group);
}

}  // namespace

int main() {
    // ---- construction, QueryInterface, defaults ---------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        CHECK(plane->GetType() == kAircraftOccupantType);
        CHECK(plane->IsFlagSet(kAircraftOccupantFlag));
        CHECK(plane->GetGZCLSID() == kGZCLSID_cSC4Airplane);
        CHECK(plane->State() == AirplaneState::kFlying);
        CHECK(plane->StateCounter() == 3);
        CHECK(plane->Mass() == 100.0f);
        CHECK(plane->MaxSpeedLow() == 25.0f);
        CHECK(plane->MaxSpeedHigh() == 150.0f);
        CHECK(plane->Tuning(0) == 0.4f);
        CHECK(plane->Tuning(11) == 0.1f);
        CHECK(plane->EffectPlanepropStill() == nullptr);
        CHECK(plane->EffectIndicate() == nullptr);
        CHECK(plane->FireFilter() == nullptr);
        CHECK(!plane->PositionLoaded());
        float orientation[3] = {9, 9, 9};
        CHECK(plane->GetOrientation(orientation));
        CHECK_NEAR(orientation[0], 0.0f);
        CHECK_NEAR(orientation[1], 0.0f);
        CHECK_NEAR(orientation[2], 1.0f);
        CHECK_NEAR(plane->Yaw(), 0.0f);
        // The aircraft id is answered by cSC4Aircraft::QueryInterface, the occupant id by the automaton.
        void* raw = nullptr;
        CHECK(plane->QueryInterface(kGZCLSID_cSC4Aircraft, &raw));
        CHECK(raw == static_cast<void*>(plane));
        CHECK(plane->Release() == 1);
        raw = nullptr;
        CHECK(plane->QueryInterface(kGZIID_cISC4Occupant, &raw));
        CHECK(raw != nullptr);
        CHECK(plane->Release() == 1);
        CHECK(plane->Release() == 0);
    }

    // ---- Init: drive parameters, effects and the fire filter ----------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        SetGroup(plane, f, 0x4200);
        f.aircraftHost.driveParameters.assign(18, 0.0f);
        f.aircraftHost.driveParameters[0] = 250.0f;    // mass
        f.aircraftHost.driveParameters[1] = 2.0f;      // drag area
        f.aircraftHost.driveParameters[2] = 0.04f;     // 40 m/s
        f.aircraftHost.driveParameters[3] = 0.2f;      // 200 m/s
        f.aircraftHost.driveParameters[4] = 0.5f;      // climb angle
        f.aircraftHost.driveParameters[6] = 0.6f;      // descent angle
        f.aircraftHost.driveParameters[8] = 0.7f;      // take-off angle
        f.aircraftHost.driveParameters[10] = 5000.0f;  // speed4
        f.aircraftHost.driveParameters[12] = 0.8f;     // drag
        f.aircraftHost.playerDriveParameters.assign(0x28, 0.0f);
        f.aircraftHost.playerDriveParameters[2] = 0.3f;      // +0x390 = 300
        f.aircraftHost.playerDriveParameters[0x18] = 1.5f;   // +0x438 -> 0.015
        f.aircraftHost.playerDriveParameters[0x24] = 2.0f;   // +0x45c -> 0.02
        f.Register("fx_00_planepropstill");
        f.Register("fx_00_planeprop");
        f.Register("fx_00_waterdrop");
        CHECK(plane->Init());
        CHECK(plane->IsInitialized());
        CHECK(plane->CheckAutomataFlag(kAutomataFlagIgnorePaths));
        CHECK_NEAR(plane->Mass(), 250.0f);
        CHECK_NEAR(plane->MaxSpeedLow(), 40.0f);
        CHECK_NEAR(plane->MaxSpeedHigh(), 200.0f);   // the drive block runs after the player-drive block
        CHECK_NEAR(plane->Tuning(0), 0.015f);
        CHECK_NEAR(plane->Tuning(9), 0.02f);
        CHECK_NEAR(plane->Tuning(2), 0.2f);          // constructor default, untouched by the blocks
        // the trial flights Init runs (bit-exact against the retail code: scripts/run_machine_oracle.sh airplane);
        // libm may differ in the last bit between architectures, so the unit test only checks the neighbourhood
        CHECK(std::fabs(plane->ClimbSlope() - 0.522453f) < 1e-4f);
        CHECK(std::fabs(plane->DescentSlope() - 0.670485f) < 1e-4f);
        CHECK(std::fabs(plane->TurnRate() - 0.707178f) < 1e-4f);
        CHECK(plane->State() == AirplaneState::kFlying);
        CHECK(plane->EffectPlanepropStill() != nullptr);
        CHECK(AsFake(plane->EffectPlanepropStill())->refs == 1);
        CHECK(plane->EffectPlaneprop() != nullptr);
        CHECK(plane->EffectDrop() != nullptr);
        CHECK(AsFake(plane->EffectDrop())->name == "fx_00_waterdrop");
        CHECK(plane->EffectIndicate() != nullptr);
        CHECK(AsFake(plane->EffectIndicate())->name == "aircraftindicate");
        CHECK(plane->FireFilter() != nullptr);
        const auto& stoppedNames = f.automatonHost.effects.stoppedNames;
        CHECK(stoppedNames.size() == 1 && stoppedNames[0] == "jetbomb");
        // A second Init is refused before doing anything.
        const std::size_t created = f.registry.created.size();
        CHECK(plane->Init());
        CHECK(f.registry.created.size() == created);
        CHECK(plane->Release() == 0);
    }

    // ---- Init: fire plane group 0x4311 uses skywriting and never keeps the looked-up effect ----------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        SetGroup(plane, f, 0x4311);
        plane->AddOccupantGroup(0x4311);
        f.Register("fx_00_planeprop");
        f.Register("fx_00_skywriting");
        f.aircraftHost.regPointFound = true;
        CHECK(plane->Init());
        CHECK(plane->IsOccupantGroup(0x4311));
        CHECK_NEAR(plane->RegPoint().x, 1.0f);
        CHECK_NEAR(plane->RegPoint().z, 3.0f);
        CHECK(plane->SkywritingColor() == 0);
        CHECK(plane->EffectDrop() != nullptr);
        CHECK(AsFake(plane->EffectDrop())->name == "skywritingwhite");
        bool createdWhite = false;
        for (const std::string& name : f.registry.created) createdWhite = createdWhite || name == "skywritingwhite";
        CHECK(createdWhite);
        CHECK(plane->Release() == 0);
    }

    // ---- stream round trip -------------------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        f.Register("fx_00_planeprop");
        CHECK(plane->Init());
        const float position[3] = {10.0f, 4.0f, 20.0f};
        plane->SetPosition(position);
        plane->SetCallNumber(0x1234);
        plane->SetDispatchState(2);
        plane->GetDispatchState();
        PortMemoryOStream out;
        CHECK(plane->Write(out));
        // Two versions: airplane (1) then aircraft (1).
        CHECK(out.bytes().size() >= 4);
        CHECK(out.bytes()[0] == 1 && out.bytes()[1] == 0);
        CHECK(out.bytes()[2] == 1 && out.bytes()[3] == 0);
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        cSC4Airplane* loaded = f.Make();
        CHECK(loaded->Read(in));
        CHECK(loaded->PositionLoaded());
        float loadedPosition[3] = {};
        loaded->GetPosition(loadedPosition);
        CHECK_NEAR(loadedPosition[0], 10.0f);
        CHECK_NEAR(loadedPosition[2], 20.0f);
        CHECK_NEAR(loaded->SavedPosition().x, 0.0f);
        CHECK(loaded->GetCallNumber() == 0);      // the call number is not part of the record
        CHECK(loaded->Release() == 0);
        // Airplane version 2 is refused before anything else is read.
        PortMemoryOStream newer;
        newer.SetUint16(2);
        PortMemoryIStream newerIn(newer.bytes().data(), newer.bytes().size());
        cSC4Airplane* refused = f.Make();
        CHECK(!refused->Read(newerIn));
        CHECK(refused->Release() == 0);
        CHECK(plane->Release() == 0);
    }

    // ---- aircraft stream (version 1) and QI ids -----------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        PortMemoryOStream out;
        CHECK(plane->Write(out));
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        cSC4Airplane* loaded = f.Make();
        CHECK(loaded->Read(in));
        CHECK(loaded->Release() == 0);
        PortMemoryOStream bad;
        bad.SetUint16(2);   // airplane version 2 -> refused by cSC4Airplane::Read
        PortMemoryIStream badIn(bad.bytes().data(), bad.bytes().size());
        cSC4Airplane* refused = f.Make();
        CHECK(!refused->Read(badIn));
        CHECK(refused->Release() == 0);
        CHECK(plane->Release() == 0);
    }

    // ---- SetOrientation -> yaw (+0x380) ------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        const float pi = 3.14159265f;
        const float aligned[3] = {0, 0, 1};
        plane->SetOrientation(aligned);
        CHECK_NEAR(plane->Yaw(), 0.0f);
        const float right[3] = {1, 0, 0};
        plane->SetOrientation(right);
        CHECK_NEAR(plane->Yaw(), -pi * 0.5f);
        const float left[3] = {-1, 0, 0};
        plane->SetOrientation(left);
        CHECK_NEAR(plane->Yaw(), pi * 0.5f);
        const float back[3] = {0, 0, -1};
        plane->SetOrientation(back);
        CHECK_NEAR(plane->Yaw(), pi);
        const float up[3] = {0, 1, 0};
        plane->SetOrientation(up);
        CHECK_NEAR(plane->Yaw(), pi * 0.5f);
        CHECK(plane->Release() == 0);
    }

    // ---- aircraft altitude / world clip / PlaceSelf -------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        cS3DVector3 point{10.0f, 0.0f, 20.0f};
        CHECK_NEAR(plane->GetAltitude(point), 20.0f);   // water above terrain
        f.aircraftHost.waterHeight = 5.0f;
        CHECK_NEAR(plane->GetAltitude(point), 10.0f);   // terrain above water
        f.aircraftHost.lotFound = true;
        CHECK_NEAR(plane->GetAltitude(point), 30.0f);   // lot altitude wins
        f.aircraftHost.lotFound = false;
        f.aircraftHost.inBounds = false;
        CHECK_NEAR(plane->GetAltitude(point), -7.0f);   // default altitude
        f.aircraftHost.inBounds = true;
        cS3DVector3 clip{6000.0f, 5000.0f, -6000.0f};
        plane->ClipToWorld(clip);
        CHECK_NEAR(clip.x, 5096.0f);
        CHECK_NEAR(clip.y, 2000.0f);
        CHECK_NEAR(clip.z, -1000.0f);
        clip = cS3DVector3{5.0f, -5.0f, 7.0f};
        plane->ClipToWorld(clip);
        CHECK_NEAR(clip.x, 5.0f);
        CHECK_NEAR(clip.y, 0.0f);
        CHECK_NEAR(clip.z, 7.0f);
        // PlaceSelf only runs while self-controlling; it uses the cell-count ranges and terrain + 500.
        CHECK(!plane->PlaceSelf());
        plane->SetSelfControlling(true);
        CHECK(plane->PlaceSelf());
        float placed[3] = {};
        plane->GetPosition(placed);
        CHECK_NEAR(placed[0], 34.0f);
        CHECK_NEAR(placed[1], 510.0f);
        CHECK_NEAR(placed[2], 56.0f);
        CHECK(plane->Release() == 0);
    }

    // ---- LoadDriveParameters / LoadPlayerDriveParameters without a group keep the defaults ----------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        plane->LoadDriveParameters();
        CHECK_NEAR(plane->Mass(), 100.0f);
        CHECK_NEAR(plane->Tuning(0), 0.4f);
        CHECK_NEAR(plane->ClimbSlope(), 0.0f);
        f.aircraftHost.playerDriveParameters.assign(0x28, 0.0f);
        plane->LoadPlayerDriveParameters();
        CHECK_NEAR(plane->Tuning(0), 0.4f);   // zeros keep the current value
        CHECK(plane->Release() == 0);
    }

    // ---- SetAirplaneState / dispatch / call number -------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        f.Register("fx_00_planeprop");
        CHECK(plane->Init());
        plane->SetDispatchState(3);
        CHECK(plane->GetDispatchState() == 3);
        plane->SetCallNumber(77);
        CHECK(plane->GetCallNumber() == 77);
        // Without player drive the landing state just changes.
        plane->SetAirplaneState(AirplaneState::kLanding);
        CHECK(plane->State() == AirplaneState::kLanding);
        CHECK(f.aircraftHost.audioPlayed.empty());
        // Entering player drive while landing plays event 8 through HandleEnterState.
        CHECK(plane->PushState(kAirplaneStatePlayerDrive, nullptr, 1000.0f, false, 0, 0, nullptr));
        CHECK(plane->IsPlayerDriving());
        CHECK(f.aircraftHost.audioPlayed.size() == 1 && f.aircraftHost.audioPlayed[0] == 8);
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(plane->GetState());
        CHECK(drive != nullptr);
        CHECK(drive->value170 == 0xf);
        CHECK(drive->timeout1d8 == 0x44fa0000u);
        CHECK(drive->value40 == 0);
        CHECK_NEAR(drive->groupSpeed, 1.0f);
        // landing -> flying: stop 9/8/10 and play 10.
        f.aircraftHost.audioPlayed.clear();
        f.aircraftHost.audioStopped.clear();
        plane->SetAirplaneState(AirplaneState::kFlying);
        CHECK(plane->State() == AirplaneState::kFlying);
        CHECK(f.aircraftHost.audioStopped.size() == 3);
        CHECK(f.aircraftHost.audioStopped[0] == 9 && f.aircraftHost.audioStopped[1] == 8 &&
              f.aircraftHost.audioStopped[2] == 10);
        CHECK(f.aircraftHost.audioPlayed.size() == 1 && f.aircraftHost.audioPlayed[0] == 10);
        // flying -> landing: play 9 (no 0xd because the old state was flying).
        f.aircraftHost.audioPlayed.clear();
        f.aircraftHost.audioStopped.clear();
        plane->SetAirplaneState(AirplaneState::kLanding);
        CHECK(plane->StateCounter() == 3);
        CHECK(f.aircraftHost.audioPlayed.size() == 1 && f.aircraftHost.audioPlayed[0] == 9);
        CHECK(f.aircraftHost.audioStopped.empty());
        // taxiing -> landing: stop then play 0xd.
        plane->SetAirplaneState(AirplaneState::kTaxiing);
        f.aircraftHost.audioPlayed.clear();
        f.aircraftHost.audioStopped.clear();
        plane->SetAirplaneState(AirplaneState::kLanding);
        CHECK(f.aircraftHost.audioStopped.size() == 1 && f.aircraftHost.audioStopped[0] == 0xd);
        CHECK(f.aircraftHost.audioPlayed.size() == 1 && f.aircraftHost.audioPlayed[0] == 0xd);
        CHECK(plane->PopState());
        CHECK(!plane->IsPlayerDriving());
        CHECK(plane->Release() == 0);
    }

    // ---- SetDispatchState sends callback event 7 ---------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        FakeMessageTarget target;
        target.AddRef();
        plane->AddCallback(&target);
        plane->SetDispatchState(5);
        CHECK(target.events.size() == 1 && target.events[0] == 7 && target.data[0] == 5);
        plane->SetDispatchState(5);   // unchanged: no message
        CHECK(target.events.size() == 1);
        plane->SetDispatchState(6);
        CHECK(target.events.size() == 2 && target.data[1] == 6);
        plane->RemoveCallback(&target);
        target.Release();
        CHECK(plane->Release() == 0);
    }

    // ---- HandleEnterState/ExitState resets and player-drive parameters -----------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        f.Register("fx_00_planeprop");
        CHECK(plane->Init());
        f.aircraftHost.playerDriveParameters.assign(0x28, 0.0f);
        f.aircraftHost.playerDriveParameters[0x18] = 9.0f;   // +0x438 -> 0.09
        CHECK(plane->PushState(kAirplaneStatePlayerDrive, nullptr, 1.0f, false, 0, 0, nullptr));
        CHECK(plane->IsPlayerDriving());
        CHECK_NEAR(plane->Tuning(0), 0.09f);   // LoadPlayerDriveParameters ran
        CHECK(plane->PopState());
        CHECK(!plane->IsPlayerDriving());
        // LoadDriveParameters does not touch the player-drive tunings (+0x438..+0x464).
        CHECK_NEAR(plane->Tuning(0), 0.09f);
        CHECK(!plane->SkywritingActive());
        CHECK(!plane->HandleEnterState(nullptr));
        CHECK(!plane->HandleExitState(nullptr));
        CHECK(plane->Release() == 0);
    }

    // ---- HandlePlayerDriveCommand / skywriting colour cycle -----------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        SetGroup(plane, f, 0x4311);
        plane->AddOccupantGroup(0x4311);
        f.Register("fx_00_planeprop");
        CHECK(plane->Init());
        FakeEffect* white = AsFake(plane->EffectDrop());
        CHECK(white != nullptr && white->name == "skywritingwhite");
        CHECK(plane->HandlePlayerDriveCommand(7, true));   // toggle on
        CHECK(plane->SkywritingActive());
        CHECK(plane->HandlePlayerDriveCommand(7, true));   // toggle off (the group still reports success)
        CHECK(!plane->SkywritingActive());
        CHECK(plane->HandlePlayerDriveCommand(9, true));   // next colour: yellow
        CHECK(plane->SkywritingColor() == 1);
        CHECK(white->releases == 1);
        CHECK(plane->EffectDrop() != nullptr && AsFake(plane->EffectDrop())->name == "skywritingyellow");
        // A non-fire plane ignores the commands.
        Fixture other;
        cSC4Airplane* plain = other.Make();
        CHECK(!plain->HandlePlayerDriveCommand(9, true));
        CHECK(plain->EffectDrop() == nullptr);
        CHECK(plain->Release() == 0);
        CHECK(plane->Release() == 0);
    }

    // ---- police group 0x4303 restarts the 2 s skywriting timer -------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        plane->AddOccupantGroup(kAirplaneGroupPolice);
        CHECK(plane->HandlePlayerDriveCommand(7, true));
        CHECK(plane->SkywritingActive());
        CHECK_NEAR(plane->SkywritingTimer(), 2.0f);
        CHECK(plane->StateCounter() == 2);
        CHECK(plane->Release() == 0);
    }

    // ---- DoAnimationTick: timer decay, player-drive short-circuit ------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        SetGroup(plane, f, 0x4200);
        plane->AddOccupantGroup(kAirplaneGroupPolice);
        f.Register("fx_00_planeprop");
        CHECK(plane->Init());
        CHECK(plane->HandlePlayerDriveCommand(7, true));
        CHECK_NEAR(plane->SkywritingTimer(), 2.0f);
        // Disaster speed multiplier 2.0 -> 1000 ms becomes 2 s, clamped to the 0.2 s step.
        f.aircraftHost.disasterActive = true;
        CHECK(plane->DoAnimationTick(0, 1000.0f));
        CHECK_NEAR(plane->SkywritingTimer(), 1.8f);
        for (int i = 0; i < 9; ++i) plane->DoAnimationTick(0, 1000.0f);
        CHECK(plane->SkywritingTimer() <= 0.0f);
        CHECK(!plane->SkywritingActive());   // group 0x4303 clears it when the timer expires
        // A player-drive state suppresses the AI/physics call and the tick still succeeds.
        if (plane->PushState(kAirplaneStatePlayerDrive, nullptr, 1000.0f, false, 0, 0, nullptr)) {
            CHECK(plane->GetBehavior() == kAirplaneStatePlayerDrive);
            CHECK(plane->DoAnimationTick(0, 16.0f));
            CHECK(plane->PopState());
        }
        CHECK(plane->Release() == 0);
    }

    // ---- UpdateEffects: indicator transform and prop effect transitions ---------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        f.Register("fx_00_planepropstill");
        f.Register("fx_00_planeprop");
        f.Register("fx_00_waterdrop");
        CHECK(plane->Init());
        plane->PushState(kAirplaneStatePlayerDrive, nullptr, 1000.0f, false, 0, 0, nullptr);
        f.aircraftHost.viewZoom = 1;
        f.aircraftHost.simulatorPaused = false;
        f.aircraftHost.simulatorAdvancing = true;
        plane->UpdateEffects();
        FakeEffect* indicate = AsFake(plane->EffectIndicate());
        CHECK(indicate != nullptr);
        CHECK(indicate->transforms == 1);
        CHECK_NEAR(indicate->transform.translation[0], plane->Position().x);
        CHECK_NEAR(indicate->transform.translation[1], 20.0f);   // GetAltitude: water 20 wins
        CHECK(indicate->starts == 1 && indicate->startTransitions[0] == 0);
        // The prop is running and the plane is stationary -> stop it and start the still prop.
        FakeEffect* running = AsFake(plane->EffectPlaneprop());
        FakeEffect* still = AsFake(plane->EffectPlanepropStill());
        running->playing = true;
        plane->UpdateEffects();
        CHECK(running->stops == 1 && running->stopFades[0] == true);
        CHECK(still->starts == 1);
        // Zoomed out -> the indicator is stopped.
        f.aircraftHost.viewZoom = 3;
        indicate->playing = true;
        plane->UpdateEffects();
        CHECK(indicate->stops >= 1);
        CHECK(plane->PopState());
        CHECK(plane->Release() == 0);
    }

    // ---- EvaluateClearance ------------------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        const cS3DVector3 start{0.0f, 0.0f, 0.0f};
        const cS3DVector3 direction{1.0f, 0.0f, 0.0f};
        f.aircraftHost.effectsWaterHeight = 20.0f;
        f.aircraftHost.inBounds = true;
        // First sample at x = 16 rises 20 m: slope 20/16.
        CHECK_NEAR(plane->EvaluateClearance(start, direction), 20.0f / 16.0f);
        // Outside the terrain the GetAltitude value (water 20) is used.
        f.aircraftHost.inBounds = false;
        f.aircraftHost.effectsWaterHeight = 500.0f;
        CHECK_NEAR(plane->EvaluateClearance(start, direction), 0.0f);   // default altitude is below the start
        CHECK(plane->Release() == 0);
    }

    // ---- FindFireNearLocation ---------------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        f.Register("fx_00_planeprop");
        CHECK(plane->Init());
        const cS3DVector3 location{100.0f, 0.0f, 200.0f};
        cS3DVector3 fire{};
        CHECK(!plane->FindFireNearLocation(location, fire));
        CHECK(f.aircraftHost.lastFilter == plane->FireFilter());
        CHECK(f.aircraftHost.fireRects.size() == 1);
        CHECK_NEAR(f.aircraftHost.fireRects[0][0], 0.0f);
        CHECK_NEAR(f.aircraftHost.fireRects[0][2], 200.0f);
        f.aircraftHost.fireFound = true;
        CHECK(plane->FindFireNearLocation(location, fire));
        CHECK_NEAR(fire.x, 111.0f);
        CHECK_NEAR(fire.z, 222.0f);
        CHECK(plane->Release() == 0);
    }

    // ---- AI_TurnToVector --------------------------------------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        const float aligned[3] = {0, 0, 1};
        plane->SetOrientation(aligned);
        CHECK(plane->AI_TurnToVector(cS3DVector3{0.0f, 0.0f, 1.0f}, 0.1f));
        CHECK_NEAR(plane->ControlWish()[1], 0.0f);
        CHECK_NEAR(plane->ControlWish()[2], 0.0f);
        // 90 degrees away: the turn controller eases by tunings[2] * dt = 0.02 in opposite directions.
        CHECK(!plane->AI_TurnToVector(cS3DVector3{1.0f, 0.0f, 0.0f}, 0.1f));
        CHECK_NEAR(plane->ControlWish()[1], -0.02f);
        CHECK_NEAR(plane->ControlWish()[2], -0.02f);
        CHECK(plane->Release() == 0);
    }

    // ---- AI_PursueGoal: skywriting, fire search and the fallback branch ----------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        CHECK(plane->Init());
        CHECK(plane->PushState(0x35, nullptr, -1.0f, false, 0, 0, nullptr));
        const int counter = plane->StateCounter();
        plane->AI_PursueGoal(0.1f);
        CHECK(plane->GetDispatchState() == 5 && plane->SkywritingActive());
        CHECK_NEAR(plane->SkywritingTimer(), 2.0f);
        CHECK(plane->StateCounter() == counter - 1);
        plane->AI_PursueGoal(0.1f);                      // timer still running: nothing pops
        CHECK(plane->GetBehavior() == 0x35 && plane->SkywritingActive());
        CHECK(plane->PopState());
        // no fire found with landing counts left: pop, then land / skywrite / go to, dispatch state 3
        CHECK(plane->PushState(0x39, nullptr, -1.0f, false, 0, 0, nullptr));
        plane->AI_PursueGoal(0.1f);
        CHECK(plane->GetDispatchState() == 3 && plane->GetBehavior() == 0x33);
        CHECK(plane->PopState() && plane->GetBehavior() == 0x35);
        CHECK(plane->PopState() && plane->GetBehavior() == 0x32);
        CHECK(plane->PopState());
        // any other behaviour pops
        CHECK(plane->PushState(0x34, nullptr, -1.0f, false, 0, 0, nullptr));
        plane->AI_PursueGoal(0.1f);
        CHECK(plane->GetBehavior() != 0x34);
        CHECK(plane->Release() == 0);
    }

    // ---- Shutdown releases the effects and the filter ---------------------------------------------------------
    {
        Fixture f;
        cSC4Airplane* plane = f.Make();
        f.Register("fx_00_planepropstill");
        f.Register("fx_00_planeprop");
        f.Register("fx_00_waterdrop");
        CHECK(plane->Init());
        FakeEffect* still = AsFake(plane->EffectPlanepropStill());
        FakeEffect* running = AsFake(plane->EffectPlaneprop());
        FakeEffect* drop = AsFake(plane->EffectDrop());
        FakeEffect* indicate = AsFake(plane->EffectIndicate());
        CHECK(still->refs == 1);
        CHECK(plane->Shutdown());
        CHECK(still->stops == 1 && still->stopFades[0] == true);
        CHECK(running->stops == 1);
        CHECK(drop->stops == 1);
        CHECK(indicate->stops == 1);
        CHECK(plane->EffectPlanepropStill() == nullptr);
        CHECK(plane->EffectIndicate() == nullptr);
        CHECK(plane->EffectDrop() == nullptr);
        CHECK(plane->Release() == 0);
    }

    // ---- the blimp ------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.aircraftHost.randomValues[0] = 34.0;
        f.aircraftHost.randomValues[1] = 56.0;
        auto* blimp = new cSC4Blimp(&f.automatonHost, &f.aircraftHost);
        blimp->AddRef();
        CHECK(blimp->GetModelResourceInstance() == 0x0818f120);
        CHECK(blimp->DoAnimationTick(0, 33.0f));                         // does nothing and says so
        f.aircraftHost.randomCalls = 0;
        cS3DVector3 from;
        from.x = 10.0f; from.y = 7.0f; from.z = 20.0f;
        cS3DVector3 heading;
        blimp->GetNewDirection(from, heading);
        const float length = std::sqrt((24.0f * 24.0f + 0.0f * 0.0f) + 36.0f * 36.0f);
        CHECK(std::fabs(heading.x - 24.0f * (1.0f / length)) < 1e-7f && heading.y == 0.0f && std::fabs(heading.z - (1.0f / length) * 36.0f) < 1e-7f);
        CHECK(f.aircraftHost.randomCalls == 2);                          // x then z, nothing else
        CHECK(blimp->Init() && blimp->Init());                           // the second call changes nothing
        blimp->Release();
    }

    std::printf(failures == 0 ? "sc4_airplane: all checks passed\n" : "sc4_airplane: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
