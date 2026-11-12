// cSC4DisasterInstanceEarthquake (Mac i386 0x0033ee44..0x00340a8a, 0x120 bytes; made by cSC4DisasterManagerEarthquake::
// AttemptDisasterStart 0x00340c8a).
//
// An earthquake is a fault line through its epicentre: two cubic Bezier curves leaving the epicentre in opposite directions
// (the line's slope is rolled once per session, each quake varies it a little, and every control point is pushed sideways
// by a random curviness). Along each curve a terrain brush travels outward (brushes 0x11 and 0x12), 8 world units a step,
// its strength and width falling with the distance travelled; a dust effect ("Quaker") rides each brush head, and repulsors
// ("quake_repulsor") along the line drive the automata away. When the brushes run out the instance waits three seconds and
// ends.
//
// States (eEarthquakeState): 1 the button is held (the intensity grows 0.1 a second up to 1, with a shake effect every
// 10 ms; the menu path leaves this state at once, so its quakes run at the starting intensity 0.1), 2 deforming, 3 done
// (waits 3 s), 4 over.
//
// NOT recovered: the sound system events (0xea451903 / 0xca45190f at the start, 0x6a451908 at the end).
#pragma once
#include <climits>
#include <cstdint>
#include <vector>

#include "sc4/Disaster.h"
#include "sc4/DisasterManagers.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

class cISCPropertyHolder;

constexpr std::uint32_t kEarthquakeTickPriority = 0xc0;
constexpr std::uint32_t kEarthquakeBrushA = 0x11, kEarthquakeBrushB = 0x12;
constexpr float kEarthquakeStep = 8.0f;                     // world units per brush step (0x00a706a8)
constexpr float kEarthquakeMinBrushWidth = 0.002f;          // 0x00a70888
constexpr float kEarthquakeMinBrushStrength = 0.001f;
constexpr std::uint32_t kEarthquakeDoneMicroseconds = 3000000;
constexpr cGZPersistResourceKey kEarthquakeTuningKey{0x6534284a, 0x29b6c670, 2};

// cISTETerrain::tBrushParameters as the earthquake fills it (the host hands it to the terrain's ApplyBrush).
struct EarthquakeBrush {
    float previousX = 0.0f, previousZ = 0.0f;   // +0x00
    float currentX = 0.0f, currentZ = 0.0f;     // +0x08
    std::int32_t zoom = -1;                     // +0x10
    bool valid = false;                         // +0x14
    float strengthScale = 1.0f;                 // +0x18
    float radiusScale = 1.0f;                   // +0x1c
    std::uint32_t redisplayFlags = 0;           // +0x20
    std::int32_t rect[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};   // +0x24: the cells the brush touched (in / out)
    float levelAltitude = 0.0f;                 // +0x34
};

// The tuning the class reads once (ReadTuningParameters 0x0033ef14); the values are the binary's before the exemplar.
struct EarthquakeTuning {
    cSC4ResponseCurve brushStrengthAtEpicenterVsIntensity;   // 0x4a08c500
    cSC4ResponseCurve brushWidthAtEpicenterVsIntensity;      // 0x4a08c501
    cSC4ResponseCurve brushStrengthVsDistance;               // 0x4a08c502
    cSC4ResponseCurve brushWidthVsDistance;                  // 0x4a08c503
    float distBetweenEffects = 40.0f;                        // 0x4a08c504
    float faultLineCurviness = 30.0f;                        // 0x4a08c505
    float maxFaultLineSlopeVariation = 0.0f;                 // 0x4a08c506 (degrees in the exemplar, radians here)
    float brushTravelSpeed = 25.0f;                          // 0x4a08c507 (per second in the exemplar, per microsecond here)
    std::uint32_t microSecsBetweenShakeEffects = 10000;      // 0x4a08c508
    float distBetweenAutomataRepulsors = 100.0f;             // 0x4a08c509
};

class IEarthquakeHost {
public:
    virtual ~IEarthquakeHost() = default;
    virtual float CitySizeX() = 0;                                              // SL::spCity slot 0x164
    virtual float CitySizeZ() = 0;                                              // slot 0x168
    virtual float Altitude(float x, float z) = 0;                               // the terrain's slot 0x30
    virtual void ApplyBrush(std::uint32_t brush, EarthquakeBrush& parameters) = 0;   // slot 0xe4
    virtual void RedisplayTerrain(const std::int32_t rect[4]) = 0;              // slot 0x114 (true, true, rect, 3)
    virtual void StartTextureUpdates() = 0;                                     // slot 0x138
    virtual void StopTextureUpdates() = 0;                                      // slot 0x13c
    virtual void BeginLongTerrainOperation() = 0;                               // slot 0x15c
    virtual void EndLongTerrainOperation() = 0;                                 // slot 0x160
    // the effects manager (city slot 0x154) slot 0x18: the named effect, placed at the point and started; a counted handle
    virtual void* CreateEffect(const char* name, const float xyz[3]) = 0;
    virtual void MoveEffect(void* effect, const float xyz[3]) = 0;              // the effect's slot 0x20
    virtual void ReleaseEffect(void* effect) = 0;
    // the automata controller manager (city slot 0xbc) slot 0x14 with no occupant: a controller of the template at the point
    virtual void* AddRepulsor(const char* templateName, const float xyz[3]) = 0;
    virtual void RemoveRepulsor(void* controller) = 0;                          // slot 0x20, then the reference goes
    virtual void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;      // SL::ATM() slot 0x14
    virtual void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;   // slot 0x18
    virtual double RandomRange(double lo, double hi) = 0;                       // the class's own cRZRandom
    virtual const cISCPropertyHolder* TuningExemplar() = 0;                     // {0x6534284a, 0x29b6c670, 2}
};
IEarthquakeHost* EarthquakeHost();
void SetEarthquakeHost(IEarthquakeHost* host);

class cSC4DisasterInstanceEarthquake final : public cISC4DisasterInstanceEarthquake, public cIGZMessageTarget2 {
public:
    cSC4DisasterInstanceEarthquake() = default;                       // @ 0x0034017c
    ~cSC4DisasterInstanceEarthquake();                                // @ 0x0034002c

    bool QueryInterface(std::uint32_t iid, void** out) override;      // @ 0x0033eece
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    void Init(int x, int z) override;                                 // @ 0x00340a8a
    void BeginDeformation() override { SetState(2); }                 // the manager's SetState(2)
    bool Shutdown() override;                                         // @ 0x0033fed0
    cISC4DisasterManager* GetManager() const override { return manager_; }   // @ 0x0033ee62
    std::uint32_t GetID() const override { return kDisasterManagerIdEarthquake; }   // @ 0x0033f130
    bool GetLocation(int&, int&) const override { return false; }     // @ 0x0033ee5a
    int GetPhase() const override { return 1; }                       // @ 0x0033ee74
    bool DoMessage(cIGZMessage2* message) override;                   // @ 0x0034057a

    void SetState(int state);                                         // @ 0x0034040c
    void DoAnimationTick(std::uint32_t microseconds);                 // @ 0x003404f4
    bool DoDeformation(std::uint32_t microseconds);                   // @ 0x0033f4ac: true when the brushes are done
    static void EvalBezier(const std::vector<cS3DVector2>& points, float t, cS3DVector2& out);   // @ 0x0033f1b8
    static const EarthquakeTuning& Tuning();                          // StaticInit 0x0033f0d0 / ReadTuningParameters
    static void ResetStatics();                                       // StaticShutdown 0x0033f11e, and the session's slope

    int State() const { return state_; }
    float Intensity() const { return intensity_; }
    void SetIntensity(float intensity) { intensity_ = intensity; }
    const cS3DVector2& Epicenter() const { return epicenter_; }
    const std::vector<cS3DVector2>& FaultLine(int side) const { return faultLine_[side & 1]; }
    float DistanceTravelled() const { return distance_; }
    std::size_t RepulsorCount() const { return repulsors_.size(); }

private:
    struct PointOnFaultLine { bool valid = false; cS3DVector2 point{0.0f, 0.0f}; };
    void ComputeFaultLine();                                          // @ 0x003405b6
    void ResetFaultLineIterator(float step);                          // @ 0x0033ee7e
    bool GetNextPointsOnFaultLine(PointOnFaultLine& a, PointOnFaultLine& b);   // @ 0x0033f3b4
    void StartDeformation();                                          // @ 0x0033fa9c
    void EndEarthquakeAction();                                       // @ 0x0033f13a
    void PlaceAutomataRepulsors();                                    // @ 0x00340392
    void ReleaseAutomataRepulsors();                                  // @ 0x0033f934
    void DoMouseDownUpdate(std::uint32_t microseconds);               // @ 0x00340450
    void* CreateParticleEffect(const char* name, const cS3DVector2& at, bool keep);   // @ 0x0033f9d0
    void PlaceParticleEffect(void* effect, const cS3DVector2& at);    // @ 0x0033f340

    std::uint32_t refCount_ = 0;
    bool initialised_ = false;                                        // +0x10
    int state_ = 0;                                                   // +0x14
    cISC4DisasterManager* manager_ = nullptr;                         // +0x18
    float intensity_ = 0.1f;                                          // +0x24
    cS3DVector2 epicenter_{-3.4028235e+38f, -3.4028235e+38f};         // +0x28
    float maxX_ = 0.0f, maxZ_ = 0.0f;                                 // +0x30, +0x34: the city's size less one
    std::uint32_t stateTime_ = 0;                                     // +0x38: microseconds in this state
    std::uint32_t shakeTime_ = 0;                                     // +0x3c
    float slopeCos_ = 0.0f, slopeSin_ = 0.0f;                         // +0x40, +0x44
    std::vector<cS3DVector2> faultLine_[2];                           // +0x48, +0x54: four control points each
    bool lineDone_[2] = {false, false};                               // +0x60, +0x61
    float iteratorStep_ = 0.0f;                                       // +0x64
    float iteratorT_ = 0.0f;                                          // +0x68
    float sinceEffect_[2] = {0.0f, 0.0f};                             // +0x6c, +0x70
    PointOnFaultLine previous_[2];                                    // +0x74 / +0x80
    float distance_ = 0.0f;                                           // +0x8c: how far the brushes have gone
    float travel_ = 0.0f;                                             // +0x90: how far they may have gone by now
    float epicenterStrength_ = 0.0f, epicenterWidth_ = 0.0f;          // +0x94, +0x98
    EarthquakeBrush brush_[2];                                        // +0x9c, +0xd4
    void* effect_[2] = {nullptr, nullptr};                            // +0x10c, +0x110
    std::vector<void*> repulsors_;                                    // +0x114
};

}  // namespace sc4
