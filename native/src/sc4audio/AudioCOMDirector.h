// The game's audio COM directors. cSC4AudioCOMDirector (director id 0xfdb5ca17) is the one cSC4App::AddCOMDirectors
// adds when the player's audio option is on; its OnStart adds the four library directors, and its PostAppInit reads
// the sound configuration and creates the game's own sound services.
//
// Mac i386 (the x86_64 addresses are in the classes' own headers): cSC4AudioCOMDirector constructor 0x002f0cba,
// OnStart 0x002f053e, PostAppInit 0x002f05f2, PreAppShutdown 0x002f0b12, GetSC4AudioCOMDirector 0x002f1684;
// GZSndSysMacCOMDirector constructor 0x007a3180, OnStart 0x007a3256; cHitManCOMDirector constructor 0x00718b22,
// OnStart 0x00718c52, PreAppInit 0x00718e66, PreAppShutdown 0x00718d92; cFreshnessCOMDirector constructor 0x0032b320.
//
// Start-up order of PostAppInit (each creation goes through GZCOM by class id, so a class left out of the build
// fails its step and the director answers false): the sound configuration {0, 0x8a5971c5, 0xea8a1115} through
// cHitIniFile::EnumDefaultEntries; the radio (created, Init'ed, registered as a system service: only this sets the
// result true); the emanation model manager, the soundscape manager, the event handler, the disaster fire handler,
// the ambience manager, the sound manager and the listener are created; then Init runs on the soundscape manager,
// the ambience manager, the sound manager, the listener, the emanation model manager, the event handler and the
// disaster fire handler. A failed step clears the result and the rest still runs; nothing is undone.
//
// PreAppShutdown: the disaster fire handler, the event handler, the emanation model manager, the listener, the sound
// manager, the ambience manager, the soundscape manager; each holder is released after its Shutdown.
//
// Deviation forced by safety: an Init is skipped when its object was not created (the original calls through the
// null holder).
#pragma once
#include <cstdint>

#include "audio/HitIniFile.h"
#include "audio/HitManDirector.h"
#include "audio/interfaces/cIGZSndEventStd.h"
#include "gz/cIGZSystemService.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZCOMDllDirector.h"

class cISC4AudioScapeManager;
class cSC4AudioListener;
namespace SC4Audio { class cSC4AudioAmbienceManager; }
class cSC4AudioSemManager;
class cSC4AudioSoundManager;

// The platform sound renderer's director: the 2D and 3D sound players and the renderer service.
class GZSndSysMacCOMDirector final : public cRZCOMDllDirector {
public:
    GZSndSysMacCOMDirector();
    std::uint32_t GetDirectorID() const override { return 0xe9d3a016; }
    bool OnStart(cIGZCOM* com) override;
};
cRZCOMDllDirector* GetGZSndSysMacCOMDirector();

// The Hit library's director. The hook bodies are sc4audio::cHitManCOMDirector's (audio/HitManDirector.cpp), which
// reaches the framework through the host seam this class implements.
class cHitManCOMDllDirector final : public cRZCOMDllDirector, private sc4audio::IHitDirectorHost {
public:
    cHitManCOMDllDirector();
    std::uint32_t GetDirectorID() const override { return sc4audio::kHitManDirectorId; }
    bool OnStart(cIGZCOM* com) override;
    bool PreAppInit() override { return hooks_.PreAppInit(); }
    bool PreAppShutdown() override { return hooks_.PreAppShutdown(); }

private:
    void AddClass(std::uint32_t) override {}   // the constructor registers the classes with their makers
    cIGZPersistResourceManager* ResourceManager() override;
    cIGZSndMgr* SoundManager() override;
    cIHitMan* HitManager() override;
    cIHitDebugInfo* DebugInfo() override;
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override;
    void RegisterSystemService(cIGZUnknown* service) override;
    void AddSystemServiceDirector(cIGZUnknown* director) override;

    sc4audio::cHitManCOMDirector hooks_;
};
cRZCOMDllDirector* GetHitManCOMDirector();

// The Freshness (adaptive score) library's director: five classes, nothing at start.
class cFreshnessCOMDirector final : public cRZCOMDllDirector {
public:
    cFreshnessCOMDirector();
    std::uint32_t GetDirectorID() const override;
};
cRZCOMDllDirector* GetFreshnessCOMDirector();

class cSC4AudioCOMDirector final : public cRZCOMDllDirector {
public:
    cSC4AudioCOMDirector();
    std::uint32_t GetDirectorID() const override { return 0xfdb5ca17; }
    bool OnStart(cIGZCOM* com) override;
    bool PostAppInit() override;
    bool PreAppShutdown() override;

    // SFX::Listener() / SFX::SoundManager() / SFX::AmbienceManager() of the original read these holders.
    SC4Audio::cSC4AudioAmbienceManager* AmbienceManager() const;
    cSC4AudioListener* Listener() const { return listener_.get(); }
    cSC4AudioSoundManager* SoundManager() const { return soundManager_.get(); }
    cISC4AudioScapeManager* ScapeManager() const { return scapeManager_.get(); }

private:
    cRZAutoRefCount<cIGZSndEventHandler> eventHandler_;        // +0x44 cSC4AudioEventHandler
    cRZAutoRefCount<cIGZSndEventHandler> fireHandler_;         // +0x48 cSC4AudioEventHandlerDisasterFire
    cRZAutoRefCount<cIGZSystemService> ambienceManager_;       // +0x4c cSC4AudioAmbienceManager (its primary view)
    cRZAutoRefCount<cSC4AudioSemManager> semManager_;          // +0x54
    cRZAutoRefCount<cSC4AudioSoundManager> soundManager_;      // +0x58
    cRZAutoRefCount<cSC4AudioListener> listener_;              // +0x5c
    cRZAutoRefCount<cISC4AudioScapeManager> scapeManager_;     // +0x60
};
cSC4AudioCOMDirector* GetSC4AudioCOMDirector();

namespace sc4audio {
// The services cHitIniFile reads the sound configuration into (the original reaches them through the framework).
void SetHitIniHost(IHitIniHost* host);
IHitIniHost* HitIniHost();
}  // namespace sc4audio
