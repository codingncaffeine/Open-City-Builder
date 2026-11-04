// cSC4DispatchManager: interface/refcount, Init/Shutdown registration, dispatch-source registry, pylon call-box
// placement (BeginDispatchTool), the dispatch lifecycle (Dispatch / GetDispatchInfo / automaton tracking / EndDispatch /
// RevokeDispatchByPylon / ReturnPylonToStation), the message handlers (occupant removed, automaton arrived/removed, camera
// moved, "return all automata"), persistence (Write/Read record layout, additive Read) and the kept quirks (ignored
// RevokeAllDispatches type, tool-mode pylon leak, GetDispatchIDArray limit checked after the write, Init clearing a
// restored tool/active pylon while re-tracking the automata).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/DispatchManager.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e * (1.0f + std::fabs(a) + std::fabs(b)); }
using namespace sc4;

namespace {

constexpr std::uint32_t kMsgOccupantRemovedFromManager = 0x99ef1143;
constexpr std::uint32_t kMsgCameraMoved = 0xa6b79602;
constexpr std::uint32_t kMsgReturnAutomata = 0xaa1b2002;
constexpr std::uint32_t kMsgAutomatonRemoved = 0x09d92fc3;
constexpr std::uint32_t kMsgBegin = 0xe94671eb;
constexpr std::uint32_t kMsgActivated = 0xe94671ec;
constexpr std::uint32_t kMsgDeactivated = 0xe94671ed;
constexpr std::uint32_t kMsgEnd = 0x894671f6;

// ---- byte streams (only the typed calls the record uses are meaningful) ---------------------------------------------------

struct FakeOStream final : cIGZOStream {
    std::vector<unsigned char> bytes;
    bool error = false;
    template <class T> void Put(T value) {
        const auto* p = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    void Flush() override {}
    bool SetSint8(std::int8_t v) override { Put(v); return true; }
    bool SetUint8(std::uint8_t v) override { Put(v); return true; }
    bool SetSint16(std::int16_t v) override { Put(v); return true; }
    bool SetUint16(std::uint16_t v) override { Put(v); return true; }
    bool SetSint32(std::int32_t v) override { Put(v); return true; }
    bool SetUint32(std::uint32_t v) override { Put(v); return true; }
    bool SetSint64(std::int64_t v) override { Put(v); return true; }
    bool SetUint64(std::uint64_t v) override { Put(v); return true; }
    bool SetFloat32(float v) override { Put(v); return true; }
    bool SetFloat64(double v) override { Put(v); return true; }
    bool SetRZCharStr(const char*) override { error = true; return false; }
    bool SetGZStr(const cIGZString&) override { error = true; return false; }
    bool SetGZSerializable(const cIGZSerializable&) override { error = true; return false; }
    bool SetVoid(const void* source, std::uint32_t byteCount) override {
        const auto* p = static_cast<const unsigned char*>(source);
        bytes.insert(bytes.end(), p, p + byteCount);
        return true;
    }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

struct FakeIStream final : cIGZIStream {
    std::vector<unsigned char> bytes;
    std::size_t position = 0;
    bool error = false;
    template <class T> T Get() {
        T value{};
        if (error || position + sizeof(T) > bytes.size()) {
            error = true;
            return value;
        }
        std::memcpy(&value, bytes.data() + position, sizeof(T));
        position += sizeof(T);
        return value;
    }
    bool Skip(std::uint32_t n) override {
        if (error || position + n > bytes.size()) { error = true; return false; }
        position += n;
        return true;
    }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool GetSint8(std::int8_t& v) override { v = Get<std::int8_t>(); return !error; }
    bool GetUint8(std::uint8_t& v) override { v = Get<std::uint8_t>(); return !error; }
    bool GetSint16(std::int16_t& v) override { v = Get<std::int16_t>(); return !error; }
    bool GetUint16(std::uint16_t& v) override { v = Get<std::uint16_t>(); return !error; }
    bool GetSint32(std::int32_t& v) override { v = Get<std::int32_t>(); return !error; }
    bool GetUint32(std::uint32_t& v) override { v = Get<std::uint32_t>(); return !error; }
    bool GetSint64(std::int64_t& v) override { v = Get<std::int64_t>(); return !error; }
    bool GetUint64(std::uint64_t& v) override { v = Get<std::uint64_t>(); return !error; }
    bool GetFloat32(float& v) override { v = Get<float>(); return !error; }
    bool GetFloat64(double& v) override { v = Get<double>(); return !error; }
    bool GetRZCharStr(char*, std::uint32_t) override { error = true; return false; }
    bool GetGZStr(cIGZString&) override { error = true; return false; }
    bool GetGZSerializable(cIGZSerializable&) override { error = true; return false; }
    bool GetVoid(void* destination, std::uint32_t byteCount) override {
        if (error || position + byteCount > bytes.size()) { error = true; return false; }
        std::memcpy(destination, bytes.data() + position, byteCount);
        position += byteCount;
        return true;
    }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

// ---- fake occupants -------------------------------------------------------------------------------------------------------

struct FakeOccupant final : cISC4Occupant {
    int refs = 0;
    std::uint32_t type = 0;
    std::uint32_t group = 0;
    float position[3] = {0, 0, 0};
    int positionSets = 0;
    SC4CellRect cells{0, 0, 0, 0};
    std::vector<std::uint32_t> groupQueries;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cISC4Occupant) { *out = this; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof(position)); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); ++positionSets; }
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = cells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t g) const override {
        const_cast<FakeOccupant*>(this)->groupQueries.push_back(g);
        return g == group;
    }
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

struct FakePylon final : IPylonOccupant {
    int refs = 0;
    bool inited = false;
    bool shutDown = false;
    std::uint32_t uiState = 0;
    tDispatchType dispatchType = static_cast<tDispatchType>(0);
    tDispatchState dispatchState = static_cast<tDispatchState>(0);
    std::uint16_t callNumber = 0;
    cISC4Occupant* sourceOccupant = nullptr;
    FakeOccupant occupant;
    int callNumberSets = 0;

    FakePylon() { occupant.cells = SC4CellRect{0, 0, 1, 1}; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    cISC4Occupant* AsOccupant() override { return &occupant; }
    bool Init() override { inited = true; return true; }
    bool Shutdown() override { shutDown = true; return true; }
    void SetUIState(std::uint32_t state) override { uiState = state; }
    void SetDispatchType(tDispatchType type) override { dispatchType = type; }
    tDispatchType GetDispatchType() const override { return dispatchType; }
    void SetDispatchState(tDispatchState state) override { dispatchState = state; }
    void SetCallNumber(std::uint16_t value) override { callNumber = value; ++callNumberSets; }
    std::uint16_t GetCallNumber() const override { return callNumber; }
    void SetSourceOccupant(cISC4Occupant* occupantPtr) override { sourceOccupant = occupantPtr; }
    cISC4Occupant* GetSourceOccupant() const override { return sourceOccupant; }
};

struct FakeAutomaton final : IDispatchAutomaton {
    int refs = 0;
    FakeOccupant* occupant = nullptr;
    std::uint16_t callNumber = 0;
    std::uint32_t dispatchInfoID = 0;
    tDispatchState dispatchState = static_cast<tDispatchState>(0);
    int removeCalls = 0;
    int returnCalls = 0;
    cIGZMessageTarget2* callback = nullptr;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    cISC4Occupant* Occupant() override { return occupant; }
    bool Remove(bool) override { ++removeCalls; return true; }
    std::uint16_t GetCallNumber() const override { return callNumber; }
    void SetCallNumber(std::uint16_t value) override { callNumber = value; }
    void SetDispatchState(tDispatchState state) override { dispatchState = state; }
    tDispatchState GetDispatchState() const override { return dispatchState; }
    void SetDispatchInfoID(std::uint32_t id) override { dispatchInfoID = id; }
    void ReturnToSourceOccupant() override { ++returnCalls; }
    void AddCallback(cIGZMessageTarget2* target) override { callback = target; }
};

struct FakeVehicleView final : IDispatchVehicleView {
    struct Track { cISC4Occupant* occupant; int trackerType; std::uint32_t callNumber; };
    int refs = 0;
    int initCalls = 0, shutdownCalls = 0, viewChangedCalls = 0, nextTrackId = 1;
    std::vector<Track> tracked;
    std::vector<std::pair<int, bool>> untracked;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { ++initCalls; return true; }
    bool Shutdown() override { ++shutdownCalls; return true; }
    void HandleViewChanged() override { ++viewChangedCalls; }
    int TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t callNumber, const cRZString*) override {
        tracked.push_back(Track{occupant, trackerType, callNumber});
        return nextTrackId++;
    }
    void UntrackOccupant(int trackId, bool force) override { untracked.emplace_back(trackId, force); }
};

struct FakeCity final : IDispatchCity {
    int refs = 0;
    int releasedStations = 0;
    int cellToWorldCalls = 0;
    int heightCalls = 0;
    float height = 0.0f;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    void CellToWorld(std::int32_t cellX, std::int32_t cellZ, float& x, float& z) override {
        ++cellToWorldCalls;
        x = static_cast<float>(cellX) * 16.0f;
        z = static_cast<float>(cellZ) * 16.0f;
    }
    float CellHeight(float, float) override { ++heightCalls; return height; }
    void ReleaseStationList(std::list<cISC4Occupant*>& stations) override {
        releasedStations += static_cast<int>(stations.size());
    }
};

struct FakeSource final : IDispatchSource {
    int refs = 0;
    std::vector<std::uint32_t> handledTypes;
    bool canDispatchResult = true;
    bool allBusy = true;   // every unit out (i386 0x0018829c refuses a dispatch without a call box then)
    bool dispatchesAtStationResult = true;
    cISC4Occupant* nearestStation = nullptr;
    std::uint32_t nearestCallNumber = 0;
    std::vector<cISC4Occupant*> stations;
    std::uint32_t callMask = 0, baseCall = 0, dispatchCount = 0;
    std::uint32_t automatonGroup = 0x4303;
    cGZPersistResourceKey automatonKey{0x11, 0x22, 0x33};
    std::uint32_t automatonFlags = 0x44;
    float lastPropertiesPosition[3] = {0, 0, 0};
    int canDispatchCalls = 0, getStationsCalls = 0, getDispatchesCalls = 0;
    int dispatchFromStationCalls = 0, dispatchFromNearestCalls = 0, revokeCalls = 0, propertiesCalls = 0;
    cISC4Occupant* lastStation = nullptr;
    std::uint32_t lastCallNumber = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool CanHandleDispatchType(std::uint32_t type) const override {
        for (std::uint32_t handled : handledTypes) if (handled == type) return true;
        return false;
    }
    bool CanDispatch(float, float, bool* flag) override {
        ++canDispatchCalls;
        *flag = allBusy;
        return canDispatchResult;
    }
    bool GetStations(std::list<cISC4Occupant*>& out) override {
        ++getStationsCalls;
        for (cISC4Occupant* station : stations) out.push_back(station);
        return true;
    }
    bool GetDispatchesAtStation(cISC4Occupant*, std::uint32_t* mask, std::uint32_t* base, std::uint32_t* count) override {
        ++getDispatchesCalls;
        *mask = callMask;
        *base = baseCall;
        *count = dispatchCount;
        return dispatchesAtStationResult;
    }
    cISC4Occupant* DispatchFromNearestStation(float, float, std::uint32_t* callNumber) override {
        ++dispatchFromNearestCalls;
        *callNumber = nearestCallNumber;
        return nearestStation;
    }
    bool DispatchFromStation(cISC4Occupant* station, std::uint32_t* callNumber) override {
        ++dispatchFromStationCalls;
        lastStation = station;
        lastCallNumber = callNumber ? *callNumber : 0;
        return true;
    }
    void RevokeDispatchFromStation(cISC4Occupant* station, std::uint32_t callNumber) override {
        ++revokeCalls;
        lastStation = station;
        lastCallNumber = callNumber;
    }
    bool GetDispatchAutomataProperties(cISC4Occupant* station, std::uint32_t& group, cGZPersistResourceKey& key,
                                       std::uint32_t& flags, const float position[3]) override {
        ++propertiesCalls;
        lastStation = station;
        group = automatonGroup;
        key = automatonKey;
        flags = automatonFlags;
        std::memcpy(lastPropertiesPosition, position, sizeof(lastPropertiesPosition));
        return true;
    }
};

struct FakeAutomataManager final : IAutomataDispatchManager {
    FakeAutomaton* created = nullptr;
    int createCalls = 0, reDispatchCalls = 0;
    std::uint32_t lastGroup = 0, lastFlags = 0;
    std::uint8_t lastType = 0, lastArgument = 0;
    cGZPersistResourceKey lastKey;
    tDispatchLocation lastFrom, lastTo;

    IDispatchAutomaton* Dispatch(std::uint32_t group, const tDispatchLocation& from, const tDispatchLocation& to,
                                 std::uint32_t flags, std::uint8_t type, const cGZPersistResourceKey& key,
                                 std::uint8_t argument) override {
        ++createCalls;
        lastGroup = group;
        lastFrom = from;
        lastTo = to;
        lastFlags = flags;
        lastType = type;
        lastKey = key;
        lastArgument = argument;
        return created;
    }
    bool Dispatch(IDispatchAutomaton* automaton, const tDispatchLocation& from, const tDispatchLocation& to,
                  std::uint32_t flags, std::uint8_t type) override {
        ++reDispatchCalls;
        (void)automaton;
        lastFrom = from;
        lastTo = to;
        lastFlags = flags;
        lastType = type;
        return true;
    }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    struct Message { std::uint32_t type; std::uint64_t data1, data2; cIGZUnknown* unknown; };
    std::vector<Message> posted;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    int refs = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        posted.push_back(Message{message->GetType(), standard->GetData1(), standard->GetData2(), standard->GetIGZUnknown()});
        return true;
    }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t messageType) override {
        added.emplace_back(target, messageType);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t messageType) override {
        removed.emplace_back(target, messageType);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeHost final : IDispatchHost {
    FakeMessageServer server;
    FakeCity city;
    FakeVehicleView view;
    FakeAutomataManager* automataManager = nullptr;
    std::vector<std::unique_ptr<FakePylon>> pylons;
    int addLayerCalls = 0, removeLayerCalls = 0;
    std::vector<const cIGZUnknown*> written;
    struct ReadEntry { std::uint32_t iid; void* object; };
    std::deque<ReadEntry> readEntries;

    cIGZMessageServer2* MessageServer() override { return &server; }
    IDispatchCity* City() override { return &city; }
    IDispatchVehicleView* CreateDispatchVehicleView() override { ++view.refs; return &view; }
    IPylonOccupant* CreatePylonOccupant() override {
        pylons.push_back(std::make_unique<FakePylon>());
        FakePylon* pylon = pylons.back().get();
        pylon->refs = 1;   // new + AddRef, as the original does
        return pylon;
    }
    IAutomataDispatchManager* GetAutomataManagerForGroup(std::uint32_t) override { return automataManager; }
    void AddDispatchViewLayer(IDispatchVehicleView*) override { ++addLayerCalls; }
    void RemoveDispatchViewLayer(IDispatchVehicleView*) override { ++removeLayerCalls; }
    bool WriteSerializable(cIGZOStream&, const cIGZUnknown* object) override {
        written.push_back(object);
        return true;
    }
    bool ReadSerializable(cIGZIStream&, std::uint32_t iid, void** out) override {
        for (auto it = readEntries.begin(); it != readEntries.end(); ++it) {
            if (it->iid != iid) continue;
            *out = it->object;
            readEntries.erase(it);
            return true;
        }
        *out = nullptr;
        return false;
    }
};

// Reads a little-endian value out of the record bytes.
template <class T> T At(const std::vector<unsigned char>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

struct Fixture {
    FakeHost host;
    FakeCity& city = host.city;
    FakeVehicleView& view = host.view;
    FakeMessageServer& server = host.server;
    FakeSource source;
    FakeAutomataManager automataManager;
    FakeAutomaton automatonA;
    FakeOccupant station;
    FakeOccupant automatonOccupant;

    Fixture() {
        SetDispatchHost(&host);
        host.automataManager = &automataManager;
        automataManager.created = &automatonA;
        automatonA.occupant = &automatonOccupant;
        automatonOccupant.group = 0x4301;   // pedestrian group -> tracker type 1
        source.handledTypes = {0, 1};
        source.stations = {&station};
        source.callMask = 0x1;
        source.baseCall = 0;
        source.dispatchCount = 1;
        source.nearestStation = &station;
        source.nearestCallNumber = 0;
        station.cells = SC4CellRect{0, 0, 1, 1};
        automataManager.created = &automatonA;
    }
    ~Fixture() { SetDispatchHost(nullptr); }

    // Fill the source with a station that offers `count` call numbers starting at `base`.
    void StationOffers(std::uint32_t mask, std::uint32_t base, std::uint32_t count) {
        source.callMask = mask;
        source.baseCall = base;
        source.dispatchCount = count;
    }
};

void PostStandard(cSC4DispatchManager& manager, std::uint32_t type, void* object, std::uint64_t data2 = 0,
                  std::uint64_t data3 = 0) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    if (object) message->SetVoid1(object);
    message->SetData2(data2);
    message->SetData3(data3);
    manager.DoMessage(message);
    message->Release();   // deletes the heap message
}

std::size_t CountPosted(const FakeMessageServer& server, std::uint32_t type) {
    std::size_t count = 0;
    for (const auto& message : server.posted) count += message.type == type ? 1 : 0;
    return count;
}

}  // namespace

int main() {
    // ---- interfaces and the shared refcount ----
    {
        auto* manager = new cSC4DispatchManager();
        CHECK(manager->GetGZCLSID() == 0x4990bd93);
        void* pointer = nullptr;
        CHECK(manager->QueryInterface(1, &pointer) && pointer == manager);
        CHECK(manager->QueryInterface(kGZIID_cISC4DispatchManager, &pointer) && pointer == manager);
        CHECK(manager->QueryInterface(kGZIID_cIGZMessageTarget2, &pointer) && pointer != nullptr && pointer != manager);
        CHECK(manager->QueryInterface(kGZIID_cIGZSerializable, &pointer) && pointer != nullptr && pointer != manager);
        CHECK(!manager->QueryInterface(0x1234, &pointer));
        CHECK(manager->AddRef() == 5);   // four successful QueryInterface calls + this AddRef
        CHECK(manager->Release() == 4 && manager->Release() == 3 && manager->Release() == 2);
        CHECK(manager->Release() == 1);
        CHECK(manager->Release() == 0);  // last release runs Shutdown and deletes
    }

    // ---- Init / Shutdown ----
    {
        Fixture fixture;
        cSC4DispatchManager manager;
        manager.AddRef();   // held for the whole block: posted messages AddRef/Release the manager
        CHECK(manager.Init());
        CHECK(manager.Init());   // idempotent
        CHECK(manager.GetDispatchVehicleView() == &fixture.view);
        CHECK(fixture.view.initCalls == 1 && fixture.host.addLayerCalls == 1 && fixture.view.refs == 1);
        CHECK(fixture.city.refs == 1);
        CHECK(fixture.server.added.size() == 3);
        CHECK(fixture.server.added[0].second == kMsgCameraMoved);
        CHECK(fixture.server.added[1].second == kMsgOccupantRemovedFromManager);
        CHECK(fixture.server.added[2].second == kMsgReturnAutomata);
        CHECK(fixture.server.refs == 1);

        CHECK(manager.Shutdown());
        CHECK(manager.Shutdown());   // idempotent
        CHECK(fixture.server.removed.size() == 3 && fixture.server.refs == 0);
        CHECK(fixture.host.removeLayerCalls == 1 && fixture.view.shutdownCalls == 1 && fixture.view.refs == 0);
        CHECK(fixture.city.refs == 0);
        CHECK(manager.GetDispatchVehicleView() == nullptr);
    }

    // ---- dispatch source registry ----
    {
        Fixture fixture;
        FakeSource police;
        police.handledTypes = {kDispatchTypePolice};
        FakeSource fire;
        fire.handledTypes = {kDispatchTypeFire};
        cSC4DispatchManager manager;   // declared last so it is destroyed before the sources it references
        CHECK(manager.GetDispatchSource(kDispatchTypeFire) == nullptr);
        manager.RegisterDispatchSource(&police);
        manager.RegisterDispatchSource(&fire);
        CHECK(police.refs == 1 && fire.refs == 1);
        CHECK(manager.GetDispatchSource(kDispatchTypePolice) == &police);
        CHECK(manager.GetDispatchSource(kDispatchTypeFire) == &fire);
        CHECK(manager.GetDispatchSource(kDispatchTypeFirePlane) == nullptr);

        manager.UnregisterDispatchSource(&police);
        CHECK(police.refs == 0);
        CHECK(manager.GetDispatchSource(kDispatchTypePolice) == nullptr);
        manager.UnregisterDispatchSource(&police);   // no-op
        CHECK(fire.refs == 1);

        manager.RegisterDispatchSource(nullptr);     // the original stores a null entry
        CHECK(manager.GetDispatchSource(kDispatchTypeFire) == &fire);
        manager.UnregisterDispatchSource(nullptr);
        CHECK(fire.refs == 1);
    }

    // ---- tool call-box placement + a full dispatch through the active pylon ----
    {
        Fixture fixture;
        cSC4DispatchManager manager;
        manager.AddRef();   // held for the whole block: posted messages AddRef/Release the manager
        CHECK(manager.Init());
        manager.RegisterDispatchSource(&fixture.source);
        fixture.StationOffers(0x3, 2, 2);   // two call numbers, 2 and 3

        CHECK(manager.BeginDispatchTool(kDispatchTypeFire));
        CHECK(manager.ToolType() == kDispatchTypeFire);
        CHECK(fixture.host.pylons.size() == 2);
        CHECK(fixture.source.getStationsCalls == 1);
        CHECK(fixture.city.cellToWorldCalls == 2 && fixture.city.heightCalls == 2);
        CHECK(fixture.city.releasedStations == 1);   // the station list went back to the city

        FakePylon* first = fixture.host.pylons[0].get();
        FakePylon* second = fixture.host.pylons[1].get();
        CHECK(first->inited && second->inited);
        CHECK(first->callNumber == 2 && second->callNumber == 3);
        CHECK(first->dispatchType == kDispatchTypeFire && second->dispatchType == kDispatchTypeFire);
        CHECK(first->dispatchState == kDispatchStateIdle && second->dispatchState == kDispatchStateIdle);
        CHECK(first->uiState == 1 && second->uiState == 1);
        CHECK(first->sourceOccupant == &fixture.station && second->sourceOccupant == &fixture.station);
        CHECK(first->refs == 1 && second->refs == 1);
        // A = (0,0), B = (32,32), length 45.2548, step length/3 -> call boxes at 1/3 and 2/3
        CHECK(Near(first->occupant.position[0], 10.666667f) && Near(first->occupant.position[2], 10.666667f));
        CHECK(Near(second->occupant.position[0], 21.333334f) && Near(second->occupant.position[2], 21.333334f));
        CHECK(manager.AvailablePylonCount() == 2);
        CHECK(manager.GetAvailablePylonCount(kDispatchTypeFire) == 2);
        CHECK(fixture.source.getDispatchesCalls == 2);   // once for the tool, once for the count query
        CHECK(manager.BeginDispatchTool(kDispatchTypeFire));   // already the active tool
        CHECK(manager.EndDispatchTool(kDispatchTypePolice) == false);

        // pick a call box and start the dispatch
        manager.SetActivePylon(first);
        CHECK(manager.GetActivePylon() == first);
        CHECK(first->uiState == 2 && first->refs == 2);
        const std::uint32_t id = manager.Dispatch(kDispatchTypeFire, 100.0f, 200.0f);
        CHECK(id == 1);
        CHECK(fixture.source.canDispatchCalls == 1);
        CHECK(fixture.source.dispatchFromStationCalls == 1);
        CHECK(fixture.source.lastStation == &fixture.station && fixture.source.lastCallNumber == 2);
        CHECK(first->dispatchType == kDispatchTypeFire);
        CHECK(Near(first->occupant.position[0], 100.0f) && Near(first->occupant.position[2], 200.0f));
        CHECK(first->refs == 2);                        // active-pylon reference + dispatch reference
        CHECK(manager.AvailablePylonCount() == 1);      // `second` is still waiting
        CHECK(fixture.source.propertiesCalls == 1);
        CHECK(fixture.automataManager.createCalls == 1);
        CHECK(fixture.automataManager.lastGroup == 0x4303);
        CHECK(fixture.automataManager.lastType == 0 && fixture.automataManager.lastArgument == 0xff);
        CHECK(fixture.automataManager.lastKey == fixture.source.automatonKey);
        CHECK(fixture.automataManager.lastFlags == 0x44);
        CHECK(Near(fixture.automataManager.lastTo.x, 100.0f) && Near(fixture.automataManager.lastTo.z, 200.0f));
        CHECK(fixture.automataManager.lastTo.type == 1 && fixture.automataManager.lastFrom.type == 0);
        CHECK(fixture.automatonA.refs == 1);
        CHECK(fixture.automatonA.callback == static_cast<cIGZMessageTarget2*>(&manager));
        CHECK(fixture.automatonA.dispatchInfoID == 1 && fixture.automatonA.callNumber == 2);
        CHECK(fixture.automatonA.dispatchState == kDispatchStateDispatched);
        CHECK(fixture.view.tracked.size() == 1);
        CHECK(fixture.view.tracked[0].occupant == &fixture.automatonOccupant);
        CHECK(fixture.view.tracked[0].trackerType == 1);   // pedestrian group
        CHECK(fixture.view.tracked[0].callNumber == 2);
        CHECK(first->callNumber == 2 && first->dispatchState == kDispatchStateDispatched);
        CHECK(CountPosted(fixture.server, kMsgBegin) == 1);
        CHECK(fixture.server.posted.back().data1 == 1 && fixture.server.posted.back().data2 == 1);
        CHECK(fixture.server.posted.back().unknown == static_cast<cISC4DispatchManager*>(&manager));

        DispatchInfo info;
        CHECK(manager.GetDispatchInfo(1, info));
        CHECK(info.type == kDispatchTypeFire && info.id == 1 && info.callNumber == 2 && info.trackId == 1);
        CHECK(info.station == &fixture.station && info.automaton == &fixture.automatonA && info.pylon == first);
        CHECK(Near(info.x, 100.0f) && Near(info.z, 200.0f) && info.active == false);
        CHECK(!manager.GetDispatchInfo(99, info));
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 1);
        CHECK(manager.GetDispatchCount(kDispatchTypePolice) == 0);
        std::uint32_t ids[4] = {0, 0, 0, 0};
        std::uint32_t limit = 4;
        manager.GetDispatchIDArray(kDispatchTypeFire, ids, limit);
        CHECK(ids[0] == 1 && limit == 4);
        CHECK(manager.GetAutomatonFromPylon(first) == &fixture.automatonA);
        CHECK(manager.GetAutomatonFromPylon(second) == nullptr);

        // the automaton reports that it arrived (event 7) with dispatch state 5
        PostStandard(manager, kMsgAutomatonRemoved, &fixture.automatonA, 7, 5);
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 1);
        CHECK(CountPosted(fixture.server, kMsgActivated) == 1);
        CHECK(fixture.server.posted.back().data1 == 1 && fixture.server.posted.back().data2 == 1);
        CHECK(first->dispatchState == kDispatchStateActive);
        CHECK(manager.GetDispatchInfo(1, info) && info.active == true);

        // revoking by pylon ends the dispatch and puts the call box back on the line
        fixture.source.getDispatchesCalls = 0;
        CHECK(manager.RevokeDispatchByPylon(first));
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 0);
        CHECK(fixture.source.revokeCalls == 1);
        CHECK(fixture.source.lastStation == &fixture.station && fixture.source.lastCallNumber == 2);
        CHECK(CountPosted(fixture.server, kMsgEnd) == 1);
        CHECK(fixture.server.posted.back().data1 == 1 && fixture.server.posted.back().data2 == 1);
        CHECK(manager.GetActivePylon() == nullptr);
        CHECK(first->uiState == 1 && first->dispatchState == kDispatchStateIdle);
        CHECK(Near(first->occupant.position[0], 10.666667f) && Near(first->occupant.position[2], 10.666667f));
        CHECK(fixture.source.getDispatchesCalls == 2);   // EndDispatch + the second ReturnPylonToStation
        CHECK(fixture.view.untracked.size() == 1 && fixture.view.untracked[0].first == 1);
        CHECK(fixture.view.untracked[0].second == false);
        CHECK(fixture.automatonA.refs == 0 && fixture.station.refs == 0);
        CHECK(manager.AvailablePylonCount() == 2);      // `first` is back in the list
        CHECK(first->refs == 1 && second->refs == 1);

        CHECK(manager.EndDispatchTool(kDispatchTypeFire));
        CHECK(manager.ToolType() == kDispatchTypeNone);
        CHECK(manager.AvailablePylonCount() == 0);
        CHECK(first->shutDown && second->shutDown);
        CHECK(first->refs == 0 && second->refs == 0);
        CHECK(manager.EndDispatchTool(kDispatchTypeFire) == false);
        CHECK(manager.Shutdown());
    }

    // ---- occupant removed: the dispatch and every waiting call box of that station go away ----
    {
        Fixture fixture;
        cSC4DispatchManager manager;
        manager.AddRef();   // held for the whole block: posted messages AddRef/Release the manager
        CHECK(manager.Init());
        manager.RegisterDispatchSource(&fixture.source);
        fixture.StationOffers(0x7, 0, 3);
        CHECK(manager.BeginDispatchTool(kDispatchTypeFire));
        CHECK(fixture.host.pylons.size() == 3);
        FakePylon* first = fixture.host.pylons[0].get();
        FakePylon* second = fixture.host.pylons[1].get();
        FakePylon* third = fixture.host.pylons[2].get();
        manager.SetActivePylon(first);
        CHECK(manager.Dispatch(kDispatchTypeFire, 5.0f, 6.0f) == 1);
        CHECK(manager.AvailablePylonCount() == 2);
        CHECK(first->refs == 2 && second->refs == 1 && third->refs == 1);   // first: active + dispatch

        PostStandard(manager, kMsgOccupantRemovedFromManager, &fixture.station);
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 0);
        CHECK(manager.GetActivePylon() == nullptr);
        CHECK(fixture.automatonA.removeCalls == 1 && fixture.automatonA.refs == 0);
        CHECK(first->shutDown && second->shutDown && third->shutDown);
        CHECK(first->refs == 0 && second->refs == 0 && third->refs == 0);
        CHECK(fixture.station.refs == 0);
        CHECK(manager.AvailablePylonCount() == 0);
        CHECK(fixture.view.untracked.size() == 1);
        // the occupant-removal path posts no "dispatch ended" message (quirk)
        CHECK(CountPosted(fixture.server, kMsgEnd) == 0);
        CHECK(CountPosted(fixture.server, kMsgBegin) == 1);
        CHECK(manager.EndDispatchTool(kDispatchTypeFire));
        CHECK(manager.Shutdown());
    }

    // ---- remaining messages ----
    {
        Fixture fixture;
        cSC4DispatchManager manager;
        manager.AddRef();   // held for the whole block: posted messages AddRef/Release the manager
        CHECK(manager.Init());
        manager.RegisterDispatchSource(&fixture.source);
        fixture.source.allBusy = false;   // a free unit: dispatching works without an active call box
        CHECK(manager.Dispatch(kDispatchTypeFire, 1.0f, 2.0f) == 1);

        PostStandard(manager, kMsgCameraMoved, nullptr);
        CHECK(fixture.view.viewChangedCalls == 1);

        PostStandard(manager, kMsgReturnAutomata, nullptr);
        CHECK(fixture.automatonA.returnCalls == 1);

        // an unknown automaton event kind is ignored
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgAutomatonRemoved);
        message->SetVoid1(&fixture.automatonA);
        message->SetData2(4);
        CHECK(!manager.DoMessage(message));
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 1);
        message->SetData2(7);
        message->SetData3(5);   // state 5 -> activated
        CHECK(manager.DoMessage(message));
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 1);
        CHECK(CountPosted(fixture.server, kMsgActivated) == 1);
        message->SetData3(0);   // state 0 -> deactivated and revoked
        CHECK(manager.DoMessage(message));
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 0);
        CHECK(CountPosted(fixture.server, kMsgDeactivated) == 1);
        CHECK(CountPosted(fixture.server, kMsgEnd) == 1);
        message->Release();
        CHECK(manager.Shutdown());
    }

    // ---- persistence: record layout, additive Read, restored Init ----
    {
        Fixture fixture;
        cSC4DispatchManager writer;
        writer.AddRef();   // held for the whole block: posted messages AddRef/Release the manager
        CHECK(writer.Init());
        writer.RegisterDispatchSource(&fixture.source);
        fixture.StationOffers(0x1, 5, 1);
        CHECK(writer.BeginDispatchTool(kDispatchTypeFire));
        FakePylon* pylon = fixture.host.pylons[0].get();
        CHECK(pylon->callNumber == 5);
        writer.SetActivePylon(pylon);
        CHECK(writer.Dispatch(kDispatchTypeFire, 10.0f, 20.0f) == 1);

        FakeOStream stream;
        CHECK(writer.Write(stream));
        // version 2, one dispatch: type 1, id 1, call 5, inactive, x/y/z, next id 1, no active-pylon bytes,
        // no available pylons, tool type 1
        CHECK(stream.bytes.size() == 43);
        CHECK(At<std::uint16_t>(stream.bytes, 0) == 2);
        CHECK(At<std::int32_t>(stream.bytes, 2) == 1);
        CHECK(At<std::uint32_t>(stream.bytes, 6) == 1);
        CHECK(At<std::int32_t>(stream.bytes, 10) == 1);
        CHECK(At<std::int32_t>(stream.bytes, 14) == 5);
        CHECK(At<std::uint8_t>(stream.bytes, 18) == 0);
        CHECK(At<float>(stream.bytes, 19) == 10.0f);
        CHECK(At<float>(stream.bytes, 23) == 0.0f);
        CHECK(At<float>(stream.bytes, 27) == 20.0f);
        CHECK(At<std::int32_t>(stream.bytes, 31) == 1);   // next dispatch id
        CHECK(At<std::int32_t>(stream.bytes, 35) == 0);   // no available call boxes
        CHECK(At<std::int32_t>(stream.bytes, 39) == static_cast<std::int32_t>(kDispatchTypeFire));
        CHECK(fixture.host.written.size() == 4);
        CHECK(fixture.host.written[0] == &fixture.station);
        CHECK(fixture.host.written[1] == &fixture.automatonA);
        CHECK(fixture.host.written[2] == pylon);
        CHECK(fixture.host.written[3] == pylon);          // the active pylon

        // read it back twice: the record is additive and the old active pylon is not released (quirks)
        fixture.host.readEntries = {{kGZIID_cISC4Occupant, &fixture.station},
                                    {kDispatchAutomatonIid, &fixture.automatonA},
                                    {kDispatchPylonIid, pylon},
                                    {kDispatchPylonIid, pylon},
                                    {kGZIID_cISC4Occupant, &fixture.station},
                                    {kDispatchAutomatonIid, &fixture.automatonA},
                                    {kDispatchPylonIid, pylon},
                                    {kDispatchPylonIid, nullptr}};
        cSC4DispatchManager reader;
        reader.AddRef();
        FakeIStream firstRead;
        firstRead.bytes = stream.bytes;
        CHECK(reader.Read(firstRead));
        CHECK(reader.DispatchCount() == 1);
        CHECK(reader.GetActivePylon() == pylon);
        CHECK(reader.ToolType() == kDispatchTypeFire);
        CHECK(reader.AvailablePylonCount() == 0);
        DispatchInfo info;
        CHECK(reader.GetDispatchInfo(1, info));
        CHECK(info.type == kDispatchTypeFire && info.callNumber == 5);
        CHECK(info.trackId == 0);   // quirk: the dispatch-view track id is not persisted
        CHECK(Near(info.x, 10.0f) && Near(info.z, 20.0f));
        CHECK(info.station == &fixture.station && info.automaton == &fixture.automatonA && info.pylon == pylon);

        FakeIStream secondRead;
        secondRead.bytes = stream.bytes;
        CHECK(reader.Read(secondRead));
        CHECK(reader.DispatchCount() == 2);   // additive, no clear
        CHECK(reader.GetActivePylon() == nullptr);   // overwritten; the old pylon was not released

        // Init on a restored manager drops the saved tool state and re-tracks the automata in the view
        fixture.view.tracked.clear();
        CHECK(reader.Init());
        CHECK(reader.ToolType() == kDispatchTypeNone);
        CHECK(reader.GetActivePylon() == nullptr);
        CHECK(fixture.view.tracked.size() == 2);   // one per restored dispatch
        CHECK(fixture.view.tracked[0].trackerType == 0);   // dispatch type 1 -> table lookup 0
        CHECK(fixture.view.tracked[0].callNumber == 5);
        CHECK(reader.Shutdown());
        CHECK(writer.Shutdown());
    }

    // ---- EndDispatch without a dispatch source leaves the dispatch objects attached (quirk) ----
    {
        Fixture fixture;
        cSC4DispatchManager manager;
        manager.AddRef();
        CHECK(manager.Init());
        manager.RegisterDispatchSource(&fixture.source);
        fixture.source.allBusy = false;
        CHECK(manager.Dispatch(kDispatchTypeFire, 1.0f, 1.0f) == 1);
        CHECK(fixture.automatonA.refs == 1 && fixture.station.refs == 1);
        manager.UnregisterDispatchSource(&fixture.source);
        CHECK(!manager.RevokeDispatchByID(1));                       // EndDispatch cannot find a source
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 0);     // the list entry is removed anyway
        CHECK(fixture.automatonA.refs == 1 && fixture.station.refs == 1);   // nothing was released
        CHECK(CountPosted(fixture.server, kMsgEnd) == 0);
        CHECK(manager.Shutdown());
    }

    // ---- kept quirks and failure paths ----
    {
        Fixture fixture;
        cSC4DispatchManager manager;
        manager.AddRef();   // held for the whole block: posted messages AddRef/Release the manager
        CHECK(manager.Init());
        manager.RegisterDispatchSource(&fixture.source);
        fixture.source.allBusy = false;

        CHECK(manager.Dispatch(kDispatchTypeFire, 0.0f, 0.0f) == 1);
        CHECK(manager.Dispatch(kDispatchTypeFire, 1.0f, 1.0f) == 2);
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 2);
        // RevokeAllDispatches ignores its type argument and revokes everything
        CHECK(manager.RevokeAllDispatches(kDispatchTypePolice));
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 0);
        CHECK(CountPosted(fixture.server, kMsgEnd) == 2);
        CHECK(fixture.source.revokeCalls == 2);

        // no active pylon and every unit out -> nothing happens
        fixture.source.allBusy = true;
        CHECK(manager.Dispatch(kDispatchTypeFire, 0.0f, 0.0f) == 0);
        fixture.source.canDispatchResult = false;
        CHECK(manager.Dispatch(kDispatchTypeFire, 0.0f, 0.0f) == 0);
        CHECK(manager.CanDispatch(kDispatchTypeFirePlane, 0.0f, 0.0f) == false);   // no fire-plane source
        CHECK(manager.GetAvailablePylonCount(kDispatchTypeFirePlane) == 0);
        CHECK(manager.BeginDispatchTool(kDispatchTypeFirePlane) == false);
        fixture.source.canDispatchResult = true;
        fixture.source.allBusy = false;

        // a failing BeginDispatch (no automata manager) revokes the new dispatch again
        fixture.automataManager.createCalls = 0;
        fixture.host.automataManager = nullptr;
        CHECK(manager.Dispatch(kDispatchTypeFire, 7.0f, 8.0f) == 0);
        CHECK(manager.GetDispatchCount(kDispatchTypeFire) == 0);
        CHECK(fixture.automataManager.createCalls == 0);
        CHECK(fixture.source.revokeCalls == 3);   // EndDispatch ran for the failed dispatch
        fixture.host.automataManager = &fixture.automataManager;

        // quirk: with the dispatch tool running, no active pylon and no matching call box the manager creates a
        // call box, drops it without a Release and still reports success
        DispatchInfo info;
        fixture.StationOffers(0x1, 0, 1);
        CHECK(manager.BeginDispatchTool(kDispatchTypeFire));
        fixture.source.dispatchFromNearestCalls = 0;
        const std::size_t pylonsBefore = fixture.host.pylons.size();
        FakePylon* waiting = fixture.host.pylons.back().get();
        const std::uint32_t callsBefore = fixture.source.dispatchFromStationCalls;
        fixture.source.nearestCallNumber = 7;   // does not match the waiting call box (call number 0)
        const std::uint32_t id = manager.Dispatch(kDispatchTypeFire, 3.0f, 4.0f);
        CHECK(id != 0);
        CHECK(fixture.source.dispatchFromNearestCalls == 1);
        CHECK(fixture.source.dispatchFromStationCalls == static_cast<int>(callsBefore));   // no active pylon
        CHECK(fixture.host.pylons.size() == pylonsBefore + 1);            // the extra call box
        FakePylon* leaked = fixture.host.pylons.back().get();
        CHECK(leaked != waiting);
        CHECK(leaked->inited);
        CHECK(leaked->sourceOccupant == &fixture.station);
        CHECK(leaked->refs == 1);                                        // never released
        CHECK(manager.GetDispatchInfo(id, info));
        CHECK(info.pylon == nullptr);
        CHECK(manager.GetAvailablePylonCount(kDispatchTypeFire) == 1);
        CHECK(manager.RevokeDispatchByID(id));
        CHECK(manager.EndDispatchTool(kDispatchTypeFire));
        CHECK(manager.Shutdown());
    }

    if (failures == 0) { std::printf("sc4_dispatchmanager_test: all checks passed\n"); return 0; }
    std::printf("sc4_dispatchmanager_test: %d failure(s)\n", failures);
    return 1;
}
