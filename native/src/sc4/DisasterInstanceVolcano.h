// cSC4DisasterInstanceVolcano (Mac i386 0x00342526..0x003434be, 0x54 bytes; made by cSC4DisasterManagerVolcano::
// CreateVolcanoInstance 0x0034346c).
//
// A volcano is the "volcano_control" effect at its place: that effect picks one of nine "creators", each a terrain brush that
// raises the cone, a scrubber that clears and burns around it, smoke, fallout and crater decals. While it erupts, lava
// (cSTEVolcanoLava) flows from the vent for a tuned time, its rate following two curves over that time. The instance is over
// when the effect has finished and the lava has stopped.
//
// States (cISC4DisasterInstanceVolcano::eState): 1 placed, 3 erupting (the effect is started and the lava made, then 5),
// 5 the lava flows, 7 the effect finished (then 5 while lava still flows, else 8), 8 over.
//
// The lava is the IVolcanoLava seam: a host without one runs the eruption without lava (the tuning's "lava should flow"
// then counts as off).
#pragma once
#include <cstdint>

#include "sc4/Disaster.h"
#include "sc4/DisasterManagers.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

class cISCPropertyHolder;

constexpr std::uint32_t kVolcanoTickPriority = 0xc0;
constexpr std::uint32_t kVolcanoEffectFinishedMessage = 0xca493c1d;
constexpr cGZPersistResourceKey kVolcanoTuningKey{0x6534284a, 0x29b6c670, 1};
constexpr float kVolcanoRateDivisor = 100000.0f;   // FLOAT 0x00a709c4: the tuning curves' rates are divided by it

// cSTEVolcanoLava as the instance drives it.
class IVolcanoLava {
public:
    virtual ~IVolcanoLava() = default;
    virtual void StartLavaFlow(float x, float z, float ventRadius, bool burn) = 0;    // @ 0x004fa97a
    virtual void SetLavaProductionRate(float rate) = 0;                               // @ 0x004f8e00
    virtual void SetSolidificationRate(float rate) = 0;                               // @ 0x004f8e12
    virtual void Shutdown() = 0;                                                      // @ 0x004f9cf4
    virtual float GetLavaHeightByCityCell(int x, int z) = 0;                          // @ 0x004f8ea2
    virtual void Release() = 0;
};

class IVolcanoHost {
public:
    virtual ~IVolcanoHost() = default;
    virtual float CitySizeX() = 0;                                              // SL::spCity slot 0x164
    virtual float CitySizeZ() = 0;                                              // slot 0x168
    virtual float Altitude(float x, float z) = 0;                               // the terrain's slot 0x30
    // the effects manager (city slot 0x154) slot 0x18: the named effect told to post `finishMessage` with `finishData`
    // when it is over, placed at the point and started; a counted handle, null when there is no such effect
    virtual void* CreateEffect(const char* name, const float xyz[3], std::uint32_t finishMessage, std::uint32_t finishData) = 0;
    virtual void StopEffect(void* effect) = 0;                                  // the effect's Stop(1), then the reference goes
    virtual void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;      // SL::ATM() slot 0x14
    virtual void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;   // slot 0x18
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;    // SL::spMS2 slot 0x14
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0; // slot 0x18
    virtual const cISCPropertyHolder* TuningExemplar() = 0;                     // {0x6534284a, 0x29b6c670, 1}
    virtual IVolcanoLava* CreateLava() = 0;                                     // a new cSTEVolcanoLava, Init'ed; one reference
};
IVolcanoHost* VolcanoHost();
void SetVolcanoHost(IVolcanoHost* host);

// The tuning the class reads at every Init (ReadTuningParameters 0x00342876).
struct VolcanoTuning {
    cSC4ResponseCurve lavaProductionRate;            // 0xea4bc6e4, over the fraction of the lava time
    cSC4ResponseCurve lavaSolidificationRate;        // 0x4a4bc714
    float totalLavaSeconds = 0.0f;                   // 0x0a4bc742
    float productionScale = 0.0f;                    // 0xaa4bc72a
    std::uint32_t ventRadius = 0;                    // 0x0a4bc752
    bool lavaShouldFlow = false;                     // 0x6a4bc8ed
};

class cSC4DisasterInstanceVolcano final : public cISC4DisasterInstanceVolcano, public cIGZMessageTarget2 {
public:
    cSC4DisasterInstanceVolcano();                                    // @ 0x0034339e
    ~cSC4DisasterInstanceVolcano();                                   // @ 0x00342cc2

    bool QueryInterface(std::uint32_t iid, void** out) override;      // @ 0x0034263a
    std::uint32_t AddRef() override { return ++refCount_; }           // @ 0x00342526
    std::uint32_t Release() override;                                 // @ 0x00342680

    bool Shutdown() override;                                         // @ 0x00342b7a
    cISC4DisasterManager* GetManager() const override { return manager_; }   // @ 0x003425ea
    std::uint32_t GetID() const override { return kDisasterManagerIdVolcano; }
    bool GetLocation(int& x, int& z) const override { x = locationX_; z = locationZ_; return true; }
    int GetPhase() const override;                                    // @ 0x003425fc
    float GetLavaHeightByCityCell(int x, int z) override;             // @ 0x003429d2
    void SetLocation(int x, int z) override;                          // @ 0x003426a6
    bool Init() override;                                             // @ 0x00342e8e
    bool SetState(int state) override;                                // @ 0x003434be
    bool DoMessage(cIGZMessage2* message) override;                   // @ 0x00342dd4

    void DoAnimationTick(std::uint32_t microseconds);                 // @ 0x00343140
    int GetState() const { return state_; }                           // @ 0x0034262e
    bool LavaFlowing() const { return lavaFlowing_; }
    bool EffectFinished() const { return effectFinished_; }
    static const VolcanoTuning& Tuning();

private:
    void ReadTuningParameters();                                      // @ 0x00342876
    void StartDeformation();                                          // @ 0x0034274c
    void EndDeformation();                                            // @ 0x0034278c
    void InitializeVolcanoLava();                                     // @ 0x00342548
    void EndVolcanoLava();                                            // @ 0x00342b06

    std::uint32_t refCount_ = 0;                                      // +0x0c
    int locationX_ = -1, locationZ_ = -1;                             // +0x10, +0x14 (world units)
    bool initialized_ = false;                                        // +0x20
    int state_ = 0;                                                   // +0x24
    cISC4DisasterManager* manager_ = nullptr;                         // +0x28
    void* effect_ = nullptr;                                          // +0x34
    float positionX_ = 0.0f, positionZ_ = 0.0f;                       // +0x38, +0x3c
    bool lavaFlowing_ = false;                                        // +0x40
    bool effectStarted_ = false;                                      // +0x41
    bool effectFinished_ = false;                                     // +0x42
    bool lavaEnded_ = false;                                          // +0x43
    bool registered_ = false;                                         // +0x44: the disaster layer counts it
    IVolcanoLava* lava_ = nullptr;                                    // +0x4c
    std::uint32_t lavaTime_ = 0;                                      // +0x50: microseconds of lava so far
    // Port: the effect's finish message carries the instance's address in the original (a 32 bit word); here it carries
    // this number.
    std::uint32_t token_ = 0;
};

}  // namespace sc4
