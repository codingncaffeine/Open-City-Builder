// cGZSndSys — the sound system service (id 0x23cecba0, priority 500000): event dispatch (a queue
// drained every 20 ms by a timer plus synchronous DoEvent), periodic update targets at two
// intervals (every tick = 20 ms, and every 250 ms), registered renderers that receive the
// listener settings, the property and instance caches and the extension -> data source class
// map. Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (vtable 0x100ab5e00, 0x278-byte
// object, ctor @ 0x100486240, Create @ 0x10043ca1a).
//
// Bases/offsets as in the binary: cIGZSndSys +0, cIGZSystemService +8, cIGZMessageTarget2 +0x10,
// cIGZSnd3DListener +0x18, cIGZSnd3DListenerEx +0x20, cIGZSndEaRendererEffects +0x28.
// QueryInterface ids: 1 / 0x106077d2 -> primary, 0x287fb697 service, 0x452294aa message target,
// 0x406077d2 listener, 0x5b30e7b3 listener Ex, 0xab79e31c effects.
//
// Registrations made while a loop over the same list is running are applied when the outermost
// loop ends (targets: pending lists applied by UpdateTargets; handlers: by DoEvent).
//
// Preserved behaviour: SetMasterVolume ignores its argument (renderers are told the stored
// value, which starts at 0x400 and never changes); the 3D listener setters forward to every
// renderer that exposes the listener interfaces and remember the values for the getters;
// IsSndPlaying / FindPlayingGZSnd only compare the sound id (not the playing state).
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <string>

#include "audio/interfaces/cIGZSndSys.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZMessageQueue2.h"
#include "gz/cIGZSystemService.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZCallbackTimer.h"
#include "rz/cRZCriticalSection.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"

class cGZSndSys final : public cIGZSndSys,
                        public cIGZSystemService,
                        public cIGZMessageTarget2,
                        public cIGZSnd3DListener,
                        public cIGZSnd3DListenerEx,
                        public cIGZSndEaRendererEffects {
public:
    static constexpr std::uint32_t kServiceId = 0x23cecba0;
    static constexpr std::int32_t kServicePriority = 500000;
    static constexpr std::uint32_t kMessageTypeSndEvent = 0xdb53383b;
    static constexpr std::uint32_t kFastIntervalMs = 0x14;
    static constexpr std::uint32_t kSlowIntervalMs = 0xfa;

    cGZSndSys();                                                       // @ 0x100486240
    ~cGZSndSys();                                                      // @ 0x100487b50
    static cGZSndSys* Create() { return new cGZSndSys(); }             // @ 0x10043ca1a

    bool QueryInterface(std::uint32_t iid, void** out) override;       // @ 0x100485de0
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount_); }   // @ 0x100486070
    std::uint32_t Release() override;                                  // @ 0x100486130 (destroys below 2)

    // --- cIGZSystemService ---
    std::uint32_t GetServiceID() const override { return serviceId_; }         // @ 0x10049a780
    void SetServiceID(std::uint32_t id) override { serviceId_ = id; }          // @ 0x10049a7a0
    std::int32_t GetServicePriority() const override { return kServicePriority; }   // @ 0x10049a7c0
    bool IsServiceRunning() const override { return true; }                    // @ 0x10049a7d0
    void SetServiceRunning(bool) override {}                                   // @ 0x10049a7f0
    bool Init() override;                                              // @ 0x1004881f0
    bool Shutdown() override;                                          // @ 0x10048a0e0
    bool OnTick(std::uint32_t) override { return false; }              // @ 0x100493900
    bool OnIdle(std::uint32_t) override { return true; }               // @ 0x100493940
    std::int32_t GetServiceTickPriority() override { return kServicePriority; }   // @ 0x10049a810

    // --- cIGZMessageTarget2 ---
    bool DoMessage(cIGZMessage2* message) override;                    // @ 0x100495450

    // --- cIGZSndSys: events ---
    cIGZSnd3DListener* AsIGZSnd3DListener() override { return static_cast<cIGZSnd3DListener*>(this); }       // @ 0x10049a5f0
    cIGZSnd3DListenerEx* AsIGZSnd3DListenerEx() override { return static_cast<cIGZSnd3DListenerEx*>(this); } // @ 0x10049a610
    bool DoEvent(cIGZSndEventStd* event) override;                     // @ 0x100496800
    bool DoEventIds(std::uint32_t id, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override;   // @ 0x100495ed0
    bool DoEventUnknown(std::uint32_t id, cIGZUnknown* u, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override;   // @ 0x100496460
    bool DoEventString(std::uint32_t id, cIGZString* s, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override;     // @ 0x100496200
    bool PostEvent(cIGZSndEventStd* event) override;                   // @ 0x100495620
    bool PostEventIds(std::uint32_t id, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override;   // @ 0x100495680
    bool PostEventUnknown(std::uint32_t id, cIGZUnknown* u, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override;   // @ 0x100495980
    bool PostEventString(std::uint32_t id, cIGZString* s, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override;     // @ 0x100495cd0
    bool EventCreate(std::uint32_t iid, void** out) override;          // @ 0x1004972f0
    bool EventCreateWithData(std::uint32_t eventId, std::uint32_t iid, void** out, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::uint32_t d4) override;   // @ 0x100497400
    bool RegisterEventHandler(cIGZSndEventHandler* handler) override;  // @ 0x100491e50
    bool UnregisterEventHandler(cIGZSndEventHandler* handler) override;   // @ 0x1004928f0
    bool Update(std::uint32_t elapsedMs) override;                     // @ 0x100493c10

    // --- configuration ---
    bool GetInitFlag() const override { return initFlag_; }            // @ 0x10049a630
    void SetDataDirectory(const cIGZString& directory) override;       // @ 0x100488190
    cIGZString* DataDirectory() override { return &dataDirectoryString_; }   // @ 0x1004881d0
    bool GetEnabledFlag() const override { return enabled_; }          // @ 0x10049a650
    void SetEnabledFlag(bool enabled) override { enabled_ = enabled; } // @ 0x10049a670
    bool SetExtensionDefaultDataSourceClassId(const cIGZString& extension, std::uint32_t classId) override;   // @ 0x1004976c0
    bool GetExtensionDefaultDataSourceClassId(const cIGZString& extension, std::uint32_t& classId) override;  // @ 0x100498740
    std::uint32_t GetNumExtensionDefaultDataSourceClassIdMappings() override { return static_cast<std::uint32_t>(extensionMap_.size()); }   // @ 0x100498d50
    bool GetExtensionDefaultDataSourceClassIdMappingClassId(std::uint32_t index, std::uint32_t& classId) override;   // @ 0x100498d90
    bool GetExtensionDefaultDataSourceClassIdMappingExtension(std::uint32_t index, cIGZString& extension) override;  // @ 0x100499400
    std::int32_t GetMasterVolume() const override { return masterVolume_; }   // @ 0x10048f910
    bool SetMasterVolume(std::int32_t volume) override;                // @ 0x10048f930 (argument ignored)
    // PORT: the effects volume of the audio preferences (0..0x400, 0 when effects are off), stored where every sound
    // reads it (cGZSnd::SetVolume scales by it) — the Windows build's master volume event 0x2a5b69c0 does this store.
    void StoreMasterVolume(std::int32_t volume) { masterVolume_ = volume; SetMasterVolume(volume); }
    bool RegisterTarget(cIGZSndUpdateTarget* target, SndCallbackInterval interval) override;   // @ 0x100490830
    bool UnregisterTarget(cIGZSndUpdateTarget* target) override;       // @ 0x100491410
    bool EnumerateTargets(UpdateTargetFunction function, std::uint32_t arg, void* userData) override;   // @ 0x100494d00
    bool EnableUpdates() override { updatesEnabled_ = true; return true; }    // @ 0x10048fd30
    bool DisableUpdates() override { updatesEnabled_ = false; return true; }  // @ 0x10048fd50
    bool RegisterRenderer(cIGZSndRenderer* renderer) override;         // @ 0x10048b080
    bool UnregisterRenderer(cIGZSndRenderer* renderer) override;       // @ 0x10048bb50
    bool IsRegisteredRenderer(cIGZSndRenderer* renderer) override;     // @ 0x10048c1d0
    std::uint32_t NumRenderers() override { return static_cast<std::uint32_t>(renderers_.size()); }   // @ 0x10048c550
    bool FindPlayingGZSnd(std::uint32_t id, std::uint32_t iid, void** out) override;   // @ 0x100493b10
    bool IsSndPlaying(std::uint32_t id) override;                      // @ 0x100493ba0
    cIGZCriticalSection* GetCriticalSection() override { return &criticalSection_; }   // @ 0x10049a690
    bool GetSndPropertyCacheByIid(cIGZSndPropertyCache** out) override;   // @ 0x100497520
    cIGZSndPropertyCache* GetSndPropertyCache() override { return propertyCache_.get(); }   // @ 0x10049a6b0
    bool GetSndInstanceCacheByIid(cIGZSndInstanceCache** out) override;   // @ 0x1004975f0
    cIGZSndInstanceCache* GetSndInstanceCache() override { return instanceCache_.get(); }   // @ 0x10049a6e0
    void AddToMainlineReleaseList(cIGZUnknown* object) override;       // @ 0x10048fd70
    void Panic() override;                                             // @ 0x100488a00
    void SetTestMode(bool testMode) override { testMode_ = testMode; } // @ 0x10049a710
    bool GetTestMode() const override { return testMode_; }            // @ 0x10049a740
    std::uint32_t GetSoundSystemThreadId() override { return 0; }      // @ 0x10049a5e0
    void SetInitFlag(bool value) { initFlag_ = value; }                // @ 0x10049a760

    // --- cIGZSnd3DListener / cIGZSnd3DListenerEx (settings are stored and forwarded) ---
    bool CommitDeferredSettings() override;                            // @ 0x10048c590
    bool GetPosition(float* x, float* y, float* z) const override;     // @ 0x10048cb20
    bool SetPosition(float x, float y, float z) override;              // @ 0x10048cbb0
    bool GetOrientation(float* fx, float* fy, float* fz, float* ux, float* uy, float* uz) const override;   // @ 0x10048d110
    bool SetOrientation(float fx, float fy, float fz, float ux, float uy, float uz) override;   // @ 0x10048d210
    bool GetVelocity(float* x, float* y, float* z) const override;     // @ 0x10048d7f0
    bool SetVelocity(float x, float y, float z) override;              // @ 0x10048d880
    bool GetAll(float* px, float* py, float* pz, float* vx, float* vy, float* vz, float* fx, float* fy, float* fz, float* ux, float* uy, float* uz) const override;   // @ 0x10048de30
    bool SetAll(float px, float py, float pz, float vx, float vy, float vz, float fx, float fy, float fz, float ux, float uy, float uz) override;   // @ 0x10048e0a0
    bool GetDopplerFactor(float* factor) const override;               // @ 0x10048e8f0
    bool SetDopplerFactor(float factor) override;                      // @ 0x10048e950
    bool GetRolloffFactor(float* factor) const override;               // @ 0x10048ee50
    bool SetRolloffFactor(float factor) override;                      // @ 0x10048eeb0
    bool GetDistanceFactor(float* factor) const override;              // @ 0x10048f3b0
    bool SetDistanceFactor(float factor) override;                     // @ 0x10048f410

    // --- cIGZSndEaRendererEffects (forwarded to every renderer that supports it) ---
    bool InitializeEffectsBus(std::int32_t bus, float level, std::int32_t a, std::int32_t b, std::int32_t c) override;   // @ 0x100499b00
    bool SetEffectMasterLevel(std::int32_t bus, float level) override; // @ 0x10049a100

    // Timer entry point (GZSndSys_Timer_Callback @ 0x100485d20 + HandleTimerCallback @ 0x100485db0).
    void HandleTimerCallback(std::uint32_t elapsedMs) { Update(elapsedMs); }
    std::uint32_t GetElapsedTime() const;                              // @ 0x100485d80

    // --- inspection ---
    std::size_t fastTargetCount() const { return fastTargets_.size(); }
    std::size_t slowTargetCount() const { return slowTargets_.size(); }
    std::size_t handlerCount() const { return handlers_.size(); }
    std::uint32_t queuedEventCount() { return queue_ ? queue_->GetMessageCount() : 0; }
    std::size_t mainlineReleaseCount() const { return mainlineRelease_.size(); }
    bool shuttingDown() const { return shuttingDown_; }

private:
    using TargetList = std::list<cRZAutoRefCount<cIGZSndUpdateTarget>>;
    using HandlerList = std::list<cRZAutoRefCount<cIGZSndEventHandler>>;
    bool UpdateTargets(UpdateTargetFunction function, std::uint32_t arg, void* userData);   // @ 0x100493de0
    // Calls `apply` with the renderer's interface `iid` for every registered renderer that has it.
    template <class T, class F>
    void ForEachRendererInterface(std::uint32_t iid, F&& apply);

    std::int32_t refCount_ = 0;                                        // +0x68
    std::uint32_t serviceId_ = kServiceId;                             // +0x6c
    bool dataDirectorySet_ = false;                                    // +0x30
    cRZString dataDirectoryString_;                                    // +0x38

    bool enabled_ = true;                                              // +0x70
    bool initFlag_ = false;                                            // +0x71
    bool testMode_ = false;                                            // +0x271
    bool shuttingDown_ = false;                                        // +0x270
    bool updatesEnabled_ = true;                                       // +0xb0
    std::int32_t masterVolume_ = 0x400;                                // +0x230
    float position_[3] = {0, 0, 0};                                    // +0x234
    float orientation_[6] = {0, 1, 0, 0, 0, 0};                        // +0x240
    float velocity_[3] = {0, 0, 0};                                    // +0x258
    float doppler_ = 0.0f;                                             // +0x264
    float rolloff_ = 1.0f;                                             // +0x268
    float distance_ = 0.0f;                                            // +0x26c

    cRZCriticalSection criticalSection_;                               // +0xa0
    cRZCriticalSection targetLock_;                                    // +0x138
    cRZCriticalSection rendererLock_;                                  // +0x168
    cRZCriticalSection handlerLock_;                                   // +0x1c8
    cRZTimer slowTimer_{cRZTimer::kMilliseconds};                      // +0x78
    cRZTimer* clock_ = nullptr;                                        // +0x90
    cRZCallbackTimer* callbackTimer_ = nullptr;                        // +0x98
    cRZAutoRefCount<cIGZMessageQueue2> queue_;                         // +0x60
    cRZAutoRefCount<cIGZSndPropertyCache> propertyCache_;              // +0x220
    cRZAutoRefCount<cIGZSndInstanceCache> instanceCache_;              // +0x228

    TargetList fastTargets_;                                           // +0xc0
    TargetList slowTargets_;                                           // +0xd8
    TargetList pendingFast_;                                           // +0xf0
    TargetList pendingSlow_;                                           // +0x108
    TargetList pendingRemove_;                                         // +0x120
    std::int32_t targetGuard_ = 0;                                     // +0x130
    std::list<cRZAutoRefCount<cIGZSndRenderer>> renderers_;            // +0x150
    HandlerList handlers_;                                             // +0x180
    HandlerList pendingHandlerAdd_;                                    // +0x198
    HandlerList pendingHandlerRemove_;                                 // +0x1b0
    std::int32_t handlerGuard_ = 0;                                    // +0x1c0
    std::map<std::string, std::uint32_t> extensionMap_;                // +0x1d8
    std::list<cRZAutoRefCount<cIGZUnknown>> mainlineRelease_;          // +0x1f0
};
