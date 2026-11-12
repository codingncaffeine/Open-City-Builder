// cSC4Watercraft — identity/serialization, the drive parameters, the ferry station state machine, the water lookahead
// and AI location picking, the movement helpers, the collision / crash helpers, the tick dispatchers and the model /
// effects wiring. The watercraft manager, the city/terrain/surface-water/lot queries, the compressed route readers and
// the named animations are fakes (the class only reaches them through IWatercraftHost); the automaton base is the
// recovered cSC4Automaton.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Watercraft.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-4f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), \
                static_cast<double>(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

constexpr std::uint32_t kModelKeyInstance = 0x1234;

// ---- fakes ---------------------------------------------------------------------------------------------------------
struct FakeModel final : IAutomatonModel {
    int refs = 1;
    int transformSets = 0, opacitySets = 0, animationTicks = 0;
    float opacity = 1.0f;
    bool translucent = false;
    float minXyz[3] = {1.0f, 0.0f, 2.0f};
    float maxXyz[3] = {5.0f, 3.0f, 6.0f};
    cS3DTransform transform;

    void AddRef() override { ++refs; }
    void Release() override { if (--refs <= 0) delete this; }
    void GetBounds(float* minOut, float* maxOut) override {
        std::memcpy(minOut, minXyz, sizeof(minXyz));
        std::memcpy(maxOut, maxXyz, sizeof(maxXyz));
    }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& in) override { transform = in; ++transformSets; }
    void SetOpacity(float value, bool isTranslucent) override { opacity = value; translucent = isTranslucent; ++opacitySets; }
    void UpdateAnimation(float) override { ++animationTicks; }
    int GetFrame() const override { return 0; }
    int GetFrameCount() const override { return 1; }
    void SetZoomAndRotation(int, int) override {}
    void SetPlayMode(s3d::PlayMode) override {}
};

struct FakeEffects final : IAutomatonEffects {
    int setModel = 0, inits = 0, updates = 0, starts = 0, stops = 0, shutdowns = 0;
    IAutomatonModel* model = nullptr;
    std::vector<std::string> startedNames, stoppedNames;
    void SetModel(IAutomatonModel* value) override { model = value; ++setModel; }
    bool Init() override { ++inits; return true; }
    void Update(const cS3DVector3&, const cS3DVector3&, float) override { ++updates; }
    void StartAll() override { ++starts; }
    void StopAll(bool) override { ++stops; }
    void StartNamed(const cRZString& effect, int, bool) override { ++starts; startedNames.push_back(effect.ToChar()); }
    void StopNamed(const cRZString& effect, int) override { ++stops; stoppedNames.push_back(effect.ToChar()); }
    void ToggleNamed(const cRZString&, int, bool) override {}
    void RecreateNamed(const cRZString&, int, bool) override {}
    bool AddRegEffect(const cRZString&, const cRZString&) override { return true; }
    void* FindRegEffect(const cRZString&) override { return nullptr; }
    void RemoveRegEffect(const cRZString&) override {}
    void Shutdown() override { ++shutdowns; }
};

struct FakeAnimation final : IWatercraftAnimation {
    int refs = 1;
    int plays = 0, pauses = 0, transforms = 0;
    bool playing = false;
    bool paused = false;
    cS3DTransform transform;
    void AddRef() override { ++refs; }
    void Release() override { if (--refs <= 0) delete this; }
    void Play(bool) override { playing = true; ++plays; }
    void SetPaused(bool value) override { paused = value; ++pauses; }
    bool IsPlaying() const override { return playing; }
    void SetTransform(const cS3DTransform& value) override { transform = value; ++transforms; }
};

struct FakeRouteReader final : IWatercraftRouteReader {
    std::vector<std::pair<std::int16_t, std::int16_t>> points;
    std::size_t index = 0;
    int setDataCalls = 0;
    void SetData(const std::vector<std::uint8_t>& data) override {
        ++setDataCalls;
        points.clear();
        index = 0;
        for (std::size_t i = 0; i + 3 < data.size(); i += 4) {
            std::int16_t x = 0, z = 0;
            std::memcpy(&x, data.data() + i, 2);
            std::memcpy(&z, data.data() + i + 2, 2);
            points.emplace_back(x, z);
        }
    }
    bool GetNext(std::int16_t& x, std::int16_t& z) override {
        if (index >= points.size()) return false;
        x = points[index].first;
        z = points[index].second;
        ++index;
        return true;
    }
};

struct FakeGroup final : IWatercraftGroupTemplate {
    std::uint32_t id = 0x1111;
    IAutomatonGroupTemplate* parent = nullptr;
    float speedMin = 0.0f, speedMax = 0.0f;
    float driveSpeed = 1.0f;
    float parameters[40] = {};
    bool hasParameters = false;
    std::uint32_t GroupId() const override { return id; }
    IAutomatonGroupTemplate* Parent() const override { return parent; }
    float SpeedMin() const override { return speedMin; }
    float SpeedMax() const override { return speedMax; }
    void GetGroupHierarchyDepth() override {}
    const float* DriveParameters() const override { return hasParameters ? parameters : nullptr; }
    float DriveSpeed() const override { return driveSpeed; }
};

struct FakeNeighbour final : cISC4Occupant {
    float position[3] = {0.0f, 0.0f, 0.0f};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof(position)); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect&) override {}
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
    std::uint32_t GetFlags() override { return 0; }
};

struct FakeHost final : IWatercraftHost {
    FakeEffects effects;
    FakeGroup group;
    std::vector<FakeModel*> models;
    std::vector<FakeRouteReader*> readers;
    std::vector<FakeAnimation*> animations;
    std::vector<std::string> animationNames;
    std::set<std::pair<int, int>> lotCells;
    std::set<std::pair<int, int>> landCells;   // cells where IsWaterCell is false
    bool city = true;
    int cityCellsX = 64, cityCellsZ = 64;
    float extent = 4000.0f;
    float terrainY = -3.5f;
    float cellTerrainY = -3.5f;
    float waterLevel = 0.0f;
    cS3DVector3 normal{0.0f, 1.0f, 0.0f};
    cSC4Watercraft* collisionBoat = nullptr;
    cSC4Watercraft* lookBoat = nullptr;
    bool dockPermission = true;
    bool waitPermission = true;
    bool canDockNow = false;
    int insertions = 0, removals = 0, refreshes = 0, rendererAdds = 0, rendererRemoves = 0, fadesIn = 0, fadesOut = 0;
    int posted = 0, addedNotifications = 0, removedNotifications = 0;
    int createdModels = 0, destroyedModels = 0;
    int createdReaders = 0;
    int doneDocking = 0, doneQueueing = 0;
    int requestedDock = 0, requestedWait = 0;
    int audioEvents = 0;
    std::int32_t fleetDocks = 3, fleetQueues = 4;
    float simSpeed = 1.0f;
    float maxSpeedUnits = 3600.0f;   // -> 1 m/s
    float fadeSeconds = 0.5f;
    bool animationAllowed = true;

    // ---- IAutomatonHost
    IAutomatonModel* CreateModel(const cGZPersistResourceKey&) override {
        ++createdModels;
        auto* model = new FakeModel();
        models.push_back(model);
        return model;
    }
    void DestroyModel(IAutomatonModel* model) override { ++destroyedModels; if (model) model->Release(); }
    IAutomatonEffects* CreateEffects() override { return &effects; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { ++rendererAdds; return true; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { ++rendererRemoves; return true; }
    void MoveModelInRenderer(IAutomatonModel*) override { ++fadesIn; }
    void ChangeModelInRenderer(IAutomatonModel*, int) override { ++fadesOut; }
    bool InsertOccupant(cISC4Occupant*) override { ++insertions; return true; }
    void RemoveOccupant(cISC4Occupant*) override { ++removals; }
    bool RefreshOccupant(cISC4Occupant*) override { ++refreshes; return true; }
    bool HasCity() override { return city; }
    float CitySizeX() override { return 4096.0f; }
    float CitySizeZ() override { return 4096.0f; }
    float TerrainHeight(float, float) override { return terrainY; }
    bool NetworkAt(int, int) override { return false; }
    bool LotAt(int cellX, int cellZ) override { return lotCells.count({cellX, cellZ}) != 0; }
    void PlaceOnNetwork(const cS3DVector3&, cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t id) override { return id == group.id ? &group : nullptr; }
    bool AnimationUpdateAllowed() override { return animationAllowed; }
    float SimSpeedModifier() override { return simSpeed; }
    float MaxSpeedUnits() override { return maxSpeedUnits; }
    float FadeDurationSeconds() override { return fadeSeconds; }
    void PostMessage(cIGZMessage2*) override { ++posted; }
    std::uint32_t GenerateSoundInstanceId() override { return 0; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override { ++audioEvents; }
    std::uint32_t RandomBelow(std::uint32_t) override { return 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double lo, double) override { return lo; }
    double RandomUnit() override { return 0.0; }

    // ---- IWatercraftHost
    cSC4Watercraft* LookForBoatsAtPosition(const float*, cSC4Watercraft*) override { return lookBoat; }
    cSC4Watercraft* CollisionDetection(float, cSC4Watercraft*, float* outPosition) override {
        if (outPosition) outPosition[0] = outPosition[1] = outPosition[2] = 0.0f;
        return collisionBoat;
    }
    bool RequestPermissionToDock(cSC4Lot*, cSC4Watercraft*) override { ++requestedDock; return dockPermission; }
    bool RequestPermissionToWait(cSC4Lot*, cSC4Watercraft*) override { ++requestedWait; return waitPermission; }
    void DoneDocking(cSC4Lot*, cSC4Watercraft*) override { ++doneDocking; }
    void DoneQueueing(cSC4Lot*, cSC4Watercraft*) override { ++doneQueueing; }
    bool CanDockNow(cSC4Watercraft*) override { return canDockNow; }
    void FleetCounts(std::int32_t& docks, std::int32_t& queues) override { docks = fleetDocks; queues = fleetQueues; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { ++addedNotifications; return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { ++removedNotifications; return true; }
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override {
        x = static_cast<float>(cellX) * 16.0f + 8.0f;
        z = static_cast<float>(cellZ) * 16.0f + 8.0f;
    }
    bool CellInCity(int cellX, int cellZ) override {
        return city && cellX >= 0 && cellX < cityCellsX && cellZ >= 0 && cellZ < cityCellsZ;
    }
    bool PointInCity(float x, float z) override {
        int cellX = 0, cellZ = 0;
        WorldToCell(x, z, cellX, cellZ);
        return CellInCity(cellX, cellZ);
    }
    void CityCellCount(int& sizeX, int& sizeZ) override { sizeX = cityCellsX; sizeZ = cityCellsZ; }
    float CityExtent() override { return extent; }
    float TerrainHeightAtCell(int, int) override { return cellTerrainY; }
    void TerrainNormal(float, float, cS3DVector3& out) override { out = normal; }
    float WaterLevel(float, float) override { return waterLevel; }
    bool IsWaterCell(int cellX, int cellZ) override { return landCells.count({cellX, cellZ}) == 0; }
    bool IsWaterAtCell(int cellX, int cellZ, int) override { return IsWaterCell(cellX, cellZ); }
    bool IsWaterAtPosition(float x, float z, int, cSC4Lot*) override {
        int cellX = 0, cellZ = 0;
        WorldToCell(x, z, cellX, cellZ);
        return IsWaterCell(cellX, cellZ);
    }
    cSC4Lot* LotAtCell(int cellX, int cellZ) override {
        return lotCells.count({cellX, cellZ}) ? reinterpret_cast<cSC4Lot*>(this) : nullptr;
    }
    cSC4Lot* LotAtPosition(const float*) override { return nullptr; }
    IWatercraftRouteReader* CreateRouteReader() override {
        ++createdReaders;
        auto* reader = new FakeRouteReader();
        readers.push_back(reader);
        return reader;
    }
    IWatercraftAnimation* CreateNamedAnimation(const char* name) override {
        animationNames.push_back(name);
        auto* animation = new FakeAnimation();
        animations.push_back(animation);
        return animation;
    }
};

cSC4Watercraft* MakeBoat(FakeHost& host, bool withModel = true) {
    cGZPersistResourceKey key;
    if (withModel) key.instance = kModelKeyInstance;
    auto* boat = new cSC4Watercraft(&host, key);
    boat->AddRef();
    return boat;
}

void AddRoute(std::vector<std::uint8_t>& route, std::int16_t x, std::int16_t z) {
    const std::size_t offset = route.size();
    route.resize(offset + 4);
    std::memcpy(route.data() + offset, &x, 2);
    std::memcpy(route.data() + offset + 2, &z, 2);
}

cSC4Lot* MakeLot(int x, int z, float altitude = 0.0f) {
    auto* lot = new cSC4Lot(x, z, 1, 1, 0);
    lot->AddRef();
    lot->SetAltitude(altitude);
    return lot;
}

bool IsOrthonormal(const cS3DTransform& transform) {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float dot = 0.0f;
            for (int k = 0; k < 3; ++k) dot += transform.rotation[k][row] * transform.rotation[k][column];
            const float expected = row == column ? 1.0f : 0.0f;
            if (std::fabs(dot - expected) > 1e-4f) return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // ---- construction / identity / QueryInterface ---------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        CHECK(boat->GetType() == kWatercraftOccupantType);
        CHECK(boat->GetGZCLSID() == kWatercraftIid);
        CHECK_NEAR(boat->GetRadius(), 0.0f);
        CHECK_NEAR(boat->GetDefaultSpeed(), 1.0f / 36.0f);
        CHECK(boat->Position().x == 0.0f && boat->Position().y == 0.0f && boat->Position().z == 0.0f);
        CHECK(boat->SimulatedPosition().x == 0.0f);
        CHECK(boat->AnimationLoadCount() == 0);
        CHECK(host.createdReaders == 2);   // current route reader + neighbour route reader

        cSC4Watercraft* watercraftInterface = nullptr;
        CHECK(boat->QueryInterface(kWatercraftIid, reinterpret_cast<void**>(&watercraftInterface)));
        CHECK(watercraftInterface == boat);
        watercraftInterface->Release();
        cISC4Occupant* occupant = nullptr;
        CHECK(boat->QueryInterface(0x26bb767e, reinterpret_cast<void**>(&occupant)));
        CHECK(occupant != nullptr);
        occupant->Release();
        cIGZMessageTarget2* target = nullptr;
        CHECK(boat->QueryInterface(0x452294aa, reinterpret_cast<void**>(&target)));
        CHECK(target == static_cast<cIGZMessageTarget2*>(boat));
        target->Release();
        CHECK(!boat->QueryInterface(0xdeadbeef, reinterpret_cast<void**>(&occupant)));

        // The default drive parameters (no automata group yet).
        const WatercraftDriveParameters& p = boat->DriveParameters();
        CHECK_NEAR(p.reverseAcceleration, 0.1f);
        CHECK_NEAR(p.movementScale, 0.2f);
        CHECK_NEAR(p.minSpeed, -0.1f);
        CHECK_NEAR(p.maxSpeed, 0.3f);
        CHECK_NEAR(p.crashStateTimer, 8000.0f);
        CHECK_NEAR(p.dockRequestRadius, 300.0f);
        CHECK_NEAR(p.dockDistance, 90.0f);
        CHECK_NEAR(p.rollRate, 1.0471976f);
        CHECK_NEAR(p.pitchRate, 0.13089970f);
        CHECK(p.rightTriggerThreshold == 0.0f);
        CHECK(boat->Release() == 0);
    }

    // ---- Write / Read -------------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        PortMemoryOStream os;
        CHECK(boat->Write(os));
        cSC4Watercraft* loaded = MakeBoat(host);
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        CHECK(loaded->Read(is));
        CHECK(loaded->GetType() == kWatercraftOccupantType);
        // The watercraft record version is 2: a different version is refused.
        PortMemoryOStream bad;
        bad.SetUint16(3);
        PortMemoryIStream badIn(bad.bytes().data(), bad.bytes().size());
        CHECK(!loaded->Read(badIn));
        CHECK(boat->Release() == 0);
        CHECK(loaded->Release() == 0);
    }

    // ---- Init / Shutdown / named animations / notifications ------------------------------------------------------
    {
        FakeHost host;
        host.group.hasParameters = true;
        host.group.parameters[0] = 0.0f;      // default
        host.group.parameters[4] = 0.5f;      // movement scale
        host.group.parameters[9] = 4.0f;      // max turn
        host.group.parameters[8] = 0.0f;      // -> -maxTurn
        host.group.parameters[18] = 0.5f;     // max roll
        host.group.parameters[7] = 0.25f;     // max speed
        host.group.parameters[17] = 0.0f;     // -> maxRoll / maxSpeed
        host.group.parameters[20] = 0.4f;     // max pitch
        host.group.parameters[19] = 0.0f;     // -> maxPitch / maxTurn
        host.group.parameters[33] = 0.0f;     // stays zero
        cSC4Watercraft* boat = MakeBoat(host);
        boat->SetAutomataGroup(&host.group);
        CHECK(boat->Init());
        CHECK(boat->IsInitialized());
        CHECK(host.createdModels == 1);
        CHECK(host.createdReaders == 2);
        CHECK(host.animationNames.size() == static_cast<std::size_t>(kWatercraftAnimationCount));
        for (int i = 0; i < kWatercraftAnimationCount; ++i) {
            CHECK(host.animationNames[static_cast<std::size_t>(i)] == kWatercraftAnimationNames[i]);
            CHECK(boat->Animation(i) != nullptr);
        }
        CHECK(boat->AnimationLoadCount() == kWatercraftAnimationCount);
        CHECK(boat->Animation(-1) == nullptr);
        CHECK(boat->Animation(kWatercraftAnimationCount) == nullptr);
        CHECK(host.addedNotifications == 1);
        CHECK(boat->AutomataFlags() == (0x8000u | 0x2000u | 0x20000u | 4u));
        CHECK(boat->IsActiveSoundEnabled());
        const WatercraftDriveParameters& p = boat->DriveParameters();
        CHECK_NEAR(p.reverseAcceleration, 0.1f);            // zero entry falls back to the default
        CHECK_NEAR(p.movementScale, 0.5f);
        CHECK_NEAR(p.maxTurn, 4.0f);
        CHECK_NEAR(p.minTurn, -4.0f);                       // derived from maxTurn
        CHECK_NEAR(p.rollRate, 2.0f);                       // maxRoll / maxSpeed
        CHECK_NEAR(p.maxRoll, 0.5f);
        CHECK_NEAR(p.pitchRate, 0.1f);                      // maxPitch / maxTurn
        CHECK_NEAR(p.maxPitch, 0.4f);
        CHECK(p.rightTriggerThreshold == 0.0f);

        // The stations are cleared and the base occupant is initialised.
        CHECK(boat->StationA().lot.get() == nullptr);
        CHECK(boat->StationB().lot.get() == nullptr);
        CHECK(boat->CurrentStation().lot.get() == nullptr);
        CHECK(boat->TowTarget() == nullptr && boat->Tower() == nullptr);

        // A second Init is a no-op.
        CHECK(boat->Init());
        CHECK(host.createdModels == 1);
        CHECK(host.animationNames.size() == static_cast<std::size_t>(kWatercraftAnimationCount));

        CHECK(boat->IsNeighborTravelBoat() == false);
        boat->AddOccupantGroup(kWatercraftGroupTravelBoat1);
        CHECK(boat->IsNeighborTravelBoat());
        CHECK(boat->BoatSize() == 1);
        CHECK(boat->Release() == 0);
        CHECK(host.removedNotifications == 1);   // Release -> Shutdown ran before the destructor
        CHECK(host.destroyedModels == 1);
    }

    // ---- BoatSize / neighbour groups ----------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* medium = MakeBoat(host);
        medium->AddOccupantGroup(kWatercraftGroupMediumBoat);
        CHECK(medium->BoatSize() == 2);
        cSC4Watercraft* large = MakeBoat(host);
        large->AddOccupantGroup(kWatercraftGroupLargeBoat);
        CHECK(large->BoatSize() == 3);
        cSC4Watercraft* travel = MakeBoat(host);
        travel->AddOccupantGroup(kWatercraftGroupTravelBoat3);
        CHECK(travel->IsNeighborTravelBoat());
        CHECK(travel->BoatSize() == 1);
        CHECK(medium->Release() == 0);
        CHECK(large->Release() == 0);
        CHECK(travel->Release() == 0);
    }

    // ---- state internal / crash state internal ------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetStateInternal(cSC4Watercraft::kChangeIdle);
        CHECK(boat->GetBehavior() == kWatercraftStateRemoveNow);
        boat->PopState();
        boat->SetStateInternal(cSC4Watercraft::kChangeRemove);
        CHECK(boat->GetBehavior() == kWatercraftStateRemoveFade);
        boat->PopState();
        boat->SetStateInternal(cSC4Watercraft::kChangeRunAground);
        CHECK(boat->GetBehavior() == kWatercraftStateRunAground);
        CHECK_NEAR(boat->GetState()->timer, 8000.0f);
        CHECK(boat->GetState()->flag);
        CHECK(!boat->CrashMarked());
        boat->PopState();
        boat->SetStateInternal(cSC4Watercraft::kChangeSinkRoll);
        CHECK(boat->GetBehavior() == kWatercraftStateSink);
        CHECK(boat->CrashMarked());   // quirk: case 4 sets the crash-marked byte then falls into case 3
        boat->PopState();
        boat->SetStateInternal(cSC4Watercraft::kChangeSink);
        CHECK(boat->GetBehavior() == kWatercraftStateSink);
        boat->PopState();

        // Only a player-driven boat enters a crash state; anything else is removed.
        boat->Crash_SetStateInternal(cSC4Watercraft::kChangeSink, false);
        CHECK(boat->GetBehavior() == kWatercraftStateRemoveFade);
        boat->PopState();
        boat->PushState(kStatePlayerDrive, nullptr, 0.0f, false, 0, 0, nullptr);
        boat->Crash_SetStateInternal(cSC4Watercraft::kChangeSink, false);
        CHECK(boat->GetBehavior() == kWatercraftStateSink);
        boat->PopState();
        boat->PopState();
        boat->Crash_SetStateInternal(cSC4Watercraft::kChangeRunAground, true);
        CHECK(boat->GetBehavior() == kWatercraftStateRunAground);
        CHECK(boat->Release() == 0);
    }

    // ---- OutOfBounds / OnLand / Engine_MovementPossible ----------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->MoveTo(nullptr, nullptr);
        CHECK(!boat->OutOfBounds());
        host.cityCellsX = 0;
        host.cityCellsZ = 0;
        CHECK(boat->OutOfBounds());   // the bounding box corner (1, _, 2) is outside an empty city
        host.cityCellsX = 64;
        host.cityCellsZ = 64;

        host.terrainY = -3.5f;
        host.cellTerrainY = -3.5f;
        CHECK(!boat->OnLand());
        host.cellTerrainY = 0.0f;    // the cell is above the boat
        CHECK(boat->OnLand());
        host.cellTerrainY = -3.5f;

        CHECK(boat->Engine_MovementPossible());
        boat->PushState(kWatercraftStateSink, nullptr, 0.0f, false, 0, 0, nullptr);
        CHECK(!boat->Engine_MovementPossible());
        boat->PopState();
        boat->SetMarkedForRemove(true);
        CHECK(!boat->Engine_MovementPossible());
        boat->SetMarkedForRemove(false);
        CHECK(boat->Engine_MovementPossible());
        CHECK(boat->Release() == 0);
    }

    // ---- movement helpers ----------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        // Movement_PositionStraight: step = dt * speed * movementScale.
        boat->SetSimulatedPosition(cS3DVector3{10.0f, -3.5f, 20.0f});
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        boat->SetSpeed(0.3f);
        boat->SetTurn(0.0f);
        cS3DTransform transform;
        cS3DVector3 position;
        boat->Movement_PositionStraight(1000.0f, transform, position);
        CHECK_NEAR(position.x, 70.0f);
        CHECK_NEAR(position.y, -3.5f);
        CHECK_NEAR(position.z, 20.0f);
        CHECK_NEAR(transform.translation[0], 70.0f);
        CHECK((transform.flags & 1) != 0);

        // Movement_Pitch writes an orthonormal rotation and marks the transform.
        cS3DTransform pitch;
        boat->SetTurn(2.0f);
        boat->Movement_Pitch(pitch);
        CHECK((pitch.flags & 4) != 0);
        CHECK(IsOrthonormal(pitch));

        // Movement_Rotation adds the roll and the planing lift.
        cS3DTransform rotation;
        rotation.translation[0] = 10.0f;
        rotation.translation[2] = 20.0f;
        boat->Movement_Rotation(rotation);
        CHECK((rotation.flags & 5) == 5);
        CHECK(IsOrthonormal(rotation));
        CHECK_NEAR(rotation.translation[1], 0.15707964f);   // maxRoll / 2

        // Engine_MovementPossible gates the whole movement step.
        boat->SetTurn(0.0f);
        cS3DVector3 before = boat->SimulatedPosition();
        boat->Movement_PositionNew(1000.0f);
        CHECK_NEAR(boat->SimulatedPosition().x, before.x + 60.0f);
        CHECK_NEAR(boat->PreviousPosition().x, before.x);
        CHECK_NEAR(boat->Position().x, boat->SimulatedPosition().x);
        CHECK(!host.models.empty());
        CHECK((host.models.front()->transform.flags & 5) == 5);

        // Zero speed and zero turn: no movement.
        boat->SetSpeed(0.0f);
        cS3DVector3 stopped = boat->SimulatedPosition();
        boat->Movement_PositionNew(1000.0f);
        CHECK_NEAR(boat->SimulatedPosition().x, stopped.x);

        // Crash_RegressToPreviousLocation rebuilds the previous position.
        boat->SetPreviousPosition(cS3DVector3{1.0f, 2.0f, 3.0f});
        boat->SetSimulatedPosition(cS3DVector3{9.0f, 9.0f, 9.0f});
        boat->Crash_RegressToPreviousLocation();
        CHECK_NEAR(boat->SimulatedPosition().x, 1.0f);
        CHECK_NEAR(boat->Position().y, 2.0f);
        CHECK_NEAR(host.models.front()->transform.translation[2], 3.0f);
        CHECK(boat->Release() == 0);
    }

    // ---- orientation helpers / acceleration curve ----------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        CHECK(boat->Helper_OrientationCloseEnough(cS3DVector3{1.0f, 0.0f, 0.0f}));
        CHECK(!boat->Helper_OrientationCloseEnough(cS3DVector3{0.0f, 0.0f, 1.0f}));
        CHECK_NEAR(boat->SimulatedOrientation().x, 1.0f);

        // AccelerationFunction is monotonic over the drive range and stays inside it.
        float previous = -1.0e30f;
        for (float value = -2000.0f; value <= 3000.0f; value += 500.0f) {
            const float result = boat->AccelerationFunction(value);
            CHECK(result >= boat->DriveParameters().minSpeed - 1e-5f);
            CHECK(result <= boat->DriveParameters().maxSpeed + 1e-5f);
            CHECK(result > previous);
            previous = result;
        }
        CHECK(boat->AccelerationFunction(0.0f) == 0.0f);
        CHECK(boat->AccelerationFunctionInverse(0.0f) == 0.0f);
        CHECK(boat->AccelerationFunctionInverse(0.3f) >= -2000.0f);
        CHECK(boat->AccelerationFunctionInverse(0.3f) <= 3000.0f);

        // Assist_PickInitialDir: the eight compass directions (0 = north-west is unreachable).
        struct Case { float x, z; int direction; };
        const Case cases[] = {
            {0.0f, -1.0f, 1}, {1.0f, -1.0f, 2}, {1.0f, 0.0f, 3}, {1.0f, 1.0f, 4},
            {0.0f, 1.0f, 5}, {-1.0f, 1.0f, 6}, {-1.0f, 0.0f, 7},
        };
        for (const Case& c : cases) {
            boat->SetSimulatedOrientation(cS3DVector3{c.x, 0.0f, c.z});
            CHECK(boat->Assist_PickInitialDir() == c.direction);
        }
        boat->SetSimulatedOrientation(cS3DVector3{0.0f, 0.0f, 0.0f});
        CHECK(boat->Assist_PickInitialDir() == 1);

        // Assist_PickNewDir and Assist_DirIntoCell tables.
        for (int option = 0; option < 8; ++option) {
            int out = 3;
            boat->Assist_PickNewDir(option, 0, &out);
            const int expected[8] = {0, 1, -1, 2, -2, 3, -3, 4};   // signed %, the original's quirk
            CHECK(out == expected[option]);
        }
        int untouched = 5;
        boat->Assist_PickNewDir(9, 3, &untouched);
        CHECK(untouched == 5);
        {
            int x = 0, z = 0;
            boat->Assist_DirIntoCell(0, x, z);
            CHECK(x == -1 && z == -1);
            x = 0; z = 0;
            boat->Assist_DirIntoCell(1, x, z);
            CHECK(x == 0 && z == -1);
            x = 0; z = 0;
            boat->Assist_DirIntoCell(3, x, z);
            CHECK(x == 1 && z == 0);
            x = 0; z = 0;
            boat->Assist_DirIntoCell(4, x, z);
            CHECK(x == 1 && z == 1);
            x = 0; z = 0;
            boat->Assist_DirIntoCell(6, x, z);
            CHECK(x == -1 && z == 1);
            x = 0; z = 0;
            boat->Assist_DirIntoCell(99, x, z);
            CHECK(x == 0 && z == 0);
        }

        // Lookahead helpers.
        boat->Lookahead().push_back(WatercraftTravelPoint{0, 0, 0, 0});
        boat->Lookahead().push_back(WatercraftTravelPoint{0, -1, 0, 1});
        boat->Lookahead().push_back(WatercraftTravelPoint{1, -1, 0, 2});
        boat->Lookahead().push_back(WatercraftTravelPoint{1, 0, 0, 3});
        CHECK(boat->Assist_DirLookup(0) == 1);
        CHECK(boat->Assist_DirLookup(1) == 3);
        // Quirk: the last two points are refused even though index == size-2 has a successor.
        CHECK(boat->Assist_DirLookup(2) == 0);
        CHECK(boat->Assist_DirLookup(3) == 0);
        CHECK(boat->Assist_DirLookup(-1) == 0);
        CHECK(boat->InvalidateLookaheadCell(0));
        CHECK(boat->Lookahead().size() == 1);   // the vector is truncated to index+1
        CHECK((boat->Lookahead()[0].flags & 2u) != 0);
        CHECK(!boat->InvalidateLookaheadCell(5));
        CHECK(boat->Release() == 0);
    }

    // ---- Crash_PitchDetermination / Crash_InanimateCollisionDetection / Crash_DetermineCase ---------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedOrientation(cS3DVector3{0.0f, 0.0f, 1.0f});   // dir 5
        boat->Crash_PitchDetermination(true, false);
        CHECK(boat->CrashRollSign() == 1);
        CHECK(boat->CrashPitchSign() == 0);
        boat->SetSimulatedOrientation(cS3DVector3{0.0f, 0.0f, -1.0f});   // dir 1 -> default toggles the first flag
        boat->Crash_PitchDetermination(false, true);
        CHECK(boat->CrashRollSign() <= 1);
        CHECK(boat->CrashPitchSign() <= 1);

        // All four corners in water and no lot: no inanimate collision.
        CHECK(!boat->Crash_InanimateCollisionDetection());
        // A lot under one corner: collision, and the pitch flags are set.
        SC4CellRect rect;
        boat->GetBoundingCityCells(rect);
        host.lotCells.insert({rect.x1, rect.z1});
        CHECK(boat->Crash_InanimateCollisionDetection());
        host.lotCells.clear();

        // All corners dry: Crash_DetermineCase picks a crash state; a non-player boat is removed instead.
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        host.landCells.insert({rect.x0, rect.z0});
        boat->Crash_DetermineCase();
        CHECK(boat->GetBehavior() == kWatercraftStateRemoveFade);
        boat->ClearStateStack();
        boat->PushState(kStatePlayerDrive, nullptr, 0.0f, false, 0, 0, nullptr);
        boat->Crash_DetermineCase();
        CHECK(boat->GetBehavior() == kWatercraftStateRunAground || boat->GetBehavior() == kWatercraftStateSink);
        CHECK(boat->Release() == 0);
    }

    // ---- ferry stations ------------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        cSC4Lot* lotA = MakeLot(10, 10);
        cSC4Lot* lotB = MakeLot(20, 20);
        FerryStationInformation a;
        a.lot = lotA;
        a.position = cS3DVector3{100.0f, 0.0f, 100.0f};
        a.dockDirection = cS3DVector3{0.0f, 0.0f, 1.0f};
        AddRoute(a.route, 1, 2);
        AddRoute(a.route, 3, 4);
        a.flags = 0x100;   // route available
        FerryStationInformation b;
        b.lot = lotB;
        b.position = cS3DVector3{200.0f, 0.0f, 200.0f};

        boat->Ferry_SetStationInformation(a, b, true);
        CHECK(boat->StationA().lot.get() == lotA);
        CHECK(boat->StationB().lot.get() == lotB);
        CHECK(boat->StationA().position.x == 100.0f);
        CHECK(boat->StationB().position.x == 200.0f);
        CHECK(boat->CurrentStation().lot.get() == lotA);
        CHECK(boat->CurrentStation().routeIndex == 2);   // 0 becomes 2
        CHECK(boat->CurrentStation().dockRequested == 1);
        CHECK(boat->DockingActive());
        CHECK(boat->CurrentRouteReader() != nullptr);
        CHECK(host.readers[0]->setDataCalls == 1);

        // Other station: A and B differ -> A.
        CHECK(boat->Ferry_OtherStationInformation().position.x == 100.0f);
        // Quirk: A == B returns B.
        FerryStationInformation same = a;
        same.position = b.position;
        boat->StationA() = same;
        CHECK(boat->Ferry_OtherStationInformation().position.x == 200.0f);
        boat->StationA() = a;

        // SwitchActiveStation needs both stations queued; then the current station becomes the other one.
        boat->StationA().SetQueued(true);
        boat->StationB().SetQueued(true);
        boat->Ferry_SwitchActiveStation();
        CHECK(boat->CurrentStation().lot.get() == lotA);   // A is the other station (A != B)
        CHECK(host.readers[0]->setDataCalls == 2);

        // Dock requests: in range and permission -> docking state.
        boat->SetSimulatedPosition(cS3DVector3{100.0f, 0.0f, 100.0f});
        host.dockPermission = true;
        boat->Ferry_DockRequests();
        CHECK(boat->GetBehavior() == kWatercraftStateDocking);
        CHECK(host.requestedDock == 1);
        boat->PopState();
        // No permission but waiting allowed -> queueing state.
        host.dockPermission = false;
        host.waitPermission = true;
        boat->Ferry_DockRequests();
        CHECK(boat->GetBehavior() == kWatercraftStateQueueing);
        CHECK(host.requestedWait == 1);
        boat->PopState();
        // No permission and no waiting -> remove state.
        host.waitPermission = false;
        boat->Ferry_DockRequests();
        CHECK(boat->GetBehavior() == kWatercraftStateRemoveFade);
        boat->PopState();
        host.dockPermission = true;

        // ClearDockRequests gives the dock back once the boat is out of range.
        boat->CurrentStation().dockRequested = 1;
        boat->SetDockingActive(true);
        boat->SetSimulatedPosition(cS3DVector3{1000.0f, 0.0f, 1000.0f});
        const int dockingBaseline = host.doneDocking;
        boat->Ferry_ClearDockRequests();
        CHECK(host.doneDocking == dockingBaseline + 1);
        CHECK(boat->CurrentStation().dockRequested == 0);
        CHECK(!boat->DockingActive());

        // EndDocking releases a requested dock.
        boat->CurrentStation().dockRequested = 1;
        boat->SetDockingActive(true);
        boat->EndDocking();
        CHECK(host.doneDocking == dockingBaseline + 2);

        // EndTowing drops both links.
        boat->Release();
        lotA->Release();
        lotB->Release();
    }

    // ---- route lookahead / AI location picking -------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});   // cell (5,5) centre
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});

        // No station route: the AI scans the eight directions and fills the lookahead.
        CHECK(boat->Engine_AIPickLocation());
        CHECK(boat->Lookahead().size() == 10);
        CHECK(boat->Lookahead()[0].direction >= 0);
        for (const WatercraftTravelPoint& point : boat->Lookahead()) {
            CHECK(host.CellInCity(point.x, point.z));
        }

        // With a station route, the first `routeIndex` points are skipped and the rest is taken from the reader.
        boat->Lookahead().clear();
        FerryStationInformation station;
        station.routeIndex = 2;
        station.flags = 0x100;   // route available
        for (int i = 0; i < 6; ++i) AddRoute(station.route, static_cast<std::int16_t>(20 + i), 20);
        boat->Ferry_InitializeCurrentRoute(station);
        CHECK(boat->CurrentStation().routeIndex == 2);
        CHECK(boat->Engine_AIPickLocation());
        CHECK(boat->Lookahead().size() == 4);   // 6 points, 2 skipped
        CHECK(boat->Lookahead()[0].x == 22);

        // Engine_AIBoatMotion does not move the boat (Movement_PositionNew does) but drives the lookahead.
        boat->SetSpeed(0.0f);
        boat->Engine_AIBoatMotion(100.0f);
        CHECK(boat->Speed() == 0.0f);

        // A neighbour route feeds the same lookahead through its reader.
        boat->Lookahead().clear();
        FakeNeighbour neighbour;
        neighbour.position[0] = 88.0f;
        neighbour.position[1] = -3.5f;
        neighbour.position[2] = 88.0f;
        std::vector<std::uint8_t> neighbourRoute;
        for (int i = 0; i < 3; ++i) AddRoute(neighbourRoute, 5, static_cast<std::int16_t>(6 + i));
        boat->SetNbrRouteData(neighbourRoute, &neighbour);
        CHECK(boat->NeighbourRouteSet());
        CHECK(boat->Neighbour() == &neighbour);
        CHECK(boat->NeighbourRouteReader() != nullptr);
        CHECK(host.readers[1]->setDataCalls == 1);
        CHECK(boat->Release() == 0);
    }

    // ---- DoAnimationTick / effects ------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        // The still path: after 5000 ms without moving the original returns false.
        bool returnedFalse = false;
        for (int i = 0; i < 8; ++i) {
            if (!boat->DoAnimationTick(0, 1000.0f)) {
                returnedFalse = true;
                break;
            }
        }
        CHECK(returnedFalse);
        CHECK(boat->StillTimer() > 0.0f);
        CHECK(!host.effects.stoppedNames.empty());   // StopRegularEffects ran at 250 ms
        for (const std::string& name : host.effects.stoppedNames) {
            CHECK(name == "SMLWake" || name == "SMLWash" || name == "SMLMedWake" || name == "SMLMedWash" ||
                  name == "LargeWake" || name == "LargeWash");
        }
        // StartRegularEffects starts the six named effects.
        const std::size_t before = host.effects.startedNames.size();
        boat->StartRegularEffects();
        CHECK(host.effects.startedNames.size() == before + 6);
        CHECK(host.effects.startedNames[before] == "SMLWake");
        CHECK(host.effects.startedNames[before + 5] == "LargeWash");
        // A moving boat restarts the still timer and does not hit the 5000 ms path.
        boat->SetSpeed(0.3f);
        CHECK(boat->DoAnimationTick(0, 1000.0f));
        CHECK(boat->StillTimer() == 0.0f);
        CHECK(boat->ReverseTimer() == 0.0f);
        boat->SetSpeed(-0.1f);
        CHECK(boat->DoAnimationTick(0, 1000.0f));
        CHECK_NEAR(boat->ReverseTimer(), 1000.0f);
        CHECK(boat->Release() == 0);
    }

    // ---- collision response --------------------------------------------------------------------------------------
    {
        FakeHost host;
        host.group.hasParameters = true;
        host.group.parameters[15] = 10.0f;   // boat collision threshold above any ratio
        cSC4Watercraft* boat = MakeBoat(host);
        boat->SetAutomataGroup(&host.group);
        boat->Init();
        cSC4Watercraft* other = MakeBoat(host);
        other->Init();
        host.collisionBoat = other;
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        boat->SetPreviousPosition(cS3DVector3{80.0f, -3.5f, 88.0f});
        boat->SetSpeed(0.3f);
        // The regression path reports false (only a crash reports true) but still regresses and scales.
        CHECK(!boat->Engine_DetectCollision());
        // Quirk: the boat regresses and keeps 75% of its speed.
        CHECK_NEAR(boat->SimulatedPosition().x, 80.0f);
        CHECK_NEAR(boat->Speed(), 0.225f);
        CHECK_NEAR(other->Speed(), 0.3f);
        host.collisionBoat = nullptr;
        CHECK(!boat->Engine_DetectCollision());
        CHECK(boat->Release() == 0);
        CHECK(other->Release() == 0);
    }

    // ---- docking / undocking / following -------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        boat->StationA().position = cS3DVector3{95.0f, -3.5f, 88.0f};
        boat->StationB().position = cS3DVector3{200.0f, -3.5f, 80.0f};   // A != B, so A is "other"
        CHECK(boat->DoDockingTick(100.0f));
        CHECK(boat->SimulatedPosition().x > 88.0f);

        // Undocking backs away from the neighbour while in water.
        boat->SetRadius(10.0f);
        FakeNeighbour neighbour;
        neighbour.position[0] = 88.0f;
        neighbour.position[1] = -3.5f;
        neighbour.position[2] = 88.0f;
        boat->SetNeighbour(&neighbour);
        boat->SetSimulatedPosition(cS3DVector3{92.0f, -3.5f, 88.0f});
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        CHECK(boat->DoUndockingTick(100.0f));
        CHECK(boat->SimulatedPosition().x > 92.0f);

        // Following uses the tower's tail; without a tower it reports false.
        CHECK(!boat->DoFollowingTick(100.0f));
        cSC4Watercraft* leader = MakeBoat(host);
        leader->Init();
        leader->SetSimulatedPosition(cS3DVector3{120.0f, -3.5f, 88.0f});
        leader->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        leader->SetRadius(4.0f);
        boat->SetTower(leader);
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        CHECK(boat->DoFollowingTick(100.0f));
        CHECK(boat->SimulatedPosition().x > 88.0f);
        boat->SetTower(nullptr);
        CHECK(boat->Release() == 0);
        CHECK(leader->Release() == 0);
    }

    // ---- player drive --------------------------------------------------------------------------------------------
    {
        FakeHost host;
        host.group.hasParameters = true;
        host.group.parameters[4] = 0.2f;
        host.group.parameters[7] = 0.3f;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->SetAutomataGroup(&host.group);
        boat->Init();
        boat->PushState(kStatePlayerDrive, nullptr, 0.0f, false, 0, 0, nullptr);
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(boat->GetState());
        CHECK(drive != nullptr);
        drive->history[0] = 0.5f;   // right trigger
        drive->history[1] = -1.0f;
        drive->steering6c = 0.25f;
        drive->hint117 = 1;
        CHECK(boat->TranslatePlayerDriveCommands(drive, 1000.0f));
        CHECK(boat->DriveMode() == 2);
        CHECK_NEAR(drive->value38, 0.5f);
        CHECK(boat->Speed() != 0.0f);
        CHECK_NEAR(drive->steerVisual264, -0.03125f);
        CHECK_NEAR(drive->wakeTurn26c, 6.2831855f);
        CHECK(boat->Animation(5) != nullptr);
        CHECK(static_cast<FakeAnimation*>(boat->Animation(5))->plays > 0);   // waterski starts while moving
        // Idle with the tow hint: the boat starts looking for a tow.
        drive->history[0] = -1.0f;
        drive->history[1] = -1.0f;
        drive->hint117 = 0;
        drive->hint119 = 1;
        boat->SetSpeed(0.0f);
        CHECK(boat->TranslatePlayerDriveCommands(drive, 1000.0f));
        CHECK(boat->TowSearchActive());

        // DoPlayerDriveTick lets the base advance the command history and then moves the boat.
        drive->history[0] = -1.0f;
        drive->history[1] = -1.0f;
        boat->SetSpeed(0.1f);
        const cS3DVector3 before = boat->SimulatedPosition();
        CHECK(boat->DoPlayerDriveTick(1000.0f));
        CHECK(boat->SimulatedPosition().x != before.x);
        CHECK(boat->Release() == 0);
    }

    // ---- crash sequences ----------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        boat->SetCrashTimer(100.0f);
        CHECK(boat->Crash_SequenceRunAground(100.0f));
        CHECK(boat->CrashProgress() != 0.0f);
        boat->SetCrashRollEnabled(false);
        boat->SetCrashTimer(100.0f);
        CHECK(boat->Crash_SequenceRunAground(100.0f));
        boat->SetCrashTimer(100.0f);
        CHECK(boat->Crash_SequenceSink());
        // Once the sink progress reaches its cap the sequence reports false.
        boat->SetCrashTimer(8000.0f);
        boat->Crash_SequenceSink();
        boat->SetCrashTimer(8000.0f);
        boat->Crash_SequenceSink();
        CHECK(!boat->Crash_SequenceSink());
        // Out of bounds: DoCrashSequenceTick pushes the remove state.
        host.cityCellsX = 0;
        host.cityCellsZ = 0;
        boat->PushState(kWatercraftStateSink, nullptr, 0.0f, false, 0, 0, nullptr);
        boat->DoCrashSequenceTick(100.0f);
        CHECK(boat->FindState(kWatercraftStateRemoveNow) != nullptr);
        CHECK(boat->Release() == 0);
    }

    // ---- SetNbrRouteData / DoMessage ----------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        boat->SetRadius(10.0f);   // limit = 30
        FakeNeighbour neighbour;
        neighbour.position[0] = 88.0f;
        neighbour.position[1] = -3.5f;
        neighbour.position[2] = 88.0f;
        std::vector<std::uint8_t> route;
        AddRoute(route, 5, 5);   // cell (5,5) centre = (88, 88), distance 0
        AddRoute(route, 6, 5);
        boat->SetNbrRouteData(route, &neighbour);
        CHECK(boat->NeighbourRouteSet());
        CHECK(boat->Neighbour() == &neighbour);
        CHECK(boat->NeighbourRoute().size() == 8);
        CHECK(host.readers[1]->setDataCalls == 1);

        // The 0x26d31ec1 message reloads the drive parameters.
        cRZMessage2Standard message;
        message.SetType(kWatercraftMessageReloadDriveParams);
        CHECK(boat->DoMessage(&message));
        // An unknown message is not handled.
        message.SetType(0x12345678);
        CHECK(!boat->DoMessage(&message));
        // The tow message clears the tow-search flag.
        boat->SetTowSearchActive(true);
        message.SetType(kWatercraftMessageTow);
        boat->DoMessage(&message);
        CHECK(!boat->TowSearchActive());
        CHECK(boat->Release() == 0);
    }

    // ---- statistics text ----------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Watercraft* boat = MakeBoat(host);
        boat->Init();
        boat->SetSimulatedPosition(cS3DVector3{88.0f, -3.5f, 88.0f});
        boat->SetSimulatedOrientation(cS3DVector3{1.0f, 0.0f, 0.0f});
        const char* text = boat->GetDebugExtraText();
        CHECK(text != nullptr);
        CHECK(std::strstr(text, "Tow: 0") != nullptr);
        CHECK(std::strstr(text, "(FT,MT): (3,4)") != nullptr);
        CHECK(std::strstr(text, "Cell(  5,  5)") != nullptr);
        CHECK(boat->Release() == 0);
    }

    std::printf(failures == 0 ? "sc4_watercraft_test: OK\n" : "sc4_watercraft_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
