// Host seams for the SimCity 4 audio layer (cSC4Audio*). The original reaches the rest of the
// game through service-locator globals (SFX::SoundManager(), SL::ViewUtilities(), SL::City(), ...);
// the port injects the few services the recovered code needs. Implemented by the game integration
// (see notes/audio-integration.md section 10).
#pragma once
#include <cstdint>

#include <vector>

#include "audio/interfaces/cIGZSndMgr.h"
#include "audio/interfaces/cIGZSndSys.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZUnknown.h"

namespace SC4Audio {

// The master volume (0..1024) kept by cSC4AudioSoundManager; the emanation models scale by it.
class cIMasterVolume : public cIGZUnknown {
public:
    virtual std::int32_t GetMasterVolume() = 0;
};

// SFX::SoundManager() @ 0x...: the registered sound manager, or null.
cIMasterVolume* SoundManager();
void SetSoundManager(cIMasterVolume* manager);

// SC4ViewUtilities as used by the 3D-with-effects model.
class cIViewport {
public:
    virtual ~cIViewport() = default;
    // Projects a world point (x, y, z) to viewport pixel coordinates in place (x, y written).
    virtual void ProjectPointToViewportCoordinates2D(float point[3]) = 0;
    virtual std::int32_t ViewportWidth() const = 0;    // +0x30
    virtual std::int32_t ViewportHeight() const = 0;   // +0x34
    // Camera eye and look-at point in world space (+0x44..0x4c and +0x50..0x58 of the original).
    virtual void GetCameraEye(float out[3]) const = 0;
    virtual void GetCameraTarget(float out[3]) const = 0;
    // Zoom level 0 (farthest) .. 4 (closest) at +0x14, and the camera up vector at +0x74.
    virtual std::int32_t GetZoomLevel() const { return 0; }
    virtual void GetCameraUp(float out[3]) const { out[0] = 0; out[1] = 1; out[2] = 0; }
};
cIViewport* ViewUtilities();   // SL::spViewUtilities
void SetViewUtilities(cIViewport* viewport);
// The game's answer when no viewport was set by hand: SL::spViewUtilities changes with every city, so the
// integration answers through a function (null outside a city).
void SetViewUtilitiesProvider(cIViewport* (*provider)());

// cSC4AudioListener::afZoomDistances @ 0x100b6ddf0: camera distance per zoom level (index 0..4),
// written by cSC4AudioListener::_CalculateZoomValues and read by the 3D sems and ambience layers.
float* ZoomDistances();

// --- Game-side services the sound manager and the sounds consult -----------------------------

// SL::spSC4->slot 0x130(): the player's audio options ({+4 enabled, +8 volume 0..1024}).
struct AudioPreferences {
    bool soundEnabled = false;
    std::uint32_t masterVolume = 0;
};

// SFX::Listener() (cSC4AudioListener, stored at +0xc0 of the audio COM director): camera eye at
// listener+0x10 and look-at target at +0x28.
struct ListenerView {
    float eye[3] = {0, 0, 0};
    float target[3] = {0, 0, 0};
};

class cIGameAudioContext {
public:
    virtual ~cIGameAudioContext() = default;
    virtual AudioPreferences GetAudioPreferences() = 0;
    // SL::spSimulator->slot 0x78 (IsPaused); false when there is no simulator.
    virtual bool IsSimulationPaused() = 0;
    // False when no listener exists (the original dereferences null in some of those paths).
    virtual bool GetListenerView(ListenerView& out) = 0;

    // World queries used by cSC4AudioListener::UpdateListener.
    // SL::OccupantManager() != null
    virtual bool HasOccupantManager() { return false; }
    // SL::OccupantManager()->slot 0x90(cityWidthCells, cityHeightCells, &x, &z): the city's extent
    // in metres.
    virtual void GetCityExtentMeters(float& x, float& z) { x = z = 0.0f; }
    // SL::Terrain()->GetSeaLevel() (slot 0xe8)
    virtual float GetSeaLevel() { return 0.0f; }
};

// SL::spRM + cISCPropertyHolder: read-only access to tuning exemplars.
class cIExemplarSource {
public:
    virtual ~cIExemplarSource() = default;
    virtual bool ExemplarExists(const cGZPersistResourceKey& key) = 0;
    virtual bool GetUint32Property(const cGZPersistResourceKey& key, std::uint32_t propertyId, std::uint32_t& out) = 0;
    virtual bool GetFloatProperty(const cGZPersistResourceKey& key, std::uint32_t propertyId, float& out) = 0;
    virtual bool GetFloatArrayProperty(const cGZPersistResourceKey& key, std::uint32_t propertyId, std::vector<float>& out) = 0;
};

// Non-owning pointers to the framework services the SC4 audio layer talks to. The original obtains
// them through cRZSysServPtr / RZGetFramework(); the integration fills these in before Init().
struct Services {
    cIGZSndSys* soundSystem = nullptr;             // service 0x23cecba0 / iid 0x106077d2
    cIGZSndMgr* soundManager = nullptr;            // service 0x1af85bfb / iid 0x3af85bb7
    cIGZMessageServer2* messageServer = nullptr;   // service 0x04fa845b / iid 0x652294c7
    cIGameAudioContext* game = nullptr;
    cIExemplarSource* exemplars = nullptr;
};
Services& GetServices();

}  // namespace SC4Audio
