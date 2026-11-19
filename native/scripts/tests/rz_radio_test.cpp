// cRZRadio: options/settings, volume and the mute counter, the station list and call-sign lookups, the disk-station
// scan (SetupDiskStations over real temporary directories), Init/Shutdown against a fake framework, and the sound
// events (cGZSndEventStd) DoEvent answers.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <string>
#include <vector>

#include "audio/GZSndEventStd.h"
#include "audio/interfaces/cIGZSndDataSourceHolder.h"
#include "audio/interfaces/cIGZSndMgr.h"
#include "audio/interfaces/cIGZSndSys.h"
#include "gz/FileSystem.h"
#include "gz/GZCOMDirector.h"
#include "rz/LanguageManager.h"
#include "rz/Radio.h"
#include "rz/cRZSysServPtr.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "hit_iface_stubs.h"
#pragma clang diagnostic pop

// The object layout of the binary (CreatecRZRadio allocates 0xf0 bytes; the settings blocks are 0x20 and 8 bytes).
static_assert(sizeof(cRZRadio) == 0xf0, "cRZRadio is 0xf0 bytes");
static_assert(sizeof(cIGZRadio::RadioSettings) == 0x20, "RadioSettings is 0x20 bytes");
static_assert(sizeof(cIGZRadio::FadeOptions) == 0x08, "FadeOptions is 8 bytes");

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

// ---------------------------------------------------------------- fakes

// The concrete station (cRZDiskRadioStation) as far as cRZRadio can see it: the interface slots it calls plus the
// directory strings and the "cycle started" byte the host exposes.
struct FakeStation final : cIGZRadioStation {
    std::uint32_t refs = 0, addRefs = 0, releases = 0;
    cRZString stationDirectory, musicDirectory, djDirectory, commercialDirectory, jingleDirectory;
    std::string callSign = "????";
    int startedFlag = 0;
    int setRadioCalls = 0, setStationCalls = 0, unsetStationCalls = 0, onIdleCalls = 0, readCallSignCalls = 0;
    int nextSelectionCalls = 0, lastCycleType = -1, shuffleStrategy = -1, playlistMode = -1;
    cIGZRadio* radio = nullptr;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown) {
            *out = this;
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { ++refs; ++addRefs; return refs; }
    std::uint32_t Release() override { ++releases; if (refs) --refs; return refs; }   // stays inspectable
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool OnIdle() override { ++onIdleCalls; return true; }
    void SetRadio(cIGZRadio* value) override { radio = value; ++setRadioCalls; }
    void SetStation() override { ++setStationCalls; }
    void UnsetStation() override { ++unsetStationCalls; }
    void GetStationCallSign(char* out) override { std::memcpy(out, callSign.data(), kRZRadioCallSignLength); }
    void GetStationDescription(cIGZString* out) override { out->AssignFromCString("description"); }
    bool IsOnTheAir() override { return true; }
    void SetPlaylistMode(std::int32_t mode) override { playlistMode = mode; }
    void SetShuffleStrategy(std::int32_t strategy) override { shuffleStrategy = strategy; }
    void SetModifierList(std::list<cRZString>*) override {}
    bool LoadAllPlaylists() override { return true; }
    bool ShufflePlaylist(std::int32_t) override { return true; }
    bool ShufflePlayList(const cRZString&, std::list<cRZString>&, std::int32_t) override { return true; }
    void ShuffleCycle() override {}
    bool IsCycleTypePresent(std::int32_t) override { return true; }
    bool MoveToNextPlaylistItem(std::int32_t) override { return true; }
    bool ReadCallSignFromDisk() override { ++readCallSignCalls; return true; }
    bool StartPlayingNextSelectionOfGivenType(std::int32_t cycleType) override {
        ++nextSelectionCalls;
        lastCycleType = cycleType;
        return true;
    }
    bool PlayWelcome() override { return true; }
    bool ExpandPlayLists(cRZString&, std::list<cRZString>&) override { return true; }
    void PlayNextSelectionNotOnExclusionList() override {}
};

struct FakeStationHost final : IRadioStationHost {
    std::vector<FakeStation*> created;
    std::vector<FakeStation*> all;   // every station ever created (created is cleared mid-test); main deletes them at the end
    cIGZRadioStation* CreateDiskStation() override {
        auto* station = new FakeStation();
        created.push_back(station);
        all.push_back(station);
        return station;
    }
    cRZString& StationDirectory(cIGZRadioStation* s) override { return static_cast<FakeStation*>(s)->stationDirectory; }
    cRZString& MusicDirectory(cIGZRadioStation* s) override { return static_cast<FakeStation*>(s)->musicDirectory; }
    cRZString& DjDirectory(cIGZRadioStation* s) override { return static_cast<FakeStation*>(s)->djDirectory; }
    cRZString& CommercialDirectory(cIGZRadioStation* s) override {
        return static_cast<FakeStation*>(s)->commercialDirectory;
    }
    cRZString& JingleDirectory(cIGZRadioStation* s) override { return static_cast<FakeStation*>(s)->jingleDirectory; }
    void ClearStartedFlag(cIGZRadioStation* s) override { static_cast<FakeStation*>(s)->startedFlag = 0; }
};

struct FakeRadioHost final : IRadioHost {
    bool usingITunes = false, itunesPlaying = false, readyToHearMusic = false;
    int stopCalls = 0, playlistCalls = 0;
    bool UsingITunes() override { return usingITunes; }
    bool ITunesIsPlaying() override { return itunesPlaying; }
    void ITunesStop() override { ++stopCalls; itunesPlaying = false; }
    void ITunesPlayPlaylist() override { ++playlistCalls; itunesPlaying = true; }
    bool ReadyToHearMusic() override { return readyToHearMusic; }
};

// cIGZSnd: plays one file, remembers volume/fades and answers the data-source-holder query PlaySong_Internal makes.
struct FakeSnd final : cIGZSnd, cIGZSndDataSourceHolder {
    std::uint32_t refs = 1;
    std::string soundFile;
    std::int32_t streamType = -1;
    std::int32_t volume = 0;
    bool playing = false, fading = false;
    std::int32_t pan = 0, lastFadeFrom = 0, lastFadeTo = 0, lastFadeDuration = 0;
    bool fadeStarted = false;
    int stopCalls = 0, playCalls = 0, holderQueries = 0;
    bool dataSourceCleared = false;
    std::int32_t dataSourceStreaming = -1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZSnd) {
            *out = static_cast<cIGZSnd*>(this);
            AddRef();
            return true;
        }
        if (iid == 0x09d002d2) {
            ++holderQueries;
            *out = static_cast<cIGZSndDataSourceHolder*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }   // stays inspectable
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool PlayWithVolumeAndLoop(std::int32_t value, bool, SndDup) override {
        volume = value;
        playing = true;
        ++playCalls;
        return true;
    }
    bool PlayWithScriptArguments(std::int32_t, std::int32_t, std::int32_t, std::int32_t) override { return true; }
    bool IsPlaying() const override { return playing; }
    bool Stop() override {
        playing = false;
        ++stopCalls;
        return true;
    }
    bool Kill() override { return Stop(); }
    bool StopAllInstances() override { return true; }
    std::uint32_t GetUseCount() const override { return 0; }
    std::uint32_t GetID() const override { return 0; }
    void SetID(std::uint32_t) override {}
    bool Load() override { return true; }
    bool Unload() override { return true; }
    bool IsLoaded() const override { return true; }
    bool Pause(bool) override { return true; }
    bool Unpause() override { return true; }
    bool IsPaused() const override { return false; }
    std::int32_t GetVolume() const override { return volume; }
    bool SetVolume(std::int32_t value) override { volume = value; return true; }
    bool FadeVolume(std::int32_t from, std::int32_t to, std::uint32_t duration, bool start) override {
        lastFadeFrom = from;
        lastFadeTo = to;
        lastFadeDuration = static_cast<std::int32_t>(duration);
        fadeStarted = start;
        fading = start;
        volume = to;
        return true;
    }
    bool IsFading() const override { return fading; }
    std::int32_t GetPan() const override { return pan; }
    bool SetPan(std::int32_t value) override { pan = value; return true; }
    std::int32_t GetFrequency() const override { return 22050; }
    bool SetFrequency(std::int32_t) override { return true; }
    std::int32_t GetOriginalFrequency() const override { return 22050; }
    bool SetEffectsType(std::int32_t, std::uint32_t) override { return true; }
    bool SetEffectsLevel(std::int32_t, std::uint32_t) override { return true; }
    std::int32_t GetSoundPriority() const override { return 0; }
    bool SetSoundPriority(std::int32_t) override { return true; }
    bool SetName(const cIGZString&) override { return true; }
    bool GetName(cIGZString&) const override { return false; }
    bool SetSoundFileFromString(const cIGZString& path, SndStreamingType streaming) override {
        soundFile.assign(path.Data(), path.Strlen());
        streamType = streaming;
        return true;
    }
    bool SetSoundFileFromChars(const char* path, std::uint32_t length, SndStreamingType streaming) override {
        soundFile.assign(path, length);
        streamType = streaming;
        return true;
    }
    // cIGZSndDataSourceHolder
    bool SetDataSource(cIGZSndDataSource* source, cIGZSnd::SndStreamingType streaming) override {
        dataSourceCleared = source == nullptr;
        dataSourceStreaming = streaming;
        return true;
    }
    cIGZSndDataSource* GetDataSource() override { return nullptr; }
};

// cIGZSndSys: records every event the radio posts and keeps the registered handler.
struct FakeSndSys final : cIGZSndSysStub {
    struct Event {
        std::uint32_t id, a, b, c, d;
        bool isString;
        std::string text;
    };
    std::vector<Event> events;
    cIGZSndEventHandler* handler = nullptr;
    int registerCalls = 0, unregisterCalls = 0;

    bool DoEventIds(std::uint32_t id, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override {
        events.push_back({id, a, b, c, d, false, {}});
        return true;
    }
    bool DoEventString(std::uint32_t id, cIGZString* text, std::uint32_t a, std::uint32_t b, std::uint32_t c,
                       std::uint32_t d) override {
        events.push_back({id, a, b, c, d, true, text ? std::string(text->Data(), text->Strlen()) : std::string()});
        if (text) {   // the event system takes its own reference and drops it after dispatching
            text->AddRef();
            text->Release();
        }
        return true;
    }
    bool RegisterEventHandler(cIGZSndEventHandler* value) override {
        ++registerCalls;
        handler = value;
        return true;
    }
    bool UnregisterEventHandler(cIGZSndEventHandler* value) override {
        ++unregisterCalls;
        if (handler == value) handler = nullptr;
        return true;
    }
    const Event* last() const { return events.empty() ? nullptr : &events.back(); }
    void clear() { events.clear(); }
};

struct FakeSndMgr final : cIGZSndMgrStub {
    std::uint32_t classId = 0x00c0ffee;
    bool GetDefaultSndDataSourcePlayer2DClassId(std::uint32_t& out) const override {
        out = classId;
        return true;
    }
};

struct FakeCom final : cIGZCOM {
    std::uint32_t refs = 1;
    std::uint32_t requestedClass = 0, requestedIid = 0;
    int createCalls = 0;
    std::vector<FakeSnd*> allSounds;   // the fake never frees what it creates; main deletes them at the end
    FakeSnd* sound = nullptr;
    bool CreateInstance(std::uint32_t clsid, std::uint32_t iid, void** out) override {
        ++createCalls;
        requestedClass = clsid;
        requestedIid = iid;
        if (iid != kGZIID_cIGZSnd) return false;
        sound = new FakeSnd();
        allSounds.push_back(sound);
        *out = static_cast<cIGZSnd*>(sound);
        return true;
    }
    cIGZFrameWork* GetFrameWork() override { return nullptr; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }
};

// cIGZFrameWork: only the tick/idle registration is used; services come from the test resolver.
struct FakeFramework final : cIGZFrameWork {
    std::uint32_t refs = 1;
    std::vector<cIGZSystemService*> tickList, idleList;
    int addTickCalls = 0, removeTickCalls = 0, addIdleCalls = 0, removeIdleCalls = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }
    bool AddSystemService(cIGZSystemService*) override { return false; }
    bool RemoveSystemService(cIGZSystemService*) override { return false; }
    bool GetSystemService(std::uint32_t, std::uint32_t, void**) override { return false; }
    bool EnumSystemServices(cIGZUnknownEnumerator&, cIGZUnknown*, std::uint32_t) override { return false; }
    bool AddHook(cIGZFrameWorkHooks*) override { return false; }
    bool RemoveHook(cIGZFrameWorkHooks*) override { return false; }
    bool AddToTick(cIGZSystemService* service) override {
        ++addTickCalls;
        tickList.push_back(service);
        return true;
    }
    bool RemoveFromTick(cIGZSystemService* service) override {
        ++removeTickCalls;
        for (auto it = tickList.begin(); it != tickList.end(); ++it) {
            if (*it == service) {
                tickList.erase(it);
                break;
            }
        }
        return true;
    }
    bool AddToOnIdle(cIGZSystemService* service) override {
        ++addIdleCalls;
        idleList.push_back(service);
        return true;
    }
    bool RemoveFromOnIdle(cIGZSystemService* service) override {
        ++removeIdleCalls;
        for (auto it = idleList.begin(); it != idleList.end(); ++it) {
            if (*it == service) {
                idleList.erase(it);
                break;
            }
        }
        return true;
    }
    int GetOnIdleInterval() const override { return 1; }
    void SetOnIdleInterval(int) override {}
    void OnTick(std::uint32_t tickNumber) override {
        for (cIGZSystemService* service : tickList) service->OnTick(tickNumber);
    }
    void OnIdle() override {
        for (cIGZSystemService* service : idleList) service->OnIdle(0);
    }
    bool IsTickEnabled() const override { return true; }
    void ToggleTick(bool) override {}
    void Quit(int) override {}
    void AbortiveQuit(int) override {}
    cIGZCmdLine* CommandLine() override { return nullptr; }
    bool IsInstall() const override { return false; }
    cIGZCOM* GetCOMObject() override { return nullptr; }
    int GetState() const override { return 0; }
    cIGZDebugStream* GetDebugStream() override { return nullptr; }
    cIGZDebugStream* DefaultDebugStream() override { return nullptr; }
    cIGZDebugStream* DebugStream() override { return nullptr; }
    void SetDebugStream(cIGZDebugStream*) override {}
    void SetDebugLevel(int) override {}
    int GetDebugLevel() const override { return 0; }
    cIGZOStream* StdOut() override { return nullptr; }
    cIGZOStream* StdErr() override { return nullptr; }
    cIGZIStream* StdIn() override { return nullptr; }
    bool GetStream(int, std::uint32_t, void**) override { return false; }
    bool SetStream(int, cIGZUnknown*) override { return false; }
    void SetApplication(cIGZApp*) override {}
    cIGZApp* Application() const override { return nullptr; }
    void ReportException(const char*) override {}
    cIGZUnknown* ExceptionNotificationObj() const override { return nullptr; }
};

// ---------------------------------------------------------------- the test rig

FakeFramework g_framework;
FakeSndSys g_sndSys;
FakeSndMgr g_sndMgr;
FakeRadioHost g_radioHost;
FakeStationHost g_stationHost;
FakeCom g_com;
cRZLanguageManager* g_languageManager = nullptr;
cGZFileSystem* g_fileSystem = nullptr;

std::string g_root;

std::string DataDirectory() { return g_root + "/SimCity 4 Data/"; }
std::string RadioDirectory() { return DataDirectory() + "ReS/Music/Radio/"; }

bool ResolveService(std::uint32_t serviceId, std::uint32_t iid, void** out) {
    if (serviceId == 0x23cecba0 && iid == 0x106077d2) {
        g_sndSys.AddRef();
        *out = static_cast<cIGZSndSys*>(&g_sndSys);
        return true;
    }
    if (serviceId == 0x1af85bfb && iid == 0x3af85bb7) {
        g_sndMgr.AddRef();
        *out = static_cast<cIGZSndMgr*>(&g_sndMgr);
        return true;
    }
    if (serviceId == 0x441e5070 && iid == 0x0054b7d5) {
        g_languageManager->AddRef();
        *out = static_cast<cIGZLanguageManager*>(g_languageManager);
        return true;
    }
    if (serviceId == 0x2073215d && iid == 0xe0203660) {
        g_fileSystem->AddRef();
        *out = static_cast<cIGZFileSystem*>(g_fileSystem);
        return true;
    }
    return false;
}

void WriteFile(const std::string& path, const std::string& contents = "x") {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

void RemoveTree(const std::string& path) { std::filesystem::remove_all(path); }

void InstallFakes() {
    std::filesystem::create_directories(DataDirectory());
    std::filesystem::create_directories(RadioDirectory() + "Commercials/");

    SetRZFramework(&g_framework);
    SetRZServiceResolver(&ResolveService);
    SetRadioHost(&g_radioHost);
    SetRadioStationHost(&g_stationHost);

    if (RZGetCOMDllDirector()->GZCOM() == nullptr) RZGetCOMDllDirector()->InitializeCOM(&g_com, cRZString(""));

    g_languageManager = new cRZLanguageManager();
    g_languageManager->AddRef();   // keep our own reference: Init drops the one it takes
    g_fileSystem = new cGZFileSystem();
    g_fileSystem->AddRef();
    cRZString dataDirectory(DataDirectory().c_str());
    g_fileSystem->SetDataDirectory(dataDirectory);   // marks the directory resolved (it exists)
}

// A radio wired by hand (no Init): language manager, sound system and the seams.
cRZRadio* MakeRadio() {
    auto* radio = new cRZRadio();
    radio->AddRef();   // keep it alive across the Releases the seams do
    radio->SetLanguageManager(g_languageManager);
    radio->SetSoundSystem(&g_sndSys);
    return radio;
}

// A radio that went through Init: it owns a FakeSnd (g_com.sound) and the two disk stations.
struct InitializedRadio {
    cRZRadio* radio = nullptr;
    FakeSnd* sound = nullptr;
    FakeStation* stationA = nullptr;
    FakeStation* stationB = nullptr;
};

InitializedRadio InitRadio() {
    RemoveTree(RadioDirectory() + "Stations/");
    WriteFile(RadioDirectory() + "Stations/Alpha/Alpha.id");
    WriteFile(RadioDirectory() + "Stations/Beta/Beta.id");

    InitializedRadio result;
    result.radio = new cRZRadio();
    result.radio->AddRef();
    g_stationHost.created.clear();
    CHECK(result.radio->Init());
    result.sound = g_com.sound;
    result.stationA = static_cast<FakeStation*>(result.radio->GetStation(0u));
    result.stationB = static_cast<FakeStation*>(result.radio->GetStation(1));
    return result;
}

FakeStation* FindCreatedStation(const std::string& name) {
    for (FakeStation* station : g_stationHost.created) {
        if (station->stationDirectory.str().find(name) != std::string::npos) return station;
    }
    return nullptr;
}

const FakeSndSys::Event* FindEvent(std::uint32_t id) {
    for (auto it = g_sndSys.events.rbegin(); it != g_sndSys.events.rend(); ++it) {
        if (it->id == id) return &*it;
    }
    return nullptr;
}

std::uint32_t CountEvents(std::uint32_t id) {
    std::uint32_t count = 0;
    for (const auto& event : g_sndSys.events) count += event.id == id ? 1 : 0;
    return count;
}

// ---------------------------------------------------------------- tests

void TestOptionsAndSettings() {
    cRZRadio* radio = MakeRadio();
    CHECK(radio->GetOption(cIGZRadio::kRadioOption0) && !radio->GetOption(cIGZRadio::kRadioOption1));
    CHECK(radio->GetOption(cIGZRadio::kRadioOption3) && !radio->GetOption(cIGZRadio::kRadioOption2));

    radio->SetOption(false, cIGZRadio::kRadioOption0);
    radio->SetOption(true, cIGZRadio::kRadioOption1);
    radio->SetOption(false, cIGZRadio::kRadioOption3);
    radio->SetOption(true, cIGZRadio::kRadioOption2);   // not an option: ignored
    CHECK(!radio->GetOption(cIGZRadio::kRadioOption0) && radio->GetOption(cIGZRadio::kRadioOption1));
    CHECK(!radio->GetOption(cIGZRadio::kRadioOption3));

    CHECK(radio->GetVolume() == kRZRadioMaxVolume);
    CHECK(radio->SetVolume(500) && radio->GetVolume() == 500);
    CHECK(radio->SetVolume(0x500) && radio->GetVolume() == kRZRadioMaxVolume);
    CHECK(radio->SetVolume(-1) && radio->GetVolume() == kRZRadioMaxVolume);   // unsigned clamp quirk

    cRZString directory("dir");
    CHECK(radio->GetRadioDirectory(directory) && directory.str().empty());   // no directory set yet
    radio->SetRadioDirectory(cRZString("/somewhere/"));
    CHECK(radio->GetRadioDirectory(directory) && directory.str() == "/somewhere/");

    // A settings round trip: every field but +0x14, which neither function touches.
    cIGZRadio::RadioSettings settings{};
    settings.volume = 321;
    settings.on = 1;
    settings.station = 0;
    settings.option0 = 0;
    settings.option1 = 1;
    settings.unused14 = 0x1234;
    settings.option3 = 0;
    settings.settingsFlags = 1;
    radio->ReadSettings(&settings);
    CHECK(radio->GetVolume() == 321 && radio->GetOption(cIGZRadio::kRadioOption1));
    CHECK(!radio->GetOption(cIGZRadio::kRadioOption0) && !radio->GetOption(cIGZRadio::kRadioOption3));
    CHECK(radio->IsOn());   // the missing station list made SetupDiskStations fail, but TurnOn still set the flag

    cIGZRadio::RadioSettings written{};
    written.unused14 = 0x5678;
    radio->WriteSettings(&written);
    CHECK(written.volume == 321 && written.on == 1 && written.station == 0);
    CHECK(written.option0 == 0 && written.option1 == 1 && written.option3 == 0);
    CHECK(written.settingsFlags == 1 && written.unused14 == 0x5678);
    radio->Release();
}

void TestMute() {
    InitializedRadio setup = InitRadio();
    cRZRadio* radio = setup.radio;
    radio->SetVolume(600);
    g_sndSys.clear();
    radio->Mute(true);
    CHECK(g_sndSys.events.size() == 1 && g_sndSys.last()->id == kRZRadioEventSetVolume && g_sndSys.last()->a == 0);
    radio->Mute(true);   // nested: no event
    CHECK(g_sndSys.events.size() == 1);
    radio->Mute(false);
    CHECK(g_sndSys.events.size() == 1);
    radio->Mute(false);
    CHECK(g_sndSys.events.size() == 2 && g_sndSys.last()->a == 600);
    radio->Mute(false);   // already at zero
    CHECK(g_sndSys.events.size() == 2);

    // Muting at volume 0 still takes the lock (the volume check comes before the subsystem checks).
    radio->SetVolume(0);
    g_sndSys.clear();
    radio->Mute(true);
    radio->Mute(false);
    CHECK(g_sndSys.events.empty());

    // Setting the volume while muted does not post, but the saved volume follows it.
    radio->SetVolume(400);
    radio->Mute(true);
    g_sndSys.clear();
    radio->SetVolume(123);
    CHECK(g_sndSys.events.empty());
    radio->Mute(false);
    CHECK(g_sndSys.events.size() == 1 && g_sndSys.last()->a == 123);
    radio->Release();
}

void TestStationList() {
    cRZRadio* radio = MakeRadio();
    auto* a = new FakeStation();
    auto* b = new FakeStation();
    a->callSign = "WAVE";
    b->callSign = "JAZZ";

    CHECK(radio->AddStation(a) && radio->GetStationCount() == 1);
    CHECK(a->setRadioCalls == 1 && a->radio == static_cast<cIGZRadio*>(radio));
    CHECK(a->addRefs == 1);
    CHECK(!radio->AddStation(a));   // duplicates are refused
    CHECK(radio->GetStation(0u) == a && radio->GetStation(1) == nullptr);
    CHECK(radio->GetStation("WAVE") == a && radio->GetStation("JAZZ") == nullptr);
    CHECK(radio->IsStationAvailable("WAVE") && !radio->IsStationAvailable("NONE"));

    std::uint32_t index = 99;
    CHECK(radio->GetCurrentStation(index) && index == 0);
    char callSign[kRZRadioCallSignBufferSize] = {};
    CHECK(radio->GetCurrentStation(callSign) && std::strncmp(callSign, "WAVE", 4) == 0);

    CHECK(radio->AddStation(b) && radio->GetStationCount() == 2);
    g_sndSys.clear();
    CHECK(radio->SetStation(1));
    CHECK(a->unsetStationCalls == 1);                        // the running station is unset
    CHECK(g_sndSys.last() && g_sndSys.last()->id == kRZRadioEventStopPlayback);
    CHECK(b->setStationCalls == 0);                          // the radio is off, so the new one is not started
    CHECK(radio->GetCurrentStation(index) && index == 1);

    CHECK(radio->SetStation(9));                             // out of range: clamped to the last station
    CHECK(radio->GetCurrentStation(index) && index == 1);

    // SetStation by name finds the station and switches to it (no station is started while the radio is off).
    CHECK(radio->SetStation("WAVE", nullptr));
    CHECK(radio->GetCurrentStation(index) && index == 0);
    CHECK(a->setStationCalls == 0 && b->unsetStationCalls == 2);

    // A fade is posted first (data1 = target volume, data2 = duration); the fading flag suppresses the stop event.
    g_sndSys.clear();
    const cIGZRadio::FadeOptions fade{300, 1500};
    CHECK(radio->SetStation("JAZZ", &fade));
    CHECK(g_sndSys.events.size() == 1 && g_sndSys.last()->id == kRZRadioEventFade);
    CHECK(g_sndSys.last()->a == 300 && g_sndSys.last()->b == 1500);
    CHECK(!radio->SetStation("NONE", nullptr));

    CHECK(radio->RemoveStation(a, false) && a->releases == 0 && radio->GetStationCount() == 1);
    CHECK(!radio->RemoveStation(a, false));
    CHECK(radio->RemoveStation(b, true) && b->releases == 1 && radio->GetStationCount() == 0);

    CHECK(!radio->GetCurrentStation(index));

    // RemoveStation by call sign, and the release flag.
    auto* c = new FakeStation();
    c->callSign = "ROCK";
    CHECK(radio->AddStation(c));
    CHECK(!radio->RemoveStation("NONE", true));
    CHECK(radio->RemoveStation("ROCK", true) && c->releases == 1 && radio->GetStationCount() == 0);
    CHECK(!radio->RemoveAllStations(true));   // always false
    radio->Release();
    delete a;   // the radio dropped its references; the fake stations never delete themselves
    delete b;
    delete c;
}

void TestTurnOnAndIdle() {
    // A radio whose station list stays empty: turning it on is allowed, and the first AddStation then starts it.
    cRZRadio* radio = MakeRadio();
    CHECK(radio->TurnOn(true) && radio->IsOn());
    auto* station = new FakeStation();
    g_sndSys.clear();
    CHECK(radio->AddStation(station));
    CHECK(station->onIdleCalls == 1);   // AddStation -> OnIdle on the first station
    CHECK(station->setStationCalls == 0);
    CHECK(CountEvents(kRZRadioEventRefreshState) == 1);

    // The idle path: refresh event, then the station's OnIdle (whose answer OnIdle returns).
    g_sndSys.clear();
    CHECK(radio->OnIdle());
    CHECK(CountEvents(kRZRadioEventRefreshState) == 1);
    CHECK(station->onIdleCalls == 2);

    // On-demand suppresses the station idle (but not the refresh event).
    radio->SetOnDemandState(true);
    g_sndSys.clear();
    CHECK(!radio->OnIdle() && station->onIdleCalls == 2 && CountEvents(kRZRadioEventRefreshState) == 1);
    radio->SetOnDemandState(false);

    // The iTunes branch: no station work, the player is started when it is allowed to.
    g_radioHost.usingITunes = true;
    g_radioHost.readyToHearMusic = true;
    g_radioHost.itunesPlaying = false;
    CHECK(!radio->OnIdle() && g_radioHost.playlistCalls == 1 && station->onIdleCalls == 2);
    g_radioHost.usingITunes = false;
    g_radioHost.itunesPlaying = true;
    const int stopCalls = g_radioHost.stopCalls;
    CHECK(radio->OnIdle() && g_radioHost.stopCalls == stopCalls + 1);   // then the normal idle still runs

    // Turning it off clears the flag (there is no radio sound here, so nothing is posted).
    g_sndSys.clear();
    CHECK(radio->TurnOn(false) && !radio->IsOn());
    CHECK(g_sndSys.events.empty());
    radio->Release();
    delete station;
}

void TestDiskStations() {
    cRZRadio* radio = MakeRadio();
    radio->SetRadioDirectory(cRZString(RadioDirectory().c_str()));

    RemoveTree(RadioDirectory() + "Stations/");
    WriteFile(RadioDirectory() + "Stations/Alpha/Alpha.id");
    WriteFile(RadioDirectory() + "Stations/Beta/Beta.id");
    WriteFile(RadioDirectory() + "Stations/NoCallId/readme.txt");
    g_stationHost.created.clear();

    CHECK(radio->SetupDiskStations());
    CHECK(radio->GetStationCount() == 2);
    CHECK(g_stationHost.created.size() == 2);
    FakeStation* alpha = FindCreatedStation("Alpha");   // the directory scan order is the file system's
    FakeStation* beta = FindCreatedStation("Beta");
    CHECK(alpha != nullptr && beta != nullptr);
    CHECK(alpha->stationDirectory.str() == RadioDirectory() + "Stations/Alpha/");
    CHECK(alpha->musicDirectory.str() == RadioDirectory() + "Stations/Alpha/Music/");
    CHECK(alpha->djDirectory.str() == RadioDirectory() + "Stations/Alpha/DJ/English/");
    CHECK(alpha->commercialDirectory.str() == RadioDirectory() + "Commercials/English/");
    CHECK(alpha->jingleDirectory.str() == RadioDirectory() + "Stations/Alpha/Jingle/English/");
    CHECK(beta->stationDirectory.str() == RadioDirectory() + "Stations/Beta/");
    CHECK(alpha->readCallSignCalls == 1 && beta->readCallSignCalls == 1);
    CHECK(alpha->shuffleStrategy == 0 && alpha->addRefs >= 1);
    CHECK(radio->GetStation(0u) != nullptr && radio->GetStation(1u) != nullptr && radio->GetStation(2u) == nullptr);

    // Bit 0 of the settings flags selects the shuffle strategy; the list is rebuilt (old stations released).
    cIGZRadio::RadioSettings settings{};
    settings.volume = kRZRadioMaxVolume;
    settings.settingsFlags = 1;
    radio->ReadSettings(&settings);
    const std::uint32_t betaReleases = beta->releases;
    g_stationHost.created.clear();
    CHECK(radio->SetupDiskStations());
    CHECK(radio->GetStationCount() == 2 && g_stationHost.created.size() == 2);
    CHECK(g_stationHost.created[0]->shuffleStrategy == 1);
    CHECK(beta->releases > betaReleases);

    // The current station is clamped when the rebuilt list got shorter.
    CHECK(radio->SetStation(1));
    std::filesystem::remove_all(RadioDirectory() + "Stations/Beta/");
    g_stationHost.created.clear();
    CHECK(radio->SetupDiskStations());
    CHECK(radio->GetStationCount() == 1);
    std::uint32_t index = 99;
    CHECK(radio->GetCurrentStation(index) && index == 0);

    // A station directory without a "*.id" file is skipped; an empty list answers false.
    RemoveTree(RadioDirectory() + "Stations/");
    WriteFile(RadioDirectory() + "Stations/NoCallId/readme.txt");
    CHECK(!radio->SetupDiskStations());
    CHECK(radio->GetStationCount() == 0);

    // The comma-separated sound filter is one fnmatch pattern, so even a real sound file is not found (quirk kept).
    WriteFile(RadioDirectory() + "Stations/Alpha/song.mp3");
    CHECK(!radio->DoesAtLeastOneSoundEntryExistForTheGivenDirectory(
        cRZString((RadioDirectory() + "Stations/Alpha/").c_str())));
    CHECK(!radio->DoesAtLeastOneSoundEntryExistForTheGivenDirectory(cRZString((RadioDirectory() + "Empty/").c_str())));

    // The language directory name gets a trailing separator; an unknown language fails.
    cRZString language;
    CHECK(radio->GetLanguageDirectoryName(1, language) && language.str() == "English/");
    language.str() = "untouched";
    CHECK(!radio->GetLanguageDirectoryName(99, language));
    radio->Release();
}

void TestDiskStationLanguageFallback() {
    cRZRadio* radio = MakeRadio();
    radio->SetRadioDirectory(cRZString(RadioDirectory().c_str()));
    g_languageManager->SetCurrentLanguage(5);   // Italian; the runtime-library name may be missing, so check the state
    CHECK(g_languageManager->GetCurrentLanguage() == 5);

    RemoveTree(RadioDirectory() + "Stations/");
    WriteFile(RadioDirectory() + "Stations/Alpha/Alpha.id");
    g_stationHost.created.clear();

    // Non-English: the current language directory has no sound files, so option 3 falls back to English.
    CHECK(radio->SetupDiskStations());
    CHECK(g_stationHost.created.size() == 1);
    CHECK(g_stationHost.created[0]->djDirectory.str() == RadioDirectory() + "Stations/Alpha/DJ/English/");
    CHECK(g_stationHost.created[0]->commercialDirectory.str() == RadioDirectory() + "Commercials/English/");
    CHECK(g_stationHost.created[0]->jingleDirectory.str() == RadioDirectory() + "Stations/Alpha/Jingle/English/");

    // Without option 3 the language directory is dropped instead.
    radio->SetOption(false, cIGZRadio::kRadioOption3);
    g_stationHost.created.clear();
    CHECK(radio->SetupDiskStations());
    CHECK(g_stationHost.created.size() == 1);
    CHECK(g_stationHost.created[0]->djDirectory.str() == RadioDirectory() + "Stations/Alpha/DJ/");
    CHECK(g_stationHost.created[0]->commercialDirectory.str() == RadioDirectory() + "Commercials/");
    CHECK(g_stationHost.created[0]->jingleDirectory.str() == RadioDirectory() + "Stations/Alpha/Jingle/");
    g_languageManager->SetCurrentLanguage(1);
    CHECK(g_languageManager->GetCurrentLanguage() == 1);
    radio->Release();
}

void TestSoundEvents() {
    const int addTickBefore = g_framework.addTickCalls;
    const int addIdleBefore = g_framework.addIdleCalls;
    const int registerBefore = g_sndSys.registerCalls;
    const int createBefore = g_com.createCalls;

    InitializedRadio setup = InitRadio();
    cRZRadio* radio = setup.radio;
    FakeSnd* sound = setup.sound;
    CHECK(sound != nullptr && radio->GetRadioSound() == sound);
    CHECK(g_sndSys.registerCalls == registerBefore + 1);
    CHECK(g_sndSys.handler == static_cast<cIGZSndEventHandler*>(radio));
    CHECK(g_framework.addTickCalls == addTickBefore + 1 && g_framework.addIdleCalls == addIdleBefore + 1);
    CHECK(g_framework.tickList.back() == static_cast<cIGZSystemService*>(radio));

    // Init built the radio directory from the data directory and scanned the stations.
    cRZString directory;
    CHECK(radio->GetRadioDirectory(directory) && directory.str() == RadioDirectory());
    CHECK(g_com.createCalls == createBefore + 1 && g_com.requestedClass == g_sndMgr.classId &&
          g_com.requestedIid == kGZIID_cIGZSnd);
    CHECK(radio->GetStationCount() == 2 && setup.stationA && setup.stationB);

    // PlaySong checks the file, then posts the string event to the sound system.
    const std::string song = g_root + "/song.mp3";
    WriteFile(song);
    g_sndSys.clear();
    CHECK(radio->PlaySong(cRZString(song.c_str())));
    const FakeSndSys::Event* play = FindEvent(kRZRadioEventPlaySong);
    CHECK(play && play->isString && play->text == song);
    CHECK(!radio->PlaySong(cRZString((g_root + "/missing.mp3").c_str())));
    CHECK(g_sndSys.events.size() == 1);

    // DoEvent of the play event: stop, clear the data source, set the file, play at the radio's volume, remember it.
    radio->SetVolume(321);
    g_sndSys.clear();
    auto* messageSong = new cRZString(song.c_str());
    messageSong->AddRef();   // the message takes a reference in SetString
    cGZSndEventStd playEvent(kRZRadioEventPlaySong, 0, 0, 0, 0);
    playEvent.SetString(messageSong);
    CHECK(radio->DoEvent(&playEvent));
    CHECK(sound->stopCalls == 1 && sound->holderQueries >= 1);
    CHECK(sound->dataSourceCleared && sound->dataSourceStreaming == 2);
    CHECK(sound->soundFile == song && sound->streamType == 1);
    CHECK(sound->playCalls == 1 && sound->volume == 321);
    cRZString currentSong;
    CHECK(radio->GetCurrentSong(currentSong) && currentSong.str().empty());   // not playing yet: nothing copied
    messageSong->Release();

    // The refresh event copies the sound's state into the radio; then GetCurrentSong answers the song.
    sound->playing = true;
    sound->fading = true;
    cGZSndEventStd refresh(kRZRadioEventRefreshState, 0, 0, 0, 0);
    CHECK(radio->DoEvent(&refresh));
    CHECK(radio->IsPlaying() && radio->IsFading());
    CHECK(radio->GetCurrentSong(currentSong) && currentSong.str() == song);
    sound->fading = false;
    CHECK(radio->DoEvent(&refresh));
    CHECK(radio->IsPlaying() && !radio->IsFading());

    // The volume event sets the radio's volume and forwards it while the radio is not fading.
    cGZSndEventStd volumeEvent(kRZRadioEventSetVolume, 700, 0, 0, 0);
    sound->volume = 321;
    CHECK(radio->DoEvent(&volumeEvent));
    CHECK(radio->GetVolume() == 700 && sound->volume == 700);

    // The fade event fades from the sound's current volume and marks the radio as fading.
    sound->playing = true;
    sound->volume = 100;
    cGZSndEventStd fadeEvent(kRZRadioEventFade, 900, 2500, 0, 0);
    CHECK(radio->DoEvent(&fadeEvent));
    CHECK(sound->lastFadeFrom == 100 && sound->lastFadeTo == 900 && sound->lastFadeDuration == 2500 &&
          sound->fadeStarted);
    sound->volume = 5;
    CHECK(radio->DoEvent(&volumeEvent));
    CHECK(radio->GetVolume() == 700 && sound->volume == 5);   // the sound was not touched while fading

    // The play-next event stops the sound and asks the current station for its next selection (flag cleared first).
    std::uint32_t index = 0;
    CHECK(radio->GetCurrentStation(index));
    FakeStation* current = static_cast<FakeStation*>(radio->GetStation(index));
    CHECK(current != nullptr);
    current->startedFlag = 1;
    cGZSndEventStd nextEvent(kRZRadioEventPlayNext, 1, 0, 0, 0);
    sound->playing = false;
    const int stopBefore = sound->stopCalls;
    CHECK(radio->DoEvent(&nextEvent));
    CHECK(current->startedFlag == 0 && current->nextSelectionCalls == 1 && current->lastCycleType == 0);
    CHECK(sound->stopCalls == stopBefore + 1);

    // With data1 == 0 while playing the event is not handled (and the sound is not stopped).
    cGZSndEventStd nextNoData(kRZRadioEventPlayNext, 0, 0, 0, 0);
    sound->playing = true;
    const int nextCalls = current->nextSelectionCalls;
    CHECK(!radio->DoEvent(&nextNoData));
    CHECK(current->nextSelectionCalls == nextCalls);

    // The stop event stops the sound; an unknown id is not handled.
    const int stopCallsBefore = sound->stopCalls;
    cGZSndEventStd stopEvent(kRZRadioEventStopPlayback, 0, 0, 0, 0);
    CHECK(radio->DoEvent(&stopEvent));
    CHECK(sound->stopCalls == stopCallsBefore + 1 && !sound->playing);
    cGZSndEventStd unknownEvent(0x11111111, 0, 0, 0, 0);
    CHECK(!radio->DoEvent(&unknownEvent));

    // Turning the radio on starts the current station; turning it off stops playback through the sound system.
    g_sndSys.clear();
    CHECK(radio->TurnOn(true) && radio->IsOn());
    CHECK(current->setStationCalls >= 1);
    CHECK(radio->TurnOn(false) && !radio->IsOn());
    CHECK(g_sndSys.last() && g_sndSys.last()->id == kRZRadioEventStopPlayback);
    CHECK(radio->TurnOn(true) && radio->IsOn());

    // OnTick runs the no-argument OnIdle: it clears the pending fade (posting the stop) and idles the station.
    setup.stationA->onIdleCalls = 0;
    setup.stationB->onIdleCalls = 0;
    g_sndSys.clear();
    CHECK(radio->OnTick(16));
    CHECK(setup.stationA->onIdleCalls + setup.stationB->onIdleCalls == 1);
    CHECK(CountEvents(kRZRadioEventRefreshState) == 1);
    CHECK(CountEvents(kRZRadioEventStopPlayback) == 1);   // fading_ was still set by the fade event

    // Shutdown unregisters and unlists the service, releasing the stations and the sound.
    const std::uint32_t soundRefsBefore = sound->refs;
    const std::uint32_t stationReleases = setup.stationA->releases;
    const int removeTickBefore = g_framework.removeTickCalls;
    const int removeIdleBefore = g_framework.removeIdleCalls;
    const int unregisterBefore = g_sndSys.unregisterCalls;
    CHECK(radio->Shutdown());
    CHECK(g_framework.removeTickCalls == removeTickBefore + 1 && g_framework.removeIdleCalls == removeIdleBefore + 1);
    CHECK(g_sndSys.unregisterCalls == unregisterBefore + 1);
    CHECK(radio->GetStationCount() == 0);
    CHECK(setup.stationA->releases > stationReleases);
    CHECK(sound->refs + 1 == soundRefsBefore);

    // The released radio sound pointer is left dangling: a second Init does not create another one (quirk kept).
    CHECK(radio->Init());
    CHECK(g_com.createCalls == createBefore + 1);
    CHECK(g_framework.addTickCalls == addTickBefore + 2 && g_framework.addIdleCalls == addIdleBefore + 2);

    // Release at count 1 runs FinalRelease: Shutdown (unregister again, release the rebuilt stations), then delete.
    FakeStation* rebuilt = static_cast<FakeStation*>(radio->GetStation(0u));
    const std::uint32_t rebuiltReleases = rebuilt ? rebuilt->releases : 0;
    const int unregisterBeforeRelease = g_sndSys.unregisterCalls;
    CHECK(radio->Release() == 0);
    CHECK(g_sndSys.unregisterCalls == unregisterBeforeRelease + 1);
    CHECK(rebuilt != nullptr && rebuilt->releases > rebuiltReleases);
}

void TestQueryInterfaceAndRefCount() {
    auto* radio = new cRZRadio();
    radio->AddRef();   // 1
    void* out = nullptr;
    CHECK(radio->QueryInterface(kGZIID_cIGZRadio, &out));
    CHECK(out == static_cast<void*>(static_cast<cIGZRadio*>(radio)));
    CHECK(out == static_cast<void*>(reinterpret_cast<char*>(radio) + 0x28));   // the interface view is at +0x28
    CHECK(radio->QueryInterface(kGZIID_cIGZUnknown, &out));                   // iid 1 also answers the radio view
    CHECK(out == static_cast<void*>(static_cast<cIGZRadio*>(radio)));
    CHECK(radio->QueryInterface(kGZIID_cIGZSystemService, &out));
    CHECK(out == static_cast<void*>(static_cast<cIGZSystemService*>(radio)));
    CHECK(!radio->QueryInterface(0xdeadbeef, &out));
    CHECK(out == static_cast<void*>(static_cast<cIGZSystemService*>(radio)));   // untouched on failure

    // The cRZUnknown view's own counter is unused (quirk): RefCount stays 0 and RemoveRef does not touch +0x3c.
    auto* unknown = static_cast<cRZUnknown*>(radio);
    CHECK(static_cast<void*>(unknown) == static_cast<void*>(reinterpret_cast<char*>(radio) + 0x8));
    CHECK(static_cast<void*>(static_cast<cIGZSndEventHandler*>(radio)) ==
          static_cast<void*>(reinterpret_cast<char*>(radio) + 0x30));
    CHECK(unknown->RefCount() == 0);
    CHECK(unknown->RemoveRef() == 0);
    // The secondary interface views dispatch to the same implementation through the generated thunks.
    CHECK(static_cast<cIGZRadio*>(radio)->GetVolume() == kRZRadioMaxVolume);
    CHECK(static_cast<cIGZRadio*>(radio)->GetStationCount() == 0);
    CHECK(static_cast<cIGZSystemService*>(radio)->GetServiceID() == kGZSID_cRZRadio);
    CHECK(static_cast<cIGZSystemService*>(radio)->OnTick(1));
    cGZSndEventStd unhandledEvent(0x22222222, 0, 0, 0, 0);
    CHECK(!static_cast<cIGZSndEventHandler*>(radio)->DoEvent(&unhandledEvent));

    CHECK(radio->Release() == 3);   // 1 + the three successful queries
    CHECK(radio->Release() == 2);
    CHECK(radio->Release() == 1);
    CHECK(radio->Release() == 0);   // FinalRelease: Shutdown (not initialized) then delete
}

void TestFactoryAndDestruction() {
    cIGZRadio* created = CreatecRZRadio();
    CHECK(created != nullptr);
    auto* radio = static_cast<cRZRadio*>(created);
    CHECK(static_cast<void*>(created) == static_cast<void*>(reinterpret_cast<char*>(radio) + 0x28));
    CHECK(radio->GetStationCount() == 0 && radio->GetVolume() == kRZRadioMaxVolume);
    radio->AddRef();
    CHECK(radio->Release() == 0);   // FinalRelease: Shutdown (not initialized) then delete

    // Release at count 1 destroys a fresh radio; ASan reports any double free.
    auto* plain = new cRZRadio();
    plain->AddRef();
    CHECK(plain->Release() == 0);
}

}  // namespace

int main() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "rz_radio_test";
    RemoveTree(root.string());
    std::filesystem::create_directories(root);
    g_root = root.string();
    InstallFakes();

    TestOptionsAndSettings();
    TestMute();
    TestStationList();
    TestTurnOnAndIdle();
    TestDiskStations();
    TestDiskStationLanguageFallback();
    TestSoundEvents();
    TestQueryInterfaceAndRefCount();
    TestFactoryAndDestruction();

    for (FakeStation* station : g_stationHost.all) delete station;
    for (FakeSnd* sound : g_com.allSounds) delete sound;
    RemoveTree(g_root);
    std::printf(failures ? "FAILED\n" : "rz_radio ok\n");
    return failures ? 1 : 0;
}
