// The game's audio on the native application. The game's own director (cSC4AudioCOMDirector, sc4audio/AudioCOMDirector.cpp)
// does the work: cSC4App::AddCOMDirectors adds it through the start-up hook set here, its OnStart adds the sound, renderer,
// Hit and Freshness directors and their services, and its PostAppInit reads the sound configuration and creates the radio,
// the sound manager, the listener, the soundscape and ambience managers and the two event handlers.
//
// The native boot runs the application's steps by hand instead of the framework's main loop, so the two framework phases
// the directors hook are run from here at the same points: AudioPreAppInit after the application's PreFrameWorkInit (the
// framework's own PreAppInit starts every service there; this starts the sound services the directors added, then runs
// the directors' PreAppInit), AudioPostAppInit after the application's PostFrameWorkInit (the directors' PostAppInit).
// The frame loop ticks the sound services (TickAudio), and AudioPreAppShutdown runs before the sound output closes.
//
// The rest of this file is what the sound classes ask the game for: the Hit library's services, the sound
// configuration's services, the game context of the sound manager and the listener (preferences, simulator pause, city
// extent, sea level), the tuning exemplars, the camera, and the event handler's neighbours.
#include "app/NativeAudioDirector.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app/GameAudio.h"   // CreatePlatformAudioBackend
#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "audio/AudioServices.h"
#include "audio/FreshHost.h"
#include "audio/HitClassIds.h"
#include "audio/interfaces/cIFreshPlayer.h"
#include "audio/GZSndSys.h"
#include "audio/GZSndSysMac.h"
#include "audio/HitChannel.h"
#include "audio/HitIniFile.h"
#include "audio/HitMan.h"
#include "audio/HitManServices.h"
#include "audio/HitPropertySetResource.h"
#include "audio/HitTrackPlayer.h"
#include "audio/interfaces/cIHitDebugInfo.h"
#include "audio/platform/AudioPlatform.h"
#include "gz/AudioPersistAdapter.h"
#include "gz/GZCOM.h"
#include "gz/GZCOMDirector.h"
#include "gz/GZFramework.h"
#include "gz/RZServices.h"
#include "gz/cGZAny.h"
#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistDBSegment.h"
#include "rz/COMDirectors.h"
#include "rz/Radio.h"
#include "rz/cRZSysServPtr.h"
#include "sc4/App.h"
#include "sc4/AudioAEMSSoundBank.h"
#include "sc4/AudioPreferencesWinProc.h"
#include "sc4/Exemplar.h"
#include "sc4/OccupantManager.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4audio/AudioAmbienceManager.h"
#include "sc4audio/AudioCOMDirector.h"
#include "sc4audio/AudioEventHandler.h"
#include "sc4audio/AudioScapeManager.h"
#include "sc4audio/Listener.h"
#include "sc4audio/SC4AudioHost.h"
#include "sc4audio/SoundManager.h"

namespace linuxapp { void AudioVoiceReport(); }   // app/linux/GameAudioSDL.cpp

namespace native {
void InstallAmbienceHosts();   // app/NativeAudioAmbience.cpp

namespace {

constexpr std::uint32_t kGZSID_HitMan = 0x1b2ce671;

bool g_installed = false;   // the director is behind the application's start-up hook
bool g_running = false;     // PostAppInit ran
std::string g_dataDirectory;   // GameData/ with the trailing slash
std::unique_ptr<AudioPlatform::IBackend> g_backend;
AudioPersistAdapter* g_persist = nullptr;

bool Trace() { static const bool on = std::getenv("SC4_AUDIODEBUG") != nullptr; return on; }

// GZCOM()->CreateInstance: the directors' classes, then the generated class table (classes whose director is not in the
// build); the object comes back with one reference.
bool CreateByClass(std::uint32_t classId, std::uint32_t iid, void** out) {
    if (cIGZCOM* com = GZCOM(); com && com->CreateInstance(classId, iid, out)) return true;
    const gzcom::ClassFactoryInfo* info = gzcom::FindClassFactory(classId);
    void* object = info && info->create ? info->create() : nullptr;
    if (!object) { if (Trace()) std::fprintf(stderr, "audio: no class %08x\n", classId); return false; }
    auto* unknown = static_cast<cIGZUnknown*>(object);
    unknown->AddRef();
    const bool ok = unknown->QueryInterface(iid, out);
    unknown->Release();
    return ok;
}

// The resource manager creates a factory registered by class id through GZCOM. The native boot registers the game's
// other resource factories with their objects (RegisterExemplarFactories), so only the audio directors' classes are
// answered here: the Hit resource factory and what the sound configuration's [ResourceFactories] names.
bool CreateAudioFactoryByClass(std::uint32_t classId, std::uint32_t iid, void** out) {
    for (cRZCOMDllDirector* director : {static_cast<cRZCOMDllDirector*>(GetSC4AudioCOMDirector()), gz::GetGZSoundCOMDirector(),
                                        GetGZSndSysMacCOMDirector(), GetHitManCOMDirector(), GetFreshnessCOMDirector()})
        if (director->GetClassObject(classId, iid, out)) return true;
    return false;
}

// The services of the hit manager (Hit::cIHitManServices): the sound system, the sound manager, the resource manager
// and the file system, one method per original call.
class HitServices final : public Hit::cIHitManServices {
public:
    bool HasSoundSystem() const override { return Audio::SoundSystem() != nullptr; }
    void EnumerateUpdateTargets(EnumCallback callback, std::uint32_t a, void* b) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->EnumerateTargets(callback, a, b);
    }
    bool GetPersistResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out) override {
        cIGZPersistRecordService* persist = Audio::PersistService();
        return persist && persist->GetResource(key, iid, out, 0, 0);
    }
    cIGZUnknown* CreatePropertySetResource(const cGZPersistResourceKey& key, const Hit::DefaultProperty* defaults,
                                           std::size_t count, bool trackResource) override {
        cGZPropertySet* set = nullptr;
        cIGZUnknown* unknown = nullptr;
        if (trackResource) {
            auto* resource = new cHitPropertySetTrackResource(key);
            set = resource;
            unknown = static_cast<cIGZUnknown*>(static_cast<cGZPropertySet*>(resource));
        } else {
            auto* resource = new cHitPropertySetResource(key);
            set = resource;
            unknown = static_cast<cIGZUnknown*>(static_cast<cGZPropertySet*>(resource));
        }
        unknown->AddRef();
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t id = defaults[i].id;
            set->SetValue(id, AnyUint32(static_cast<std::uint32_t>(defaults[i].value)));
        }
        if (cIGZPersistRecordService* persist = Audio::PersistService()) persist->RegisterResource(unknown);
        return unknown;
    }
    // Through the game's resource manager (Sound.dat's track program lives there), else the game's packages.
    bool ReadPersistRecord(const cGZPersistResourceKey& key, std::vector<std::uint8_t>& bytes, std::string& name) override {
        bool read = false;
        cIGZPersistDBSegment* segment = nullptr;
        if (SL::spRM && SL::spRM->FindDBSegment(key, &segment) && segment) {
            const std::uint32_t size = segment->GetRecordSize(key);
            if (size != 0xffffffffu) {
                bytes.assign(size, 0);
                std::uint32_t got = size;
                read = segment->ReadRecord(key, bytes.data(), got) != 0xffffffffu && got == size;
            }
            segment->Release();
        }
        if (!read && !LoadResourceBytes(key.type, key.group, key.instance, bytes)) return false;
        char text[64];
        std::snprintf(text, sizeof text, "%08x-%08x-%08x", key.type, key.group, key.instance);
        name = text;
        return true;
    }
    bool GetDefaultSndPlayerClassId(bool positional3D, std::uint32_t& classId) override {
        cIGZSndMgr* manager = Audio::SoundManagerService();
        if (!manager) return false;
        return positional3D ? manager->GetDefaultSndDataSourcePlayer3DClassId(classId) : manager->GetDefaultSndDataSourcePlayer2DClassId(classId);
    }
    bool CreateComInstance(std::uint32_t classId, std::uint32_t iid, void** out) override { return CreateByClass(classId, iid, out); }
    bool GetSoundById(std::uint32_t soundId, std::uint32_t iid, void** out) override {
        cIGZSndMgr* manager = Audio::SoundManagerService();
        return manager && manager->GetSoundById(soundId, 0, nullptr, iid, out);
    }
    void RegisterUpdateTarget(cIGZSndUpdateTarget* target, std::uint32_t intervalMs) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->RegisterTarget(target, intervalMs);
    }
    void UnregisterUpdateTarget(cIGZSndUpdateTarget* target) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->UnregisterTarget(target);
    }
    bool SetGlobalProperty(std::uint32_t id, const cIGZAny& value) override {
        cIGZSndSys* system = Audio::SoundSystem();
        cIGZSndPropertyCache* cache = system ? system->GetSndPropertyCache() : nullptr;
        return cache && cache->SetGlobalProperty(id, value);
    }
    bool GetGlobalProperty(std::uint32_t id, cIGZAny* value) override {
        cIGZSndSys* system = Audio::SoundSystem();
        cIGZSndPropertyCache* cache = system ? system->GetSndPropertyCache() : nullptr;
        return cache && cache->GetGlobalProperty(id, value);
    }
    bool SetInstanceProperty(std::uint32_t instance, std::uint32_t id, const cIGZAny& value) override {
        cIGZSndSys* system = Audio::SoundSystem();
        cIGZSndPropertyCache* cache = system ? system->GetSndPropertyCache() : nullptr;
        return cache && cache->SetInstanceProperty(instance, id, value);
    }
    bool ReadFileBytes(const std::string& path, std::vector<std::uint8_t>& bytes) override {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        bytes.resize(static_cast<std::size_t>(std::ftell(file)));
        std::fseek(file, 0, SEEK_SET);
        const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        return got == bytes.size();
    }
    std::string ApplicationDirectory() const override { return g_dataDirectory; }
    std::string DataDirectory() const override { return g_dataDirectory; }
    void ParseDebugInfo(const cIGZString&, const std::uint8_t*, std::uint32_t) override {}
    bool CreateTrackPlayer(std::uint32_t iid, void** out) override {
        auto* player = new cHitTrackPlayer();
        player->AddRef();
        const bool ok = player->QueryInterface(iid, out);
        player->Release();
        return ok;
    }
    bool CreateChannel(void** outSnd) override {
        auto* channel = new cHitChannel();
        channel->Init();
        channel->AddRef();
        const bool ok = channel->QueryInterface(0x206077d2, outSnd);
        channel->Release();
        return ok;
    }
};

// The services of cHitIniFile::EnumDefaultEntries.
class HitIniHost final : public sc4audio::IHitIniHost {
public:
    cIGZSndMgr* SoundManager() override { return Audio::SoundManagerService(); }
    cIGZSndSys* SoundSystem() override { return Audio::SoundSystem(); }
    cIHitMan* HitManager() override { return RZ::HitManInstance(); }
    cIGZPersistResourceManager* ResourceManager() override { return SL::spRM; }
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override { return CreateByClass(classId, iid, out); }
    bool FileExists(const std::string& path) override {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (file) std::fclose(file);
        return file != nullptr;
    }
    bool InitPackedSegment(cIGZPersistDBSegment* segment, std::uint32_t segmentId, const std::string& path, bool flag) override {
        const cRZString text(path.c_str());
        auto* packed = dynamic_cast<cGZDBSegmentPackedFile*>(segment);
        return packed && packed->Init(segmentId, &text, flag);
    }
};

// The Freshness library's services (Fresh::cIFreshHost): the sound manager's sounds by name and by id, the sound
// system's update targets, and a score's text read from its record.
class FreshHost final : public Fresh::cIFreshHost {
public:
    bool HasSoundSystem() const override { return Audio::SoundSystem() != nullptr; }
    cIGZSnd* CreateSndByFileName(const cIGZString& fileName) override {
        cIGZSndMgr* manager = Audio::SoundManagerService();
        cRZString name(fileName.ToChar());
        void* out = nullptr;
        return manager && manager->GetSoundByName(name, 0, nullptr, kGZIID_cIGZSnd, &out) ? static_cast<cIGZSnd*>(out) : nullptr;
    }
    cIGZSnd* CreateSndByResourceId(std::uint32_t resourceId) override {
        cIGZSndMgr* manager = Audio::SoundManagerService();
        void* out = nullptr;
        return manager && manager->GetSoundById(resourceId, 0, nullptr, kGZIID_cIGZSnd, &out) ? static_cast<cIGZSnd*>(out) : nullptr;
    }
    bool IsSoundSystemInTestMode() const override { cIGZSndSys* system = Audio::SoundSystem(); return system && system->GetTestMode(); }
    cIFreshPlayer* CreateFreshPlayerInstance() override {
        void* out = nullptr;
        return CreateByClass(HitIds::kClsidFreshPlayer, kGZIID_cIFreshPlayer, &out) ? static_cast<cIFreshPlayer*>(out) : nullptr;
    }
    void AddUpdateTarget(cIGZSndUpdateTarget* target, std::uint32_t intervalMs) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->RegisterTarget(target, intervalMs);
    }
    void RemoveUpdateTarget(cIGZSndUpdateTarget* target) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->UnregisterTarget(target);
    }
    bool ReadScoreText(const cGZPersistResourceKey& key, std::string& outText) override {
        cIGZPersistDBSegment* segment = nullptr;
        if (!SL::spRM || !SL::spRM->FindDBSegment(key, &segment) || !segment) return false;
        bool read = false;
        const std::uint32_t size = segment->GetRecordSize(key);
        if (size != 0xffffffffu) {
            outText.assign(size, '\0');
            std::uint32_t got = size;
            read = segment->ReadRecord(key, outText.data(), got) != 0xffffffffu && got == size;
        }
        segment->Release();
        return read;
    }
};

sc4::cSTETerrain* Terrain() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}

sc4::cISC4OccupantManager* Occupants() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
    manager->Release();   // borrowed: the city keeps it
    return manager;
}

// What the sound manager and the listener ask the game (SL::spSC4 vslot 0x130, SL::spSimulator vslot 0x78, SFX::Listener(),
// SL::spOccupantManager vslot 0x90, SL::spTerrain vslot 0xe8).
class GameContext final : public SC4Audio::cIGameAudioContext {
public:
    SC4Audio::AudioPreferences GetAudioPreferences() override {
        SC4Audio::AudioPreferences out;
        if (const std::uint8_t* preferences = SL::spPreferences) {
            out.soundEnabled = preferences[sc4::prefs::kAudioEnabled] != 0;
            std::memcpy(&out.masterVolume, preferences + sc4audio::kPrefsEffectsVolume, sizeof out.masterVolume);
        }
        return out;
    }
    bool IsSimulationPaused() override { sc4::cSC4Simulator* simulator = CitySimulatorObject(); return simulator && simulator->IsPaused(); }
    bool GetListenerView(SC4Audio::ListenerView& out) override {
        cSC4AudioListener* listener = GetSC4AudioCOMDirector()->Listener();
        if (!listener) return false;
        out = listener->View();
        return true;
    }
    bool HasOccupantManager() override { return Occupants() != nullptr; }
    void GetCityExtentMeters(float& x, float& z) override {
        sc4::cSC4City* city = HostCity();
        x = city ? static_cast<float>(city->CellCountX()) * 16.0f : 0.0f;
        z = city ? static_cast<float>(city->CellCountZ()) * 16.0f : 0.0f;
    }
    float GetSeaLevel() override { sc4::cSTETerrain* terrain = Terrain(); return terrain ? terrain->GetSeaLevel() : 0.0f; }
};

// The tuning exemplars (SL::spRM->GetResource(key, 0xa52160f5) and cSCPropertyHelp::GetPropertyValue).
class ExemplarSource final : public SC4Audio::cIExemplarSource {
public:
    bool ExemplarExists(const cGZPersistResourceKey& key) override { Holder holder(key); return holder.properties != nullptr; }
    bool GetUint32Property(const cGZPersistResourceKey& key, std::uint32_t propertyId, std::uint32_t& out) override {
        Holder holder(key);
        return holder.properties && sc4::cSCPropertyHelp::GetPropertyValue(holder.properties, propertyId, out);
    }
    bool GetFloatProperty(const cGZPersistResourceKey& key, std::uint32_t propertyId, float& out) override {
        Holder holder(key);
        return holder.properties && sc4::cSCPropertyHelp::GetPropertyValue(holder.properties, propertyId, out);
    }
    bool GetFloatArrayProperty(const cGZPersistResourceKey& key, std::uint32_t propertyId, std::vector<float>& out) override {
        Holder holder(key);
        return holder.properties && sc4::cSCPropertyHelp::GetPropertyValue(holder.properties, propertyId, out);
    }

private:
    struct Holder {
        explicit Holder(const cGZPersistResourceKey& key) {
            void* raw = nullptr;
            if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return;
            exemplar = static_cast<sc4::cISCResExemplar*>(raw);
            properties = exemplar->AsISCPropertyHolder();
        }
        ~Holder() { if (exemplar) exemplar->Release(); }
        sc4::cISCResExemplar* exemplar = nullptr;
        sc4::cISCPropertyHolder* properties = nullptr;
    };
};

// SL::spViewUtilities as the sound classes use it.
class Viewport final : public SC4Audio::cIViewport {
public:
    void ProjectPointToViewportCoordinates2D(float point[3]) override { SL::spViewUtilities->ProjectPointToViewportCoordinates2D(point); }
    std::int32_t ViewportWidth() const override { return SL::spViewUtilities->ViewportWidth(); }
    std::int32_t ViewportHeight() const override { return SL::spViewUtilities->ViewportHeight(); }
    void GetCameraEye(float out[3]) const override { Copy(SL::spViewUtilities->EyePosition(), out); }
    void GetCameraTarget(float out[3]) const override { Copy(SL::spViewUtilities->LookAtPosition(), out); }
    std::int32_t GetZoomLevel() const override { return SL::spViewUtilities->Zoom(); }
    void GetCameraUp(float out[3]) const override { Copy(SL::spViewUtilities->UpVector(), out); }

private:
    static void Copy(const cS3DVector3& from, float out[3]) { out[0] = from.x; out[1] = from.y; out[2] = from.z; }
};
Viewport g_viewport;
SC4Audio::cIViewport* CurrentViewport() { return SL::spViewUtilities ? &g_viewport : nullptr; }

// The event handler's neighbours: the Audio Options dialog's statics, the city, the occupants.
class EventHandlerHost final : public sc4audio::IAudioEventHandlerHost {
public:
    using Dialog = sc4::cSC4AudioPreferencesWinProc;
    void PreferencesStaticInit() override { Dialog::StaticInit(); }
    std::uint32_t FadeOutTimeMs() override { return Dialog::FadeOutTimeMs(); }
    std::uint32_t VolumeFromSliderValue(std::uint32_t sliderValue, AudioType type) override {
        return Dialog::GetVolumeFromSliderValue(sliderValue, type == kAudioTypeMusic ? Dialog::kAudioTypeMusic : Dialog::kAudioTypeSound);
    }
    void SetDefaultMusicListPreferences() override { Dialog::SetDefaultMusicListPreferences(); }
    void UpdateExclusionList(AudioMode mode) override { Dialog::UpdateExclusionList(mode); }
    bool CityEstablished() override { sc4::cSC4City* city = HostCity(); return city && city->GetEstablished(); }
    // A network occupant (it answers 0xa821ef94) by its occupant view's position.
    bool NetworkOccupantPosition(cIGZUnknown* networkOccupant, float* xyz) override {
        void* network = nullptr;
        if (!networkOccupant || !networkOccupant->QueryInterface(sc4audio::kGZIID_cISC4NetworkOccupantAudio, &network) || !network) return false;
        static_cast<cIGZUnknown*>(network)->Release();
        void* raw = nullptr;
        if (!networkOccupant->QueryInterface(sc4audio::kGZIID_cISC4OccupantAudio, &raw) || !raw) return false;
        auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
        occupant->GetPosition(xyz);
        occupant->Release();
        return true;
    }
    bool IterateOccupants(OccupantCallback callback, void* context, sc4::cISC4OccupantFilter* filter) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        return occupants && occupants->IterateOccupants(callback, context, nullptr, nullptr, filter);
    }
};

HitServices g_hitServices;
HitIniHost g_iniHost;
FreshHost g_freshHost;
GameContext g_gameContext;
ExemplarSource g_exemplars;
EventHandlerHost g_eventHost;

template <class T> T* Service(cGZFramework& framework, std::uint32_t serviceId, std::uint32_t iid) {
    void* found = nullptr;
    if (!framework.GetSystemService(serviceId, iid, &found) || !found) return nullptr;
    auto* service = static_cast<T*>(found);
    service->Release();   // borrowed: the framework keeps its services
    return service;
}

// The services the framework ticks for the sound: the radio (tick and idle), the soundscape manager and the ambience
// manager (tick, while they are registered).
constexpr std::uint32_t kTickedServices[] = {kGZSID_cRZRadio, kGZSID_cISC4AudioScapeManager, 0xaa12f911};

}  // namespace

void InstallAudioDirector(cSC4App& app, const std::string& dataDirectory) {
    g_backend.reset(CreatePlatformAudioBackend());
    if (!g_backend) { std::printf("audio: no sound output; the game runs silent\n"); return; }
    AudioPlatform::SetBackend(g_backend.get());
    g_dataDirectory = dataDirectory;
    Hit::SetServices(&g_hitServices);
    sc4audio::SetHitIniHost(&g_iniHost);
    Fresh::SetHost(&g_freshHost);
    cGZPersistResourceManager::SetClassCreator(CreateAudioFactoryByClass);   // the Hit resource factory, registered by class id
    SC4Audio::GetServices().game = &g_gameContext;
    SC4Audio::GetServices().exemplars = &g_exemplars;
    SC4Audio::SetViewUtilitiesProvider(&CurrentViewport);
    sc4audio::SetAudioEventHandlerHost(&g_eventHost);
    InstallAmbienceHosts();
    app.StartupHooks().audioDirector = []() -> cRZCOMDllDirector* { return GetSC4AudioCOMDirector(); };
    g_installed = true;
}

void AudioPreAppInit(cGZFramework& framework) {
    if (!g_installed) return;
    cIGZSndSys* system = Service<cIGZSndSys>(framework, kGZSID_cGZSndSys, kGZIID_cIGZSndSys);
    cIGZSndMgr* manager = Service<cIGZSndMgr>(framework, kGZSID_cGZSndMgr, kGZIID_cIGZSndMgr);
    if (!system || !manager) {   // the player's audio option is off (the application did not add the director)
        g_installed = false;
        return;
    }
    // The services the sound classes of this build look up through their own seam (audio/AudioServices.h).
    g_persist = new AudioPersistAdapter(SL::spRM);
    Audio::SetPersistService(g_persist);
    Audio::SetMessageServer(RZ::MS2());
    Audio::SetFileSystemDataDirectory(g_dataDirectory);
    Audio::SetSoundSystem(system);
    Audio::SetSoundManagerService(manager);
    system->SetDataDirectory(cRZString(g_dataDirectory.c_str()));

    // The framework's PreAppInit for the services the directors added: Init by falling priority, then running.
    std::vector<cIGZSystemService*> services;
    for (const std::uint32_t id : {kGZSID_cGZSndSys, kGZSID_cGZSndMgr, cGZSndSysMac::kServiceId, kGZSID_HitMan, kGZSID_cHitDebugInfo})
        if (auto* service = Service<cIGZSystemService>(framework, id, kGZIID_cIGZSystemService)) services.push_back(service);
    std::stable_sort(services.begin(), services.end(),
                     [](cIGZSystemService* a, cIGZSystemService* b) { return a->GetServicePriority() > b->GetServicePriority(); });
    for (cIGZSystemService* service : services) {
        const bool ok = service->Init();
        if (ok) service->SetServiceRunning(true);
        if (Trace()) std::fprintf(stderr, "audio: service %08x priority %d Init %d\n", service->GetServiceID(), service->GetServicePriority(), int(ok));
    }
    if (cHitMan* hitMan = RZ::HitManInstance(); hitMan && Trace())
        hitMan->SetTrackFlowErrorHandler([](cIHitTrackPlayer* player, const char* message) {
            std::fprintf(stderr, "audio: track %p: %s\n", static_cast<void*>(player), message);
        });
    framework.HookPreAppInit();
}

bool AudioPostAppInit(cGZFramework& framework) {
    if (!g_installed) return false;
    // Sound.dat (the samples and the track definitions) joins the game's resource manager.
    {
        auto* sounds = new cGZDBSegmentPackedFile();
        sounds->AddRef();
        sounds->Init();
        sounds->SetPath(cRZString((g_dataDirectory + "Sound.dat").c_str()));
        if (sounds->Open(true, false)) SL::spRM->RegisterDBSegment(*sounds);
        else std::printf("audio: cannot open Sound.dat\n");
    }
    SC4Audio::Services& services = SC4Audio::GetServices();
    services.soundSystem = Audio::SoundSystem();
    services.soundManager = Audio::SoundManagerService();
    services.messageServer = RZ::MS2();
    framework.HookPostAppInit();
    g_running = true;

    cSC4AudioCOMDirector* director = GetSC4AudioCOMDirector();
    cRZSysServPtr<cIGZRadio, kGZSID_cRZRadio, kGZIID_cIGZRadio> radio;
    const bool ok = director->SoundManager() && director->Listener() && director->ScapeManager() && radio;
    std::printf("audio: the game's sound services %s (radio %s, %u stations)\n", ok ? "started" : "did NOT all start",
                radio ? "on the air" : "missing", radio ? radio->GetStationCount() : 0u);
    return ok;
}

void TickAudio() {
    if (!g_running) return;
    if (cGZFramework* framework = cGZFramework::FrameWork()) framework->TickServices(kTickedServices, std::size(kTickedServices));
}

// The directors' PreAppShutdown, then the framework's shutdown of the services they added (rising priority), and the
// sound output last: its voices are streams of the platform's audio device.
void AudioPreAppShutdown() {
    if (!g_running) return;
    g_running = false;
    cGZFramework* framework = cGZFramework::FrameWork();
    if (!framework) return;
    GetSC4AudioCOMDirector()->PreAppShutdown();
    GetHitManCOMDirector()->PreAppShutdown();
    gz::GetGZSoundCOMDirector()->PreAppShutdown();
    std::vector<cIGZSystemService*> services;
    for (const std::uint32_t id : {kGZSID_cRZRadio, kGZSID_cGZSndSys, kGZSID_cGZSndMgr, cGZSndSysMac::kServiceId, kGZSID_HitMan, kGZSID_cHitDebugInfo})
        if (auto* service = Service<cIGZSystemService>(*framework, id, kGZIID_cIGZSystemService)) services.push_back(service);
    std::stable_sort(services.begin(), services.end(),
                     [](cIGZSystemService* a, cIGZSystemService* b) { return a->GetServicePriority() < b->GetServicePriority(); });
    for (cIGZSystemService* service : services) {
        service->Shutdown();
        service->SetServiceRunning(false);
    }
    AudioPlatform::SetBackend(nullptr);
    g_backend.reset();
}

bool AudioRunning() { return g_running; }

// The AEMS sound banks read their records as files: SC4GetResourceAsFile.
void InstallSoundBankHost() {
    static struct final : sc4::ISoundBankHost {
        bool GetResourceAsFile(const cGZPersistResourceKey& key, cIGZFile** out, bool flag) override { return sc4::SC4GetResourceAsFile(key, out, flag); }
    } host;
    sc4::SetSoundBankHost(&host);
}

// Kit command `audio`: one line each for the radio, the sound manager and the listener.
void NativeAudioReport() {
    if (!g_running) { std::printf("audio: not running\n"); return; }
    cSC4AudioCOMDirector* director = GetSC4AudioCOMDirector();
    cRZSysServPtr<cIGZRadio, kGZSID_cRZRadio, kGZIID_cIGZRadio> radio;
    if (radio) {
        char callSign[kRZRadioCallSignBufferSize] = {};
        const bool tuned = radio->GetCurrentStation(callSign);
        callSign[kRZRadioCallSignLength] = 0;
        cRZString song;
        const bool hasSong = radio->GetCurrentSong(song);
        const std::string name = hasSong ? song.str().substr(song.str().find_last_of('/') + 1) : std::string();
        std::printf("audio: radio on %d volume %d stations %u station \"%s\" playing %d song \"%s\"\n", int(radio->IsOn()), radio->GetVolume(),
                    radio->GetStationCount(), tuned ? callSign : "", int(radio->IsPlaying()), name.c_str());
        if (Trace()) {
            cRZString directory;
            radio->GetRadioDirectory(directory);
            std::printf("audio: radio directory \"%s\" file system %d\n", directory.ToChar(), int(RZ::FS() != nullptr));
        }
    } else {
        std::printf("audio: no radio\n");
    }
    if (cSC4AudioSoundManager* sounds = director->SoundManager())
        std::printf("audio: sound manager master %u sounds %zu (looping %d, one-shot %d) max per sound %u\n", sounds->GetMasterVolume(),
                    sounds->SoundCount(), sounds->masterSoundCount(), sounds->nonMasterSoundCount(), sounds->maxInstancesPerSound());
    if (cSC4AudioSoundManager* sounds = director->SoundManager(); sounds && Trace())
        for (const tSound* sound : sounds->Sounds())
            std::printf("audio:   sound %08x instance %08x volume %d flags %x pauses %d playing %d at %.0f %.0f %.0f\n", sound->soundId(),
                        sound->instanceId(), sound->volume(), sound->flags(), sound->pauseCount(), int(sound->snd() && sound->snd()->IsPlaying()),
                        sound->position()[0], sound->position()[1], sound->position()[2]);
    if (Trace()) linuxapp::AudioVoiceReport();
    if (cSC4AudioListener* listener = director->Listener())
        std::printf("audio: listener eye %.0f %.0f %.0f target %.0f %.0f %.0f\n", listener->eye()[0], listener->eye()[1], listener->eye()[2],
                    listener->target()[0], listener->target()[1], listener->target()[2]);
}

}  // namespace native
