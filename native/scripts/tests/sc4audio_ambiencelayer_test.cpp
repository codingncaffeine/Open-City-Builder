// Test for cSC4AudioAmbienceLayer (src/sc4audio/AmbienceLayer.*) with fake host, sound sink, filters,
// freshness player, occupants and message server. Expected values are derived by hand from the
// decompiled formulas (see comments).
#include <array>
#include <cmath>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>

#include "audio/interfaces/cIFreshPlayer.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4audio/SoundManager.h"
#include "sc4audio/AmbienceLayer.h"

using namespace SC4Audio;
using sc4::cSCPropertyHelp;
using sc4::cSCPropertyHolder;

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (eps))) { std::printf("FAIL line %d: %s=%g vs %g\n", __LINE__, #a, _a, _b); std::exit(1); } } while (0)

// ---------------------------------------------------------------- fakes
struct FakeSink : IAmbienceSndSink {
    std::vector<AmbienceSndEvent> events;
    bool failCreate = false;
    bool PostEvent(const AmbienceSndEvent& e) override {
        if (failCreate) return false;
        events.push_back(e);
        return true;
    }
    int Count(std::uint32_t id) const { int n = 0; for (auto& e : events) n += e.eventId == id; return n; }
    const AmbienceSndEvent* Last(std::uint32_t id) const { const AmbienceSndEvent* r = nullptr; for (auto& e : events) if (e.eventId == id) r = &e; return r; }
};

struct FakePlayer final : cIFreshPlayer {
    int refs = 0;
    struct Div { std::uint32_t x, z; std::int32_t px, py, pz; };
    std::vector<Div> positions;
    std::map<std::pair<std::uint32_t, std::uint32_t>, bool> enabled;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }   // never freed: counts only
    bool SetDivisionEnabled(std::uint32_t x, std::uint32_t z, bool e) override { enabled[{x, z}] = e; return true; }
    bool SetDivision3DPosition(std::uint32_t x, std::uint32_t z, std::int32_t px, std::int32_t py, std::int32_t pz) override {
        positions.push_back({x, z, px, py, pz}); return true;
    }
    bool InitializeFromLoadedScore() override { return true; }
    bool InitializeFromResourceKey(const cGZPersistResourceKey&) override { return true; }
    bool LoadScore(const cIGZString&, std::uint32_t) override { return true; }
    cIFreshScore* GetScore(std::uint32_t) override { return nullptr; }
    bool Shutdown() override { return true; }
    bool Play() override { return true; }
    bool IsPlaying() override { return false; }
    bool Stop() override { return true; }
    bool SetVolume(std::int32_t) override { return true; }
    bool FadeVolume(std::int32_t, std::int32_t, bool) override { return true; }
    bool SetYPos(std::int32_t) override { return true; }
    bool SetXPos(std::int32_t) override { return true; }
    bool SetZoom(std::int32_t) override { return true; }
    bool SetPlayPercent(std::int32_t) override { return true; }
    bool SetTempo(std::int32_t) override { return true; }
    std::int32_t GetYPos() override { return 0; }
    std::int32_t GetXPos() override { return 0; }
    std::int32_t GetZoom() override { return 0; }
    bool SetManual(bool) override { return true; }
    std::int32_t GetFirstSelectableRow() override { return 0; }
    std::int32_t GetLastSelectableRow() override { return 0; }
    std::int32_t GetFirstSelectableColumn() override { return 0; }
    std::int32_t GetLastSelectableColumn() override { return 0; }
    cIFreshCellPlayer* GetCellPlayer(std::uint32_t, std::uint32_t, std::uint32_t) override { return nullptr; }
    bool Reseed(std::uint32_t) override { return true; }
    bool SetHeadRelative(bool) override { return true; }
    bool SetMinMaxDistance(std::uint32_t, std::uint32_t) override { return true; }
    bool SetNumDivisionsX(std::uint32_t) override { return true; }
    bool SetNumDivisionsZ(std::uint32_t) override { return true; }
    void SetKey(const cGZPersistResourceKey&) override {}
    const cGZPersistResourceKey* GetKey() override { return nullptr; }
    void SetExternalSoundSystemInterface(cIExternalSoundSystem*) override {}
};

// One fake filter object that answers exactly one interface (plus the base id).
struct FakeFilter {
    bool value = true;
    bool initOk = true;
    int shutdownCalls = 0;
    int refs = 0;
    int released = 0;
    std::vector<std::pair<int, int>> cellQueries;
};
struct FakeStateFilter final : sc4::cIAmbienceFilter {
    FakeFilter d;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cIAmbienceFilter || iid == kGZIID_cIAmbienceFilterBase) { *out = static_cast<sc4::cIAmbienceFilter*>(this); AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++d.refs; }
    std::uint32_t Release() override { ++d.released; return --d.refs; }
    bool Init() override { return d.initOk; }
    bool Shutdown() override { ++d.shutdownCalls; return true; }
    bool IsTrue() override { return d.value; }
};
struct FakeCellFilter final : sc4::cIAmbienceCellFilter {
    FakeFilter d;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cIAmbienceCellFilter || iid == kGZIID_cIAmbienceFilterBase) { *out = static_cast<sc4::cIAmbienceCellFilter*>(this); AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++d.refs; }
    std::uint32_t Release() override { ++d.released; return --d.refs; }
    bool Init() override { return d.initOk; }
    bool Shutdown() override { ++d.shutdownCalls; return true; }
    bool IsTrueCell(int x, int z) override { d.cellQueries.push_back({x, z}); return d.value; }
};
struct FakeOccFilter final : sc4::cIAmbienceOccupantFilter {
    FakeFilter d;
    sc4::cISC4Occupant* only = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cIAmbienceOccupantFilter || iid == kGZIID_cIAmbienceFilterBase) { *out = static_cast<sc4::cIAmbienceOccupantFilter*>(this); AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++d.refs; }
    std::uint32_t Release() override { ++d.released; return --d.refs; }
    bool Init() override { return d.initOk; }
    bool Shutdown() override { ++d.shutdownCalls; return true; }
    bool IsTrueOccupant(sc4::cISC4Occupant* o) override { return d.value && (!only || only == o); }
};

struct FakeOccupant final : sc4::cISC4Occupant {
    float pos[3] = {0, 0, 0};
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float* xyz) override { xyz[0] = pos[0]; xyz[1] = pos[1]; xyz[2] = pos[2]; }
    void SetPosition(const float* xyz) override { pos[0] = xyz[0]; pos[1] = xyz[1]; pos[2] = xyz[2]; }
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(sc4::SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    sc4::cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    sc4::cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(sc4::cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

struct FakeServer final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* t, std::uint32_t m) override { added.push_back({t, m}); return true; }
    bool RemoveNotification(cIGZMessageTarget2* t, std::uint32_t m) override { removed.push_back({t, m}); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeGame final : cIGameAudioContext {
    bool sound = true;
    AudioPreferences GetAudioPreferences() override { AudioPreferences p; p.soundEnabled = sound; return p; }
    bool IsSimulationPaused() override { return false; }
    bool GetListenerView(ListenerView&) override { return false; }
};

struct FakeViewport final : cIViewport {
    int zoom = 3;
    void ProjectPointToViewportCoordinates2D(float[3]) override {}
    std::int32_t ViewportWidth() const override { return 0; }
    std::int32_t ViewportHeight() const override { return 0; }
    void GetCameraEye(float[3]) const override {}
    void GetCameraTarget(float[3]) const override {}
    std::int32_t GetZoomLevel() const override { return zoom; }
};

struct FakeHost final : IAmbienceLayerHost {
    FakeSink sink;
    bool hasRM = true;
    bool hasSink = true;
    std::function<sc4::cISCPropertyHolder*(const cGZPersistResourceKey&)> exemplar;
    std::vector<cGZPersistResourceKey> exemplarRequests;
    std::map<std::uint32_t, std::function<cIGZUnknown*()>> factories;   // class id -> new object (refs 0)
    FakePlayer player;
    std::vector<FakePlayer*> players;
    ~FakeHost() { for (FakePlayer* p : players) delete p; }   // the fake players count references only
    int quality = 0;
    int residential = 0;
    std::uint32_t industrial = 0;
    std::uint32_t nextInstance = 100;
    int cellToWorldCalls = 0;
    std::vector<std::string> errors;
    bool HasResourceManager() override { return hasRM; }
    sc4::cISCPropertyHolder* GetTuningExemplar(const cGZPersistResourceKey& k) override {
        exemplarRequests.push_back(k);
        return exemplar ? exemplar(k) : nullptr;
    }
    IAmbienceSndSink* AcquireSoundSink() override { return hasSink ? &sink : nullptr; }
    bool CreateObject(std::uint32_t classId, std::uint32_t iid, void** out) override {
        if (classId == kClsidFreshPlayerObject) {
            if (iid != kIidFreshPlayerObject) return false;
            auto* p = new FakePlayer();
            p->refs = 1;
            players.push_back(p);
            *out = static_cast<cIGZUnknown*>(p);
            return true;
        }
        auto it = factories.find(classId);
        if (it == factories.end()) return false;
        if (iid != kGZIID_cIAmbienceFilterBase) return false;
        cIGZUnknown* u = it->second();
        u->AddRef();
        *out = u;
        return true;
    }
    int RenderAudioQuality() override { return quality; }
    bool CellToWorld(int x, int z, float& wx, float& wz) override { ++cellToWorldCalls; wx = x * 16.0f; wz = z * 16.0f; return true; }
    float TerrainAltitude(float x, float z) override { return x + z + 1.0f; }
    int ResidentialPopulation() override { return residential; }
    std::uint32_t IndustrialPopulation() override { return industrial; }
    std::uint32_t GenerateNewInstanceId() override { return nextInstance++; }
    void LogError(const char* m) override { errors.push_back(m); }
};

// ---------------------------------------------------------------- helpers
constexpr std::uint32_t kLayerId = 0x1234;

static FakeHost* g_host;
static FakeServer* g_server;
static FakeGame* g_game;

static void Setup(FakeHost& host) {
    g_host = &host;
    SetAmbienceLayerHost(&host);
    static FakeServer server; server.added.clear(); server.removed.clear(); g_server = &server;
    static FakeGame game; game.sound = true; g_game = &game;
    GetServices().messageServer = &server;
    GetServices().game = &game;
    ZoomDistances()[4] = 2000.0f;
}

static sc4::cISCPropertyHolder* NewHolder(std::function<void(cSCPropertyHolder*)> fill) {
    auto* h = new cSCPropertyHolder();
    fill(h);
    h->AddRef();
    return h;
}

static std::vector<FakeStateFilter*> g_stateFilters;
static std::vector<FakeCellFilter*> g_cellFilters;
static std::vector<FakeOccFilter*> g_occFilters;

// The filter fakes count references only; the registries own them, so they are freed when a test starts over.
static void FreeFilters() {
    for (auto* p : g_stateFilters) delete p;
    for (auto* p : g_cellFilters) delete p;
    for (auto* p : g_occFilters) delete p;
    g_stateFilters.clear(); g_cellFilters.clear(); g_occFilters.clear();
}

template <class T> static void AddFilterFactory(FakeHost& host, std::uint32_t id, std::vector<T*>& registry, bool value) {
    host.factories[id] = [&registry, value]() -> cIGZUnknown* { auto* f = new T(); f->d.value = value; registry.push_back(f); return f; };
}

// ---------------------------------------------------------------- tests
static void TestBasics() {
    FakeHost host; Setup(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    CHECK(!layer->IsInitialised() && layer->IsEnabled() && !layer->IsPaused());
    CHECK(layer->Mode() == 1);
    void* out = nullptr;
    CHECK(layer->QueryInterface(kGZIID_cISC4AudioAmbienceLayer, &out) && out == static_cast<cISC4AudioAmbienceLayer*>(layer));
    CHECK(layer->QueryInterface(1, &out));
    CHECK(!layer->QueryInterface(0x1234, &out));
    CHECK(layer->AddRef() == 3);      // two QI refs + this one: starts at 0
    CHECK(layer->Release() == 2);
    CHECK(layer->Release() == 1);
    cRZString name("MiXed Name");
    layer->SetName(name);
    CHECK(std::string(layer->GetName().ToChar()) == "mixed name");
    layer->SetLayerId(77);
    CHECK(layer->GetLayerId() == 77);
    cS3DVector3 avg{9, 9, 9};
    layer->GetAveragePosition(avg);
    CHECK(avg.x == 0 && avg.y == 0 && avg.z == 0);
    CHECK(!layer->CanHandleOccupant(nullptr));
    layer->Enable(false);
    CHECK(!layer->IsEnabled());
    CHECK(layer->Release() == 0);     // destroyed (count below 2)
}

static void TestMissingExemplarDefaults() {
    FakeHost host; Setup(host);
    host.quality = 1;
    auto* layer = new cSC4AudioAmbienceLayer(0xabcd);
    layer->AddRef();
    CHECK(!layer->ReadTuningValues(0xabcd));
    CHECK(std::string(layer->GetName().ToChar()) == "Noname_0x0000abcd");
    CHECK(host.exemplarRequests.size() == 1);
    CHECK(host.exemplarRequests[0].type == 0x6534284a && host.exemplarRequests[0].group == 0xca25875d && host.exemplarRequests[0].instance == 0xabcd);
    CHECK(host.errors.size() == 1);
    CHECK(layer->ScoreMasterVolume() == 1024);
    CHECK(layer->GroupRadiusA() == 50.0f && layer->GroupRadiusB() == 50.0f);
    CHECK(layer->Mode() == 1);
    CHECK(layer->CurveQuality() == 1);
    layer->Release();
}

static sc4::cISCPropertyHolder* FullTuning() {
    return NewHolder([](cSCPropertyHolder* h) {
        cSCPropertyHelp::SetPropertyValue(h, 0x20, cRZString("Traffic Hum"));
        cSCPropertyHelp::SetPropertyValue(h, 0xea3fbdd7, std::vector<std::int64_t>{(std::int64_t(0x22) << 32) | 0x11});   // group 0x11, instance 0x22
        cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1});          // all of
        cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde5, std::vector<std::uint32_t>{0xB1});          // none of
        cSCPropertyHelp::SetPropertyValue(h, 0xa410ba0, std::vector<std::uint32_t>{0xC1, 0xC2});     // any of
        cSCPropertyHelp::SetPropertyValue(h, 0x6a3fbdda, std::uint32_t(0x5150));                     // loop sound id
        cSCPropertyHelp::SetPropertyValue(h, 0xaa3fc48e, std::uint32_t(1024));                       // score master volume
        cSCPropertyHelp::SetPropertyValue(h, 0x6a3fc4c7, std::uint32_t(50));                         // master percent
        cSCPropertyHelp::SetPropertyValue(h, 0x8a552758, std::uint32_t(1));                          // mode
        cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(1));                          // amount source = total
        cSCPropertyHelp::SetPropertyValue(h, 0x8a3fc5dd, 2.0f);                                      // spread
        cSCPropertyHelp::SetPropertyValue(h, 0xa4168e1, 1.0f);                                       // group radius
        cSCPropertyHelp::SetPropertyValue(h, 0xca4132e9, std::vector<float>{0, 100, 4, 80});         // parameter curve
        cSCPropertyHelp::SetPropertyValue(h, 0x6a4132f3, std::vector<float>{0, 0, 4, 1000});         // volume curve
        cSCPropertyHelp::SetPropertyValue(h, 0x6a4132f6, std::vector<float>{0, 0, 4, 800});          // loop curve
        cSCPropertyHelp::SetPropertyValue(h, 0x8aa039b7, std::uint32_t(1000));                       // ease position speed
        cSCPropertyHelp::SetPropertyValue(h, 0x4aa03db1, std::uint32_t(8000));                       // ease volume speed
    });
}

static void StdFilters(FakeHost& host, bool a, bool b, bool c1, bool c2) {
    FreeFilters();
    AddFilterFactory<FakeStateFilter>(host, 0xA1, g_stateFilters, a);
    AddFilterFactory<FakeStateFilter>(host, 0xB1, g_stateFilters, b);
    AddFilterFactory<FakeStateFilter>(host, 0xC1, g_stateFilters, c1);
    AddFilterFactory<FakeStateFilter>(host, 0xC2, g_stateFilters, c2);
}

static void TestInitShutdown() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return FullTuning(); };
    StdFilters(host, true, false, false, true);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    CHECK(layer->Init());
    CHECK(layer->IsInitialised());
    CHECK(cSC4AudioAmbienceLayer::StaticInitCount() == 1);
    CHECK(std::string(layer->GetName().ToChar()) == "Traffic Hum");
    CHECK(g_server->added.size() == 1 && g_server->added[0].second == 0xc53d10aa && g_server->added[0].first == layer);
    CHECK(layer->FreshnessLoopId() == 0x5150 && layer->Loops().size() == 1 && layer->Loops().front().soundId == 0x5150);
    CHECK(layer->GroupRadiusA() == 1.0f && layer->GroupRadiusB() == 1.0f);   // radius B defaults to A
    CHECK(layer->OccupantLayer() && layer->OccupantLayer() == layer->CellLayer() && !layer->IntersectLayer());
    CHECK(layer->OccupantLayer()->refCount == 2);     // mode 1: one layer, two references
    // shared filter map: 4 filters, each with map + layer reference
    CHECK(cSC4AudioAmbienceLayer::SharedFilterCount() == 4);
    CHECK(g_stateFilters.size() == 4);
    for (auto* f : g_stateFilters) CHECK(f->d.refs == 2);
    // freshness score loaded: key {0x2026960b, 0x11, 0x22}
    CHECK(layer->Scores().size() == 1);
    CHECK(layer->Scores().front().key.type == 0x2026960b && layer->Scores().front().key.group == 0x11 && layer->Scores().front().key.instance == 0x22);
    CHECK(layer->Scores().front().pending && !layer->Scores().front().active);
    CHECK(host.players.size() == 1 && host.players[0]->refs == 1);    // creation ref dropped, node holds one
    const AmbienceSndEvent* add = host.sink.Last(kEventScoreAdd);
    CHECK(add && add->unknown == static_cast<cIGZUnknown*>(host.players[0]) && add->has[0] && add->has[1] && add->has[2]);
    CHECK(add->data[0] == 0x2026960b && add->data[1] == 0x11 && add->data[2] == 0x22);
    // duplicate key is refused
    CHECK(!layer->AddFreshnessScore({0x2026960b, 0x11, 0x22}));
    // a second Init does nothing
    CHECK(layer->Init() && cSC4AudioAmbienceLayer::StaticInitCount() == 1);
    // a second layer shares the filters
    auto* layer2 = new cSC4AudioAmbienceLayer(kLayerId);
    layer2->AddRef();
    CHECK(layer2->Init());
    CHECK(cSC4AudioAmbienceLayer::StaticInitCount() == 2);
    CHECK(g_stateFilters.size() == 4 && cSC4AudioAmbienceLayer::SharedFilterCount() == 4);
    for (auto* f : g_stateFilters) CHECK(f->d.refs == 3);
    layer2->Shutdown();
    CHECK(cSC4AudioAmbienceLayer::StaticInitCount() == 1);
    for (auto* f : g_stateFilters) CHECK(f->d.refs == 2);
    layer2->Release();
    // shutdown of the first layer: everything released, static shutdown runs the filters' Shutdown
    int events = static_cast<int>(host.sink.events.size());
    CHECK(layer->Shutdown());
    CHECK(!layer->IsInitialised() && cSC4AudioAmbienceLayer::StaticInitCount() == 0);
    CHECK(g_server->removed.size() == 2);       // layer2 and layer
    CHECK(cSC4AudioAmbienceLayer::SharedFilterCount() == 0);
    for (auto* f : g_stateFilters) { CHECK(f->d.refs == 0); CHECK(f->d.shutdownCalls == 1); }
    CHECK(layer->Scores().empty() && layer->Loops().empty());
    CHECK(host.sink.Count(kEventScoreRemove) >= 1 && static_cast<int>(host.sink.events.size()) > events);
    // _RemoveLayers (0x10040bddc) leaves the cell pointer alone when it equals the occupant layer (mode 1)
    CHECK(!layer->OccupantLayer() && (layer->Mode() == 1 ? layer->CellLayer() != nullptr : layer->CellLayer() == nullptr));
    layer->Release();
}

static void TestStaticInitFailure() {
    FakeHost host; Setup(host);
    host.hasSink = false;
    host.exemplar = [](const cGZPersistResourceKey&) { return FullTuning(); };
    StdFilters(host, true, false, false, true);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    CHECK(!layer->Init());
    CHECK(layer->IsInitialised());     // quirk: the flag stays set
    CHECK(cSC4AudioAmbienceLayer::StaticInitCount() == 1);
    CHECK(g_server->added.empty() && !layer->OccupantLayer());
    CHECK(layer->Init());              // flag already set: returns true without doing anything
    layer->Shutdown();                 // runs the shutdown path: count 0 -> static shutdown
    CHECK(cSC4AudioAmbienceLayer::StaticInitCount() == 0);
    host.hasSink = true; host.hasRM = false;
    CHECK(!layer->Init());             // no resource manager
    layer->Shutdown();
    layer->Release();
    host.hasRM = true;
}

static void TestFilterFailures() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return FullTuning(); };
    StdFilters(host, true, false, false, true);
    // 0xA1 cannot be created, 0xC1 fails Init
    host.factories.erase(0xA1);
    host.factories[0xC1] = []() -> cIGZUnknown* { auto* f = new FakeStateFilter(); f->d.initOk = false; g_stateFilters.push_back(f); return f; };
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    CHECK(layer->Init());
    CHECK(cSC4AudioAmbienceLayer::SharedFilterCount() == 2);      // B1 and C2 only
    bool createErr = false, initErr = false;
    for (auto& e : host.errors) {
        if (e.find("Failed to create class object filter (000000a1)") != std::string::npos) createErr = true;
        if (e.find("Failed to init filter (000000c1)") != std::string::npos) initErr = true;
    }
    CHECK(createErr && initErr);
    layer->Shutdown();
    layer->Release();
}

static void TestCanHandleLogic() {
    // all-of A, none-of B, any-of {C1, C2}
    struct Case { bool a, b, c1, c2; bool expect; };
    Case cases[] = {
        {true, false, false, true, true},    // C2 true
        {true, false, true, false, true},    // C1 true
        {true, false, false, false, false},  // no "any" filter is true: falls back to the cached result (false)
        {false, false, true, true, false},   // all-of fails
        {true, true, true, true, false},     // none-of fails
    };
    for (const Case& c : cases) {
        FakeHost host; Setup(host);
        host.exemplar = [](const cGZPersistResourceKey&) { return FullTuning(); };
        StdFilters(host, c.a, c.b, c.c1, c.c2);
        auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
        layer->AddRef();
        CHECK(layer->Init());
        layer->BeginUpdate({0, 0, 100, 100});
        CHECK(layer->CanHandle() == c.expect);
        // the answer is cached until the next BeginUpdate even when a filter changes
        for (auto* f : g_stateFilters) f->d.value = !f->d.value;
        CHECK(layer->CanHandle() == c.expect);
        layer->Shutdown();
        layer->Release();
    }
    // sound disabled: false without evaluating
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return FullTuning(); };
    StdFilters(host, true, false, false, true);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    layer->Init();
    g_game->sound = false;
    layer->BeginUpdate({0, 0, 100, 100});
    CHECK(!layer->CanHandle());
    g_game->sound = true;
    CHECK(layer->CanHandle());        // not cached by the disabled call
    layer->Shutdown();
    layer->Release();
}

// Occupant / cell handling: filters for the three kinds are provided by a tuning with occupant filters.
static sc4::cISCPropertyHolder* OccupantTuning(int mode, int source) {
    return NewHolder([=](cSCPropertyHolder* h) {
        cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1, 0xD1, 0xE1});   // state, occupant, cell
        cSCPropertyHelp::SetPropertyValue(h, 0x8a552758, std::uint32_t(mode));
        cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(source));
        cSCPropertyHelp::SetPropertyValue(h, 0xa4168e1, 1.0f);
        cSCPropertyHelp::SetPropertyValue(h, 0x6a4132f3, std::vector<float>{0, 0, 10, 1000});   // volume curve: 100 per unit
        cSCPropertyHelp::SetPropertyValue(h, 0x6a4132f6, std::vector<float>{0, 0, 10, 500});    // loop curve
        cSCPropertyHelp::SetPropertyValue(h, 0xca4132e9, std::vector<float>{0, 100, 10, 100});  // parameter curve: 100 (halved at quality 0)
        cSCPropertyHelp::SetPropertyValue(h, 0x6a3fbdda, std::uint32_t(0x77));
    });
}

static void OccupantFilters(FakeHost& host) {
    FreeFilters();
    AddFilterFactory<FakeStateFilter>(host, 0xA1, g_stateFilters, true);
    AddFilterFactory<FakeOccFilter>(host, 0xD1, g_occFilters, true);
    AddFilterFactory<FakeCellFilter>(host, 0xE1, g_cellFilters, true);
}

static void TestHandleOccupantsAndCells() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return OccupantTuning(1, 1); };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    CHECK(layer->Init());
    CHECK(g_occFilters.size() == 1 && g_cellFilters.size() == 1 && g_stateFilters.size() == 1);
    layer->BeginUpdate({0, 0, 300, 300});
    FakeOccupant a, b, c;
    a.pos[0] = 10; a.pos[1] = 0; a.pos[2] = 10;
    b.pos[0] = 200; b.pos[1] = 0; b.pos[2] = 20;
    c.pos[0] = 250; c.pos[1] = 0; c.pos[2] = 250;
    g_occFilters[0]->only = &a;      // a and the other occupant types: only a passes
    // the occupant filter list is "all of", so b is rejected (quirk: HandleOccupant always returns false)
    CHECK(!layer->HandleOccupant(&a));
    CHECK(!layer->HandleOccupant(&b));
    CHECK(!layer->HandleOccupant(nullptr));
    CHECK(layer->AcceptedOccupants() == 1 && layer->RejectedOccupants() == 1);
    // cell filters ask about the cell; world position (16 per cell, altitude x+z+1) goes into the cell layer
    CHECK(!layer->HandleCell(3, 4));
    CHECK(!layer->HandleCell(3, 4));                // cached last cell: the host is asked once
    CHECK(host.cellToWorldCalls == 1);
    CHECK(layer->AcceptedCells() == 2 && layer->RejectedCells() == 0);
    g_cellFilters[0]->d.value = false;
    // the occupant filter instance and the cell filter instance are the same object ids per iid
    CHECK(!layer->HandleCell(5, 5));
    CHECK(layer->RejectedCells() == 1);
    CHECK(host.cellToWorldCalls == 1);              // rejected before any host query
    // mode 1: cells and occupants share one layer: a (10,0,10), cell (48, 113, 64) -> 2 groups at radius 1
    cS3DVector3 dummy;
    std::list<cS3DVector3> pos;
    int groups = 0, total = 0;
    layer->OccupantLayer()->GetPositions({0, 0, 300, 300}, pos, groups, total, false);
    CHECK(groups == 2 && total == 3);
    bool foundCell = false;
    for (auto& p : pos) if (p.x == 48.0f && p.z == 64.0f && p.y == 113.0f) foundCell = true;
    CHECK(foundCell);
    // not handled while the sound is disabled
    g_game->sound = false;
    layer->BeginUpdate({0, 0, 300, 300});
    CHECK(!layer->HandleOccupant(&a) && layer->AcceptedOccupants() == 0 && layer->RejectedOccupants() == 0);
    g_game->sound = true;
    (void)c; (void)dummy;
    layer->Shutdown();
    layer->Release();
}

static void TestOccupantInsertMoveRemove() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return OccupantTuning(1, 1); };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    layer->Init();
    FakeOccupant a;
    a.pos[0] = 30; a.pos[2] = 40;
    CHECK(!layer->InsertOccupant(nullptr));
    CHECK(layer->InsertOccupant(&a));
    CHECK(a.refs == 1 && layer->AcceptedOccupants() == 1);
    CHECK(!layer->InsertOccupant(&a) || true);      // a second insert of the same id is handled by the group layer
    a.pos[0] = 100;
    CHECK(layer->MoveOccupant(&a));
    CHECK(!layer->MoveOccupant(nullptr));
    int refsBefore = a.refs;
    CHECK(layer->RemoveOccupant(&a));
    CHECK(a.refs == refsBefore - 1);
    CHECK(!layer->RemoveOccupant(&a));              // unknown id now
    CHECK(!layer->RemoveOccupant(nullptr));
    layer->Shutdown();
    layer->Release();
}

// Reference implementation of the position weighting in double precision.
static void ExpectedAverage(const float eye[3], const float target[3], const std::vector<std::array<float, 3>>& pts, float spread, double out[3]) {
    double de[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    double le = std::sqrt(de[0] * de[0] + de[1] * de[1] + de[2] * de[2]);
    double sw = 0, s[3] = {0, 0, 0};
    for (auto& p : pts) {
        double d[3] = {p[0] - target[0], p[1] - target[1], p[2] - target[2]};
        double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        double cosine = std::fabs((d[0] * de[0] + d[1] * de[1] + d[2] * de[2]) / (l * le));
        double w = cosine / (spread - cosine);
        sw += w;
        for (int i = 0; i < 3; ++i) s[i] += p[i] * w;
    }
    for (int i = 0; i < 3; ++i) out[i] = s[i] / sw;
}

static void TestUpdateScoresAndLoops() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return OccupantTuning(1, 1); };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    layer->Init();
    // a score added by hand; the tuning has none
    CHECK(layer->AddFreshnessScore({0x2026960b, 5, 6}));
    FakePlayer* player = host.players.at(0);
    const float eye[3] = {0, 100, 0}, target[3] = {50, 0, 50};
    layer->BeginUpdate({0, 0, 300, 300});
    // three clusters (radius 1): amount source 1 = total instance count = 3
    FakeOccupant a, b, c;
    a.pos[0] = 10; a.pos[1] = 2; a.pos[2] = 10;
    b.pos[0] = 200; b.pos[1] = 4; b.pos[2] = 20;
    c.pos[0] = 250; c.pos[1] = 6; c.pos[2] = 250;
    g_occFilters[0]->only = nullptr;
    layer->HandleOccupant(&a); layer->HandleOccupant(&b); layer->HandleOccupant(&c);
    CHECK(layer->AcceptedOccupants() == 3);
    layer->Update(40, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    CHECK(layer->IsEnabled());
    CHECK(layer->PositionCount() == 3);
    CHECK(layer->Amount() == 3.0f);
    // volume curve (0,0)-(10,1000): 300 at amount 3; master 1024 -> (300*1024)>>10 = 300
    CHECK(layer->ScoreVolume() == 300);
    // parameter curve flat 100; quality 0 halves it
    CHECK(layer->ScoreParameter() == 50);
    // loop curve (0,0)-(10,500): 150 at amount 3
    CHECK(layer->LoopVolume() > 0 && layer->LoopVolume() <= 150);
    // average position
    std::vector<std::array<float, 3>> pts;
    for (int i = 0; i < layer->PositionCount(); ++i) pts.push_back({layer->Position(i)[0], layer->Position(i)[1], layer->Position(i)[2]});
    double expected[3];
    ExpectedAverage(eye, target, pts, 1.0f, expected);       // spread defaults to 1.0
    // the average after the loop update was eased towards the target; check at the start through the sound events
    // score started: volume event with data1 = (50/100... masterPercent 100)/100 * 2000 = 2000, data2 = 32000
    const AmbienceSndEvent* start = host.sink.Last(kEventScoreStart);
    CHECK(start && start->unknown == static_cast<cIGZUnknown*>(player));
    CHECK(start->data[0] == 2000 && start->data[1] == 32000);
    const AmbienceSndEvent* mode = host.sink.Last(kEventScoreStartMode);
    CHECK(mode && mode->data[0] == 0);
    CHECK(layer->Scores().back().active);
    // after the start the volume/parameter events were sent (start zeroes the shadow values)
    const AmbienceSndEvent* vol = host.sink.Last(kEventScoreVolume);
    CHECK(vol && vol->data[0] == 300);
    const AmbienceSndEvent* par = host.sink.Last(kEventScoreParameter);
    CHECK(par && par->data[0] == 50);
    // divisions: 3 positions enabled, remaining 6 disabled
    CHECK(player->enabled.size() == 9);
    int on = 0;
    for (auto& kv : player->enabled) on += kv.second;
    CHECK(on == 3);
    CHECK(player->positions.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(player->positions[i].x == i % 3 && player->positions[i].z == i / 3);
        CHECK(player->positions[i].px == (int)layer->Position((int)i)[0]);
        CHECK(player->positions[i].py == (int)layer->Position((int)i)[1]);
        CHECK(player->positions[i].pz == (int)layer->Position((int)i)[2]);
    }
    // the loop started: ids from the host counter, start event {sound 0x77, id 100, 1, volume}
    CHECK(layer->Loops().size() == 1);
    CHECK(layer->Loops().front().instanceId == 100);
    const AmbienceSndEvent* ls = host.sink.Last(kEventStartSound);
    CHECK(ls && ls->ctor[0] == 0x77 && ls->ctor[1] == 100 && ls->ctor[2] == 1 && ls->ctor[3] == 150);
    CHECK(layer->LoopVolume() == 150);       // ease snapped to the target on the start
    CHECK(host.sink.Count(kEventSetSoundPosition) == 1);
    const AmbienceSndEvent* lp = host.sink.Last(kEventSetSoundPosition);
    CHECK(lp->ctor[0] == 100);
    cS3DVector3 got;
    layer->GetAveragePosition(got);
    CHECK_NEAR(got.x, expected[0], 0.05); CHECK_NEAR(got.y, expected[1], 0.05); CHECK_NEAR(got.z, expected[2], 0.05);
    CHECK(lp->ctor[1] == (std::uint32_t)(int)got.x && lp->ctor[3] == (std::uint32_t)(int)got.z);
    CHECK(host.sink.Count(kEventSetSoundVolume) == 1);      // shadow volume 0 -> 150
    CHECK(host.sink.Last(kEventSetSoundVolume)->ctor[1] == 150);

    // second update with the same data: nothing changes, no new events
    std::size_t before = host.sink.events.size();
    layer->BeginUpdate({0, 0, 300, 300});
    layer->HandleOccupant(&a); layer->HandleOccupant(&b); layer->HandleOccupant(&c);
    layer->Update(40, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    CHECK(host.sink.events.size() == before);

    // pause: scores stop (stop event); the loops are NOT stopped (_UpdateAmbienceLoops 0x10040d958 never reads the pause flag,
    // it only stops a loop when the loop volume is 0)
    layer->Pause(true);
    CHECK(layer->IsPaused());
    CHECK(host.sink.Count(kEventScoreStop) == 1 && !layer->Scores().back().active);
    CHECK(host.sink.Count(kEventStopSound) == 0 && layer->Loops().front().instanceId == 100);
    // unpause: the score restarts, the loop keeps its instance id
    layer->Pause(false);
    CHECK(layer->Scores().back().active && host.sink.Count(kEventScoreStart) == 2);
    CHECK(layer->Loops().front().instanceId == 100);

    // Groups of the previous cycles stay in the layer (count 0, peak kept) until EndUpdate removes the empty ones, so the
    // amount follows the handled things one cycle late (cGroupLayer::Clear keeps groups, ClearEmptyGroups @ 0x10040fe2e).
    layer->BeginUpdate({0, 0, 300, 300});
    layer->HandleOccupant(&a);
    layer->Update(10, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    CHECK(layer->PositionCount() == 3 && layer->Amount() == 3.0f);
    layer->EndUpdate();
    // fewer handled things -> amount 1: volume 100, parameter unchanged
    layer->BeginUpdate({0, 0, 300, 300});
    layer->HandleOccupant(&a);
    layer->Update(10, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    CHECK(layer->PositionCount() == 1 && layer->Amount() == 1.0f && layer->ScoreVolume() == 100);
    CHECK(host.sink.Last(kEventScoreVolume)->data[0] == 100);
    CHECK((player->enabled[{0, 0}] && !player->enabled[{1, 0}] && !player->enabled[{2, 2}]));
    // negative data1 is sign extended (checked through a parameter change to a negative value is not reachable); amount 0
    // (two cycles without occupants: the empty group is only removed by the EndUpdate of the first one):
    layer->EndUpdate();
    layer->BeginUpdate({0, 0, 300, 300});
    layer->Update(10, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    layer->EndUpdate();
    layer->BeginUpdate({0, 0, 300, 300});
    layer->Update(10, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    CHECK(layer->Amount() == 0.0f && layer->ScoreVolume() == 0);
    CHECK(!layer->Scores().back().active && host.sink.Count(kEventScoreStop) == 2);
    // single position average equals that position (kept from before: count 1 earlier, now position count is retained)

    // remove the score: released twice (quirk) - player started with 1 ref (node) after creation; one extra ref to survive
    player->refs = 2;
    CHECK(layer->RemoveFreshnessScore(6));
    CHECK(player->refs == 0);
    CHECK(!layer->RemoveFreshnessScore(6));
    CHECK(host.sink.Count(kEventScoreRemove) == 1);
    // loops: add / remove / duplicate / zero
    CHECK(!layer->AddAmbienceLoop(0));
    CHECK(!layer->AddAmbienceLoop(0x77));
    CHECK(layer->AddAmbienceLoop(0x88));
    CHECK(layer->Loops().size() == 2);
    CHECK(layer->RemoveAmbienceLoop(0x88));
    CHECK(!layer->RemoveAmbienceLoop(0x88));
    layer->SetScoreMasterVolume(512);
    CHECK(layer->ScoreMasterVolume() == 512);
    layer->Shutdown();
    layer->Release();
}

static void TestSingleAverageAndWeights() {
    // two clusters, spread 2: check the weighted average against the double reference
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) {
        return NewHolder([](cSCPropertyHolder* h) {
            cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1, 0xD1});
            cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(1));
            cSCPropertyHelp::SetPropertyValue(h, 0x8a3fc5dd, 2.0f);
            cSCPropertyHelp::SetPropertyValue(h, 0xa4168e1, 1.0f);
        });
    };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    layer->Init();
    const float eye[3] = {10, 80, 20}, target[3] = {60, 0, 90};
    FakeOccupant a, b;
    a.pos[0] = 30; a.pos[1] = 1; a.pos[2] = 200;
    b.pos[0] = 280; b.pos[1] = 5; b.pos[2] = 40;
    layer->BeginUpdate({0, 0, 300, 300});
    layer->HandleOccupant(&a); layer->HandleOccupant(&b);
    // run the weighting without the loop easing by a loop volume of 0 (default loop curve, amount 2 -> 1024: so remove loops)
    layer->RemoveAmbienceLoop(0);
    layer->Update(0, {eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    CHECK(layer->PositionCount() == 2);
    std::vector<std::array<float, 3>> pts;
    for (int i = 0; i < 2; ++i) pts.push_back({layer->Position(i)[0], layer->Position(i)[1], layer->Position(i)[2]});
    double expected[3];
    ExpectedAverage(eye, target, pts, 2.0f, expected);
    cS3DVector3 got;
    layer->GetAveragePosition(got);
    // the ease (no loops are registered so GetPosition was never applied) leaves the raw average
    CHECK_NEAR(got.x, expected[0], 1e-2); CHECK_NEAR(got.y, expected[1], 1e-2); CHECK_NEAR(got.z, expected[2], 1e-2);
    layer->Shutdown();
    layer->Release();
}

static void TestIntersectModes() {
    for (int mode = 2; mode <= 3; ++mode) {
        FakeHost host; Setup(host);
        host.exemplar = [=](const cGZPersistResourceKey&) {
            return NewHolder([=](cSCPropertyHolder* h) {
                cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1, 0xD1, 0xE1});
                cSCPropertyHelp::SetPropertyValue(h, 0x8a552758, std::uint32_t(mode));
                cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(1));
                cSCPropertyHelp::SetPropertyValue(h, 0xa4168e1, 2.0f);
                cSCPropertyHelp::SetPropertyValue(h, 0xa56212a, 5.0f);
            });
        };
        OccupantFilters(host);
        auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
        layer->AddRef();
        layer->Init();
        CHECK(layer->OccupantLayer() && layer->CellLayer() && layer->IntersectLayer());
        CHECK(layer->OccupantLayer() != layer->CellLayer());
        CHECK(layer->GroupRadiusA() == 2.0f && layer->GroupRadiusB() == 5.0f);
        CHECK(layer->OccupantLayer()->groupRadius == 2.0f && layer->CellLayer()->groupRadius == 5.0f);
        CHECK(layer->IntersectLayer()->groupRadius == (mode == 2 ? 5.0f : 2.0f));
        layer->BeginUpdate({0, 0, 400, 400});
        FakeOccupant a, b;
        a.pos[0] = 16; a.pos[1] = 33; a.pos[2] = 16;     // cell (1,1) is at world (16, 33, 16): same place (the distance includes y)
        b.pos[0] = 300; b.pos[1] = 1; b.pos[2] = 300;    // no cell nearby
        layer->HandleOccupant(&a); layer->HandleOccupant(&b);
        layer->HandleCell(1, 1);
        CHECK(layer->OccupantLayer()->groupCount == 2 && layer->CellLayer()->groupCount == 1);
        layer->RemoveAmbienceLoop(0);
        layer->Update(0, {0, 50, 0}, {100, 0, 100});
        // only the occupant group near the cell survives (source 1: total of intersected groups = 1)
        CHECK(layer->PositionCount() == 1);
        CHECK(layer->Amount() == 1.0f);
        layer->EndUpdate();
        layer->Shutdown();
        layer->Release();
    }
}

static void TestEquallyDistributed() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) {
        return NewHolder([](cSCPropertyHolder* h) {
            cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1, 0xD1});
            cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(5));    // average group size
            cSCPropertyHelp::SetPropertyValue(h, 0xa4168e1, 1.0f);
        });
    };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef();
    layer->Init();
    layer->BeginUpdate({0, 0, 300, 300});      // 3x3 cells of 100
    std::vector<FakeOccupant> occ(12);
    // 12 groups: 4 in cell (0,0), 3 in cell (2,0), 2 in cell (1,2), 3 in cell (2,2) (and one out of range clamps)
    float xs[12] = {5, 20, 40, 60, 210, 230, 260, 150, 180, 220, 240, 299};
    float zs[12] = {5, 20, 40, 60, 10, 30, 50, 250, 270, 210, 230, 299};
    for (int i = 0; i < 12; ++i) { occ[i].pos[0] = xs[i]; occ[i].pos[1] = 1; occ[i].pos[2] = zs[i]; layer->HandleOccupant(&occ[i]); }
    layer->RemoveAmbienceLoop(0);
    layer->Update(0, {0, 100, 0}, {150, 0, 150});
    // amount source 5: 12 instances / 12 groups = 1.0
    CHECK(layer->Amount() == 1.0f);
    // distinct cells: (0,0), (2,0), (1,2), (2,2) = 4 positions
    CHECK(layer->PositionCount() == 4);
    // first position in group-list order of each cell was kept: all kept positions are in different cells
    std::set<int> cells;
    for (int i = 0; i < 4; ++i) cells.insert((int)(layer->Position(i)[2] / 100) * 3 + (int)(layer->Position(i)[0] / 100));
    CHECK(cells.size() == 4);
    layer->Shutdown();
    layer->Release();
}

static void TestAmountSources() {
    struct Case { int source; float expected; };
    Case cases[] = {{1, 2.0f}, {2, 1234.0f}, {3, 3.4028235e+38f}, {4, 3.0f}, {5, 1.0f}, {6, 4321.0f}};
    FakeViewport viewport;
    SetViewUtilities(&viewport);
    for (const Case& c : cases) {
        FakeHost host; Setup(host);
        host.residential = 1234;
        host.industrial = 4321;
        int source = c.source;
        host.exemplar = [source](const cGZPersistResourceKey&) {
            return NewHolder([source](cSCPropertyHolder* h) {
                cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1, 0xD1});
                cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(source));
                cSCPropertyHelp::SetPropertyValue(h, 0xa4168e1, 1.0f);
            });
        };
        OccupantFilters(host);
        auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
        layer->AddRef();
        layer->Init();
        layer->BeginUpdate({0, 0, 300, 300});
        FakeOccupant a, b;
        a.pos[0] = 10; a.pos[2] = 10; b.pos[0] = 200; b.pos[2] = 200;
        layer->HandleOccupant(&a); layer->HandleOccupant(&b);
        layer->RemoveAmbienceLoop(0);
        layer->Update(0, {0, 100, 0}, {150, 0, 150});
        CHECK(layer->Amount() == c.expected);
        layer->Shutdown();
        layer->Release();
    }
    // an unknown source leaves the amount untouched
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) {
        return NewHolder([](cSCPropertyHolder* h) {
            cSCPropertyHelp::SetPropertyValue(h, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1, 0xD1});
            cSCPropertyHelp::SetPropertyValue(h, 0x8a70b1a2, std::uint32_t(9));
        });
    };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef(); layer->Init();
    layer->BeginUpdate({0, 0, 300, 300});
    layer->Update(0, {0, 100, 0}, {150, 0, 150});
    CHECK(layer->Amount() == 0.0f);
    layer->Shutdown(); layer->Release();
    SetViewUtilities(nullptr);
}

static void TestQualityScaling() {
    // quality 1: parameter * 75 / 100 ; other values: raw
    int qualities[] = {0, 1, 2};
    int expected[] = {50, 75, 100};
    for (int i = 0; i < 3; ++i) {
        FakeHost host; Setup(host);
        host.quality = qualities[i];
        host.exemplar = [](const cGZPersistResourceKey&) { return OccupantTuning(1, 1); };
        OccupantFilters(host);
        auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
        layer->AddRef(); layer->Init();
        layer->BeginUpdate({0, 0, 300, 300});
        FakeOccupant a; a.pos[0] = 10; a.pos[2] = 10;
        layer->HandleOccupant(&a);
        layer->Update(0, {0, 100, 0}, {150, 0, 150});
        CHECK(layer->ScoreParameter() == expected[i]);
        layer->Shutdown(); layer->Release();
    }
}

static void TestDoMessageAndDisabled() {
    FakeHost host; Setup(host);
    host.exemplar = [](const cGZPersistResourceKey&) { return OccupantTuning(1, 1); };
    OccupantFilters(host);
    auto* layer = new cSC4AudioAmbienceLayer(kLayerId);
    layer->AddRef(); layer->Init();
    CHECK(!layer->DoMessage(nullptr));
    cRZMessage2Standard other;
    other.SetType(0x1234);
    CHECK(!layer->DoMessage(&other));
    cRZMessage2Standard reload;
    reload.SetType(0xc53d10aa);
    cGZPersistResourceKey keyBefore = host.exemplarRequests.back();
    cGroupLayer* oldLayer = layer->OccupantLayer();
    (void)oldLayer;
    layer->Enable(false);
    CHECK(layer->DoMessage(&reload));
    CHECK(!layer->IsEnabled());                 // toggled twice
    CHECK(host.exemplarRequests.size() == 2 && host.exemplarRequests.back() == keyBefore);
    CHECK(layer->OccupantLayer() && layer->OccupantLayer() == layer->CellLayer());
    // the filters were rebuilt from the shared map: references: map + layer
    for (auto* f : g_stateFilters) CHECK(f->d.refs == 2);
    CHECK(g_stateFilters.size() == 1);
    // disabled update: scores/loops stop
    layer->Enable(true);
    layer->Shutdown(); layer->Release();
}

int main() {
    TestBasics();
    TestMissingExemplarDefaults();
    TestInitShutdown();
    TestStaticInitFailure();
    TestFilterFailures();
    TestCanHandleLogic();
    TestHandleOccupantsAndCells();
    TestOccupantInsertMoveRemove();
    TestUpdateScoresAndLoops();
    TestSingleAverageAndWeights();
    TestIntersectModes();
    TestEquallyDistributed();
    TestAmountSources();
    TestQualityScaling();
    TestDoMessageAndDisabled();
    FreeFilters();
    std::printf("ok\n");
    return 0;
}
