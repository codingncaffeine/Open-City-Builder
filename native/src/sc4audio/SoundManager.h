// cSC4AudioSoundManager and tSound — the SimCity 4 layer between the game and the sound system:
// one managed sound per (soundId, instanceId), master/slave grouping of looping sounds that share a
// soundId (their positions are averaged into one voice), distance- and simulation-pause handling,
// per-sound instance limits and a response curve that attenuates overlapping instances.
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (vtable 0x100aa3e50, 0xd8-byte object,
// ctor @ 0x1003b7880, tSound functions @ 0x1003b9952..0x1003ba976).
//
// Bases/offsets as in the binary: cIGZSndEventHandler +0, cIGZSndUpdateTarget +8,
// cIGZSndCompletionFunctionObject +0x10, cIGZMessageTarget2 +0x18; refcount +0x20 (starts at 0
// like every GZ object; Release destroys at count < 2).
//
// Register ids are the legacy numbers of nsHitReg (translated by the register set); see
// kReg* below for the proof of each role.
#pragma once
#include <cstdint>
#include <list>
#include <unordered_map>

#include "audio/interfaces/cIGZSnd.h"
#include "audio/interfaces/cIGZSndCompletionFunctionObject.h"
#include "audio/interfaces/cIGZSndEventStd.h"
#include "audio/interfaces/cIGZSndRegisterSet.h"
#include "audio/interfaces/cIGZSndUpdateTarget.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4audio/SC4AudioHost.h"

namespace SC4Audio {

// Register numbers used on a managed sound's register set (all translated by nsHitReg).
inline constexpr std::int32_t kRegArgMode = 0x10;        // cHitTrackPlayer::Play argument mapping; set to 3
inline constexpr std::int32_t kRegVolume = 0x15;         // effective volume (0..1024)
inline constexpr std::int32_t kRegPositionX = 0x20;
inline constexpr std::int32_t kRegPositionY = 0x21;
inline constexpr std::int32_t kRegPositionZ = 0x22;
inline constexpr std::int32_t kRegLoopFlag = 0x33;       // non-zero: looping sound (eligible for master/slave grouping)
inline constexpr std::int32_t kRegGameObject = 0x34;     // receives Play's first argument: the instance id
inline constexpr std::int32_t kRegDefaultMinimumDistance = 0x38;   // hundredths of the sound's near distance
inline constexpr std::int32_t kRegEmanationModel = 0x39;           // sem id of the sound's emanation model

inline constexpr std::uint32_t kModelEmanation2D = 0x7dcded2d;
inline constexpr std::uint32_t kModelEmanation3D = 0x7dcded3d;
inline constexpr std::uint32_t kModelEmanation3DEffects = 0x0a63a543;

// Event ids handled by cSC4AudioSoundManager::DoEvent (the same ids are used by the poster methods).
inline constexpr std::uint32_t kEventStartSound = 0xea451903;      // data: soundId, instanceId, flags
inline constexpr std::uint32_t kEventStartSoundAlias = 1;
inline constexpr std::uint32_t kEventStopSound = 0x6a451908;       // data: instanceId, immediate
inline constexpr std::uint32_t kEventStopSoundAlias = 2;
inline constexpr std::uint32_t kEventSetSoundPosition = 0xca45190f;   // data: instanceId, x, y, z (ints)
inline constexpr std::uint32_t kEventSetSoundVolume = 0x4a451915;     // data: instanceId, volume
inline constexpr std::uint32_t kEventResetSound = 0x4a4d35e0;         // data: instanceId
inline constexpr std::uint32_t kEventEnumerateSounds = 0x4a4fe6f6;    // IGZUnknown: cISC4AudioSoundVisitor
inline constexpr std::uint32_t kEventUpdateAllSounds = 0x4a8c52c8;
inline constexpr std::uint32_t kEventSetMasterVolume = 0x2a5b69c0;    // data: volume
inline constexpr std::uint32_t kEventStopAllSounds = 0xcaa44ca7;      // data: immediate
inline constexpr std::uint32_t kEventFocusChange = 0x6a95cba2;        // data: hasFocus

// Message ids handled by cSC4AudioSoundManager::DoMessage.
inline constexpr std::uint32_t kMessageStopAllSounds = 0x26d31ec2;
inline constexpr std::uint32_t kMessageSceneStopAll = 0x28e9397c;
inline constexpr std::uint32_t kMessageSceneStart = 0x28e9397b;       // consumed, no action
inline constexpr std::uint32_t kMessageUpdateAll1 = 0x8a4a69c7;
inline constexpr std::uint32_t kMessageUpdateAll2 = 0xaa7fb7e0;
inline constexpr std::uint32_t kMessageUpdateAll3 = 0x4a7fb7e9;
inline constexpr std::uint32_t kMessageFocusChange = 0x4348b111;      // data1: has focus
inline constexpr std::uint32_t kMessageReloadTuning = 0xc53d10aa;     // handled, never registered by Init

}  // namespace SC4Audio

class cSC4AudioSoundManager;

// A sound being managed. Intrusive reference count that starts at 1 and destroys on Release at 1
// (the original uses a pooled allocator, stSoundAllocator).
class tSound {
public:
    tSound();
    ~tSound();                                       // @ 0x1003ba19c
    tSound(const tSound&) = delete;
    tSound& operator=(const tSound&) = delete;

    void AddRef() { ++refCount_; }
    std::int32_t Release();                          // @ 0x1003b9f32

    // --- state ---
    std::uint32_t soundId() const { return soundId_; }
    std::uint32_t instanceId() const { return instanceId_; }
    std::int32_t volume() const { return volume_; }
    const float* position() const { return position_; }
    const float* averagePosition() const { return averagePosition_; }
    float minimumDistance() const { return minimumDistance_; }
    float maximumDistance() const { return maximumDistance_; }
    std::uint32_t flags() const { return flags_; }
    std::int32_t pauseCount() const { return pauseCount_; }
    tSound* master() const { return master_; }
    std::size_t slaveCount() const { return slaves_.size(); }
    cIGZSnd* snd() const { return snd_.get(); }
    cIGZSndRegisterSet* registers() const { return registers_.get(); }
    float volumeScale() const { return volumeScale_; }

    // --- operations (original names) ---
    std::int32_t Pause(bool kill);                   // @ 0x1003b9952 (returns the new pause count)
    std::int32_t Resume();                           // @ 0x1003b999c
    bool EnableCompletionNotification(bool enable);  // @ 0x1003b9b08
    bool AddSlave(tSound* slave);                    // @ 0x1003b9c2a
    bool RemoveSlave(tSound* slave);                 // @ 0x1003b9edc
    void TransferSlaves();                           // @ 0x1003b9dc8
    void StartStopSoundForSimulationPause();         // @ 0x1003b9cf0
    void StartStopSoundForDistance();                // @ 0x1003ba068
    bool ShouldPauseForDistance();                   // @ 0x1003ba3f4
    void SetPosition(const float position[3]);       // @ 0x1003b9f74
    void SetVolume(std::int32_t volume);             // @ 0x1003b9fb2
    void Update(std::uint32_t elapsedMs);            // @ 0x1003ba002 (volume ramp)
    void UpdatePosition();                           // @ 0x1003ba226
    void UpdateVolPan3D();                           // @ 0x1003ba2dc
    void UpdateVolPanEffects();                      // @ 0x1003ba376
    void UpdateAveragePosition(float direction[3], const float listener[3]);   // @ 0x1003ba4fc

    // Volume ramp used by Update: moves currentRamp towards targetRamp by rate per millisecond and
    // applies it as the volume. Nothing in the recovered code starts a ramp yet.
    void StartVolumeRamp(float current, float target, float ratePerMs) {
        rampCurrent_ = current; rampTarget_ = target; rampRate_ = ratePerMs;
    }

private:
    friend class cSC4AudioSoundManager;

    std::int32_t refCount_ = 1;                          // +0
    std::uint32_t soundId_ = 0;                          // +4
    std::uint32_t instanceId_ = 0;                       // +8
    cRZAutoRefCount<cIGZSnd> snd_;                       // +0x10
    cRZAutoRefCount<cIGZSndRegisterSet> registers_;      // +0x18
    cRZAutoRefCount<cSC4AudioSoundManager> manager_;     // +0x20
    std::int32_t volume_ = 0;                            // +0x28 requested volume (0..1024)
    float volumeScale_ = 1.0f;                           // +0x2c attenuation from overlapping instances
    float position_[3] = {0, 0, 0};                      // +0x30
    std::uint32_t flags_ = 0;                            // +0x40: 1 volume set, 2 position set,
                                                         //        8 paused for distance, 0x10 paused for
                                                         //        simulation, 0x20 pause with simulation
    std::uint32_t startTime_ = 0;                        // +0x48 manager clock at start
    float minimumDistance_ = 0;                          // +0x4c
    float maximumDistance_ = 0;                          // +0x50 (4x minimum)
    std::int32_t pauseCount_ = 0;                        // +0x54
    float rampCurrent_ = 0, rampTarget_ = 0, rampRate_ = 0;   // +0x58 / +0x5c / +0x60
    std::list<tSound*> slaves_;                          // +0x68 (non-owning)
    tSound* master_ = nullptr;                           // +0x80
    float averagePosition_[3] = {0, 0, 0};               // +0x88
};

// Receiver of kEventEnumerateSounds (interface id 0xea4fc425): called for every managed sound until
// it returns false.
class cISC4AudioSoundVisitor : public cIGZUnknown {
public:
    virtual bool VisitSound(tSound* sound) = 0;          // [3] +0x18
};
constexpr std::uint32_t kGZIID_cISC4AudioSoundVisitor = 0xea4fc425;

class cSC4AudioSoundManager final : public cIGZSndEventHandler,
                                    public cIGZSndUpdateTarget,
                                    public cIGZSndCompletionFunctionObject,
                                    public cIGZMessageTarget2 {
public:
    cSC4AudioSoundManager();                             // @ 0x1003b7880
    ~cSC4AudioSoundManager();                            // @ 0x1003b79d6
    static cSC4AudioSoundManager* Create() { return new cSC4AudioSoundManager(); }

    // --- cIGZUnknown (shared by all four bases): ids 1 and 0x4a47dc0d. Release destroys when the
    // count is below 2. ---
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1003b8ffe
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount_); }   // @ 0x1003b9092
    std::uint32_t Release() override;                              // @ 0x1003b90ca

    bool DoEvent(cIGZSndEventStd* event) override;                 // @ 0x1003b84c6 (slot 3)
    void Update(std::int32_t elapsedMs) override;                  // @ 0x1003b8426 (cIGZSndUpdateTarget)
    void OnSoundCompleted(cIGZSnd* sound) override;                // @ 0x1003b8bec DoCompletionFunction
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1003b8e3a

    // --- public API (each posts an event to the sound system, which calls back into DoEvent) ---
    bool Init();                                                   // @ 0x1003b7ac0
    bool Shutdown();                                               // @ 0x1003b7f62
    bool StopAllSounds(bool immediate);                            // @ 0x1003b80dc
    bool SetMasterVolume(std::uint32_t volume);                    // @ 0x1003b817c
    std::uint32_t GetMasterVolume() const { return masterVolume_; }  // @ 0x1003b821e
    bool StartSound(std::uint32_t soundId, std::uint32_t instanceId);   // @ 0x1003b8236
    bool SetSoundPosition(std::uint32_t instanceId, float x, float y, float z);   // @ 0x1003b82da
    bool UpdateAllSounds();                                        // @ 0x1003b8388

    // @ 0x1003b9ae0: process-wide counter; 0 is skipped, so ids start at 1.
    static std::uint32_t GenerateNewInstanceId();
    // Port-only: resets the counter (tests).
    static void ResetInstanceIdCounterForTesting();

    // Adapter registered with SC4Audio::SetSoundManager so the emanation models can read the master
    // volume (SFX::SoundManager() of the original returns this object).
    SC4Audio::cIMasterVolume* AsMasterVolume() { return &masterVolumeView_; }

    // --- inspection ---
    std::size_t SoundCount() const { return sounds_.size(); }
    const std::list<tSound*>& Sounds() const { return sounds_; }   // in start order
    tSound* FindSound(std::uint32_t instanceId) const;
    std::int32_t nonMasterSoundCount() const { return nonMasterCount_; }
    std::int32_t masterSoundCount() const { return masterCount_; }
    std::int32_t totalSoundCount() const { return totalCount_; }
    std::uint32_t elapsedMilliseconds() const { return elapsedMs_; }
    std::uint32_t maxInstancesPerSound() const { return maxInstancesPerSound_; }
    std::uint32_t maxNonMasterInstances() const { return maxNonMasterInstances_; }
    std::uint32_t tuningValue30() const { return tuningValue30_; }
    std::int32_t focusRefreshTicks() const { return focusRefreshTicks_; }
    const cSC4ResponseCurve& responseCurve() const { return responseCurve_; }
    cIGZSndCompletionFunctionObject* AsCompletionFunctionObject() { return this; }

private:
    friend class tSound;

    class MasterVolumeView final : public SC4Audio::cIMasterVolume {
    public:
        explicit MasterVolumeView(cSC4AudioSoundManager* owner) : owner_(owner) {}
        bool QueryInterface(std::uint32_t iid, void** out) override { return owner_->QueryInterface(iid, out); }
        std::uint32_t AddRef() override { return owner_->AddRef(); }
        std::uint32_t Release() override { return owner_->Release(); }
        std::int32_t GetMasterVolume() override { return static_cast<std::int32_t>(owner_->masterVolume_); }
    private:
        cSC4AudioSoundManager* owner_;
    };

    bool PostEvent(std::uint32_t eventId, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::uint32_t d4);
    bool ReadTuningValues();                                       // @ 0x1003b7d2a
    bool HandleStartSound(cIGZSndEventStd* event);                 // @ 0x1003b8622
    bool HandleStopSound(cIGZSndEventStd* event);                  // @ 0x1003b86ca
    bool HandleSetSoundPosition(cIGZSndEventStd* event);           // @ 0x1003b8754
    bool HandleSetSoundVolume(cIGZSndEventStd* event);             // @ 0x1003b880a
    bool HandleResetSound(cIGZSndEventStd* event);                 // @ 0x1003b888e
    bool HandleEnumerateSounds(cIGZSndEventStd* event);            // @ 0x1003b88fa
    bool HandleSetMasterVolume(cIGZSndEventStd* event);            // @ 0x1003b8a00
    bool HandleFocusChangeEvent(cIGZSndEventStd* event);           // @ 0x1003b8af4
    bool HandleFocusChangeMessage(cIGZMessage2* message);          // @ 0x1003b8ef4
    bool UpdateAllSoundsNow();                                     // @ 0x1003b8a6a (_UpdateAllSounds)
    void StopAllSoundsNow(bool immediate);                         // @ 0x1003b8ab6
    bool StopSoundNow(std::uint32_t instanceId, bool immediate);   // @ 0x1003b8cac
    bool ResetSound(std::uint32_t instanceId);                     // @ 0x1003b9146
    bool StartSoundNow(std::uint32_t soundId, std::uint32_t instanceId, std::uint32_t flags);   // @ 0x1003b925e
    bool SetSoundPositionNow(std::uint32_t instanceId, float x, float y, float z);   // @ 0x1003b9828
    bool SetSoundVolumeNow(std::uint32_t instanceId, std::uint32_t volume);          // @ 0x1003b98d2
    bool GetPlayingSoundMinPlayTime(std::uint32_t soundId, tSound** newest, std::int32_t& minPlayTime);   // @ 0x1003b9a74
    void RemoveFromSoundList(tSound* sound);                       // @ 0x1003b9d82

    std::int32_t refCount_ = 0;                          // +0x20
    std::uint32_t masterVolume_ = 0x400;                 // +0x24
    std::uint32_t elapsedMs_ = 0;                        // +0x28
    std::int32_t reentryCount_ = 0;                      // +0x2c incremented around the Update loop
    std::int32_t focusRefreshTicks_ = 0;                 // +0x40 volume refresh ticks after regaining focus
    std::unordered_map<std::uint32_t, tSound*> byInstance_;   // +0x48 (holds one reference per sound)
    std::list<tSound*> sounds_;                          // +0x70 in start order (non-owning)
    std::int32_t totalCount_ = 0;                        // +0x88
    std::int32_t nonMasterCount_ = 0;                    // +0x90 playing non-looping sounds
    std::int32_t masterCount_ = 0;                       // +0x94 looping sounds that are not slaves
    cRZAutoRefCount<cIGZSndSys> soundSystem_;            // +0x98
    cRZAutoRefCount<cIGZSndMgr> soundManager_;           // +0xa0
    std::uint32_t maxInstancesPerSound_ = 3;             // +0xa8 (tuning property 0xaabaa0d3)
    std::uint32_t maxNonMasterInstances_ = 10;           // +0xac (tuning property 0x0a946a57)
    std::uint32_t tuningValue30_ = 30;                   // +0xb0 (tuning property 0x4a946a5d), not read here
    cSC4ResponseCurve responseCurve_;                    // +0xb8
    MasterVolumeView masterVolumeView_{this};

};
