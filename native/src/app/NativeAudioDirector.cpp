// The audio directors on the native application: what cGZSndSysMacCOMDirector, cHitManCOMDirector and
// cSC4AudioCOMDirector::PostAppInit do at start-up (0x100625020, 0x10043b11c, 0x1003e01ba). The services are created,
// registered with the framework, and the game's own sound .ini (SimCity_1.dat {0, 0x8a5971c5, 0xea8a1115}) configures them:
// the default sound players, the sample banks, the track lists and the hit manager's type groups.
#include "app/NativeAudioDirector.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/NativeUi.h"
#include "audio/AudioServices.h"
#include "audio/GZSnd.h"
#include "audio/GZSnd3D.h"
#include "audio/GZSndMgr.h"
#include "audio/SegmentDataSources.h"
#include "audio/GZSndSys.h"
#include "audio/GZSndSysMac.h"
#include "audio/HitChannel.h"
#include "audio/HitClassIds.h"
#include "audio/HitIniFile.h"
#include "audio/HitSegmentCreators.h"
#include "audio/HitMan.h"
#include "audio/HitManDirector.h"
#include "audio/HitManServices.h"
#include "audio/HitPropertySetResource.h"
#include "audio/HitResourceFactory.h"
#include "audio/HitTrackPlayer.h"
#include "gz/AudioPersistAdapter.h"
#include "gz/GZCOM.h"
#include "gz/GZFramework.h"
#include "gz/cGZAny.h"
#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistDBSegment.h"
#include "sc4/ServiceLocator.h"

namespace native {
namespace {

cGZFramework* g_framework = nullptr;
std::string g_dataDirectory;   // GameData/ with the trailing slash

// GZCOM()->CreateInstance through the generated class table; the object comes back with one reference.
bool CreateByClass(std::uint32_t classId, std::uint32_t iid, void** out) {
    // The classes the sound directors register (cGZSoundCOMDirector's table: 0xdc1f73d6 the segment data source creator,
    // 0xfded7f31 / 0xfded7f34 the 2D / 3D sound players) are not in the generated table.
    cIGZUnknown* unknown = nullptr;
    switch (classId) {
        case 0xdc1f73d6: unknown = cGZSndSegmentDataSourceCreator::Create(); break;
        case 0xfded7f31: unknown = static_cast<cIGZSnd*>(cGZSnd::Create()); break;
        case 0xfded7f34: unknown = static_cast<cIGZSnd*>(cGZSnd3D::Create()); break;
        case HitIds::kClsidHitMan: unknown = static_cast<cIHitMan*>(new cHitMan()); break;
        case HitIds::kClsidHitSegmentTrackCreator: unknown = static_cast<cIGZSoundCreator*>(new cHitSegmentTrackCreator()); break;
        case HitIds::kClsidHitSegmentDataSourceChannelCreator: unknown = static_cast<cIGZSoundCreator*>(new cHitSegmentDataSourceChannelCreator()); break;
        case HitIds::kClsidHitResourceFactory: unknown = static_cast<cIGZPersistResourceFactory*>(new sc4audio::cHitResourceFactory()); break;
        default: break;
    }
    if (!unknown) {
        const gzcom::ClassFactoryInfo* info = gzcom::FindClassFactory(classId);
        void* object = info && info->create ? info->create() : nullptr;
        if (!object) { if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: no class %08x\n", classId); return false; }
        unknown = static_cast<cIGZUnknown*>(object);
    }
    unknown->AddRef();
    const bool ok = unknown->QueryInterface(iid, out);
    unknown->Release();
    return ok;
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

// cHitManCOMDirector's framework side.
class HitDirectorHost final : public sc4audio::IHitDirectorHost {
public:
    void AddClass(std::uint32_t) override {}   // the generated class table already knows the Hit classes
    cIGZPersistResourceManager* ResourceManager() override { return SL::spRM; }
    cIGZSndMgr* SoundManager() override { return Audio::SoundManagerService(); }
    cIHitMan* HitManager() override { return hitMan; }
    cIHitDebugInfo* DebugInfo() override { return nullptr; }
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override { return CreateByClass(classId, iid, out); }
    // The framework is already running, so the service is initialised here as the other sound services are below
    // (cHitMan::Init makes it the hit manager instance the creators and the ini file reach).
    void RegisterSystemService(cIGZUnknown* service) override {
        cIGZSystemService* asService = nullptr;
        if (!service || !service->QueryInterface(kGZIID_cIGZSystemService, reinterpret_cast<void**>(&asService))) return;
        g_framework->AddSystemService(asService);
        asService->Init();
        asService->Release();
    }
    void AddSystemServiceDirector(cIGZUnknown*) override {}
    cIHitMan* hitMan = nullptr;
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

HitServices g_hitServices;
HitDirectorHost g_hitDirector;
HitIniHost g_iniHost;
AudioPersistAdapter* g_persist = nullptr;

}  // namespace

bool StartAudioDirectors(cGZFramework& framework, const std::string& dataDirectory) {
    g_framework = &framework;
    g_dataDirectory = dataDirectory;
    // The persistence layer the sound classes read their resources through is the game's own resource manager.
    if (!SL::spRM) return false;
    cGZPersistResourceManager::SetClassCreator(CreateByClass);   // the Hit resource factory, registered by class id
    g_persist = new AudioPersistAdapter(SL::spRM);
    Audio::SetPersistService(g_persist);
    Audio::SetMessageServer(SL::spMS2);
    Audio::SetFileSystemDataDirectory(dataDirectory);

    // Sound.dat (the samples and the track definitions) joins the game's resource manager.
    {
        auto* sounds = new cGZDBSegmentPackedFile();
        sounds->AddRef();
        sounds->Init();
        sounds->SetPath(cRZString((dataDirectory + "Sound.dat").c_str()));
        if (sounds->Open(true, false)) SL::spRM->RegisterDBSegment(*sounds);
        else std::printf("audio: cannot open Sound.dat\n");
    }
    // cGZSndSysMacCOMDirector / the sound services: the system, the manager and the platform renderer.
    auto* system = cGZSndSys::Create();
    system->AddRef();
    Audio::SetSoundSystem(system);
    system->SetDataDirectory(cRZString(dataDirectory.c_str()));
    framework.AddSystemService(system);
    system->Init();
    auto* manager = cGZSndMgr::Create();
    manager->AddRef();
    Audio::SetSoundManagerService(manager);
    framework.AddSystemService(manager);
    manager->Init();
    auto* renderer = cGZSndSysMac::Create();
    renderer->AddRef();
    framework.AddSystemService(renderer);
    renderer->Init();

    // cHitManCOMDirector: the hit manager and its services.
    Hit::SetServices(&g_hitServices);
    sc4audio::cHitManCOMDirector hitDirector(&g_hitDirector);
    hitDirector.OnStart(nullptr);
    g_hitDirector.hitMan = RZ::HitManInstance();
    if (g_hitDirector.hitMan && std::getenv("SC4_DEBUG"))
        g_hitDirector.hitMan->SetTrackFlowErrorHandler([](cIHitTrackPlayer* player, const char* message) {
            std::fprintf(stderr, "audio: track %p: %s\n", static_cast<void*>(player), message);
        });
    hitDirector.PreAppInit();

    // cSC4AudioCOMDirector::PostAppInit: the game's sound configuration.
    std::vector<std::uint8_t> bytes;
    if (!LoadResourceBytes(0, 0x8a5971c5, 0xea8a1115, bytes)) {
        std::printf("audio: sound .ini not found\n");
        return false;
    }
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp/") + "sc4_hit.ini";
    if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); } else return false;
    sc4audio::cHitIniFile ini(&g_iniHost);
    ini.SetFilename(cRZString(path.c_str()));
    const bool ok = ini.EnumDefaultEntries();
    std::printf("audio: sound .ini %s\n", ok ? "loaded" : "FAILED");
    return ok;
}

}  // namespace native
