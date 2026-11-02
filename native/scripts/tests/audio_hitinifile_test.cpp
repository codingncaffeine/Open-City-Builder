#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "audio/HitIniFile.h"
#include "hit_iface_stubs.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4audio;

namespace {

struct Mgr : cIGZSndMgrStub {
    std::uint32_t def2D = 0, def3D = 0;
    std::vector<std::pair<std::uint32_t, cIGZSndDataSourceCreator*>> creators;
    std::vector<cIGZSoundCreator*> soundCreators;
    bool SetDefaultSndDataSourcePlayer2DClassId(std::uint32_t id) override { def2D = id; return true; }
    bool SetDefaultSndDataSourcePlayer3DClassId(std::uint32_t id) override { def3D = id; return true; }
    bool RegisterSndDataSourceCreator(cIGZSndDataSourceCreator* c, std::uint32_t id) override { creators.push_back({id, c}); return true; }
    bool RegisterSndCreator(cIGZSoundCreator* c) override { soundCreators.push_back(c); return true; }
    bool GetSndDataSourceCreator(std::uint32_t id, std::uint32_t, void** out) override {
        for (auto& c : creators) if (c.first == id) { *out = c.second; c.second->AddRef(); return true; }
        return false;
    }
};

struct Sys : cIGZSndSysStub {
    cRZString directory{"/snd/"};
    std::vector<std::pair<std::string, std::uint32_t>> extensions;
    cIGZString* DataDirectory() override { return &directory; }
    bool SetExtensionDefaultDataSourceClassId(const cIGZString& ext, std::uint32_t id) override { extensions.push_back({ext.ToChar(), id}); return true; }
};

struct Hit : cIHitManStub {
    std::vector<std::string> loaded;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> groups;
    std::vector<std::uint32_t> segments;
    bool LoadHitFile(const cIGZString& path) override { loaded.push_back(path.ToChar()); return true; }
    bool AddTypeToGroupAssociation(std::uint32_t type, std::uint32_t group) override { groups.push_back({type, group}); return true; }
    void AddDBSegment(std::uint32_t id, cIGZPersistDBSegment*) override { segments.push_back(id); }
};

struct Res : cIGZPersistResourceManagerStub {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> factories;
    bool RegisterObjectFactory(std::uint32_t classId, std::uint32_t type, cIGZPersistResourceFactory*) override { factories.push_back({classId, type}); return true; }
};

struct DataSource : cIGZSndDataSourceCreator {
    std::vector<std::pair<std::uint32_t, std::string>> defs;
    int refs = 1;
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    bool CreateSndDataSource(std::uint32_t, std::uint32_t, cIGZVariant*, std::uint32_t, void**) override { return false; }
    bool AddSndDataSourceDef(std::uint32_t id, cIGZString& d) override { defs.push_back({id, d.ToChar()}); return true; }
    void RemoveSndDataSourceDef(std::uint32_t) override {}
};

struct SoundCreator : cIGZSoundCreator, cIGZSoundCreatorSoundDefRegistrar {
    int refs = 1;
    std::vector<std::pair<std::uint32_t, std::string>> defs;
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZSoundCreatorSoundDefRegistrar) return false;
        *out = static_cast<cIGZSoundCreatorSoundDefRegistrar*>(this);
        ++refs;
        return true;
    }
    bool CreateSound(std::uint32_t, std::uint32_t, cIGZVariant*, std::uint32_t, void**) override { return false; }
    bool AddSoundDef(std::uint32_t id, const cIGZString& d, std::uint32_t, std::uint32_t) override { defs.push_back({id, d.ToChar()}); return true; }
};

struct Host : IHitIniHost {
    Mgr mgr; Sys sys; Hit hit; Res res;
    DataSource dataSource; SoundCreator soundCreator;
    std::vector<std::uint32_t> created;
    cIGZSndMgr* SoundManager() override { return &mgr; }
    cIGZSndSys* SoundSystem() override { return &sys; }
    cIHitMan* HitManager() override { return &hit; }
    cIGZPersistResourceManager* ResourceManager() override { return &res; }
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override {
        created.push_back(classId);
        if (iid == kGZIID_cIGZSndDataSourceCreator) { *out = &dataSource; dataSource.AddRef(); return true; }
        if (iid == kGZIID_cIGZSoundCreator) { *out = static_cast<cIGZSoundCreator*>(&soundCreator); soundCreator.AddRef(); return true; }
        return false;
    }
    bool FileExists(const std::string&) override { return false; }
    bool InitPackedSegment(cIGZPersistDBSegment*, std::uint32_t, const std::string&, bool) override { return true; }
};

}  // namespace

int main() {
    char path[] = "/tmp/hitini_testXXXXXX";
    const int fd = mkstemp(path);
    const std::string text =
        "[Config]\nDefaultSndDataSourcePlayer2DClassId=0x10\nDefaultSndDataSourcePlayer3DClassId=0x20\nAllowTextAsKey=true\n"
        "[SndFileDefaultDataSourceClassIds]\n1=0x30,wav\n2=0x31,mp3\n"
        "[TrackDataBlocks]\n1=Sound\\Track.hit\n"
        "[HitManTypeGroups]\n0x500=0x1,0x2,0x3\n0x600=0x9\n"
        "[DBSegmentDirectoryFiles]\n0x900=0x7b:hls,0x20:str\n"   // no directory segment: the types still join the group
        "[SndDataSourceCreators]\nMySources=0x40,7\n"
        "[MySources]\n100=first def\n101=second def\n"
        "[SndCreators]\n0x50=Defs,\n0x51=Plain\n"          // the game's own line has no comma: the whole value is the section
        "[Defs]\n5=boom\n"
        "[Plain]\n6=bang\n"
        "[ResourceFactories]\n0x700=0x800\n";
    CHECK(write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    close(fd);

    Host host;
    auto* ini = new cHitIniFile(&host);
    ini->AddRef();
    CHECK(ini->DataDirectory() == "/snd/");
    ini->SetFilename(cRZString(path));
    CHECK(ini->EnumDefaultEntries());
    CHECK(host.mgr.def2D == 0x10 && host.mgr.def3D == 0x20 && ini->AllowTextAsKeys());
    CHECK(host.sys.extensions.size() == 2 && host.sys.extensions[0].first == "wav" && host.sys.extensions[0].second == 0x30);
    CHECK(host.hit.loaded.size() == 1 && host.hit.loaded[0] == "/snd/Sound/Track.hit");     // data dir + path, separators fixed
    // stage 4c (directory files) runs before the type groups: every "type:ext" member, the first one too
    CHECK(host.hit.groups.size() == 6 && host.hit.groups[0] == std::make_pair(0x7bu, 0x900u) && host.hit.groups[1] == std::make_pair(0x20u, 0x900u));
    CHECK(host.hit.groups.size() == 6 && host.hit.groups[2] == std::make_pair(0x500u, 1u) && host.hit.groups[4] == std::make_pair(0x500u, 3u) &&
          host.hit.groups[5] == std::make_pair(0x600u, 9u));
    CHECK(host.mgr.creators.size() == 1 && host.mgr.creators[0].first == 7);
    CHECK(ini->DataSourceCreatorSections().at(7).str() == "MySources");
    CHECK(host.dataSource.defs.size() == 2 && host.dataSource.defs[0].first == 100 && host.dataSource.defs[1].second == "second def");
    CHECK(host.mgr.soundCreators.size() == 2 && host.soundCreator.defs.size() == 2 && host.soundCreator.defs[0].second == "boom" &&
          host.soundCreator.defs[1].first == 6 && host.soundCreator.defs[1].second == "bang");
    CHECK(host.res.factories.size() == 1 && host.res.factories[0] == std::make_pair(0x800u, 0x700u));
    void* qi = nullptr;
    CHECK(ini->QueryInterface(kGZIID_cIHitIniFile, &qi) && qi == ini && !ini->QueryInterface(0x1234, &qi));
    ini->Release();
    ini->Release();
    unlink(path);
    std::printf(failures ? "FAILED\n" : "audio_hitinifile: all passed\n");
    return failures ? 1 : 0;
}
