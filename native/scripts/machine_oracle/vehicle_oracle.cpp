// vehicle_oracle - test tooling: the cSC4Pedestrian maths against the retail machine code (see GameImage.h). It is named
// "vehicle" because scripts/run_machine_oracle.sh links the sources of the matching unit test (sc4_vehicle, which holds the
// pedestrian test). Random positions, orientations and random-number draws go into a fake original pedestrian (fields at
// their original offsets, virtual calls answered by a slot table) and into the port; what the original passes to
// MoveForwardWithCollision / MoveTo / PlaceOnNetwork and the orientation UpdateOrientation writes must agree bit for bit
// with the port. Build and run with scripts/run_machine_oracle.sh vehicle.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <set>
#include <vector>

#include "GameImage.h"
#include "sc4/Pedestrian.h"
#include "sc4/TrafficNetworkMap.h"

using namespace sc4;
using namespace machine_oracle;

namespace {

int gMismatches = 0, gChecks = 0;
bool SameBits(const float* a, const float* b, int n) { return std::memcmp(a, b, n * sizeof(float)) == 0; }

// ---- the scripted random numbers both sides draw ----------------------------------------------------------------------
std::deque<double> gDoubles;
std::uint32_t gBelow = 0;
double NextDouble() {
    const double v = gDoubles.empty() ? 0.0 : gDoubles.front();
    if (!gDoubles.empty()) gDoubles.pop_front();
    return v;
}

// ---- the original side -----------------------------------------------------------------------------------------------
FakeService originalVtable, leaderVtable, controllerVtable, city, network;
long gStackGuard = 0x5ca1ab1e;
float gMoveVector[3];
int gMoveCalls = 0;
float gMovedTo[3], gMovedDirection[3];
int gMoveToCalls = 0;
float gPlaced[3];
int gPlaceCalls = 0;
float gLeaderPosition[3], gControllerPosition[3];

double RandomDoubleHook(void*, double, double) { return NextDouble(); }
std::uint32_t RandomBelowHook(void*, std::uint32_t) { return gBelow; }
char VerifyHook(void*) { return 1; }
char MoveForwardHook(void*, const float* v, float) { std::memcpy(gMoveVector, v, sizeof gMoveVector); ++gMoveCalls; return 1; }
void SetDesiredHook(void*, float, float) {}
char NoCollisionHook(void*, const float*, float) { return 0; }
void* GetStateSlot(void* self) { return *reinterpret_cast<void**>(static_cast<char*>(self) + 0x400); }   // the test keeps the state pointer at +0x400
void NothingSlot(void*) {}
void RemoveSlot(void*, bool) {}
void MoveToSlot(void*, const float* position, const float* direction) {
    std::memcpy(gMovedTo, position, sizeof gMovedTo);
    if (direction) std::memcpy(gMovedDirection, direction, sizeof gMovedDirection);
    ++gMoveToCalls;
}
void GetLeaderPositionSlot(void*, float* out) { std::memcpy(out, gLeaderPosition, sizeof gLeaderPosition); }
void GetControllerPositionSlot(void*, float* out) { std::memcpy(out, gControllerPosition, sizeof gControllerPosition); }
void* NetworkSlot(void*) { return &network; }
void PlaceSlot(void*, const float*, const float* to, std::uint32_t) { std::memcpy(gPlaced, to, sizeof gPlaced); ++gPlaceCalls; }

alignas(16) unsigned char original[0x420];
alignas(16) unsigned char originalState[0x60];
alignas(16) unsigned char originalController[0x20];
alignas(16) unsigned char originalLeader[0x20];

template <typename T> T& Field(unsigned char* object, int offset) { return *reinterpret_cast<T*>(object + offset); }

void ResetOriginal(const float* position, const float* orientation, unsigned stateType) {
    std::memset(original, 0, sizeof original);
    std::memset(originalState, 0, sizeof originalState);
    Field<void*>(original, 0) = originalVtable.slots;
    Field<void*>(original, 0x400) = originalState;
    std::memcpy(original + 0x74, position, 12);
    std::memcpy(original + 0x8c, orientation, 12);
    Field<std::uint32_t>(originalState, 8) = stateType;
    gMoveCalls = gMoveToCalls = gPlaceCalls = 0;
}

// ---- the port side ---------------------------------------------------------------------------------------------------
struct OracleAutomatonHost final : IAutomatonHost {
    IAutomatonModel* CreateModel(const cGZPersistResourceKey&) override { return nullptr; }
    void DestroyModel(IAutomatonModel*) override {}
    IAutomatonEffects* CreateEffects() override { return nullptr; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { return true; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { return true; }
    void MoveModelInRenderer(IAutomatonModel*) override {}
    void ChangeModelInRenderer(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return true; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return true; }
    bool HasCity() override { return true; }
    float CitySizeX() override { return 1024.0f; }
    float CitySizeZ() override { return 1024.0f; }
    float TerrainHeight(float, float) override { return 0.0f; }
    bool NetworkAt(int, int) override { return true; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, const cS3DVector3& to, std::uint32_t) override {
        gPlaced[0] = to.x; gPlaced[1] = to.y; gPlaced[2] = to.z;
        ++gPlaceCalls;
    }
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomataGroupTemplate* FindAutomataGroup(std::uint32_t) override { return nullptr; }
    bool AnimationUpdateAllowed() override { return true; }
    float SimSpeedModifier() override { return 1.0f; }
    float MaxSpeedUnits() override { return 3600.0f; }
    float FadeDurationSeconds() override { return 0.2f; }
    void PostMessage(cIGZMessage2*) override {}
    std::uint32_t GenerateSoundInstanceId() override { return 1; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t) override { return gBelow; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double, double) override { return NextDouble(); }
    double RandomUnit() override { return 0.5; }
};

struct StreetBase : cISC4Occupant {
    int refs = 1;
    SC4CellRect cells{2, 3, 2, 3};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float*) override {}
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect& r) override { r = cells; }
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
struct Street : ITrafficNetworkMapOccupant {
    StreetBase base;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool HasNetworkFlag(std::uint32_t) const override { return true; }
    bool HasAnyNetworkFlag(std::uint32_t) const override { return true; }
    void SetNetworkFlag(std::uint32_t) override {}
    void ClearNetworkFlag(std::uint32_t) override {}
    bool ConnectsToNeighbor(int, std::uint32_t) const override { return true; }
    cISC4Occupant* AsOccupant() override { return &base; }
    ITrafficNetworkMapPathInfo* GetPathInfo() override { return nullptr; }
    void ReleasePathInfo() override {}
    bool PathConnectionExists(std::uint32_t, std::uint8_t, std::uint8_t) const override { return true; }
    bool IsIntersection() const override { return false; }
    float GetAltitude(float, float) const override { return 0.0f; }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { x = 2; z = 3; }
    bool IsOfType(std::uint32_t) const override { return true; }
    bool IsUsable() const override { return true; }
};
struct MapHost final : ITrafficNetworkMapHost {
    int CityCellsX() override { return 8; }
    int CityCellsZ() override { return 8; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = int(std::floor(x / 16.0f)); cz = int(std::floor(z / 16.0f)); }
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

struct OraclePedestrianHost final : IPedestrianHost {
    cSC4TrafficNetworkMap* map = nullptr;
    cSC4TrafficNetworkMap* TrafficNetworkMap() override { return map; }
    float CitySizeX() override { return 1024.0f; }
    float CitySizeZ() override { return 1024.0f; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = int(std::floor(x / 16.0f)); cz = int(std::floor(z / 16.0f)); }
    int CityCellsX() override { return 8; }
    int CityCellsZ() override { return 8; }
    ITrafficAutomataManager* TrafficAutomataManager() override { return nullptr; }
    cISC4PathFinder* PathFinder() override { return nullptr; }
    void NeighborConnectionCells(std::vector<std::int32_t>&) override {}
    void StartAudioEvent(std::uint32_t) override {}
    void StopAudioEvent(std::uint32_t) override {}
    ITrafficParticleSystem* CreateParticleSystem() override { return nullptr; }
    ITrafficAutomatonOccupant* ReadNetworkOccupant(cIGZIStream&) override { return nullptr; }
    const TrafficAutomatonTuning* Tuning() override { return nullptr; }
    void OccupantsInBox(const float*, const float*, std::uint32_t, int, std::vector<cISC4Occupant*>&) override {}
    void IgniteOccupant(cISC4Occupant*) override {}
    ITrafficNetworkMapLot* LotAt(int, int) override { return nullptr; }
    const PedestrianTuning* PedestrianTuningTable() override { return nullptr; }
};

struct PositionController final : cSC4AutomataController {
    bool CheckFlag(std::uint32_t) const override { return false; }
    void GetPosition(float* xyz) const override { std::memcpy(xyz, gControllerPosition, 12); }
    cISC4Occupant* GetOccupant() const override { return nullptr; }
};

// The port pedestrian with its moves captured and its fields reachable.
struct OraclePedestrian final : cSC4Pedestrian {
    using cSC4Pedestrian::cSC4Pedestrian;
    float movedTo[3] = {}, movedDirection[3] = {};
    int moves = 0;
    void MoveTo(const float* position, const float* direction) override {
        std::memcpy(movedTo, position, 12);
        if (direction) std::memcpy(movedDirection, direction, 12);
        ++moves;
    }
    void UpdateAttraction() override {}
    void Place(const float* position, const float* orientation) {
        position_ = {position[0], position[1], position[2]};
        orientation_ = {orientation[0], orientation[1], orientation[2]};
    }
    void SetLeaderObject(cSC4Automaton* leader) { leader_ = leader; }
    float* CrowdDirection() { return &crowdReactionDirection_.x; }
    void SetAttractionForOracle(const float* v) { attraction_ = {v[0], v[1], v[2]}; }
};

void Report(const char* what, bool same, int trial, const float* port, const float* orig, int n) {
    ++gChecks;
    if (same) return;
    ++gMismatches;
    if (gMismatches <= 12) {
        std::printf("MISMATCH %s trial %d: port", what, trial);
        for (int i = 0; i < n; ++i) std::printf(" %.9g", static_cast<double>(port[i]));
        std::printf(" / original");
        for (int i = 0; i < n; ++i) std::printf(" %.9g", static_cast<double>(orig[i]));
        std::printf("\n");
    }
}

}  // namespace

int main() {
    InstallCrashReport();
    if (!MapGame("game-binaries/MacOS/Sim City 4 Deluxe Editionsub")) return 2;
    At<long*>(0x100a657c8) = &gStackGuard;
    originalVtable.Set(0x1c0, reinterpret_cast<const void*>(&GetStateSlot));
    originalVtable.Set(0x380, reinterpret_cast<const void*>(&NothingSlot));   // UpdateAttraction
    originalVtable.Set(0x68, reinterpret_cast<const void*>(&RemoveSlot));
    originalVtable.Set(0x80, reinterpret_cast<const void*>(&MoveToSlot));
    leaderVtable.Set(0x38, reinterpret_cast<const void*>(&GetLeaderPositionSlot));
    controllerVtable.Set(0x68, reinterpret_cast<const void*>(&GetControllerPositionSlot));
    network.Set(0x160, reinterpret_cast<const void*>(&PlaceSlot));
    city.Set(0x140, reinterpret_cast<const void*>(&NetworkSlot));
    city.Install(0x100b21308);   // SL::spCity
    if (!Hook(0x10003db14, reinterpret_cast<const void*>(&RandomDoubleHook)) ||     // RandomDoubleRangeUniform
        !Hook(0x10003da18, reinterpret_cast<const void*>(&RandomBelowHook)) ||      // RandomUint32Uniform
        !Hook(0x1000f328e, reinterpret_cast<const void*>(&VerifyHook)) ||           // VerifyNetworkOccupant
        !Hook(0x1000f2de8, reinterpret_cast<const void*>(&MoveForwardHook)) ||      // MoveForwardWithCollision
        !Hook(0x1000f2ffa, reinterpret_cast<const void*>(&NoCollisionHook)) ||      // TestCollisionExpensive
        !Hook(0x1000f1728, reinterpret_cast<const void*>(&SetDesiredHook))) {       // TweenStruct::SetDesired
        std::perror("hook");
        return 2;
    }
    using Orient = void (*)(void*, const float*, float*);
    using Tick = char (*)(void*, float);
    using RiotLeader = char (*)(void*, float, bool);
    const auto updateOrientation = Function<Orient>(0x1000f34d8);
    const auto riotFollower = Function<Tick>(0x1000f293a);
    const auto riotLeader = Function<RiotLeader>(0x1000f1ca8);
    const auto crowd = Function<Tick>(0x1000f21c2);
    const auto beeLine = Function<Tick>(0x1000f2124);

    OracleAutomatonHost automatonHost;
    SetAutomatonHost(&automatonHost);
    MapHost mapHost;
    cSC4TrafficNetworkMap map(&mapHost);
    map.Init();
    Street street;
    map.HandleNetworkOccupantUsable(&street, false);   // a tile at cell (2, 3)
    OraclePedestrianHost host;
    host.map = &map;
    auto* port = new OraclePedestrian(&host);
    port->AddRef();
    auto* portLeader = new cSC4Automaton(&automatonHost);
    portLeader->AddRef();
    port->SetLeaderObject(portLeader);

    std::mt19937 rng(20261029);
    std::uniform_real_distribution<float> inCellX(32.0f, 47.9f), inCellZ(48.0f, 63.9f), height(-5.0f, 40.0f),
        axis(-1.0f, 1.0f), ms(1.0f, 90.0f), wide(-60.0f, 60.0f);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const int trials = 4000;
    for (int t = 0; t < trials; ++t) {
        const float position[3] = {inCellX(rng), height(rng), inCellZ(rng)};
        float orientation[3] = {axis(rng), axis(rng), axis(rng)};
        if (t % 9 == 0) { orientation[0] = 0.0f; orientation[2] = 0.0f; }   // no horizontal heading
        if (t % 11 == 0) orientation[2] = 0.0f;
        const float deltaMs = ms(rng);

        // ---- UpdateOrientation (including targets on the position) ----
        {
            float target[3] = {position[0] + wide(rng), height(rng), position[2] + wide(rng)};
            if (t % 7 == 0) { target[0] = position[0]; target[2] = position[2]; }
            if (t % 13 == 0) { target[0] = position[0] + 3e-6f; target[2] = position[2] - 4e-6f; }
            ResetOriginal(position, orientation, 0);
            float fromOriginal[3] = {7, 7, 7}, fromPort[3] = {8, 8, 8};
            updateOrientation(original, target, fromOriginal);
            port->Place(position, orientation);
            port->UpdateOrientation(cS3DVector3{target[0], target[1], target[2]}, fromPort);
            Report("UpdateOrientation", SameBits(fromOriginal, fromPort, 3), t, fromPort, fromOriginal, 3);
        }

        // ---- the riot leader's steering (the cell is the remembered one, so no re-heading) ----
        {
            const unsigned heading = static_cast<unsigned>(rng() % 4);
            const double nudge = unit(rng);
            ResetOriginal(position, orientation, 8);
            Field<std::int32_t>(originalState, 0x38) = 2;
            Field<std::int32_t>(originalState, 0x3c) = 3;
            Field<std::uint8_t>(originalState, 0x40) = static_cast<std::uint8_t>(heading);
            gDoubles = {nudge};
            riotLeader(original, deltaMs, false);
            float fromOriginal[3];
            std::memcpy(fromOriginal, gMoveVector, 12);

            auto* state = new tAutomataStateRiotLeader();
            state->stateType = 8;
            state->value38 = 2; state->value3c = 3; state->value40 = static_cast<std::uint8_t>(heading);
            port->StateStack().push_back(state);
            port->Place(position, orientation);
            port->moves = 0;
            gDoubles = {nudge};
            port->DoRiotLeaderTick(deltaMs, false);
            port->StateStack().pop_back();
            delete state;
            Report("DoRiotLeaderTick", gMoveCalls == 1 && port->moves == 1 && SameBits(fromOriginal, port->movedDirection, 3), t,
                   port->movedDirection, fromOriginal, 3);
        }

        // ---- the riot follower's steering ----
        {
            const bool pushed = t % 3 == 0;
            float leaderAt[3] = {position[0] + wide(rng), height(rng), position[2] + wide(rng)};
            if (t % 5 == 0) { leaderAt[0] = position[0] + 2.0f; leaderAt[2] = position[2] - 1.0f; }
            std::deque<double> draws;
            if (pushed) { draws.push_back(unit(rng)); draws.push_back(unit(rng)); }
            else draws.push_back(unit(rng) * 0.6);
            ResetOriginal(position, orientation, 9);
            Field<void*>(original, 0xf0) = originalLeader;
            Field<void*>(originalLeader, 0) = leaderVtable.slots;
            Field<std::uint8_t>(original, 0x39c) = pushed ? 1 : 0;
            std::memcpy(gLeaderPosition, leaderAt, 12);
            gDoubles = draws;
            riotFollower(original, deltaMs);
            float fromOriginal[3];
            std::memcpy(fromOriginal, gMoveVector, 12);

            portLeader->SetPosition(leaderAt);
            port->Place(position, orientation);
            port->SetCrowdReactionDirectionSet(pushed);
            port->moves = 0;
            gDoubles = draws;
            port->DoRiotFollowerTick(deltaMs);
            Report("DoRiotFollowerTick", gMoveCalls == 1 && port->moves == 1 && SameBits(fromOriginal, port->movedDirection, 3), t,
                   port->movedDirection, fromOriginal, 3);
        }

        // ---- the bee-line tick: the normalised attraction vector ----
        {
            float attraction[3] = {wide(rng), wide(rng), wide(rng)};
            if (t % 17 == 0) attraction[0] = attraction[1] = attraction[2] = 0.0f;
            ResetOriginal(position, orientation, 2);
            std::memcpy(original + 0xe0, attraction, 12);
            gMoveCalls = 0;
            beeLine(original, deltaMs);
            const bool originalMoved = gMoveCalls == 1;
            float fromOriginal[3];
            std::memcpy(fromOriginal, gMoveVector, 12);
            // the port normalises the same vector (its MoveForward capture is the MoveTo direction)
            port->Place(position, orientation);
            port->SetAttractionForOracle(attraction);
            port->moves = 0;
            port->DoBeeLineTick(deltaMs);
            const bool portMoved = port->moves == 1;
            Report("DoBeeLineTick", originalMoved == portMoved && (!originalMoved || SameBits(fromOriginal, port->movedDirection, 3)), t,
                   port->movedDirection, fromOriginal, 3);
        }

        // ---- the crowd tick: the pending push, the target of the move and its ground-plane direction ----
        {
            float controllerAt[3] = {position[0] + wide(rng), height(rng), position[2] + wide(rng)};
            const bool pushed = t % 2 == 0;
            float push[3] = {wide(rng) * 0.1f, wide(rng) * 0.1f, wide(rng) * 0.1f};
            if (t % 19 == 0) push[0] = push[1] = push[2] = 0.0f;
            const float crowdMs = 1.0f + std::fabs(deltaMs) * 0.5f;
            ResetOriginal(position, orientation, 3);
            Field<void*>(originalState, 0x10) = originalController;
            Field<void*>(originalController, 0) = controllerVtable.slots;
            Field<std::uint8_t>(original, 0x39c) = pushed ? 1 : 0;
            std::memcpy(original + 0x3a0, push, 12);
            std::memcpy(gControllerPosition, controllerAt, 12);
            crowd(original, crowdMs);
            float placedOriginal[3], directionOriginal[3], pushOriginal[3];
            std::memcpy(placedOriginal, gPlaced, 12);
            std::memcpy(directionOriginal, gMovedDirection, 12);
            std::memcpy(pushOriginal, original + 0x3a0, 12);
            const bool originalMoved = gMoveToCalls == 1;
            const bool originalFlag = Field<std::uint8_t>(original, 0x39c) != 0;

            PositionController controller;
            auto* state = new tAutomataState();
            state->stateType = 3;
            state->controller = &controller;
            port->StateStack().push_back(state);
            port->Place(position, orientation);
            port->SetCrowdReactionDirectionSet(pushed);
            std::memcpy(port->CrowdDirection(), push, 12);
            port->moves = 0;
            gPlaceCalls = 0;
            port->DoCrowdTick(crowdMs);
            port->StateStack().pop_back();
            state->controller = nullptr;
            delete state;
            Report("DoCrowdTick target", gPlaceCalls == 1 && SameBits(placedOriginal, gPlaced, 3), t, gPlaced, placedOriginal, 3);
            Report("DoCrowdTick direction", originalMoved == (port->moves == 1) && (!originalMoved || SameBits(directionOriginal, port->movedDirection, 3)),
                   t, port->movedDirection, directionOriginal, 3);
            Report("DoCrowdTick push", originalFlag == port->CrowdReactionDirectionSet() && SameBits(pushOriginal, port->CrowdDirection(), 3), t,
                   port->CrowdDirection(), pushOriginal, 3);
        }
    }
    std::printf("vehicle_oracle (pedestrian): %d checks, %d mismatches\n", gChecks, gMismatches);
    return gMismatches == 0 ? 0 : 1;
}
