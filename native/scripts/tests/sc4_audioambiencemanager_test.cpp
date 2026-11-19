// Test for cSC4AudioAmbienceManager (src/sc4audio/AudioAmbienceManager.*): construction and COM
// identity, the framework/sound/message wiring of Init and Shutdown, the tuning exemplar and layer
// creation, the attention rectangle, the three-stage tick walk (cells and occupants reach the layers),
// the occupant messages, alarms, lot condition changes, automaton and railway-crossing messages, the
// score-event dispatch and the message dispatch. Expected values are derived by hand from the
// decompiled formulas (see the comments).
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio/interfaces/cIFreshPlayer.h"
#include "audio/interfaces/cIGZSndEventStd.h"
#include "audio/interfaces/cIGZSndSys.h"
#include "sc4/Lot.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4audio/AudioAmbienceManager.h"

using namespace SC4Audio;
using sc4::cSCPropertyHelp;
using sc4::cSCPropertyHolder;

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { \
    std::printf("FAIL line %d: %s == %s (%lld vs %lld)\n", __LINE__, #a, #b, (long long)_a, (long long)_b); std::exit(1); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (eps))) { \
    std::printf("FAIL line %d: %s=%g vs %g\n", __LINE__, #a, _a, _b); std::exit(1); } } while (0)

// ---------------------------------------------------------------- reachability
// The fakes below count references and are never freed (the "objects" stay reachable from these lists),
// so LeakSanitizer stays quiet like in the other audio tests.
static std::vector<void*> g_keepAlive;
static std::vector<struct FakeFilter*> g_ownedFilters;   // heap filters, freed at the end of main

// ---------------------------------------------------------------- messages / events
struct FakeMessage : cIGZMessage2Standard {
    std::uint32_t type = 0;
    bool has[4] = {false, false, false, false};
    std::uint64_t data[4] = {0, 0, 0, 0};
    cIGZUnknown* unknown = nullptr;
    void* void1 = nullptr;
    int refs = 1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZMessage2Standard || iid == 1) {
            *out = static_cast<cIGZMessage2Standard*>(this);
            ++refs;
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t t) override { type = t; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
    bool GetHasData1() const override { return has[0]; }
    std::uint64_t GetData1() const override { return data[0]; }
    void* GetVoid1() const override { return void1; }
    void SetData1(std::uint64_t v) override { has[0] = true; data[0] = v; }
    void SetVoid1(void* v) override { void1 = v; }
    bool GetHasData2() const override { return has[1]; }
    std::uint64_t GetData2() const override { return data[1]; }
    void* GetVoid2() const override { return nullptr; }
    void SetData2(std::uint64_t v) override { has[1] = true; data[1] = v; }
    void SetVoid2(void*) override {}
    bool GetHasData3() const override { return has[2]; }
    std::uint64_t GetData3() const override { return data[2]; }
    void* GetVoid3() const override { return nullptr; }
    void SetData3(std::uint64_t v) override { has[2] = true; data[2] = v; }
    void SetVoid3(void*) override {}
    bool GetHasData4() const override { return has[3]; }
    std::uint64_t GetData4() const override { return data[3]; }
    void* GetVoid4() const override { return nullptr; }
    void SetData4(std::uint64_t v) override { has[3] = true; data[3] = v; }
    void SetVoid4(void*) override {}
    bool GetHasString() const override { return false; }
    bool GetString(std::uint32_t, void**) override { return false; }
    void SetString(cIGZString*) override {}
    bool GetHasIGZUnknown() const override { return unknown != nullptr; }
    cIGZUnknown* GetIGZUnknown() override { return unknown; }
    void SetIGZUnknown(cIGZUnknown* value) override { unknown = value; }
    bool GetHasExtra() const override { return false; }
    bool GetExtra(std::uint32_t, void**) override { return false; }
    void SetExtra(cIGZSerializable*) override {}
};

struct FakeEvent : cIGZSndEventStd {
    std::uint32_t id = 0;
    FakeMessage message;
    bool queryable = true;
    int refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (queryable && (iid == kGZIID_cIGZMessage2Standard || iid == 1)) {
            *out = static_cast<cIGZMessage2Standard*>(&message);
            ++message.refs;
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool GetHasEventId() const override { return true; }
    std::uint32_t GetEventId() const override { return id; }
    void SetEventId(std::uint32_t value) override { id = value; }
    cIGZMessage2Standard* AsMessage2Standard() override { return &message; }
};

// ---------------------------------------------------------------- framework pieces
struct FakeServer : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return false; }
    bool MessagePost(cIGZMessage2*, bool) override { return false; }
    bool AddNotification(cIGZMessageTarget2* t, std::uint32_t id) override { added.push_back({t, id}); return true; }
    bool RemoveNotification(cIGZMessageTarget2* t, std::uint32_t id) override { removed.push_back({t, id}); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return false; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 2; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeSndSys : cIGZSndSys {
    std::vector<cIGZSndEventHandler*> handlers;
    std::vector<std::array<std::uint32_t, 5>> posted;   // id + four payload words, in PostEventIds order

    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    cIGZSnd3DListener* AsIGZSnd3DListener() override { return nullptr; }
    cIGZSnd3DListenerEx* AsIGZSnd3DListenerEx() override { return nullptr; }
    bool DoEvent(cIGZSndEventStd*) override { return false; }
    bool DoEventIds(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
    bool DoEventUnknown(std::uint32_t, cIGZUnknown*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
    bool DoEventString(std::uint32_t, cIGZString*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
    bool PostEvent(cIGZSndEventStd*) override { return false; }
    bool PostEventIds(std::uint32_t id, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) override {
        posted.push_back({id, a, b, c, d});
        return true;
    }
    bool PostEventUnknown(std::uint32_t, cIGZUnknown*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
    bool PostEventString(std::uint32_t, cIGZString*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
    bool EventCreate(std::uint32_t, void**) override { return false; }
    bool EventCreateWithData(std::uint32_t, std::uint32_t, void**, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
    bool RegisterEventHandler(cIGZSndEventHandler* h) override { handlers.push_back(h); return true; }
    bool UnregisterEventHandler(cIGZSndEventHandler* h) override {
        for (auto it = handlers.begin(); it != handlers.end(); ++it) if (*it == h) { handlers.erase(it); return true; }
        return false;
    }
    bool Update(std::uint32_t) override { return true; }
    bool GetInitFlag() const override { return true; }
    void SetDataDirectory(const cIGZString&) override {}
    cIGZString* DataDirectory() override { return nullptr; }
    bool GetEnabledFlag() const override { return true; }
    void SetEnabledFlag(bool) override {}
    bool SetExtensionDefaultDataSourceClassId(const cIGZString&, std::uint32_t) override { return false; }
    bool GetExtensionDefaultDataSourceClassId(const cIGZString&, std::uint32_t&) override { return false; }
    std::uint32_t GetNumExtensionDefaultDataSourceClassIdMappings() override { return 0; }
    bool GetExtensionDefaultDataSourceClassIdMappingClassId(std::uint32_t, std::uint32_t&) override { return false; }
    bool GetExtensionDefaultDataSourceClassIdMappingExtension(std::uint32_t, cIGZString&) override { return false; }
    std::int32_t GetMasterVolume() const override { return 1024; }
    bool SetMasterVolume(std::int32_t) override { return true; }
    bool RegisterTarget(cIGZSndUpdateTarget*, SndCallbackInterval) override { return true; }
    bool UnregisterTarget(cIGZSndUpdateTarget*) override { return true; }
    bool EnumerateTargets(UpdateTargetFunction, std::uint32_t, void*) override { return true; }
    bool EnableUpdates() override { return true; }
    bool DisableUpdates() override { return true; }
    bool RegisterRenderer(cIGZSndRenderer*) override { return true; }
    bool UnregisterRenderer(cIGZSndRenderer*) override { return true; }
    bool IsRegisteredRenderer(cIGZSndRenderer*) override { return false; }
    std::uint32_t NumRenderers() override { return 0; }
    bool FindPlayingGZSnd(std::uint32_t, std::uint32_t, void**) override { return false; }
    bool IsSndPlaying(std::uint32_t) override { return false; }
    cIGZCriticalSection* GetCriticalSection() override { return nullptr; }
    bool GetSndPropertyCacheByIid(cIGZSndPropertyCache**) override { return false; }
    cIGZSndPropertyCache* GetSndPropertyCache() override { return nullptr; }
    bool GetSndInstanceCacheByIid(cIGZSndInstanceCache**) override { return false; }
    cIGZSndInstanceCache* GetSndInstanceCache() override { return nullptr; }
    void AddToMainlineReleaseList(cIGZUnknown*) override {}
    void Panic() override {}
    void SetTestMode(bool) override {}
    bool GetTestMode() const override { return false; }
    std::uint32_t GetSoundSystemThreadId() override { return 1; }
};

struct FakeGame : cIGameAudioContext {
    AudioPreferences prefs{true, 800};
    bool haveView = true;
    ListenerView view;
    AudioPreferences GetAudioPreferences() override { return prefs; }
    bool IsSimulationPaused() override { return false; }
    bool GetListenerView(ListenerView& out) override { out = view; return haveView; }
};

struct FakeViewport : cIViewport {
    int zoom = 2;
    void ProjectPointToViewportCoordinates2D(float[3]) override {}
    std::int32_t ViewportWidth() const override { return 1024; }
    std::int32_t ViewportHeight() const override { return 768; }
    void GetCameraEye(float[3]) const override {}
    void GetCameraTarget(float[3]) const override {}
    std::int32_t GetZoomLevel() const override { return zoom; }
};

// A counted framework object (the audio scape manager stand-in).
struct RefUnknown : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x29e39db4) { *out = static_cast<cIGZUnknown*>(this); ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
};

// ---------------------------------------------------------------- occupants
struct FakeOccupant : sc4::cISC4Occupant {
    sc4::cSCPropertyHolder properties;
    std::uint32_t flags = 0;
    std::uint32_t type = 0x278128a0;
    float pos[3] = {0, 0, 0};
    int refs = 0;
    cIGZUnknown* automaton = nullptr;    // answered for iid 0xa9b40f05
    bool buildingView = false;          // answered for iid 0x87dda3a9
    bool propView = false;              // answered for iid 0xe9793a65

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cISC4Occupant) { *out = static_cast<sc4::cISC4Occupant*>(this); ++refs; return true; }
        if (iid == 0xa9b40f05 && automaton) { *out = automaton; automaton->AddRef(); return true; }
        if (iid == 0x87dda3a9 && buildingView) { *out = static_cast<sc4::cISC4Occupant*>(this); ++refs; return true; }
        if (iid == 0xe9793a65 && propView) { *out = static_cast<sc4::cISC4Occupant*>(this); ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return &properties; }
    std::uint32_t GetType() const override { return type; }
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
    void SetFlag(std::uint32_t flag) override { flags |= flag; }
    void SetAllFlags(std::uint32_t value) override { flags = value; }
    void ClearFlag(std::uint32_t flag) override { flags &= ~flag; }
    bool IsFlagSet(std::uint32_t flag) override { return (flags & flag) != 0; }
    std::uint32_t GetFlags() override { return flags; }
};

struct FakeLot : IAmbienceLot {
    std::int32_t state = 0;
    std::list<sc4::cISC4Occupant*> occupants;
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4LotView) { *out = static_cast<IAmbienceLot*>(this); ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::int32_t State() override { return state; }
    void GetOccupants(std::list<sc4::cISC4Occupant*>& out) override {
        out = occupants;
        for (sc4::cISC4Occupant* o : out) o->AddRef();
    }
};

struct FakeAutomaton : IAmbienceAutomaton {
    sc4::cISC4Occupant* occupant = nullptr;
    float pos[3] = {0, 0, 0};
    std::vector<cIGZMessageTarget2*> callbacks;
    std::uint32_t stateBit = 0;      // FindState answers non-null when state == stateBit
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0xa9b40f05) { *out = static_cast<IAmbienceAutomaton*>(this); ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    sc4::cISC4Occupant* AsOccupant() override { return occupant; }
    void GetPosition(float* xyz) override { xyz[0] = pos[0]; xyz[1] = pos[1]; xyz[2] = pos[2]; }
    void AddCallback(cIGZMessageTarget2* target) override { callbacks.push_back(target); }
    void RemoveCallback(cIGZMessageTarget2* target) override {
        for (auto it = callbacks.begin(); it != callbacks.end(); ++it) if (*it == target) { callbacks.erase(it); return; }
    }
    void* FindState(std::uint32_t state) override { return state == stateBit ? static_cast<void*>(this) : nullptr; }
};

// ---------------------------------------------------------------- occupant manager
struct FakeOccupantManager : sc4::cISC4OccupantManager {
    std::list<sc4::cISC4Occupant*> stored;
    std::vector<std::pair<int, int>> tickCells;
    std::vector<std::pair<int, int>> iterateRects;
    int refs = 0;
    int releaseListCalls = 0;

    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool SetCitySize(float, float) override { return true; }
    void GetOccupantManagerCellSizes(float& x, float& z) const override { x = z = 16.0f; }
    void GetWorldCellCount(int& x, int& z) const override { x = z = 64; }
    void GetOccupantManagerCellCount(int& x, int& z) const override { x = z = 64; }
    void GetOccupantManagerCellBounds(int& x0, int& z0, int& x1, int& z1) const override { x0 = z0 = 0; x1 = z1 = 63; }
    void WorldCellToOccupantManagerCell(int x, int z, int& ox, int& oz) const override { ox = x; oz = z; }
    void OccupantManagerCellToWorldCell(int x, int z, int& ox, int& oz) const override { ox = x; oz = z; }
    void WorldCellToStandardCityCell(int x, int z, int& ox, int& oz) const override { ox = x; oz = z; }
    void StandardCityCellToWorldCell(int x, int z, int& ox, int& oz) const override { ox = x; oz = z; }
    void WorldCellToPosition(int x, int z, float& px, float& pz) const override { px = static_cast<float>(x) * 16.0f; pz = static_cast<float>(z) * 16.0f; }
    void PositionToWorldCell(float px, float pz, int& x, int& z) const override { x = static_cast<int>(px) / 16; z = static_cast<int>(pz) / 16; }
    void OccupantManagerCellToPosition(int x, int z, float& px, float& pz) const override { px = static_cast<float>(x) * 16.0f; pz = static_cast<float>(z) * 16.0f; }
    void PositionToOccupantManagerCell(float px, float pz, int& x, int& z) const override { x = static_cast<int>(px) / 16; z = static_cast<int>(pz) / 16; }
    void StandardCityCellToPosition(int x, int z, float& px, float& pz) const override { px = static_cast<float>(x) * 16.0f; pz = static_cast<float>(z) * 16.0f; }
    void PositionToStandardCityCell(float px, float pz, int& x, int& z) const override {
        x = static_cast<int>(px) / 16;
        z = static_cast<int>(pz) / 16;
    }
    bool InsertOccupant(sc4::cISC4Occupant* occupant, std::uint32_t) override { stored.push_back(occupant); return true; }
    bool RemoveOccupant(sc4::cISC4Occupant* occupant, bool, std::uint32_t) override {
        stored.remove(occupant);
        return true;
    }
    bool RemoveOccupants(int, bool, std::uint32_t) override { return true; }
    bool RemoveOccupants(sc4::cISC4OccupantFilter*, bool, std::uint32_t) override { return true; }
    bool MoveOccupant(sc4::cISC4Occupant*, bool) override { return true; }
    bool IsCellEmpty(int, int) const override { return true; }
    int GetBoundingCells(int, int, int (*)[2], int& count) override { count = 0; return 0; }
    bool FindOccupant(sc4::cISC4Occupant*, bool, int*, int*) override { return false; }
    bool GetFirstOccupantByPosition(sc4::cISC4Occupant*& out, float, float, std::uint32_t) override { out = nullptr; return false; }
    bool GetFirstOccupantByPosition(sc4::cISC4Occupant*& out, float, float, sc4::cISC4OccupantFilter*) override { out = nullptr; return false; }
    bool GetFirstOccupant(sc4::cISC4Occupant*& out, int, int, std::uint32_t) override { out = nullptr; return false; }
    bool GetFirstOccupant(sc4::cISC4Occupant*& out, int, int, sc4::cISC4OccupantFilter*) override { out = nullptr; return false; }
    bool GetFirstOccupantByStandardCityCell(sc4::cISC4Occupant*& out, int, int, std::uint32_t) override { out = nullptr; return false; }
    bool GetFirstOccupantByStandardCityCell(sc4::cISC4Occupant*& out, int, int, sc4::cISC4OccupantFilter*) override { out = nullptr; return false; }
    bool GetFirstOccupantByStandardCityCells(sc4::cISC4Occupant*& out, const int*, const int*, std::uint32_t) override { out = nullptr; return false; }
    bool GetFirstOccupantByStandardCityCells(sc4::cISC4Occupant*& out, const int*, const int*, sc4::cISC4OccupantFilter*) override { out = nullptr; return false; }
    bool GetOccupantsByBBox(sc4::OccupantList&, const float*, const float*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByBBox(sc4::OccupantList&, const float*, const float*, sc4::cISC4OccupantFilter*, std::uint32_t) override { return false; }
    bool GetOccupantsByOccupantManagerCells(sc4::OccupantList&, const int*, const int*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByOccupantManagerCells(sc4::OccupantList&, const int*, const int*, sc4::cISC4OccupantFilter*, std::uint32_t) override { return false; }
    bool GetOccupantsByStandardCityCells(sc4::OccupantList&, const int*, const int*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByStandardCityCells(sc4::OccupantList&, const int*, const int*, sc4::cISC4OccupantFilter*, std::uint32_t) override { return false; }
    bool IterateOccupantsByBBox(sc4::OccupantCallback, void*, const float*, const float*, std::uint32_t) override { return false; }
    bool IterateOccupantsByBBox(sc4::OccupantCallback, void*, const float*, const float*, sc4::cISC4OccupantFilter*) override { return false; }
    bool IterateOccupants(sc4::OccupantCallback callback, void* context, const int*, const int*,
                          sc4::cISC4OccupantFilter* filter) override {
        for (sc4::cISC4Occupant* occupant : stored) {
            if (filter && !filter->IsOccupantIncluded(occupant)) continue;
            if (!callback(occupant, context)) break;
        }
        return true;
    }
    bool IterateOccupants(sc4::OccupantCallback callback, void* context, const int*, const int*, std::uint32_t type) override {
        for (sc4::cISC4Occupant* occupant : stored) {
            if (type != 0 && occupant->GetType() != type) continue;
            if (!callback(occupant, context)) break;
        }
        return true;
    }
    bool IterateOccupantsByStandardCityCell(sc4::OccupantCallback callback, void* context, const int* a, const int* b,
                                            sc4::cISC4OccupantFilter* filter) override {
        iterateRects.push_back({a ? a[0] * 1000 + a[1] : -1, b ? b[0] * 1000 + b[1] : -1});
        for (sc4::cISC4Occupant* occupant : stored) {
            float p[3];
            occupant->GetPosition(p);
            const int cx = static_cast<int>(p[0]) / 16, cz = static_cast<int>(p[2]) / 16;
            if (a && (cx < a[0] || cz < a[1])) continue;
            if (b && (cx > b[0] || cz > b[1])) continue;
            tickCells.push_back({cx, cz});
            if (filter && !filter->IsOccupantIncluded(occupant)) continue;
            if (!callback(occupant, context)) break;
        }
        return true;
    }
    bool IterateOccupantsByStandardCityCell(sc4::OccupantCallback, void*, const int*, const int*, std::uint32_t) override { return false; }
    void ReleaseOccupantList(sc4::OccupantList& list) override {
        ++releaseListCalls;
        for (sc4::cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
};

// The two occupant filters the manager keeps (+0x48 tick walk, +0x190 industrial population): the
// recovered cSC4AudioAmbienceManagerOccupantFilter accepts everything and so does this fake.
struct FakeOccupantFilter final : sc4::cISC4OccupantFilter {
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == 0x46a80026) { *out = static_cast<sc4::cISC4OccupantFilter*>(this); AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (--refs <= 0) { delete this; return 0; }
        return static_cast<std::uint32_t>(refs);
    }
    bool IsOccupantIncluded(sc4::cISC4Occupant*) override { return true; }
    bool IsOccupantTypeIncluded(std::uint32_t) override { return true; }
    bool IsPropertyHolderIncluded(sc4::cISCPropertyHolder*) override { return true; }
};

// ---------------------------------------------------------------- ambience filters (for the layers)
struct FakeFilter final : sc4::cIAmbienceFilter, sc4::cIAmbienceCellFilter, sc4::cIAmbienceOccupantFilter {
    int refs = 0;
    int released = 0;
    int shutdowns = 0;
    bool stateAnswer = true;
    bool cellAnswer = true;
    bool occupantAnswer = true;
    std::vector<std::pair<int, int>> cellQueries;
    int occupantQueries = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cIAmbienceFilter) *out = static_cast<sc4::cIAmbienceFilter*>(this);
        else if (iid == sc4::kGZIID_cIAmbienceCellFilter) *out = static_cast<sc4::cIAmbienceCellFilter*>(this);
        else if (iid == sc4::kGZIID_cIAmbienceOccupantFilter || iid == sc4::kGZIID_cIAmbienceOccupantFilterAlt)
            *out = static_cast<sc4::cIAmbienceOccupantFilter*>(this);
        else if (iid == kGZIID_cIAmbienceFilterBase) *out = static_cast<sc4::cIAmbienceFilter*>(this);
        else return false;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { ++released; return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { ++shutdowns; return true; }
    bool IsTrue() override { return stateAnswer; }
    bool IsTrueCell(int x, int z) override { cellQueries.push_back({x, z}); return cellAnswer; }
    bool IsTrueOccupant(sc4::cISC4Occupant*) override { ++occupantQueries; return occupantAnswer; }
};

struct FakeFreshSink final : IAmbienceSndSink {
    std::vector<AmbienceSndEvent> events;
    bool PostEvent(const AmbienceSndEvent& event) override { events.push_back(event); return true; }
};

struct FakeLayerHost final : IAmbienceLayerHost {
    bool hasRM = true;
    bool hasSink = true;
    FakeFreshSink sink;
    int quality = 0;
    int residential = 0;
    std::uint32_t industrial = 0;
    std::uint32_t nextInstance = 500;
    std::map<std::uint32_t, FakeFilter*> filters;
    std::vector<std::pair<int, int>> cellToWorld;
    std::vector<std::uint32_t> exemplarInstances;
    std::vector<std::string> errors;
    bool filterInit = true;

    bool HasResourceManager() override { return hasRM; }
    sc4::cISCPropertyHolder* GetTuningExemplar(const cGZPersistResourceKey& key) override {
        exemplarInstances.push_back(key.instance);
        // one "all of" state filter that always passes: the layer then handles every cell and occupant
        auto* holder = new cSCPropertyHolder();
        cSCPropertyHelp::SetPropertyValue(holder, 0x20, cRZString("Traffic Hum"));
        cSCPropertyHelp::SetPropertyValue(holder, 0x2a3fbde1, std::vector<std::uint32_t>{0xA1});
        holder->AddRef();
        return holder;      // the layer releases it after reading
    }
    IAmbienceSndSink* AcquireSoundSink() override { return hasSink ? &sink : nullptr; }
    bool CreateObject(std::uint32_t classId, std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIAmbienceFilterBase) return false;
        auto it = filters.find(classId);
        if (it == filters.end()) return false;
        it->second->AddRef();
        *out = static_cast<sc4::cIAmbienceFilter*>(it->second);
        return true;
    }
    int RenderAudioQuality() override { return quality; }
    bool CellToWorld(int x, int z, float& wx, float& wz) override {
        cellToWorld.push_back({x, z});
        wx = static_cast<float>(x) * 16.0f;
        wz = static_cast<float>(z) * 16.0f;
        return true;
    }
    float TerrainAltitude(float x, float z) override { return x + z; }
    int ResidentialPopulation() override { return residential; }
    std::uint32_t IndustrialPopulation() override { return industrial; }
    std::uint32_t GenerateNewInstanceId() override { return nextInstance++; }
    void LogError(const char* message) override { errors.push_back(message); }
};

// ---------------------------------------------------------------- the manager host
struct FakeManagerHost final : IAmbienceManagerHost {
    bool framework = true;
    RefUnknown scape;
    FakeSndSys sound;
    FakeServer* server = nullptr;
    FakeOccupantManager* occupants = nullptr;
    FakeLot* lot = nullptr;
    int cellsX = 64, cellsZ = 64;
    std::vector<cIGZSystemService*> systemServices;
    std::vector<cIGZSystemService*> removedServices;
    std::vector<cIGZSystemService*> tickServices;
    std::vector<cIGZSystemService*> untickServices;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> createCalls;
    bool createResult = true;
    std::vector<IAmbienceLot*> lots;
    bool hasTuning = true;
    std::vector<float> zoomDistances{50.0f, 75.0f, 100.0f, 150.0f, 300.0f};
    std::vector<std::uint32_t> layerIds;
    bool haveZoomDistances = true;
    int hour = 9, minute = 0;
    bool haveClock = true;
    std::uint32_t nextInstance = 1000;
    std::vector<std::string> errors;

    bool HasFramework() override { return framework; }
    void AddSystemService(cIGZSystemService* service) override { systemServices.push_back(service); }
    void RemoveSystemService(cIGZSystemService* service) override { removedServices.push_back(service); }
    void AddToTick(cIGZSystemService* service) override { tickServices.push_back(service); }
    void RemoveFromTick(cIGZSystemService* service) override { untickServices.push_back(service); }
    cIGZUnknown* AudioScapeManager() override { return &scape; }
    cIGZSndSys* SoundSystem() override { return &sound; }
    cIGZMessageServer2* MessageServer() override { return server; }
    sc4::cISC4OccupantManager* OccupantManager() override { return occupants; }
    IAmbienceLot* LotOfOccupant(sc4::cISC4Occupant*) override { return lot; }
    void CityCellExtent(int& x, int& z) override { x = cellsX; z = cellsZ; }
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override {
        createCalls.push_back({classId, iid});
        if (!createResult) return false;
        auto* filter = new FakeOccupantFilter();
        filter->AddRef();
        *out = filter;
        return true;
    }
    bool HasTuning() override { return hasTuning; }
    bool GetTuningFloats(std::uint32_t propertyId, std::vector<float>& out) override {
        if (propertyId != kPropZoomDistances || !haveZoomDistances) return false;
        out = zoomDistances;
        return true;
    }
    bool GetTuningUints(std::uint32_t propertyId, std::vector<std::uint32_t>& out) override {
        if (propertyId != kPropLayerIds) return false;
        out = layerIds;
        return true;
    }
    bool ClockTime(int& h, int& m) override {
        if (!haveClock) return false;
        h = hour;
        m = minute;
        return true;
    }
    std::uint32_t GenerateNewInstanceId() override { return nextInstance++; }
    void LogError(const char* message) override { errors.push_back(message); }
};

// ---------------------------------------------------------------- fresh player
struct FakeFreshPlayer final : cIFreshPlayer {
    int refs = 0;
    bool initResult = true;
    std::vector<std::string> calls;
    cGZPersistResourceKey lastKey{};
    int minDistance = -1, maxDistance = -1;
    int headRelative = -1, playPercent = -1, volume = -1;
    int fadeMs = -1, fadeTarget = -1;
    bool fadeStop = false;
    int count(const char* name) const {
        int n = 0;
        for (const std::string& call : calls) n += call == name;
        return n;
    }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIFreshPlayer) {
            *out = static_cast<cIFreshPlayer*>(this);
            ++refs;
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool InitializeFromLoadedScore() override { return true; }
    bool InitializeFromResourceKey(const cGZPersistResourceKey& key) override {
        calls.push_back("InitializeFromResourceKey");
        lastKey = key;
        return initResult;
    }
    bool LoadScore(const cIGZString&, std::uint32_t) override { return true; }
    cIFreshScore* GetScore(std::uint32_t) override { return nullptr; }
    bool Shutdown() override { calls.push_back("Shutdown"); return true; }
    bool Play() override { calls.push_back("Play"); return true; }
    bool IsPlaying() override { return false; }
    bool Stop() override { calls.push_back("Stop"); return true; }
    bool SetVolume(std::int32_t value) override { calls.push_back("SetVolume"); volume = value; return true; }
    bool FadeVolume(std::int32_t ms, std::int32_t target, bool stop) override {
        calls.push_back("FadeVolume");
        fadeMs = ms;
        fadeTarget = target;
        fadeStop = stop;
        return true;
    }
    bool SetYPos(std::int32_t) override { return true; }
    bool SetXPos(std::int32_t) override { return true; }
    bool SetZoom(std::int32_t) override { return true; }
    bool SetPlayPercent(std::int32_t value) override { calls.push_back("SetPlayPercent"); playPercent = value; return true; }
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
    bool SetHeadRelative(bool value) override { calls.push_back("SetHeadRelative"); headRelative = value ? 1 : 0; return true; }
    bool SetMinMaxDistance(std::uint32_t minimum, std::uint32_t maximum) override {
        calls.push_back("SetMinMaxDistance");
        minDistance = static_cast<int>(minimum);
        maxDistance = static_cast<int>(maximum);
        return true;
    }
    bool SetNumDivisionsX(std::uint32_t) override { return true; }
    bool SetNumDivisionsZ(std::uint32_t) override { return true; }
    bool SetDivisionEnabled(std::uint32_t, std::uint32_t, bool) override { return true; }
    bool SetDivision3DPosition(std::uint32_t, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) override { return true; }
    void SetKey(const cGZPersistResourceKey&) override {}
    const cGZPersistResourceKey* GetKey() override { return nullptr; }
    void SetExternalSoundSystemInterface(cIExternalSoundSystem*) override {}
};

// ---------------------------------------------------------------- fixture
namespace {

struct Fixture {
    FakeManagerHost host;
    FakeLayerHost layerHost;
    FakeServer server;
    FakeGame game;
    FakeViewport viewport;
    FakeOccupantManager occupantManager;
    std::vector<FakeFilter*> filters;

    Fixture() {
        host.server = &server;
        host.occupants = &occupantManager;
        auto* filter = new FakeFilter();
        filter->AddRef();
        g_keepAlive.push_back(filter);
        g_ownedFilters.push_back(filter);
        filters.push_back(filter);
        layerHost.filters[0xA1] = filter;

        SetAmbienceManagerHost(&host);
        SetAmbienceLayerHost(&layerHost);
        SetViewUtilities(&viewport);
        GetServices().game = &game;
        GetServices().messageServer = &server;
        GetServices().exemplars = nullptr;
        ZoomDistances()[4] = 2000.0f;

        game.view.target[0] = 500.0f;
        game.view.target[1] = 100.0f;
        game.view.target[2] = 500.0f;
        game.view.eye[0] = 500.0f;
        game.view.eye[1] = 300.0f;
        game.view.eye[2] = 700.0f;
    }
    ~Fixture() {
        if (manager) {
            manager->Shutdown();
            manager->Release();
            manager = nullptr;
        }
        SetAmbienceManagerHost(nullptr);
        SetAmbienceLayerHost(nullptr);
        SetViewUtilities(nullptr);
        GetServices().game = nullptr;
        GetServices().messageServer = nullptr;
    }

    void InitLayers(const std::vector<std::uint32_t>& ids) {
        host.layerIds = ids;
        FakeMessage message;
        message.type = 0x26d31ec1;
        CHECK(manager->DoMessage(&message));
    }
    cSC4AudioAmbienceManager* Create() {
        manager = cSC4AudioAmbienceManager::Create();
        manager->AddRef();
        return manager;
    }
    void Shutdown() {
        if (!manager) return;
        manager->Shutdown();
        manager->Release();
        manager = nullptr;
    }
    cSC4AudioAmbienceManager* manager = nullptr;
};

// The number of distinct (x/2, z/2) blocks among the cells the layers were handed.
std::size_t DistinctBlocks(const FakeLayerHost& host) {
    std::set<std::pair<int, int>> blocks;
    for (const auto& cell : host.cellToWorld) blocks.insert({cell.first / 2, cell.second / 2});
    return blocks.size();
}

// ---------------------------------------------------------------- tests
void TestConstructionAndCom() {
    cSC4AudioAmbienceManager* manager = cSC4AudioAmbienceManager::Create();
    CHECK(manager != nullptr);
    CHECK(manager->GetServiceID() == 0xaa12f911u);
    CHECK(!manager->IsServiceRunning());
    CHECK(manager->GetServicePriority() == 0);
    CHECK(manager->GetIndustrialPopulation() == 0u);
    CHECK(manager->LayerCount() == 0);
    CHECK(manager->AlarmCount() == 0);
    CHECK(manager->Stage() == 0);
    CHECK(manager->IsActive());          // the constructor leaves the active flag set (quirk)
    CHECK(manager->ZoomLevel() == 0);
    CHECK(manager->AttentionX0() == 0.0f && manager->AttentionX1() == 0.0f);
    CHECK(manager->ListenerTarget()[0] == 0.0f && manager->ListenerEye()[1] == 0.0f);
    CHECK(!manager->QueryInterface(0x1234, nullptr));

    void* out = nullptr;
    CHECK(manager->QueryInterface(1, &out));
    CHECK(out == static_cast<cIGZSystemService*>(manager));
    out = nullptr;
    CHECK(manager->QueryInterface(kGZIID_cISC4AudioAmbienceManager, &out));
    CHECK(out == static_cast<cIGZSystemService*>(manager));   // quirk: the primary pointer for the +0x10 iid
    out = nullptr;
    CHECK(manager->QueryInterface(kGZIID_cIGZSystemService, &out));
    CHECK(out == static_cast<cIGZSystemService*>(manager));
    CHECK(manager->AddRef() == 4);       // the three queries and this one, refcount starts at 0
    CHECK(manager->Release() == 3);

    manager->SetServiceID(0x11223344);
    CHECK(manager->GetServiceID() == 0x11223344u);
    manager->SetServiceRunning(true);
    CHECK(manager->IsServiceRunning());
    manager->SetServiceRunning(false);
    CHECK(!manager->IsServiceRunning());
    CHECK(manager->OnIdle(16));
    CHECK(manager->OnTick(16));          // active with nothing wired: no-op but true
    CHECK(manager->Release() == 2);
    CHECK(manager->Release() == 1);
    CHECK(manager->Release() == 0);      // destroyed
}

void TestInitShutdown() {
    {
        Fixture f;
        SetAmbienceManagerHost(nullptr);
        auto* manager = cSC4AudioAmbienceManager::Create();
        CHECK(!manager->Init());
        manager->Release();
        SetAmbienceManagerHost(&f.host);
    }
    {
        Fixture f;
        f.host.framework = false;
        auto* manager = f.Create();
        CHECK(!manager->Init());
        CHECK(f.host.systemServices.empty());
        f.Shutdown();
    }
    {
        Fixture f;
        f.host.scape.refs = 100;             // the fixture's scape manager is borrowed from the host
        auto* manager = f.Create();
        CHECK(f.host.scape.refs == 100);
        CHECK(manager->Init());
        // one reference was taken for the manager (+0x30)
        CHECK(f.host.scape.refs == 101);
        CHECK(f.host.systemServices.size() == 1 && f.host.systemServices[0] == static_cast<cIGZSystemService*>(manager));
        CHECK(f.host.sound.handlers.size() == 1 &&
              f.host.sound.handlers[0] == static_cast<cIGZSndEventHandler*>(manager));
        CHECK(f.host.createCalls.size() == 1 && f.host.createCalls[0].first == 0x4a401ba7 &&
              f.host.createCalls[0].second == 0x4a40074c);
        CHECK(manager->IndustrialFilter() != nullptr);
        CHECK(manager->SoundSystem() == &f.host.sound);
        CHECK(manager->ScapeManager() == &f.host.scape);
        // the twelve message registrations, in the original's order
        static const std::uint32_t expected[] = {0x26d31ec1, 0x26d31ec2, 0x8a4a69c7, 0x99ef1142, 0x99ef1146,
                                                 0x99ef1143, 0xe99b3d4d, 0xe8bda2e7, 0x9d92fc3,  0xc53d10aa,
                                                 0x8a878385, 0x4348b111};
        CHECK(f.server.added.size() == 12);
        for (int i = 0; i < 12; ++i) {
            CHECK(f.server.added[static_cast<std::size_t>(i)].second == expected[i]);
            CHECK(f.server.added[static_cast<std::size_t>(i)].first == static_cast<cIGZMessageTarget2*>(manager));
        }
        // a second Init replaces the industrial filter (release + create) and re-registers
        CHECK(manager->Init());
        CHECK(f.host.createCalls.size() == 2);
        CHECK(f.server.added.size() == 24);
        CHECK(f.host.sound.handlers.size() == 2);   // a second Init registers the handler again
        CHECK(f.host.scape.refs == 101);     // unchanged: the same service
        // Shutdown
        CHECK(manager->Shutdown());
        CHECK(f.host.removedServices.size() == 1);
        CHECK(f.host.sound.handlers.size() == 1);   // one of the two registrations is withdrawn
        CHECK(f.server.removed.size() == 12);
        static const std::uint32_t removed[] = {0x4348b111, 0x8a878385, 0x26d31ec1, 0x26d31ec2, 0x8a4a69c7, 0x99ef1142,
                                                0x99ef1146, 0x99ef1143, 0xe8bda2e7, 0xe99b3d4d, 0x9d92fc3,  0xc53d10aa};
        for (int i = 0; i < 12; ++i) CHECK(f.server.removed[static_cast<std::size_t>(i)].second == removed[i]);
        CHECK(f.host.scape.refs == 100);     // +0x30 released by Shutdown
        CHECK(manager->IndustrialFilter() == nullptr);
        CHECK(manager->SoundSystem() == &f.host.sound);   // quirk: the sound system stays until destruction
        f.Shutdown();
    }
    {
        Fixture f;
        f.host.framework = true;
        f.host.createResult = false;
        auto* manager = f.Create();
        CHECK(!manager->Init());             // the factory failure fails Init *after* the registrations
        CHECK(manager->IndustrialFilter() == nullptr);
        f.Shutdown();
    }
}

void TestTuningAndLayers() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    // missing zoom-distance property: the original writes five 200.0f through the storage (deviation:
    // the port sizes the vector first)
    FakeMessage reload;
    reload.type = 0xc53d10aa;
    f.host.haveZoomDistances = false;
    manager->DoMessage(&reload);
    CHECK(manager->ZoomDistances().size() == 5);
    for (float d : manager->ZoomDistances()) CHECK(d == 200.0f);
    // a real exemplar
    f.host.haveZoomDistances = true;
    manager->DoMessage(&reload);
    CHECK(manager->ZoomDistances().size() == 5 && manager->ZoomDistances()[2] == 100.0f);

    f.host.layerIds = {0x111, 0x222};
    FakeMessage init;
    init.type = 0x26d31ec1;
    CHECK(manager->DoMessage(&init));
    CHECK(manager->LayerCount() == 2);
    CHECK(manager->LayerIds().size() == 2);
    const auto& layers = manager->Layers();
    for (cSC4AudioAmbienceLayer* layer : layers) {
        CHECK(layer->IsInitialised());
        CHECK(layer->ScoreMasterVolume() == 800);     // the game's audio volume
        CHECK(std::string(layer->GetName().ToChar()) == "Traffic Hum");   // the layer exemplar was read
    }
    // the layer id is only set by SetLayerId (the manager never calls it), so the exemplar lookups the
    // layers made are the evidence that one layer per id was created, in exemplar order
    CHECK(f.layerHost.exemplarInstances.size() == 2);
    CHECK(f.layerHost.exemplarInstances[0] == 0x111 && f.layerHost.exemplarInstances[1] == 0x222);
    CHECK(f.server.added.size() == 12 + 2);           // the two layers registered for the reload message
    CHECK(f.host.tickServices.size() == 1);
    // Init creates the industrial filter (+0x190); the init message creates the tick filter (+0x48)
    // through the same factory (the original allocates that one with new, see the header)
    CHECK(f.host.createCalls.size() == 2);
    CHECK(manager->TickFilter() != nullptr);
    CHECK(manager->Stage() == 0);
    // this is the first attention-rect update of the process: the previous rect is seeded with it
    CHECK_EQ(manager->PreviousCells()[0], manager->CurrentCells()[0]);
    CHECK_EQ(manager->PreviousCells()[2], manager->CurrentCells()[2]);
    // a disabled sound option gives score master volume 0
    f.game.prefs.soundEnabled = false;
    f.game.prefs.masterVolume = 123;
    f.host.layerIds = {0x333};
    // remove the old layers first so the list holds only the new one
    CHECK(manager->Shutdown());
    CHECK(manager->Init());
    CHECK(manager->DoMessage(&init));
    CHECK(manager->LayerCount() == 1);
    CHECK(manager->Layers().front()->ScoreMasterVolume() == 0);
    f.Shutdown();
}

void TestAttentionRect() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    // first update: 400..600 metres around the listener target (zoom 2 = 100 m), clipped to the city
    FakeMessage init;
    init.type = 0x26d31ec1;
    CHECK(manager->DoMessage(&init));
    CHECK_NEAR(manager->AttentionX0(), 400.0f, 0.01);
    CHECK_NEAR(manager->AttentionZ0(), 400.0f, 0.01);
    CHECK_NEAR(manager->AttentionX1(), 600.0f, 0.01);
    CHECK_NEAR(manager->AttentionZ1(), 600.0f, 0.01);
    CHECK(manager->ZoomLevel() == 2);
    CHECK_NEAR(manager->ListenerTarget()[0], 500.0f, 0.01);   // +0x6c = listener target
    CHECK_NEAR(manager->ListenerEye()[1], 300.0f, 0.01);      // +0x78 = listener eye
    // 400/16 = 25 -> 24, 600/16 = 37 -> 36 (even cells)
    CHECK_EQ(manager->CurrentCells()[0], 24);
    CHECK_EQ(manager->CurrentCells()[1], 24);
    CHECK_EQ(manager->CurrentCells()[2], 36);
    CHECK_EQ(manager->CurrentCells()[3], 36);
    // move the camera, then tell the manager the listener moved
    f.game.view.target[0] = 180.0f;
    f.game.view.target[2] = 180.0f;
    FakeMessage moved;
    moved.type = 0x8a4a69c7;
    CHECK(manager->DoMessage(&moved));
    CHECK_EQ(manager->PreviousCells()[0], 24);    // the old current becomes the previous
    CHECK_EQ(manager->CurrentCells()[0], 4);      // (180-100)/16 = 5, masked to even -> 4
    CHECK_EQ(manager->CurrentCells()[2], 16);     // (180+100)/16 = 17 -> 16
    CHECK_EQ(manager->PreviousCells()[2], 36);
    // clipping: a target near the origin clamps the minimum corner at 0
    f.game.view.target[0] = 10.0f;
    f.game.view.target[2] = 10.0f;
    CHECK(manager->DoMessage(&moved));
    CHECK_EQ(manager->CurrentCells()[0], 0);
    CHECK_EQ(manager->CurrentCells()[1], 0);
    f.Shutdown();
}

void TestTickWalk() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});
    CHECK(manager->LayerCount() == 1);
    cSC4AudioAmbienceLayer* layer = manager->Layers().front();

    // One occupant in the middle of the attention rectangle; the tick walk must reach it through the
    // occupant manager's cell iteration and forward it to the layer.
    FakeOccupant occupant;
    occupant.flags = 4;    // visible: HandleOccupant forwards it
    occupant.pos[0] = 500.0f;
    occupant.pos[2] = 500.0f;
    g_keepAlive.push_back(&occupant);
    f.occupantManager.stored.push_back(&occupant);

    CHECK_EQ(manager->CurrentCells()[0], 24);
    CHECK_EQ(manager->CurrentCells()[2], 36);
    // The iterator covers cells 12..18 (24/2 .. 36/2): 7x7 = 49 blocks. Drive the stage machine until it
    // has finished a full pass.
    int ticks = 0;
    for (; ticks < 10000 && manager->Stage() != 1; ++ticks) {
        CHECK(manager->OnTick(16));
        CHECK(manager->Stage() == 1 || manager->Stage() == 2);
    }
    CHECK(manager->Stage() == 1);
    CHECK(DistinctBlocks(f.layerHost) == 49);
    for (const auto& cell : f.layerHost.cellToWorld) {
        CHECK(cell.first >= 24 && cell.first <= 37);
        CHECK(cell.second >= 24 && cell.second <= 37);
    }
    CHECK(layer->AcceptedCells() == 49 * 4);     // all four cells of every block pass the cell filter
    CHECK(layer->AcceptedOccupants() >= 1);      // the occupant was handed to the layer during the walk
    CHECK(layer->PositionCount() == 9);          // the layer's update ran with cells to cluster

    // another pass: the iterator starts over
    const std::size_t blocksBefore = DistinctBlocks(f.layerHost);
    for (ticks = 0; ticks < 10000 && manager->Stage() != 1; ++ticks) {
        CHECK(manager->OnTick(16));
    }
    CHECK(manager->Stage() == 1);
    CHECK(DistinctBlocks(f.layerHost) == blocksBefore);   // same 49 blocks

    // a focus loss pauses every layer and parks the stage at 4 (OnTick then does nothing)
    FakeMessage focus;
    focus.type = 0x4348b111;
    focus.SetData1(0);
    CHECK(manager->DoMessage(&focus));
    CHECK(manager->Stage() == 4);
    CHECK(layer->IsPaused());
    const std::size_t blocksPaused = DistinctBlocks(f.layerHost);
    CHECK(manager->OnTick(16));
    CHECK(DistinctBlocks(f.layerHost) == blocksPaused);
    // focus back: the stage returns to 1 and the layer resumes
    focus.SetData1(1);
    CHECK(manager->DoMessage(&focus));
    CHECK(manager->Stage() == 1);
    CHECK(!layer->IsPaused());
    f.occupantManager.stored.clear();
    f.Shutdown();
}

void TestOccupantMessages() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});

    // An occupant whose sound properties make HandleInsertOccupant start its sounds.
    FakeOccupant occupant;
    occupant.flags = 5;    // IsFlagSet(5) admits the sound
    occupant.pos[0] = 100.0f;
    occupant.pos[1] = 10.0f;
    occupant.pos[2] = 200.0f;
    g_keepAlive.push_back(&occupant);
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundId, std::uint32_t(0x5001));
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundId2, std::uint32_t(0x5002));
    std::vector<float> times{10.0f, 18.0f};
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropAlarmTimes, times);
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropAlarmSound, std::uint32_t(0x6001));
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropAlarmSoundRepeat, std::uint32_t(0x6002));

    f.host.sound.posted.clear();
    FakeMessage insert;
    insert.type = sc4::kMessageOccupantInserted;
    insert.unknown = &occupant;
    occupant.AddRef();     // the message holds a reference
    CHECK(manager->DoMessage(&insert));
    CHECK_EQ(manager->GetIndustrialPopulation(), 1u);      // every occupant counts (the filter ignores its argument)
    CHECK_EQ(manager->AlarmCount(), 2u);                   // one alarm per alarm sound id
    // 0xea451903 (start) then 0xca45190f (position) for the main sound and for the second sound
    CHECK(f.host.sound.posted.size() == 4);
    CHECK_EQ(f.host.sound.posted[0][0], 0xea451903u);
    CHECK_EQ(f.host.sound.posted[0][1], 0x5001u);
    const std::uint32_t instance = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&occupant)) + 0x5001u;
    CHECK_EQ(f.host.sound.posted[0][2], instance);
    CHECK_EQ(f.host.sound.posted[0][3], 1u);
    CHECK_EQ(f.host.sound.posted[1][0], 0xca45190fu);
    CHECK_EQ(f.host.sound.posted[1][1], instance);
    CHECK_EQ(f.host.sound.posted[1][2], 100);
    CHECK_EQ(f.host.sound.posted[1][3], 10);
    CHECK_EQ(f.host.sound.posted[1][4], 200);
    CHECK_EQ(f.host.sound.posted[2][0], 0xea451903u);
    CHECK_EQ(f.host.sound.posted[2][1], 0x5002u);
    CHECK_EQ(f.host.sound.posted[2][3], 0u);      // the second sound is posted with flag false
    // The marked occupant is ignored (property 0x29244db present).
    FakeOccupant marked;
    marked.flags = 5;
    g_keepAlive.push_back(&marked);
    cSCPropertyHelp::SetPropertyValue(&marked.properties, kPropMarked, true);
    f.host.sound.posted.clear();
    FakeMessage insertMarked;
    insertMarked.type = sc4::kMessageOccupantInserted;
    insertMarked.unknown = &marked;
    marked.AddRef();
    CHECK(manager->DoMessage(&insertMarked));
    CHECK(manager->GetIndustrialPopulation() == 1u);    // unchanged
    CHECK(f.host.sound.posted.empty());

    // Move: the position event uses (int)occupant (without the sound id) and the second sound id triggers
    // _MoveOccupantSndInstance, which posts the position a second time.
    f.host.sound.posted.clear();
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundStateB, std::uint32_t(0x7001));
    FakeMessage move;
    move.type = sc4::kMessageOccupantRemoved;   // 0x99ef1146 = moved
    move.unknown = &occupant;
    occupant.AddRef();
    CHECK(manager->DoMessage(&move));
    CHECK(f.host.sound.posted.size() == 2);
    CHECK_EQ(f.host.sound.posted[0][0], 0xca45190fu);
    CHECK_EQ(f.host.sound.posted[0][1], static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&occupant)));
    CHECK_EQ(f.host.sound.posted[1][0], 0xca45190fu);

    // Remove: stops the state sounds, the main sound and drops both alarms.
    f.host.sound.posted.clear();
    FakeMessage removed;
    removed.type = sc4::kMessageOccupantRemovedFromManager;
    removed.unknown = &occupant;
    occupant.AddRef();
    CHECK(manager->DoMessage(&removed));
    CHECK_EQ(manager->GetIndustrialPopulation(), 0u);
    CHECK_EQ(manager->AlarmCount(), 0u);
    bool sawStop = false;
    for (const auto& event : f.host.sound.posted) {
        if (event[0] == 0x6a451908u && event[1] == instance) sawStop = true;
    }
    CHECK(sawStop);

    // A message without an object, an object that is not an occupant and an unknown type all answer true.
    FakeMessage empty;
    empty.type = sc4::kMessageOccupantInserted;
    CHECK(manager->DoMessage(&empty));
    FakeOccupant stranger;
    FakeMessage notOccupant;
    notOccupant.type = sc4::kMessageOccupantInserted;
    notOccupant.unknown = &stranger;
    CHECK(manager->DoMessage(&notOccupant));
    FakeMessage unknownType;
    unknownType.type = sc4::kMessageLotStateChanged;   // routed to the handler, which ignores it
    CHECK(manager->DoMessage(&unknownType));
    CHECK(manager->DoMessage(nullptr) == false);
    f.occupantManager.stored.clear();
    f.Shutdown();
}

void TestAlarms() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});

    FakeOccupant occupant;
    occupant.flags = 5;
    occupant.pos[0] = 64.0f;
    occupant.pos[2] = 128.0f;
    g_keepAlive.push_back(&occupant);
    std::vector<float> alarmTimes{10.0f};
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropAlarmTimes, alarmTimes);
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropAlarmSound, std::uint32_t(0x6001));
    f.host.hour = 9;
    f.host.minute = 30;      // added at 9:30
    FakeMessage insert;
    insert.type = sc4::kMessageOccupantInserted;
    insert.unknown = &occupant;
    occupant.AddRef();
    CHECK(manager->DoMessage(&insert));
    CHECK_EQ(manager->AlarmCount(), 1u);
    CHECK_NEAR(manager->Alarms().front().hour, 9.5f, 0.001);
    CHECK_EQ(manager->Alarms().front().count, 1);
    CHECK_EQ(manager->Alarms().front().soundId, 0x6001u);
    CHECK(!manager->Alarms().front().active);

    // Before 10:00 nothing fires.
    f.host.sound.posted.clear();
    f.host.hour = 9;
    f.host.minute = 45;
    CHECK(manager->OnTick(16));
    CHECK(f.host.sound.posted.empty());
    // At 10:05 the alarm fires once: the sound is created at the generated instance id and positioned.
    f.host.hour = 10;
    f.host.minute = 5;
    CHECK(manager->OnTick(16));
    CHECK(f.host.sound.posted.size() == 2);
    CHECK_EQ(f.host.sound.posted[0][0], 0xea451903u);
    CHECK_EQ(f.host.sound.posted[0][1], 0x6001u);
    CHECK_EQ(f.host.sound.posted[0][2], 1000u);    // host's first generated instance id
    CHECK_EQ(f.host.sound.posted[0][3], 1u);
    CHECK_EQ(f.host.sound.posted[1][0], 0xca45190fu);
    CHECK_EQ(f.host.sound.posted[1][1], 1000u);
    CHECK_EQ(f.host.sound.posted[1][2], 64);
    CHECK_EQ(f.host.sound.posted[1][3], 0);
    CHECK_EQ(f.host.sound.posted[1][4], 128);
    // It does not fire again in the same minute (the last clock reading was updated).
    f.host.sound.posted.clear();
    CHECK(manager->OnTick(16));
    CHECK(f.host.sound.posted.empty());

    // Flags 0x10 refuse an alarm, IsFlagSet(5) is required.
    FakeOccupant refused;
    refused.flags = 5 | 0x10;
    g_keepAlive.push_back(&refused);
    cSCPropertyHelp::SetPropertyValue(&refused.properties, kPropAlarmTimes, alarmTimes);
    cSCPropertyHelp::SetPropertyValue(&refused.properties, kPropAlarmSound, std::uint32_t(0x6009));
    FakeMessage insertRefused;
    insertRefused.type = sc4::kMessageOccupantInserted;
    insertRefused.unknown = &refused;
    refused.AddRef();
    CHECK(manager->DoMessage(&insertRefused));
    CHECK_EQ(manager->AlarmCount(), 1u);

    // No clock: the alarm update does nothing.
    f.host.haveClock = false;
    f.host.sound.posted.clear();
    f.host.hour = 11;
    CHECK(manager->OnTick(16));
    CHECK(f.host.sound.posted.empty());
    f.host.haveClock = true;
    f.Shutdown();
}

void TestLotConditionChange() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});
    FakeLot lot;
    lot.state = 5;
    f.host.lot = &lot;

    FakeOccupant occupant;
    occupant.flags = 5;              // _AddOccupantSndInstance requires flag 5
    occupant.buildingView = true;    // the lot loop accepts it through the building-occupant view
    g_keepAlive.push_back(&occupant);
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundStateA, std::uint32_t(0x7001));
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundStateB, std::uint32_t(0x7002));
    lot.occupants.push_back(&occupant);

    const std::uint32_t occupantBase = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&occupant));
    const auto countStop = [&](std::uint32_t soundId) {
        int n = 0;
        for (const auto& event : f.host.sound.posted)
            n += event[0] == 0x6a451908u && event[1] == occupantBase + soundId;
        return n;
    };
    const auto countStart = [&](std::uint32_t soundId) {
        int n = 0;
        for (const auto& event : f.host.sound.posted) n += event[0] == 0xea451903u && event[1] == soundId;
        return n;
    };
    FakeMessage message;
    message.type = sc4::kMessageLotConditionChanged;
    message.unknown = &lot;

    // state 5 is in the 0x61 mask (states 0, 5 and 6): stop A, start B.
    f.host.sound.posted.clear();
    lot.AddRef();
    message.SetData1(1);
    message.SetData2(5);
    CHECK(manager->DoMessage(&message));
    CHECK_EQ(f.occupantManager.releaseListCalls, 1);     // the occupant list went back to the manager
    CHECK_EQ(countStop(0x7001), 1);
    CHECK_EQ(countStart(0x7002), 1);
    CHECK_EQ(countStop(0x7002), 0);

    // state 2 (2 | 1 == 3): stop B, start A.
    f.host.sound.posted.clear();
    lot.AddRef();
    message.SetData2(2);
    CHECK(manager->DoMessage(&message));
    CHECK_EQ(countStop(0x7002), 1);
    CHECK_EQ(countStart(0x7001), 1);
    CHECK_EQ(countStop(0x7001), 0);

    // state 4 (abandoned): both sounds stop.
    f.host.sound.posted.clear();
    lot.AddRef();
    message.SetData2(4);
    CHECK(manager->DoMessage(&message));
    CHECK_EQ(countStop(0x7001), 1);
    CHECK_EQ(countStop(0x7002), 1);

    // condition 1 with state 0 also starts the vacant sound (on top of the mask branch).
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundVacant, std::uint32_t(0x7003));
    f.host.sound.posted.clear();
    lot.AddRef();
    message.SetData1(1);
    message.SetData2(0);
    CHECK(manager->DoMessage(&message));
    // state 0 is in the mask: stop A and start B; the lot being vacated adds the vacant sound
    CHECK_EQ(countStop(0x7001), 1);
    CHECK_EQ(countStart(0x7002), 1);
    CHECK_EQ(countStart(0x7003), 1);

    // A prop-occupant view is accepted too; an occupant that answers neither view is ignored.
    FakeOccupant propOnly;
    propOnly.flags = 5;
    propOnly.propView = true;
    g_keepAlive.push_back(&propOnly);
    cSCPropertyHelp::SetPropertyValue(&propOnly.properties, kPropSoundStateA, std::uint32_t(0x7101));
    FakeOccupant neither;
    neither.flags = 5;
    g_keepAlive.push_back(&neither);
    cSCPropertyHelp::SetPropertyValue(&neither.properties, kPropSoundStateA, std::uint32_t(0x7201));
    lot.occupants.clear();
    lot.occupants.push_back(&propOnly);
    lot.occupants.push_back(&neither);
    f.host.sound.posted.clear();
    lot.AddRef();
    message.SetData1(1);
    message.SetData2(1);      // state 1: both sounds stop (A is removed)
    CHECK(manager->DoMessage(&message));
    bool sawProp = false, sawNeither = false;
    for (const auto& event : f.host.sound.posted) {
        if (event[0] != 0x6a451908u) continue;
        sawProp = sawProp || event[1] == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&propOnly)) + 0x7101u;
        sawNeither = sawNeither || event[1] == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&neither)) + 0x7201u;
    }
    CHECK(sawProp);
    CHECK(!sawNeither);
    // a condition change without a lot is ignored
    f.host.lot = nullptr;
    FakeMessage noLot;
    noLot.type = sc4::kMessageLotConditionChanged;
    CHECK(manager->DoMessage(&noLot));
    f.host.lot = &lot;
    f.Shutdown();
}

void TestAutomatonAndRailway() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});

    FakeOccupant occupant;
    occupant.flags = 5;
    occupant.pos[0] = 32.0f;
    occupant.pos[2] = 64.0f;
    g_keepAlive.push_back(&occupant);
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropSoundId, std::uint32_t(0x8001));
    cSCPropertyHelp::SetPropertyValue(&occupant.properties, kPropRailwayCrossingSound, std::uint32_t(0x8002));

    FakeAutomaton automaton;
    automaton.occupant = &occupant;
    automaton.pos[0] = 48.0f;
    automaton.pos[2] = 96.0f;
    automaton.stateBit = 0x10;
    g_keepAlive.push_back(&automaton);
    occupant.automaton = &automaton;

    // Inserting the occupant registers the manager as the automaton's callback target.
    FakeMessage insert;
    insert.type = sc4::kMessageOccupantInserted;
    insert.unknown = &occupant;
    occupant.AddRef();
    CHECK(manager->DoMessage(&insert));
    CHECK(automaton.callbacks.size() == 1 && automaton.callbacks[0] == static_cast<cIGZMessageTarget2*>(manager));

    // Automaton message, kind 12, signal 0x10, state present: the sound stops.
    f.host.sound.posted.clear();
    FakeMessage automatonMessage;
    automatonMessage.type = 0x9d92fc3;
    automatonMessage.SetData2(12);
    automatonMessage.SetData3(0x10);
    automatonMessage.SetVoid1(&automaton);
    CHECK(manager->DoMessage(&automatonMessage));
    CHECK(!f.host.sound.posted.empty());
    CHECK_EQ(f.host.sound.posted[0][0], 0x6a451908u);       // play = !(hasState || kind == 12) -> stop

    // kind 13 with the state: still a stop (play = !(hasState || kind == 12)).
    automaton.stateBit = 0x10;
    f.host.sound.posted.clear();
    CHECK(manager->DoMessage(&automatonMessage));       // kind is still 12 here
    CHECK(!f.host.sound.posted.empty() && f.host.sound.posted[0][0] == 0x6a451908u);
    // kind 13 without the state: play.
    automatonMessage.SetData2(13);
    automaton.stateBit = 0;
    f.host.sound.posted.clear();
    CHECK(manager->DoMessage(&automatonMessage));
    CHECK(!f.host.sound.posted.empty() && f.host.sound.posted[0][0] == 0xea451903u);

    // signal 0x1e with kind 13: play (kind != 12).
    automatonMessage.SetData2(13);
    automatonMessage.SetData3(0x1e);
    f.host.sound.posted.clear();
    CHECK(manager->DoMessage(&automatonMessage));
    CHECK(!f.host.sound.posted.empty() && f.host.sound.posted[0][0] == 0xea451903u);

    // kind 9: only the position is updated, at (int)occupant + sound id.
    automatonMessage.SetData2(9);
    automatonMessage.SetData3(0);
    f.host.sound.posted.clear();
    CHECK(manager->DoMessage(&automatonMessage));
    CHECK(f.host.sound.posted.size() == 1);
    CHECK_EQ(f.host.sound.posted[0][0], 0xca45190fu);
    CHECK_EQ(f.host.sound.posted[0][1], static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&occupant)) + 0x8001u);
    CHECK_EQ(f.host.sound.posted[0][2], 48);
    CHECK_EQ(f.host.sound.posted[0][4], 96);

    // An unknown kind does nothing but answers true.
    automatonMessage.SetData2(7);
    f.host.sound.posted.clear();
    CHECK(manager->DoMessage(&automatonMessage));
    CHECK(f.host.sound.posted.empty());

    // Removing the occupant unregisters the callback target.
    FakeMessage removed;
    removed.type = sc4::kMessageOccupantRemovedFromManager;
    removed.unknown = &occupant;
    occupant.AddRef();
    CHECK(manager->DoMessage(&removed));
    CHECK(automaton.callbacks.empty());

    // Railway crossing: the crossing sound is started at the automaton's position, with the payload
    // object's address as the instance id.
    f.host.sound.posted.clear();
    FakeMessage crossing;
    crossing.type = 0x8a878385;
    crossing.unknown = &automaton;
    automaton.refs = 0;
    CHECK(manager->DoMessage(&crossing));
    CHECK(f.host.sound.posted.size() == 2);
    CHECK_EQ(f.host.sound.posted[0][0], 0xea451903u);
    CHECK_EQ(f.host.sound.posted[0][1], 0x8002u);
    CHECK_EQ(f.host.sound.posted[0][2], static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&automaton)));
    CHECK_EQ(f.host.sound.posted[0][3], 1u);
    CHECK_EQ(f.host.sound.posted[1][0], 0xca45190fu);
    CHECK_EQ(f.host.sound.posted[1][2], 48);
    CHECK_EQ(f.host.sound.posted[1][4], 96);
    // the wrong type answers false when it reaches the railway-crossing handler; the manager's own
    // dispatch answers true for the types it has no handler for (0x12345678 is not registered).
    FakeMessage crossingWithoutAutomaton;
    crossingWithoutAutomaton.type = 0x8a878385;
    FakeOccupant notAnAutomaton;
    g_keepAlive.push_back(&notAnAutomaton);
    crossingWithoutAutomaton.unknown = &notAnAutomaton;   // no automaton view: nothing is started
    f.host.sound.posted.clear();
    CHECK(manager->DoMessage(&crossingWithoutAutomaton));
    CHECK(f.host.sound.posted.empty());
    f.Shutdown();
}

void TestDoEvent() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});
    cSC4AudioAmbienceLayer* layer = manager->Layers().front();

    FakeFreshPlayer player;
    g_keepAlive.push_back(&player);
    FakeEvent event;
    g_keepAlive.push_back(&event);
    event.message.unknown = &player;
    player.refs = 1;

    // The score-add event loads the score from the three payload words.
    event.id = kEventScoreAdd;
    event.message.SetData1(0x2026960b);
    event.message.SetData2(0x11);
    event.message.SetData3(0x22);
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK(player.count("InitializeFromResourceKey") == 1);
    CHECK_EQ(player.lastKey.type, 0x2026960b);
    CHECK_EQ(player.lastKey.group, 0x11);
    CHECK_EQ(player.lastKey.instance, 0x22);
    CHECK(player.refs == 1);      // released

    // A failed load is reported to the host log.
    f.host.errors.clear();
    player.initResult = false;
    event.message.SetData1(0x2026960c);
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK(f.host.errors.size() == 1);
    CHECK(f.host.errors[0].find("Failed to load freshness score") != std::string::npos);
    player.initResult = true;

    // start mode: head-relative from data1 then play
    event.id = kEventScoreStartMode;
    event.message.SetData1(1);
    player.calls.clear();
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK(player.headRelative == 1 && player.count("Play") == 1);

    // start: the two volume words become the min/max distance
    event.id = kEventScoreStart;
    event.message.SetData1(123);
    event.message.SetData2(4567);
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK_EQ(player.minDistance, 123);
    CHECK_EQ(player.maxDistance, 4567);

    // parameter
    event.id = kEventScoreParameter;
    event.message.SetData1(77);
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK_EQ(player.playPercent, 77);

    // volume
    event.id = kEventScoreVolume;
    event.message.SetData1(static_cast<std::uint64_t>(static_cast<std::int64_t>(-5)));
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK_EQ(player.volume, -5);

    // stop: fade out over 3000 ms
    event.id = kEventScoreStop;
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK_EQ(player.fadeMs, 3000);
    CHECK_EQ(player.fadeTarget, 0);
    CHECK(player.fadeStop);

    // remove: stop then shut down
    event.id = kEventScoreRemove;
    player.calls.clear();
    player.refs = 1;
    CHECK(manager->DoEvent(&event));
    CHECK(player.calls.size() == 2 && player.calls[0] == "Stop" && player.calls[1] == "Shutdown");

    // the master-volume event sets every layer's score volume and answers false
    event.id = 0x2a5b69c0;
    event.message.SetData1(512);
    player.refs = 1;
    CHECK(!manager->DoEvent(&event));
    CHECK_EQ(layer->ScoreMasterVolume(), 512u);

    // unknown event: no player method, answers false
    event.id = 0x11111111;
    player.calls.clear();
    player.refs = 1;
    CHECK(!manager->DoEvent(&event));
    CHECK(player.calls.empty());
    CHECK(player.refs == 1);

    // an event whose payload is not a fresh player answers false; a null event too
    event.id = kEventScoreStart;
    cIGZUnknown* savedUnknown = event.message.unknown;
    FakeOccupant notPlayer;
    event.message.unknown = &notPlayer;
    CHECK(!manager->DoEvent(&event));
    event.message.unknown = savedUnknown;
    CHECK(!manager->DoEvent(nullptr));
    FakeEvent noPayload;
    noPayload.id = kEventScoreStart;
    CHECK(!manager->DoEvent(&noPayload));
    f.Shutdown();
}

void TestDoMessageDispatch() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});

    // unsupported type: true, nothing happens
    FakeMessage unrelated;
    unrelated.type = 0x22222222;
    CHECK(manager->DoMessage(&unrelated));
    // city shutdown: removes the layers, answers false
    FakeMessage shutdown;
    shutdown.type = 0x26d31ec2;
    CHECK(!manager->DoMessage(&shutdown));
    CHECK(manager->LayerCount() == 0);
    CHECK(f.host.untickServices.size() == 1);
    CHECK(!manager->IsActive());
    // a tick after the shutdown message does nothing
    CHECK(manager->OnTick(16));
    // city init again rebuilds from the (still known) exemplar ids
    FakeMessage init;
    init.type = 0x26d31ec1;
    CHECK(manager->DoMessage(&init));
    CHECK(manager->LayerCount() == 1);
    CHECK(manager->IsActive());
    // tuning reload re-reads the exemplar
    f.host.zoomDistances[0] = 11.0f;
    FakeMessage reload;
    reload.type = 0xc53d10aa;
    CHECK(manager->DoMessage(&reload));
    CHECK_EQ(manager->ZoomDistances()[0], 11.0f);
    // a null message answers false
    CHECK(!manager->DoMessage(nullptr));
    f.Shutdown();
}

void TestDestructorWithoutShutdown() {
    Fixture f;
    auto* manager = f.Create();
    CHECK(manager->Init());
    f.InitLayers({0x111});
    CHECK(manager->LayerCount() == 1);
    cSC4AudioAmbienceLayer* layer = manager->Layers().front();
    layer->AddRef();     // keep the layer alive so we can clean it up after the leak
    const std::size_t notificationsBefore = f.server.removed.size();
    // The destructor does not shut the layers down (quirk): no notification is withdrawn.
    manager->Release();
    f.manager = nullptr;
    CHECK(f.server.removed.size() == notificationsBefore);
    CHECK(f.host.untickServices.empty());
    // clean up by hand: the manager dropped its reference but never released the layer, so it still has
    // the reference the layer list held.
    layer->Shutdown();
    CHECK(f.server.removed.size() == notificationsBefore + 1);
    layer->Release();
    CHECK(f.server.removed.size() == notificationsBefore + 1);
    layer->Release();      // the extra reference taken above
}

}  // namespace

int main() {
    TestConstructionAndCom();
    TestInitShutdown();
    TestTuningAndLayers();
    TestAttentionRect();
    TestTickWalk();
    TestOccupantMessages();
    TestAlarms();
    TestLotConditionChange();
    TestAutomatonAndRailway();
    TestDoEvent();
    TestDoMessageDispatch();
    TestDestructorWithoutShutdown();
    for (FakeFilter* filter : g_ownedFilters) delete filter;
    std::printf("ok\n");
    return 0;
}
