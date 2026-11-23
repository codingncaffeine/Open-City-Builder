// airplane_oracle - test tooling: the cSC4Airplane flight model against the retail machine code (see GameImage.h).
// Random flight states go into a fake original object (fields at their original offsets) and into the port; the
// results must agree bit for bit. Build and run with scripts/run_machine_oracle.sh airplane.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <random>
#include <vector>

#include "GameImage.h"
#include "sc4/Airplane.h"
#include "sc4/AutomataStates.h"

namespace sc4 {
struct AirplaneOracleAccess {
    static void Set(cSC4Airplane& p, const float* f, int state) {
        p.state_ = static_cast<AirplaneState>(state);
        p.roll_ = f[0]; p.pitch_ = f[1]; p.yaw_ = f[2]; p.mass_ = f[3]; p.dragArea_ = f[4];
        p.maxSpeedLow_ = f[5]; p.maxSpeedHigh_ = f[6]; p.field3b8_ = f[7]; p.field3bc_ = f[8];
        p.approachAngle_ = f[9]; p.approachScale_ = f[10]; p.approachFactor_ = f[11]; p.field3dc_ = f[12];
        p.climbScale_ = f[13]; p.descentScale_ = f[14]; p.climbAngle_ = f[15]; p.descentAngle_ = f[16];
        p.takeoffAngle_ = f[17]; p.tunings_[7] = f[18]; p.tunings_[8] = f[19];
    }
    // controls (+0x314..+0x328), the twelve tunings (+0x438), orientation (+0x8c) and velocity (+0x36c)
    static void SetMotion(cSC4Airplane& p, const float* controls, const float* tunings, const float* orientation,
                          const float* velocity) {
        for (int i = 0; i < 4; ++i) p.controlWish_[i] = controls[i];
        p.controlAim_ = controls[4]; p.controlAim2_ = controls[5];
        for (int i = 0; i < 12; ++i) p.tunings_[i] = tunings[i];
        p.orientation_ = {orientation[0], orientation[1], orientation[2]};
        p.velocity_ = {velocity[0], velocity[1], velocity[2]};
    }
    static void SetPositionState(cSC4Airplane& p, const float* position, int state) {
        p.position_ = {position[0], position[1], position[2]};
        p.state_ = static_cast<AirplaneState>(state);
    }
    static int& Phase(cSC4Airplane& p) { return p.field434_; }
    static void SetDescentSlope(cSC4Airplane& p, float slope) { p.descentSlope_ = slope; }
    static int& TakeOffStep(cSC4Airplane& p) { return p.takeOffStep_; }
    static void SetPoints(cSC4Airplane& p, const float* pts) {   // points_[0..3], 12 floats
        for (int i = 0; i < 4; ++i) p.points_[i] = {pts[3 * i], pts[3 * i + 1], pts[3 * i + 2]};
    }
    static void TakeOffState(const cSC4Airplane& p, float* out) {   // +0x3e8 triple
        out[0] = p.field3e8_; out[1] = p.field3ec_; out[2] = p.field3f0_;
    }
    static int& TurnCount(cSC4Airplane& p) { return p.field424_; }
    static void SetLegStart(cSC4Airplane& p, const float* v) { p.field3e8_ = v[0]; p.field3ec_ = v[1]; p.field3f0_ = v[2]; }
    static int& LandStep(cSC4Airplane& p) { return p.field3e4_; }
    static void SetLanding(cSC4Airplane& p, const float* target, const float* saved) {
        p.field428_ = target[0]; p.field42c_ = target[1]; p.field430_ = target[2];
        p.savedPosition_ = {saved[0], saved[1], saved[2]};
    }
    static void Motion(const cSC4Airplane& p, float* out) {   // position, orientation, velocity
        const cS3DVector3* v[3] = {&p.position_, &p.orientation_, &p.velocity_};
        for (int i = 0; i < 3; ++i) { out[3 * i] = v[i]->x; out[3 * i + 1] = v[i]->y; out[3 * i + 2] = v[i]->z; }
    }
    static void SetAttitude(cSC4Airplane& p, float roll, float pitch, float yaw) { p.roll_ = roll; p.pitch_ = pitch; p.yaw_ = yaw; }
    static void SetBox(cSC4Airplane& p, const float* box) { std::memcpy(p.boundingBox_, box, sizeof p.boundingBox_); }
    static void SetState(cSC4Airplane& p, int state) { p.state_ = static_cast<AirplaneState>(state); }
    static void SetBankLimit(cSC4Airplane& p, float value) { p.descentAngle_ = value; }
    static void SetSlopes(cSC4Airplane& p, float climb, float descent) { p.climbSlope_ = climb; p.descentSlope_ = descent; }
    static void SetTurnRate(cSC4Airplane& p, float rate) { p.turnRate_ = rate; }
    static void Controls(const cSC4Airplane& p, float* out) {
        for (int i = 0; i < 4; ++i) out[i] = p.controlWish_[i];
        out[4] = p.controlAim_; out[5] = p.controlAim2_;
    }
};
}  // namespace sc4

using namespace sc4;
using namespace machine_oracle;

namespace {
// the city the original's services and the port's host both describe
float gCitySize[2] = {1024.0f, 2048.0f}, gAltitudeClamp = 300.0f, gTallestBuilding = 250.0f;

// a sloped terrain with a lake level, an effect-height field and a runway list, the same for both sides
bool InBounds(float x, float z) { return x >= 0.0f && z >= 0.0f && x <= gCitySize[0] && z <= gCitySize[1]; }
float Height(float x, float z) { return 20.0f + 0.013f * x - 0.021f * z; }
float Water(float x, float) { return 15.0f + 0.001f * x; }
float EffectHeight(float x, float z) { return Height(x, z) + 0.05f * (x - z); }
bool WaterCell(std::uint32_t x, std::uint32_t z) { return (x * 7 + z * 3) % 5 == 0; }
std::vector<cS3DVector3> gRunways;
std::uint32_t gPick = 0;
std::vector<double> gWeightsOriginal, gWeightsPort;

struct OracleAircraftHost final : IAircraftHost {
    bool TerrainPointInBounds(float x, float z) override { return InBounds(x, z); }
    float TerrainHeight(float x, float z) override { return Height(x, z); }
    float WaterHeight(float x, float z) override { return Water(x, z); }
    float DefaultAltitude() override { return 7.0f; }
    float AltitudeClamp() override { return gAltitudeClamp; }
    float TallestBuildingAltitude() override { return gTallestBuilding; }
    bool LotAltitudeAt(const cS3DVector3&, float*) override { return false; }
    float CitySizeX() override { return gCitySize[0]; }
    float CitySizeZ() override { return gCitySize[1]; }
    int CityCellCountX() override { return 64; }
    int CityCellCountZ() override { return 128; }
    void WorldToCell(float x, float z, std::int32_t& cx, std::int32_t& cz) override { cx = int(x / 16.0f); cz = int(z / 16.0f); }
    bool FindFireOccupant(const cS3DVector3&, const cS3DVector3&, cISC4OccupantFilter*, cS3DVector3&) override { return false; }
    double RandomDoubleRange(double lo, double) override { return lo; }
    const float* FindDriveParameters(IAutomataGroupTemplate*) override { return nullptr; }
    const float* FindPlayerDriveParameters(IAutomataGroupTemplate*) override { return nullptr; }
    IAirplaneEffect* CreateEffect(const cRZString&) override { return nullptr; }
    bool FindRegPointTransform(const cRZString&, cS3DVector3&) override { return false; }
    float EffectsWaterHeight(float x, float z) override { return EffectHeight(x, z); }
    void GetRunwayList(const cS3DVector3&, std::vector<cS3DVector3>& out) override { out = gRunways; }
    std::uint32_t RandomWeightedChoice(std::uint32_t count, const double* weights) override {
        gWeightsPort.assign(weights, weights + count);
        return gPick;
    }
    const void* LotAt(const cS3DVector3&) override { return nullptr; }
    bool FindCollider(AircraftColliderQuery query, const float* lo, const float* hi, AircraftCollider& hit) override;
    float OpacityFadeSeconds() override { return 1.0f; }
    bool CellHasWater(std::uint32_t x, std::uint32_t z) override { return WaterCell(x, z); }
    bool DisasterActive() override { return false; }
    float DisasterSpeedMultiplier() override { return 1.0f; }
    bool SimulatorPaused() override { return false; }
    bool SimulatorAdvancing() override { return true; }
    int ViewZoom() override { return 5; }
    void PlayAudioEvent(std::uint32_t) override {}
    void StopAudioEvent(std::uint32_t) override {}
};
float CitySizeX(void*) { return gCitySize[0]; }
float CitySizeZ(void*) { return gCitySize[1]; }
float AltitudeClamp(void*) { return gAltitudeClamp; }
float TallestBuilding(void*) { return gTallestBuilding; }
FakeService city, terrain, effects, effectMaps, lots, originalVtable;
bool InBoundsSlot(void*, float x, float z) { return InBounds(x, z); }
float HeightSlot(void*, float x, float z) { return Height(x, z); }
float WaterSlot(void*, float x, float z) { return Water(x, z); }
float DefaultSlot(void*) { return 7.0f; }
void* NoLot(void*, const float*) { return nullptr; }
void* EffectMapsSlot(void*) { return &effectMaps; }
float EffectHeightSlot(void*, float x, float z, int) { return EffectHeight(x, z); }
unsigned char managerObject[0xc00];   // the aircraft manager: only its filter pointer (+0xb38) is read
void* ManagerSlot(void*) { return managerObject; }
void RunwayListHook(void*, const float*, std::vector<cS3DVector3>* out) { *out = gRunways; }
std::uint32_t WeightedChoiceHook(void*, std::uint32_t count, const double* weights) {
    gWeightsOriginal.assign(weights, weights + count);
    return gPick;
}
void PositionToCellSlot(void*, float x, float z, int* cx, int* cz) { *cx = int(x / 16.0f); *cz = int(z / 16.0f); }
bool CellHasWaterSlot(void*, std::uint32_t x, std::uint32_t z) { return WaterCell(x, z); }
long gStackGuard = 0x5ca1ab1e;   // what ___stack_chk_guard points at
unsigned char gOriginalTransform[0x40];
void SetTransformSlot(void*, const void* transform) { std::memcpy(gOriginalTransform, transform, sizeof gOriginalTransform); }
// collision queries: which query (0 buildings, 1 aircraft, 2 type 0xc772bf98) hits, and the hit occupant
int gHitQuery = -1;
AircraftCollider gHit;
std::vector<std::vector<float>> gBoxesOriginal, gBoxesPort;
FakeService hitOccupant;
void HitPosition(void*, float* out) { out[0] = gHit.position.x; out[1] = gHit.position.y; out[2] = gHit.position.z; }
void HitBounds(void*, float* lo, float* hi) { std::memcpy(lo, gHit.boxMin, 12); std::memcpy(hi, gHit.boxMax, 12); }
struct ListNode { ListNode* prev; ListNode* next; void* value; };
struct OccupantList { ListNode* prev; ListNode* next; std::size_t size; };
ListNode gNode;
bool Query(int which, OccupantList* list, const float* lo, const float* hi) {
    gBoxesOriginal.push_back({lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]});
    if (which != gHitQuery) return false;
    gNode = {reinterpret_cast<ListNode*>(list), reinterpret_cast<ListNode*>(list), &hitOccupant};
    list->prev = list->next = &gNode;
    list->size = 1;
    return true;
}
bool QueryByType(void*, OccupantList* list, const float* lo, const float* hi, std::uint32_t type, int) {
    return Query(type == 0x278128a0 ? 0 : 2, list, lo, hi);
}
bool QueryByFilter(void*, OccupantList* list, const float* lo, const float* hi, void*, int) { return Query(1, list, lo, hi); }
void ReleaseList(void*, OccupantList*) {}
void ClearList(OccupantList* list) { list->prev = list->next = reinterpret_cast<ListNode*>(list); list->size = 0; }
FakeService occupants;
bool BaseTranslate(void*, void*, float) { return true; }   // cSC4Automaton::TranslatePlayerDriveCommands: port stand-in
void NoPlacement(void*) {}
int gOriginalRemoved = 0;
void RemoveSlot(void*, bool) { ++gOriginalRemoved; }
void* NewHook(std::size_t n) { return ::operator new(n); }
void DeleteHook(void* p) { ::operator delete(p); }

// original offsets of the fields AirplaneOracleAccess::Set fills, in the same order
constexpr int kOffsets[20] = {0x378, 0x37c, 0x380, 0x384, 0x388, 0x38c, 0x390, 0x3b8, 0x3bc, 0x3d0,
                              0x3d4, 0x3d8, 0x3dc, 0x394, 0x398, 0x3ac, 0x3b0, 0x3b4, 0x454, 0x458};
alignas(16) unsigned char original[0x4a0];

void SetOriginal(const float* f, int state) {
    std::memset(original, 0, sizeof original);
    std::memcpy(original + 0x310, &state, 4);
    for (int i = 0; i < 20; ++i) std::memcpy(original + kOffsets[i], &f[i], 4);
}
float Field(int offset) { float v; std::memcpy(&v, original + offset, 4); return v; }

bool Same(float a, float b) { return std::memcmp(&a, &b, 4) == 0 || (std::isnan(a) && std::isnan(b)); }

struct Transform { std::uint8_t counter, flags; float rotation[9]; float translation[3]; float scale; };

int failures = 0;
void Check(bool ok, const char* what, int trial) {
    if (!ok && ++failures <= 20) std::printf("MISMATCH %s (trial %d)\n", what, trial);
}
}  // namespace

bool OracleAircraftHost::FindCollider(AircraftColliderQuery query, const float* lo, const float* hi, AircraftCollider& hit) {
    gBoxesPort.push_back({lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]});
    if (static_cast<int>(query) != gHitQuery) return false;
    hit = gHit;
    return true;
}

int main() {
    InstallCrashReport();
    if (!MapGame("game-binaries/MacOS/Sim City 4 Deluxe Editionsub")) return 2;
    BindStub(0x10092af3a, dlsym(RTLD_DEFAULT, "__sincosf_stret"));
    BindStub(0x10092af8e, reinterpret_cast<const void*>(static_cast<float (*)(float)>(&acosf)));
    At<float>(0x100b8f8f8) = 0.0f;   // nS3D::k_zero
    At<float>(0x100b8f8fc) = 1.0f;   // nS3D::k_one
    // the model axes __GLOBAL__sub_I_SC4Airplane.cpp writes (Aircraft.h kAircraftModel*)
    At<float>(0x100b8fa00) = 0.0f; At<float>(0x100b8fa04) = 1.0f; At<float>(0x100b8fa08) = 0.0f;
    At<float>(0x100b8fa10) = 0.0f; At<float>(0x100b8fa14) = 0.0f; At<float>(0x100b8fa18) = 1.0f;   // kModelFacing
    At<float>(0x100b8fa1c) = 1.0f; At<float>(0x100b8fa20) = 0.0f; At<float>(0x100b8fa24) = 0.0f;
    At<float>(0x100b8fa28) = 0.0f; At<float>(0x100b8fa2c) = 1.0f; At<float>(0x100b8fa30) = 0.0f;   // kWorldUp

    city.Set(0x2c8, reinterpret_cast<const void*>(&CitySizeX));
    city.Set(0x2d0, reinterpret_cast<const void*>(&CitySizeZ));
    city.Install(0x100b21308);                                         // SL::spCity
    terrain.Set(0xb0, reinterpret_cast<const void*>(&AltitudeClamp));
    terrain.Install(0x100b21318);                                      // SL::spTerrain
    effects.Set(0x50, reinterpret_cast<const void*>(&TallestBuilding));
    effects.Set(0x48, reinterpret_cast<const void*>(&EffectMapsSlot));
    effects.Install(0x100b21368);                                      // SL::spEffectsManager
    effectMaps.Set(0x30, reinterpret_cast<const void*>(&EffectHeightSlot));
    terrain.Set(0x58, reinterpret_cast<const void*>(&InBoundsSlot));
    terrain.Set(0x60, reinterpret_cast<const void*>(&HeightSlot));
    terrain.Set(0xe8, reinterpret_cast<const void*>(&DefaultSlot));
    terrain.Set(0xf8, reinterpret_cast<const void*>(&WaterSlot));
    lots.Set(0x30, reinterpret_cast<const void*>(&NoLot));
    lots.Install(0x100b21340);                                         // SL::spLotManager
    city.Set(0x168, reinterpret_cast<const void*>(&ManagerSlot));      // the aircraft manager
    originalVtable.Set(0x68, reinterpret_cast<const void*>(&RemoveSlot));   // cSC4Automaton::Remove
    originalVtable.Set(0x290, reinterpret_cast<const void*>(&SetTransformSlot));   // cSC4Automaton::SetTransform
    city.Set(0x2f8, reinterpret_cast<const void*>(&PositionToCellSlot));
    terrain.Set(0x110, reinterpret_cast<const void*>(&CellHasWaterSlot));
    hitOccupant.Set(0x40, reinterpret_cast<const void*>(&HitPosition));
    hitOccupant.Set(0x50, reinterpret_cast<const void*>(&HitBounds));
    occupants.Set(0x120, reinterpret_cast<const void*>(&QueryByType));
    occupants.Set(0x128, reinterpret_cast<const void*>(&QueryByFilter));
    occupants.Set(0x180, reinterpret_cast<const void*>(&ReleaseList));
    occupants.Install(0x100b21348);                                    // SL::spOccupantManager
    At<long*>(0x100a657c8) = &gStackGuard;                              // the ___stack_chk_guard GOT slot
    originalVtable.slots[0x258 / 8] = reinterpret_cast<void*>(Here(0x100107148));   // cSC4Automaton::GetBoundingBox
    if (!Hook(0x1000a5c20, reinterpret_cast<const void*>(&ClearList))) return 2;    // the occupant list's clear
    if (!Hook(0x10010978e, reinterpret_cast<const void*>(&BaseTranslate)) ||
        !Hook(0x1001e3e84, reinterpret_cast<const void*>(&NoPlacement)))
        return 2;
    if (!Hook(0x10050175a, reinterpret_cast<const void*>(&RunwayListHook)) ||      // cSC4AircraftManager::GetRunwayList
        !Hook(0x10003da38, reinterpret_cast<const void*>(&WeightedChoiceHook))) {  // RandomUint32WeightedChoice
        std::perror("hook");
        return 2;
    }
    BindStub(0x10092aebc, reinterpret_cast<const void*>(&NewHook));
    BindStub(0x10092aeb0, reinterpret_cast<const void*>(&DeleteHook));
    BindStub(0x10092b432, reinterpret_cast<const void*>(&memcpy));

    using Forces = void* (*)(float*, void*, const float*, const float*, const float*);
    using Orientation = void (*)(void*, const float*, float*, const float*, void*, float);
    using Scalar = float (*)(void*);
    const auto forces = Function<Forces>(0x1006cff10);
    const auto orient = Function<Orientation>(0x1006d14e8);
    const auto climb = Function<Scalar>(0x1006cdbb2), descent = Function<Scalar>(0x1006cde04),
               turn = Function<Scalar>(0x1006ce05e);

    std::mt19937 rng(20261028);
    std::uniform_real_distribution<float> angle(-1.6f, 1.6f), unit(-1.0f, 1.0f), speed(-80.0f, 80.0f);
    OracleAircraftHost host;
    cSC4Airplane port(nullptr, &host);
    const int trials = 4000;
    for (int t = 0; t < trials; ++t) {
        // realistic parameters around the constructor defaults, plus random flight attitude
        float f[20] = {angle(rng), angle(rng), angle(rng) * 2, 50.0f + 100.0f * (unit(rng) + 1), 0.5f + unit(rng) * 0.4f,
                       20.0f + 10.0f * unit(rng), 120.0f + 40.0f * unit(rng), 8000.0f + 4000.0f * unit(rng),
                       0.4f + 0.2f * unit(rng), 0.5235988f + 0.2f * unit(rng), 0.08f + 0.05f * unit(rng),
                       0.68f + 0.2f * unit(rng), 0.1f * unit(rng), 0.0654f, 0.3927f, 0.2618f + 0.1f * unit(rng),
                       0.7854f, 0.5236f, 0.2f + 0.1f * unit(rng), 0.4f + 0.2f * unit(rng)};
        if (t % 7 == 0) f[1] = 0.0f;
        const int state = static_cast<int>(rng() % 4);
        SetOriginal(f, state);
        AirplaneOracleAccess::Set(port, f, state);

        float velocity[3] = {speed(rng), speed(rng) * 0.3f, speed(rng)};
        if (t % 11 == 0) velocity[0] = velocity[1] = velocity[2] = 0.0f;
        float forward[3] = {unit(rng), unit(rng) * 0.5f, unit(rng)};
        const float len = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        for (float& v : forward) v /= len;
        AirplaneControls controls;
        controls.steer[0] = unit(rng); controls.steer[1] = unit(rng); controls.steer[2] = unit(rng);
        controls.throttle = unit(rng); controls.lift = unit(rng) * 0.5f + 0.5f; controls.value14 = unit(rng) * 0.2f;

        float out[3];
        forces(out, original, velocity, forward, &controls.steer[0]);
        const cS3DVector3 mine = port.CalculateForces({velocity[0], velocity[1], velocity[2]},
                                                      {forward[0], forward[1], forward[2]}, controls);
        Check(Same(out[0], mine.x) && Same(out[1], mine.y) && Same(out[2], mine.z), "CalculateForces", t);

        Transform tOriginal{};
        tOriginal.counter = 7;
        float heading[3] = {0, 0, 0};
        const float delta = 0.01f + 0.2f * (unit(rng) + 1);
        orient(original, velocity, heading, &controls.steer[0], &tOriginal, delta);
        cS3DTransform tPort;
        tPort.changeCounter = 7;
        tPort.flags = 0;
        cS3DVector3 headingPort;
        port.UpdateOrientation({velocity[0], velocity[1], velocity[2]}, headingPort, controls, tPort, delta);
        bool same = Same(heading[0], headingPort.x) && Same(heading[1], headingPort.y) && Same(heading[2], headingPort.z) &&
                    tOriginal.counter == tPort.changeCounter && tOriginal.flags == tPort.flags &&
                    Same(Field(0x378), port.Roll()) && Same(Field(0x37c), port.Pitch()) && Same(Field(0x380), port.Yaw());
        for (int i = 0; i < 9; ++i) same = same && Same(tOriginal.rotation[i], tPort.rotation[i / 3][i % 3]);
        Check(same, "UpdateOrientation", t);

        {   // AI_Stop
            float c[6], tune[12], o[3], v[3];
            for (float& x : c) x = unit(rng);
            for (float& x : tune) x = 0.05f + 0.5f * (unit(rng) + 1);
            for (float& x : o) x = unit(rng);
            for (float& x : v) x = speed(rng) * (t % 5 == 0 ? 0.001f : 1.0f);
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x438, tune, sizeof tune);
            std::memcpy(original + 0x8c, o, sizeof o);
            std::memcpy(original + 0x36c, v, sizeof v);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            const bool a = Function<bool (*)(void*, float)>(0x1006d3f70)(original, delta);
            const bool b = port.AI_Stop(delta);
            float mine[6];
            AirplaneOracleAccess::Controls(port, mine);
            bool same = a == b;
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            Check(same, "AI_Stop", t);

            // AI_ApproachAltitude on the same motion state
            const float position[3] = {speed(rng), 100.0f + speed(rng) * (t % 3 == 0 ? 0.05f : 1.0f), speed(rng)};
            const int st = static_cast<int>(rng() % 4);
            for (float& x : c) x = unit(rng) * 1.3f;
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x74, position, sizeof position);
            std::memcpy(original + 0x310, &st, 4);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            AirplaneOracleAccess::SetPositionState(port, position, st);
            const bool lift = rng() & 1;
            const int ra = Function<int (*)(void*, float, bool, float)>(0x1006d4a0c)(original, 100.0f, lift, delta);
            const int rb = port.AI_ApproachAltitude(100.0f, lift, delta);
            AirplaneOracleAccess::Controls(port, mine);
            same = ra == rb;
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            Check(same, "AI_ApproachAltitude", t);

            // the steering helpers: same motion state, random geometry
            const float pa[3] = {speed(rng), speed(rng) * 0.1f, speed(rng)};
            float pb[3] = {unit(rng), unit(rng) * 0.2f, unit(rng)};
            const float pe[3] = {unit(rng), unit(rng) * 0.2f, unit(rng)};
            if (t % 9 == 0) { pb[0] = o[0] + 0.01f * unit(rng); pb[2] = o[2]; }
            auto reset = [&] {
                std::memcpy(original + 0x314, c, sizeof c);
                AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            };
            auto compare = [&](bool ra2, bool rb2, const char* what) {
                AirplaneOracleAccess::Controls(port, mine);
                bool ok = ra2 == rb2;
                for (int i = 0; i < 6; ++i) ok = ok && Same(Field(0x314 + 4 * i), mine[i]);
                Check(ok, what, t);
            };
            const cS3DVector3 va{pa[0], pa[1], pa[2]}, vb{pb[0], pb[1], pb[2]}, ve{pe[0], pe[1], pe[2]};
            reset();
            compare(Function<bool (*)(void*, const float*, const float*, float)>(0x1006d51a2)(original, pa, pb, delta),
                    port.AI_FollowRay(va, vb, delta), "AI_FollowRay");
            reset();
            compare(Function<bool (*)(void*, const float*, float)>(0x1006d4852)(original, pb, delta),
                    port.AI_TurnToVector(vb, delta), "AI_TurnToVector");
            reset();
            Function<void (*)(void*, const float*, float)>(0x1006d44cc)(original, pa, delta);
            port.AI_Taxi(va, delta);
            compare(true, true, "AI_Taxi");
            const int phase = static_cast<int>(rng() % 4);
            const float rate = 0.05f + 0.5f * (unit(rng) + 1);
            reset();
            std::memcpy(original + 0x434, &phase, 4);
            std::memcpy(original + 0x3cc, &rate, 4);
            AirplaneOracleAccess::Phase(port) = phase;
            AirplaneOracleAccess::SetTurnRate(port, rate);
            const bool fa = Function<bool (*)(void*, const float*, const float*, const float*, float)>(0x1006d4df8)(
                original, pb, pa, pe, delta);
            const bool fb = port.AI_FollowVectorAndTurn(vb, va, ve, delta);
            int phaseOriginal;
            std::memcpy(&phaseOriginal, original + 0x434, 4);
            Check(phaseOriginal == AirplaneOracleAccess::Phase(port), "AI_FollowVectorAndTurn phase", t);
            compare(fa, fb, "AI_FollowVectorAndTurn");
        }

        gStage = "PickApproachBox";
        {   // PickApproachBox: runway end anywhere around the city, any heading, random city
            gCitySize[0] = 500.0f + 1500.0f * (unit(rng) + 1);
            gCitySize[1] = 500.0f + 1500.0f * (unit(rng) + 1);
            gAltitudeClamp = 200.0f + 100.0f * unit(rng);
            gTallestBuilding = 200.0f + 150.0f * unit(rng);
            const float slope = 0.05f + 0.2f * (unit(rng) + 1);
            std::memcpy(original + 0x3c8, &slope, 4);
            AirplaneOracleAccess::SetDescentSlope(port, slope);
            const float end[3] = {gCitySize[0] * (unit(rng) + 1) * 0.6f, 20.0f + 30.0f * unit(rng),
                                  gCitySize[1] * (unit(rng) + 1) * 0.6f};
            float dir[3] = {unit(rng), 0.0f, unit(rng)};
            const float len = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
            dir[0] /= len; dir[2] /= len;
            Function<void (*)(void*, const float*, const float*)>(0x1006d2932)(original, end, dir);
            port.PickApproachBox({end[0], end[1], end[2]}, {dir[0], dir[1], dir[2]});
            bool same = true;
            for (int i = 0; i < 4; ++i) {
                const cS3DVector3& q = port.ApproachPoint(i);
                same = same && Same(Field(0x3f4 + 12 * i), q.x) && Same(Field(0x3f8 + 12 * i), q.y) &&
                       Same(Field(0x3fc + 12 * i), q.z);
            }
            Check(same, "PickApproachBox", t);
        }

        {   // PickRunwayDirection: one to four runways inside the city, a random pick
            const int runways = 1 + static_cast<int>(rng() % 4);
            gRunways.clear();
            for (int r = 0; r < 2 * runways; ++r)
                gRunways.push_back({gCitySize[0] * 0.5f * (unit(rng) + 1), 10.0f + 5.0f * unit(rng),
                                    gCitySize[1] * 0.5f * (unit(rng) + 1)});
            if (t % 50 == 0) gRunways.pop_back();   // odd list: the plane removes itself
            gPick = static_cast<std::uint32_t>(rng() % gRunways.size());
            const float climbSlope = 0.1f * (unit(rng) + 1), descentSlope = 0.1f * (unit(rng) + 1);
            std::memcpy(original + 0x3c4, &climbSlope, 4);
            std::memcpy(original + 0x3c8, &descentSlope, 4);
            AirplaneOracleAccess::SetSlopes(port, climbSlope, descentSlope);
            void* vtable = originalVtable.slots;   // the object's first qword is its slot table
            std::memcpy(original, &vtable, 8);
            const bool landing = rng() & 1;
            const float where[3] = {100.0f, 50.0f, 100.0f};
            float startO[3] = {0, 0, 0}, dirO[3] = {0, 0, 0};
            gOriginalRemoved = 0;
            gWeightsOriginal.clear();
            gWeightsPort.clear();
            gStage = "PickRunwayDirection original";
            Function<void (*)(void*, const float*, float*, float*, bool)>(0x1006d258c)(original, where, startO, dirO,
                                                                                         landing);
            cS3DVector3 startP, dirP;
            if (gRunways.size() % 2 == 0) {
                port.PickRunwayDirection({where[0], where[1], where[2]}, startP, dirP, landing);
                const cS3DVector3 target = port.RunwayTarget();
                bool same = gOriginalRemoved == 0 && gWeightsOriginal.size() == gWeightsPort.size() &&
                            std::memcmp(gWeightsOriginal.data(), gWeightsPort.data(), 8 * gWeightsPort.size()) == 0 &&
                            Same(startO[0], startP.x) && Same(startO[1], startP.y) && Same(startO[2], startP.z) &&
                            Same(dirO[0], dirP.x) && Same(dirO[1], dirP.y) && Same(dirO[2], dirP.z) &&
                            Same(Field(0x428), target.x) && Same(Field(0x42c), target.y) && Same(Field(0x430), target.z);
                Check(same, "PickRunwayDirection", t);
            } else {
                Check(gOriginalRemoved == 1, "PickRunwayDirection odd list", t);
            }
        }

        {   // AI_TakeOff, any step, plane near the runway or off it
            gStage = "AI_TakeOff";
            float c[6], tune[12], o[3], v[3], pts[12];
            for (float& x : c) x = unit(rng);
            for (float& x : tune) x = 0.05f + 0.5f * (unit(rng) + 1);
            o[0] = unit(rng); o[1] = 0.0f; o[2] = unit(rng);
            for (float& x : v) x = speed(rng) * 0.2f;
            const cS3DVector3 a = gRunways[0];
            for (int i = 0; i < 4; ++i) {
                pts[3 * i] = a.x + 300.0f * unit(rng); pts[3 * i + 1] = a.y; pts[3 * i + 2] = a.z + 300.0f * unit(rng);
            }
            const float near = t % 3 == 0 ? 2.0f : 60.0f;
            const float position[3] = {pts[0] + near * unit(rng), pts[1] + 5.0f * unit(rng), pts[2] + near * unit(rng)};
            const int step = static_cast<int>(rng() % 5);
            const int st = static_cast<int>(rng() % 4);
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x438, tune, sizeof tune);
            std::memcpy(original + 0x8c, o, sizeof o);
            std::memcpy(original + 0x36c, v, sizeof v);
            std::memcpy(original + 0x74, position, sizeof position);
            std::memcpy(original + 0x3f4, pts, sizeof pts);
            std::memcpy(original + 0x310, &st, 4);
            std::memcpy(original + 0x3e0, &step, 4);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            AirplaneOracleAccess::SetPositionState(port, position, st);
            AirplaneOracleAccess::SetPoints(port, pts);
            AirplaneOracleAccess::TakeOffStep(port) = step;
            const float d = 0.05f + 0.1f * (unit(rng) + 1);
            const bool ra = Function<bool (*)(void*, float)>(0x1006d40e2)(original, d);
            const bool rb = port.AI_TakeOff(d);
            float mine[6], keep[3];
            AirplaneOracleAccess::Controls(port, mine);
            AirplaneOracleAccess::TakeOffState(port, keep);
            int stepOriginal;
            std::memcpy(&stepOriginal, original + 0x3e0, 4);
            bool same = ra == rb && stepOriginal == AirplaneOracleAccess::TakeOffStep(port);
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            for (int i = 0; i < 4; ++i) {
                const cS3DVector3& q = port.ApproachPoint(i);
                same = same && Same(Field(0x3f4 + 12 * i), q.x) && Same(Field(0x3f8 + 12 * i), q.y) &&
                       Same(Field(0x3fc + 12 * i), q.z);
            }
            if (step == 0) for (int i = 0; i < 3; ++i) same = same && Same(Field(0x3e8 + 4 * i), keep[i]);
            Check(same, "AI_TakeOff", t);
        }

        {   // AI_GotoDestination: air and ground states, destinations near and far, any turn progress
            gStage = "AI_GotoDestination";
            float c[6], tune[12], o[3], v[3], pts[12], legStart[3];
            for (float& x : c) x = unit(rng);
            for (float& x : tune) x = 0.05f + 0.5f * (unit(rng) + 1);
            o[0] = unit(rng); o[1] = 0.1f * unit(rng); o[2] = unit(rng);
            for (float& x : v) x = speed(rng);
            for (float& x : pts) x = 500.0f * (unit(rng) + 1);
            const float position[3] = {gCitySize[0] * 0.5f * (unit(rng) + 1), 150.0f + 100.0f * unit(rng),
                                       gCitySize[1] * 0.5f * (unit(rng) + 1)};
            const float reach = t % 2 ? 300.0f : 1500.0f;
            const float dest[3] = {position[0] + reach * unit(rng), 50.0f + 40.0f * unit(rng), position[2] + reach * unit(rng)};
            for (int i = 0; i < 3; ++i) legStart[i] = position[i] + (t % 4 ? 400.0f : 50.0f) * unit(rng);
            const int st = static_cast<int>(rng() % 6), turns = static_cast<int>(rng() % 2),
                      phase = static_cast<int>(rng() % 3), step = 1 + static_cast<int>(rng() % 3);
            const float slope = 0.05f + 0.1f * (unit(rng) + 1), rate = 0.1f + 0.4f * (unit(rng) + 1);
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x438, tune, sizeof tune);
            std::memcpy(original + 0x8c, o, sizeof o);
            std::memcpy(original + 0x36c, v, sizeof v);
            std::memcpy(original + 0x74, position, sizeof position);
            std::memcpy(original + 0x3f4, pts, sizeof pts);
            std::memcpy(original + 0x3e8, legStart, sizeof legStart);
            std::memcpy(original + 0x310, &st, 4);
            std::memcpy(original + 0x424, &turns, 4);
            std::memcpy(original + 0x434, &phase, 4);
            std::memcpy(original + 0x3e0, &step, 4);
            std::memcpy(original + 0x3c8, &slope, 4);
            std::memcpy(original + 0x3cc, &rate, 4);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            AirplaneOracleAccess::SetPositionState(port, position, st);
            AirplaneOracleAccess::SetPoints(port, pts);
            AirplaneOracleAccess::SetLegStart(port, legStart);
            AirplaneOracleAccess::TurnCount(port) = turns;
            AirplaneOracleAccess::Phase(port) = phase;
            AirplaneOracleAccess::TakeOffStep(port) = step;
            AirplaneOracleAccess::SetDescentSlope(port, slope);
            AirplaneOracleAccess::SetTurnRate(port, rate);
            const float d = 0.05f + 0.1f * (unit(rng) + 1);
            const bool ra = Function<bool (*)(void*, const float*, float)>(0x1006d2f12)(original, dest, d);
            const bool rb = port.AI_GotoDestination({dest[0], dest[1], dest[2]}, d);
            float mine[6], keep[3];
            AirplaneOracleAccess::Controls(port, mine);
            AirplaneOracleAccess::TakeOffState(port, keep);
            int ints[4];
            std::memcpy(&ints[0], original + 0x424, 4);
            std::memcpy(&ints[1], original + 0x434, 4);
            std::memcpy(&ints[2], original + 0x3e0, 4);
            bool same = ra == rb && ints[0] == AirplaneOracleAccess::TurnCount(port) &&
                        ints[1] == AirplaneOracleAccess::Phase(port) && ints[2] == AirplaneOracleAccess::TakeOffStep(port);
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            for (int i = 0; i < 3; ++i) same = same && Same(Field(0x3e8 + 4 * i), keep[i]);
            Check(same, "AI_GotoDestination", t);
        }

        {   // AI_Land, every step, box corners around a runway, position anywhere on the approach
            gStage = "AI_Land";
            float c[6], tune[12], o[3], v[3], pts[12], legStart[3], target[3], saved[3];
            for (float& x : c) x = unit(rng);
            for (float& x : tune) x = 0.05f + 0.5f * (unit(rng) + 1);
            o[0] = unit(rng); o[1] = 0.1f * unit(rng); o[2] = unit(rng);
            const float fast = t % 3 == 0 ? 3.0f : 60.0f;
            for (float& x : v) x = fast * unit(rng);
            const float cx = gCitySize[0] * 0.5f, cz = gCitySize[1] * 0.5f;
            for (int i = 0; i < 4; ++i) {
                pts[3 * i] = cx + 600.0f * unit(rng); pts[3 * i + 1] = 60.0f + 50.0f * unit(rng);
                pts[3 * i + 2] = cz + 600.0f * unit(rng);
            }
            target[0] = pts[9] + 300.0f * unit(rng); target[1] = pts[10]; target[2] = pts[11] + 300.0f * unit(rng);
            const float near = t % 4 == 0 ? 3.0f : 200.0f;
            for (int i = 0; i < 3; ++i) saved[i] = pts[9 + i] + near * unit(rng);
            const float position[3] = {pts[9] + 400.0f * unit(rng), 40.0f + 40.0f * unit(rng), pts[11] + 400.0f * unit(rng)};
            for (int i = 0; i < 3; ++i) legStart[i] = position[i] + 300.0f * unit(rng);
            const int step = static_cast<int>(rng() % 6), st = static_cast<int>(rng() % 4),
                      corner = static_cast<int>(rng() % 4), phase = static_cast<int>(rng() % 3);
            const float slope = 0.05f + 0.1f * (unit(rng) + 1), rate = 0.1f + 0.4f * (unit(rng) + 1);
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x438, tune, sizeof tune);
            std::memcpy(original + 0x8c, o, sizeof o);
            std::memcpy(original + 0x36c, v, sizeof v);
            std::memcpy(original + 0x74, position, sizeof position);
            std::memcpy(original + 0x3f4, pts, sizeof pts);
            std::memcpy(original + 0x3e8, legStart, sizeof legStart);
            std::memcpy(original + 0x428, target, sizeof target);
            std::memcpy(original + 0x48c, saved, sizeof saved);
            std::memcpy(original + 0x310, &st, 4);
            std::memcpy(original + 0x3e4, &step, 4);
            std::memcpy(original + 0x424, &corner, 4);
            std::memcpy(original + 0x434, &phase, 4);
            std::memcpy(original + 0x3c8, &slope, 4);
            std::memcpy(original + 0x3cc, &rate, 4);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            AirplaneOracleAccess::SetPositionState(port, position, st);
            AirplaneOracleAccess::SetPoints(port, pts);
            AirplaneOracleAccess::SetLegStart(port, legStart);
            AirplaneOracleAccess::SetLanding(port, target, saved);
            AirplaneOracleAccess::LandStep(port) = step;
            AirplaneOracleAccess::TurnCount(port) = corner;
            AirplaneOracleAccess::Phase(port) = phase;
            AirplaneOracleAccess::SetDescentSlope(port, slope);
            AirplaneOracleAccess::SetTurnRate(port, rate);
            const float d = 0.05f + 0.1f * (unit(rng) + 1);
            const bool ra = Function<bool (*)(void*, float)>(0x1006d33f8)(original, d);
            const bool rb = port.AI_Land(d);
            float mine[6], keep[3], motion[9];
            AirplaneOracleAccess::Controls(port, mine);
            AirplaneOracleAccess::TakeOffState(port, keep);
            AirplaneOracleAccess::Motion(port, motion);
            int ints[5];
            for (int i = 0; i < 5; ++i) std::memcpy(&ints[i], original + (const int[]){0x3e4, 0x3e0, 0x424, 0x434, 0x310}[i], 4);
            bool same = ra == rb && ints[0] == AirplaneOracleAccess::LandStep(port) &&
                        ints[1] == AirplaneOracleAccess::TakeOffStep(port) && ints[2] == AirplaneOracleAccess::TurnCount(port) &&
                        ints[3] == AirplaneOracleAccess::Phase(port) && ints[4] == static_cast<int>(port.State());
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            for (int i = 0; i < 3; ++i) same = same && Same(Field(0x3e8 + 4 * i), keep[i]);
            const int motionOffsets[3] = {0x74, 0x8c, 0x36c};
            for (int i = 0; i < 9; ++i) same = same && Same(Field(motionOffsets[i / 3] + 4 * (i % 3)), motion[i]);
            for (int i = 0; i < 4; ++i) {
                const cS3DVector3& q = port.ApproachPoint(i);
                same = same && Same(Field(0x3f4 + 12 * i), q.x) && Same(Field(0x3f8 + 12 * i), q.y) &&
                       Same(Field(0x3fc + 12 * i), q.z);
            }
            // an odd runway list makes step 0 remove the plane: the original's Remove is a no-op fake here
            if (!(step == 0 && gRunways.size() % 2 != 0)) Check(same, "AI_Land", t);
        }

        {   // UpdatePhysics: every state, on and above the terrain, with and without runways nearby
            gStage = "UpdatePhysics";
            SetOriginal(f, 0);
            AirplaneOracleAccess::Set(port, f, 0);
            float c[6], tune[12], o[3], v[3];
            for (float& x : c) x = unit(rng);
            if (t % 3 == 0) { c[2] = 0.0f; c[3] = 0.0f; }
            for (float& x : tune) x = 0.05f + 0.5f * (unit(rng) + 1);
            o[0] = unit(rng); o[1] = 0.2f * unit(rng); o[2] = unit(rng);
            const float fast = t % 4 == 0 ? 0.05f : 40.0f;
            for (float& x : v) x = fast * unit(rng);
            const bool outside = t % 13 == 0;
            const float px = outside ? -50.0f : gCitySize[0] * 0.5f * (unit(rng) + 1);
            const float pz = gCitySize[1] * 0.5f * (unit(rng) + 1);
            const float position[3] = {px, Height(px, pz) + (t % 2 ? 0.5f : 25.0f) * (unit(rng) + 1), pz};
            const int st = static_cast<int>(rng() % 4);
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x438, tune, sizeof tune);
            std::memcpy(original + 0x8c, o, sizeof o);
            std::memcpy(original + 0x36c, v, sizeof v);
            std::memcpy(original + 0x74, position, sizeof position);
            std::memcpy(original + 0x310, &st, 4);
            void* vtable = originalVtable.slots;
            std::memcpy(original, &vtable, 8);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            AirplaneOracleAccess::SetPositionState(port, position, st);
            const std::vector<cS3DVector3> keepRunways = gRunways;
            if (t % 2) gRunways.clear();
            const float d = 0.02f + 0.05f * (unit(rng) + 1);
            std::memset(gOriginalTransform, 0, sizeof gOriginalTransform);
            Function<void (*)(void*, float)>(0x1006cee14)(original, d);
            port.UpdatePhysics(d);
            gRunways = keepRunways;
            float mine[6], motion[9];
            AirplaneOracleAccess::Controls(port, mine);
            AirplaneOracleAccess::Motion(port, motion);
            int stateOriginal;
            std::memcpy(&stateOriginal, original + 0x310, 4);
            bool same = stateOriginal == static_cast<int>(port.State()) && Same(Field(0x378), port.Roll()) &&
                        Same(Field(0x37c), port.Pitch()) && Same(Field(0x380), port.Yaw());
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            const int motionOffsets[3] = {0x74, 0x8c, 0x36c};
            for (int i = 0; i < 9; ++i) same = same && Same(Field(motionOffsets[i / 3] + 4 * (i % 3)), motion[i]);
            float translation[3];
            std::memcpy(translation, gOriginalTransform + 0x28, sizeof translation);
            same = same && gOriginalTransform[1] == 5 && gOriginalTransform[0] == 5 && Same(translation[0], motion[0]) &&
                   Same(translation[1], motion[1]) && Same(translation[2], motion[2]);
            Check(same, "UpdatePhysics", t);
        }

        {   // DetectCollisions: no hit or a hit from each query, obstacle boxes around the plane
            gStage = "DetectCollisions";
            float o[3], v[3], box[6];
            o[0] = unit(rng); o[1] = 0.3f * unit(rng); o[2] = unit(rng);
            for (float& x : v) x = 30.0f * unit(rng);
            const float px = gCitySize[0] * 0.5f * (unit(rng) + 1), pz = gCitySize[1] * 0.5f * (unit(rng) + 1);
            const float position[3] = {px, Height(px, pz) + 20.0f * (unit(rng) + 1), pz};
            for (int i = 0; i < 3; ++i) { box[i] = position[i] - 5.0f - 3.0f * (unit(rng) + 1); box[3 + i] = position[i] + 5.0f; }
            gHitQuery = static_cast<int>(rng() % 4) - 1;
            gHit.position = {position[0] + 15.0f * unit(rng), position[1] + 15.0f * unit(rng), position[2] + 15.0f * unit(rng)};
            for (int i = 0; i < 3; ++i) {
                const float centre = (&gHit.position.x)[i];
                gHit.boxMin[i] = centre - 4.0f - 8.0f * (unit(rng) + 1);
                gHit.boxMax[i] = centre + 4.0f + 8.0f * (unit(rng) + 1);
            }
            const float yaw = angle(rng);
            std::memcpy(original + 0x8c, o, sizeof o);
            std::memcpy(original + 0x36c, v, sizeof v);
            std::memcpy(original + 0x74, position, sizeof position);
            std::memcpy(original + 0x25c, box, sizeof box);
            std::memcpy(original + 0x380, &yaw, 4);
            float c[6] = {}, tune[12] = {};
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            std::memcpy(original + 0x314, c, sizeof c);
            AirplaneOracleAccess::SetPositionState(port, position, 0);
            AirplaneOracleAccess::SetBox(port, box);
            AirplaneOracleAccess::SetAttitude(port, port.Roll(), port.Pitch(), yaw);
            gBoxesOriginal.clear();
            gBoxesPort.clear();
            Function<void (*)(void*)>(0x1006d0cd0)(original);
            port.DetectCollisions();
            gHitQuery = -1;
            float motion[9];
            AirplaneOracleAccess::Motion(port, motion);
            bool same = gBoxesOriginal == gBoxesPort && Same(Field(0x380), port.Yaw());
            const int motionOffsets[3] = {0x74, 0x8c, 0x36c};
            for (int i = 0; i < 9; ++i) same = same && Same(Field(motionOffsets[i / 3] + 4 * (i % 3)), motion[i]);
            Check(same, "DetectCollisions", t);
        }

        {   // TranslatePlayerDriveCommands: random held keys and controls in every state
            gStage = "TranslatePlayerDriveCommands";
            float c[6], tune[12], v[3], o[3] = {1.0f, 0.0f, 0.0f};
            for (float& x : c) x = 1.2f * unit(rng);
            if (t % 5 == 0) for (float& x : c) x *= 0.005f;
            for (float& x : tune) x = 0.05f + 0.5f * (unit(rng) + 1);
            for (float& x : v) x = 20.0f * unit(rng);
            const int st = static_cast<int>(rng() % 4);
            alignas(16) unsigned char record[0x2a0] = {};
            tAutomataStatePlayerDrive drive;
            for (int i = 0; i < 13; ++i) {
                drive.history[i] = (rng() % 3 == 0) ? 0.5f * (unit(rng) + 1) : -1.0f;
                std::memcpy(record + 0xa8 + 4 * i, &drive.history[i], 4);
            }
            const float bank = 0.5f + unit(rng) * 0.4f;
            std::memcpy(original + 0x314, c, sizeof c);
            std::memcpy(original + 0x438, tune, sizeof tune);
            std::memcpy(original + 0x36c, v, sizeof v);
            std::memcpy(original + 0x310, &st, 4);
            std::memcpy(original + 0x3b0, &bank, 4);
            std::memset(original + 0x30c, 0, 1);
            AirplaneOracleAccess::SetMotion(port, c, tune, o, v);
            AirplaneOracleAccess::SetState(port, st);
            AirplaneOracleAccess::SetBankLimit(port, bank);
            const float ms = 5.0f + 60.0f * (unit(rng) + 1);
            Function<bool (*)(void*, void*, float)>(0x1006cf894)(original, record, ms);
            port.TranslatePlayerDriveCommands(&drive, ms);
            float mine[6];
            AirplaneOracleAccess::Controls(port, mine);
            bool same = true;
            for (int i = 0; i < 6; ++i) same = same && Same(Field(0x314 + 4 * i), mine[i]);
            float out[4];
            std::memcpy(&out[0], record + 0x3c, 4);
            std::memcpy(&out[1], record + 0x6c, 4);
            std::memcpy(&out[2], record + 0x260, 4);
            std::memcpy(&out[3], record + 0x264, 4);
            same = same && Same(out[0], drive.value3c) && Same(out[1], drive.steering6c) &&
                   Same(out[2], drive.pitchGaugeScale) && Same(out[3], drive.pitchGauge);
            Check(same, "TranslatePlayerDriveCommands", t);
        }

        if (t % 8 == 0) {
            SetOriginal(f, 3);
            AirplaneOracleAccess::Set(port, f, 3);
            Check(Same(climb(original), port.CalculateClimbSlope()), "CalculateClimbSlope", t);
            SetOriginal(f, 3);
            AirplaneOracleAccess::Set(port, f, 3);
            Check(Same(descent(original), port.CalculateDescentSlope()), "CalculateDescentSlope", t);
            SetOriginal(f, 3);
            AirplaneOracleAccess::Set(port, f, 3);
            Check(Same(turn(original), port.CalculateTurnRate()), "CalculateTurnRate", t);
        }
    }
    std::printf("airplane_oracle: %d trials, %d mismatches\n", trials, failures);
    return failures ? 1 : 0;
}
