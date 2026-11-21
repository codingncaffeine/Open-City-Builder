// cSC4MySimDispatch: interfaces/refcount, Init/Shutdown registration and the clock/path-finder references, Reset, the
// destination queue and default destination, the cLocation serialization, tracking through the dispatch view, the
// idle -> travelling -> path-found -> dispatch state machine (with host seams for the path finder, the automata manager,
// the transit network and the dispatch view), the message handlers (follow camera, five-minute tick, deferred Init) and
// the Write/Read record.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/MySimDispatch.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

// ---- byte streams (only the typed calls the record uses are meaningful) -------------------------------------------------

struct TestOStream final : cIGZOStream {
    std::vector<unsigned char> bytes;
    std::vector<std::string> strings;
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
    bool SetRZCharStr(const char* v) override { strings.emplace_back(v ? v : ""); return true; }
    bool SetGZStr(const cIGZString& v) override { strings.emplace_back(v.ToChar() ? v.ToChar() : ""); return true; }
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

struct TestIStream final : cIGZIStream {
    std::vector<unsigned char> bytes;
    std::vector<std::string> strings;
    std::size_t position = 0;
    std::size_t stringIndex = 0;
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
    bool GetGZStr(cIGZString& v) override {
        if (stringIndex >= strings.size()) { error = true; return false; }
        v.Copy(cRZString(strings[stringIndex++].c_str()));
        return true;
    }
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

// ---- fakes ------------------------------------------------------------------------------------------------------------

struct FakeOwner final : IMySimDispatchOwner {
    int refs = 0;
    int wealth = 1;
    bool male = true;
    struct Message { std::uint32_t type; std::uint64_t data1, data2, data3, data4; };
    std::vector<Message> messages;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool DoMessage(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        messages.push_back(Message{standard->GetType(), standard->GetData1(), standard->GetData2(),
                                   standard->GetData3(), standard->GetData4()});
        return true;
    }
    int GetWealth() const override { return wealth; }
    bool IsMale() const override { return male; }
};

struct FakeAutomaton;

struct FakeOccupant final : cISC4Occupant {
    int refs = 0;
    std::uint32_t type = 0x1234;
    std::uint32_t group = 0x4200;
    bool isBuilding = false;
    bool isPylon = false;
    FakeAutomaton* automaton = nullptr;
    float position[3] = {160.0f, 0.0f, 160.0f};
    std::vector<std::uint32_t> groupQueries;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof(position)); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = SC4CellRect{10, 10, 10, 10}; }
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
    bool IsOccupantGroup(std::uint32_t value) const override {
        auto* self = const_cast<FakeOccupant*>(this);
        self->groupQueries.push_back(value);
        return value == group;
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

struct FakeAutomaton final : IMySimDispatchAutomaton, IMySimDispatchRoute {
    int refs = 0;
    FakeOccupant* occupant = nullptr;
    std::deque<RouteData> route;
    std::uint32_t state = 0xffffffffu;
    std::uint8_t priority = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> anims;
    std::vector<std::pair<std::uint32_t, bool>> flagSets;
    cIGZMessageTarget2* callback = nullptr;
    int removeCalls = 0;
    bool routeResult = true;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kMySimDispatchIidAutomaton || iid == kMySimDispatchIidTrafficAutomaton) {
            *out = static_cast<IMySimDispatchAutomaton*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }

    cISC4Occupant* AsOccupant() override { return occupant; }
    void Remove(bool) override { ++removeCalls; }
    bool PlayAnim(std::uint32_t animId) override { anims.push_back(animId); return true; }
    void SetAutomataFlag(std::uint32_t flag, bool on) override {
        flagSets.emplace_back(flag, on);
        if (on) flags |= flag; else flags &= ~flag;
    }
    bool CheckAutomataFlag(std::uint32_t flag) override { return (flags & flag) != 0; }
    void AddCallback(cIGZMessageTarget2* target) override { callback = target; }
    void RemoveCallback(cIGZMessageTarget2* target) override { if (callback == target) callback = nullptr; }
    int GetStateId() const override { return static_cast<int>(state); }
    IMySimDispatchAutomaton* controllerSeen = reinterpret_cast<IMySimDispatchAutomaton*>(1);
    std::uint32_t animOffsetSeen = 0xffffffffu, sfxSeen = 0xffffffffu;
    bool SetState(std::uint32_t newState, IMySimDispatchAutomaton* controller, float, bool, std::uint32_t animOffset, std::uint32_t sfx, const void*) override {
        state = newState;
        controllerSeen = controller;
        animOffsetSeen = animOffset;
        sfxSeen = sfx;
        return true;
    }
    void SetPriority(std::uint8_t value) override { priority = value; }
    IMySimDispatchRoute* RouteInterface() override { AddRef(); return this; }
    bool GetRouteInfo(std::deque<RouteData>& out) const override { out = route; return true; }
    bool SetRouteInfo(const std::deque<RouteData>& value, bool, bool) override { route = value; return routeResult; }
};

bool FakeOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZIID_cISC4Occupant) { *out = static_cast<cISC4Occupant*>(this); AddRef(); return true; }
    if (iid == kGZIID_cISC4BuildingOccupant && isBuilding) { *out = static_cast<cISC4Occupant*>(this); AddRef(); return true; }
    if (iid == kMySimDispatchIidPylon && isPylon) { *out = static_cast<cISC4Occupant*>(this); AddRef(); return true; }
    if ((iid == kMySimDispatchIidAutomaton || iid == kMySimDispatchIidTrafficAutomaton) && automaton) {
        automaton->AddRef();
        *out = static_cast<IMySimDispatchAutomaton*>(automaton);
        return true;
    }
    return false;
}

struct FakeMessageServer final : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
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

struct FakeClock final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
};

struct FakeVehicleView final : IMySimDispatchVehicleView {
    int refs = 0;
    struct Track { cISC4Occupant* occupant; int trackerType; std::uint32_t callNumber; };
    std::vector<Track> tracked;
    std::vector<std::pair<int, bool>> untracked;
    int nextHandle = 5;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    int TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t callNumber, const cRZString*) override {
        tracked.push_back(Track{occupant, trackerType, callNumber});
        return nextHandle++;
    }
    void UntrackOccupant(int trackId, bool force) override { untracked.emplace_back(trackId, force); }
};

struct FakeAutomataManager final : IMySimDispatchAutomataManager {
    int refs = 0;
    std::vector<std::unique_ptr<FakeAutomaton>> automata;
    std::vector<FakeAutomaton*> created;
    FakeOccupant* automatonOccupant = nullptr;
    int kindSeen = -1;
    std::uint32_t flagsSeen = 0;
    cGZPersistResourceKey keySeen;
    std::uint8_t prioritySeen = 0;
    int driveActiveCalls = 0, viewActiveCalls = 0, clearDriveCalls = 0;

    IMySimDispatchAutomaton* CreateAutomaton(int kind, const float[2], std::uint32_t flags,
                                            const cGZPersistResourceKey& key, cIGZMessageTarget2*,
                                            std::uint8_t priority, int) override {
        kindSeen = kind;
        flagsSeen = flags;
        keySeen = key;
        prioritySeen = priority;
        automata.push_back(std::make_unique<FakeAutomaton>());
        FakeAutomaton* automaton = automata.back().get();
        automaton->refs = 1;   // one reference handed to the dispatch
        automaton->occupant = automatonOccupant;
        if (automatonOccupant) automatonOccupant->automaton = automaton;
        created.push_back(automaton);
        return automaton;
    }
    bool PlayerDriveActive() override { ++driveActiveCalls; return true; }
    bool DriveViewActive() override { ++viewActiveCalls; return true; }
    void SetPlayerDriveAutomaton(IMySimDispatchAutomaton*) override { ++clearDriveCalls; }
};

struct FakePathFinder final : cISC4PathFinder {
    int refs = 0;
    std::deque<RouteData>* route = nullptr;
    std::uint32_t flags = 0;
    std::uint16_t travelTypes = 0;
    int destinationFilter = 0;
    std::uint8_t destinationFlags = 0;
    std::uint32_t density = 0;
    SC4RectInt32 startRect{}, endRect{};
    int endCellX = 0, endCellZ = 0;
    int resetCalls = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetFlag(std::uint32_t flag) override { flags |= flag; }
    void ClearFlag(std::uint32_t flag) override { flags &= ~flag; }
    void ClearAllFlags() override { flags = 0; }
    void SetRouteInfo(std::deque<RouteData>* value) override { route = value; }
    void SetTripDestinationFilter(int value) override { destinationFilter = value; }
    void SetDestinationFlag(std::uint8_t flag) override { destinationFlags |= flag; }
    void ClearDestinationFlag(std::uint8_t flag) override { destinationFlags &= static_cast<std::uint8_t>(~flag); }
    void ClearAllDestinationFlags() override { destinationFlags = 0; }
    void SetTravelTypeFlag(std::uint16_t flag, bool alsoStart) override {
        travelTypes = static_cast<std::uint16_t>(travelTypes | flag);
        if (alsoStart) travelTypes = static_cast<std::uint16_t>(travelTypes | flag);
    }
    void ClearTravelTypeFlag(std::uint16_t flag) override { travelTypes = static_cast<std::uint16_t>(travelTypes & ~flag); }
    void ClearAllTravelTypeFlags() override { travelTypes = 0; }
    void SetStartingCostArray(float*) override {}
    void SetMaxTripLength(float) override {}
    void SetTrafficDensity(std::uint32_t value) override { density = value; }
    void SetStartRect(SC4RectInt32 rect) override { startRect = rect; }
    void SetEndRect(SC4RectInt32 rect) override { endRect = rect; }
    void SetMaxWorkTime(std::uint32_t) override {}
    void Reset() override { ++resetCalls; }
    std::uint32_t GetTrafficArrived() override { return 0; }
    float GetTripLength() override { return 0.0f; }
    float GetTripLengthScaledForDisplay() override { return 0.0f; }
    void GetEndCell(int& x, int& z) const override { x = endCellX; z = endCellZ; }
    std::uint8_t GetLastTravelType() const override { return 0; }
    int FindPath() override { return 0; }
    bool WasTripReversible() override { return false; }
    void GetCompressedRoute(std::vector<std::uint8_t>&, std::uint32_t&) override {}
};

struct FakeHost final : IMySimDispatchHost {
    FakeMessageServer server;
    FakePathFinder pathFinder;
    FakeClock clock;
    FakeVehicleView view;
    FakeAutomataManager vehicleManager;
    FakeAutomataManager pedestrianManager;
    std::vector<std::uint32_t> managerGroups;
    std::vector<cIGZUnknown*> written;
    struct ReadEntry { std::uint32_t iid; cIGZUnknown* object; };
    std::deque<ReadEntry> readEntries;
    bool findPathResult = 1;
    std::deque<RouteData> findPathRoute;
    bool tileNetwork = true;
    int cityX = 64, cityZ = 64;
    bool anyTravelConnection = true;
    bool neighborConnection = false;
    bool lotCellRectSets = false;
    cISC4Occupant* buildingAtCell = nullptr;
    std::vector<std::pair<cIGZMessageTarget2*, int>> registered, unregistered;
    int ownAvatarCalls = 0, enterViewCalls = 0, adviceCalls = 0, clearDriveCalls = 0;

    cIGZMessageServer2* MessageServer() override { return &server; }
    void RegisterInputControlTarget(cIGZMessageTarget2* target) override { registered.emplace_back(target, 0xff); }
    void UnregisterInputControlTarget(cIGZMessageTarget2* target) override { unregistered.emplace_back(target, 0xff); }
    cISC4PathFinder* PathFinder() override { return &pathFinder; }
    int FindPath(cISC4PathFinder* finder) override {
        auto* fake = static_cast<FakePathFinder*>(finder);
        if (findPathResult == 1 && fake->route) *fake->route = findPathRoute;
        return findPathResult;
    }
    cIGZUnknown* Clock() override { return &clock; }
    IMySimDispatchVehicleView* DispatchVehicleView() override { return &view; }
    bool noManagers = false;
    IMySimDispatchAutomataManager* AutomataManager(std::uint32_t group) override {
        managerGroups.push_back(group);
        if (noManagers) return nullptr;
        if (group == 0x4149u || group == 0x414au || group == 0x4107u) return &pedestrianManager;
        return &vehicleManager;
    }
    int CityCellsX() override { return cityX; }
    int CityCellsZ() override { return cityZ; }
    bool AnyTravelConnection(int, int) override { return anyTravelConnection; }
    bool NeighborConnection(int, int, int& outX, int& outZ) override {
        outX = 1; outZ = 1;
        return neighborConnection;
    }
    cISC4Occupant* BuildingAtCell(int, int) override { return buildingAtCell; }
    bool LotCellRect(int cellX, int cellZ, int outRect[4]) override {
        if (!lotCellRectSets) return false;
        outRect[0] = outRect[2] = cellX;
        outRect[1] = outRect[3] = cellZ;
        return true;
    }
    std::map<cISC4Occupant*, std::array<int, 4>> occupantRects;
    bool LotCellRectForOccupant(cISC4Occupant* occupant, int outRect[4]) override {
        auto it = occupantRects.find(occupant);
        if (it == occupantRects.end()) return false;
        for (int i = 0; i < 4; ++i) outRect[i] = it->second[std::size_t(i)];
        return true;
    }
    bool TileHasNetwork(int, int, std::uint32_t) override { return tileNetwork; }
    bool NetworkTilesInRect(const int rect[4], std::deque<std::pair<int, int>>& tiles) override {
        tiles.push_back({rect[2], rect[3]});
        return true;
    }
    std::vector<cISC4Occupant*> cellOccupants;           // what every occupant-manager cell query returns
    std::vector<std::array<int, 4>> rectsSeen;
    void OccupantsInCellRect(const int rect[4], std::deque<cISC4Occupant*>& out) override {
        rectsSeen.push_back({rect[0], rect[1], rect[2], rect[3]});
        for (cISC4Occupant* occupant : cellOccupants) { occupant->AddRef(); out.push_back(occupant); }
    }
    void ClearOccupantList(std::deque<cISC4Occupant*>& list) override {
        for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    IMySimDispatchAutomaton* AutomatonOf(cISC4Occupant* occupant) override {
        auto* fake = dynamic_cast<FakeOccupant*>(occupant);
        if (!fake || !fake->automaton) return nullptr;
        fake->automaton->AddRef();
        return fake->automaton;
    }
    IMySimDispatchRoute* RouteOf(cISC4Occupant* occupant) override {
        auto* fake = dynamic_cast<FakeOccupant*>(occupant);
        if (!fake || !fake->automaton) return nullptr;
        fake->automaton->AddRef();
        return static_cast<IMySimDispatchRoute*>(fake->automaton);
    }
    cISC4Occupant* CallbackOccupant(void* automaton) override {
        return automaton ? static_cast<IMySimDispatchAutomaton*>(automaton)->AsOccupant() : nullptr;
    }
    bool UDriveViewAvailable() override { return false; }
    void EnterUDriveView(IMySimDispatchAutomaton*, cIGZMessageTarget2*) override { ++enterViewCalls; }
    void ShowUDriveAdvice() override { ++adviceCalls; }
    void ClearVehiclePlayerDrive() override { ++clearDriveCalls; }
    bool WriteSerializable(cIGZOStream&, const cIGZUnknown* object) override {
        written.push_back(const_cast<cIGZUnknown*>(object));
        return true;
    }
    bool ReadSerializable(cIGZIStream&, std::uint32_t iid, void** out) override {
        for (auto it = readEntries.begin(); it != readEntries.end(); ++it) {
            if (it->iid != iid) continue;
            cIGZUnknown* object = it->object;
            readEntries.erase(it);
            if (!object) { *out = nullptr; return true; }
            object->AddRef();
            *out = object;
            return true;
        }
        *out = nullptr;
        return false;
    }
};

struct Fixture {
    FakeHost host;
    FakeOwner owner;
    cSC4MySimDispatch* dispatch = nullptr;

    Fixture() {
        SetMySimDispatchHost(&host);
        owner.refs = 0;
        dispatch = new cSC4MySimDispatch(&owner);   // ctor takes one owner reference
    }
    ~Fixture() {
        if (dispatch) dispatch->Release();
        SetMySimDispatchHost(nullptr);
    }
};

cRZMessage2Standard* MakeMessage(std::uint32_t type, std::uint64_t data1 = 0, std::uint64_t data2 = 0) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    if (data1) message->SetData1(data1);
    if (data2) message->SetData2(data2);
    return message;
}

MySimLocation MakeLocation(FakeOccupant* occupant, int cellX, int cellZ, bool hasOccupant = true) {
    MySimLocation location;
    location.occupant = occupant;
    location.SetCellPosition(cellX, cellZ);
    location.hasOccupant = hasOccupant;
    location.commuteLocation = false;
    location.data24 = 0;
    location.data28 = 0;
    return location;
}

}  // namespace

int main() {
    // FakeOccupants must outlive every dispatch that keeps a reference to them (the Fixtures are block-scoped).
    std::deque<std::unique_ptr<FakeOccupant>> occupantPool;
    auto newOccupant = [&occupantPool]() -> FakeOccupant& {
        occupantPool.push_back(std::make_unique<FakeOccupant>());
        return *occupantPool.back();
    };

    // ---- interfaces and the shared refcount ----
    {
        FakeHost host;
        FakeOwner owner;
        SetMySimDispatchHost(&host);
        auto* dispatch = new cSC4MySimDispatch(&owner);
        CHECK(owner.refs == 1);
        CHECK(dispatch->GetGZCLSID() == 0xcbc14674);
        void* pointer = nullptr;
        CHECK(host.server.refs == 0);   // the ctor touches no service
        CHECK(dispatch->QueryInterface(1, &pointer) && pointer == dispatch);   // cRZUnknown fallback
        CHECK(dispatch->QueryInterface(0xeb8b6f9a, &pointer) && pointer == dispatch);
        CHECK(dispatch->QueryInterface(0x452294aa, &pointer) && pointer != nullptr && pointer != dispatch);
        CHECK(dispatch->QueryInterface(0xe4fda3d4, &pointer) && pointer != nullptr && pointer != dispatch);
        CHECK(!dispatch->QueryInterface(0x1234, &pointer));
        CHECK(dispatch->AddRef() == 5);   // four successful interfaces + this AddRef
        CHECK(dispatch->Release() == 4 && dispatch->Release() == 3 && dispatch->Release() == 2);
        CHECK(dispatch->Release() == 1);
        CHECK(dispatch->GetState() == -1);   // the ctor leaves the state untouched
        CHECK(dispatch->GetTravelType() == 0xff);
        CHECK(dispatch->TrackHandle() == -1);
        CHECK(dispatch->Release() == 0 && owner.refs == 0);
        SetMySimDispatchHost(nullptr);
    }

    // ---- Init / Shutdown ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        CHECK(dispatch.Init());
        CHECK(fixture.host.server.added.size() == 4);
        CHECK(fixture.host.server.added[0].second == kMySimDispatchMsgFiveMinuteTick);
        CHECK(fixture.host.server.added[1].second == kMySimDispatchMsgFollow);
        CHECK(fixture.host.server.added[2].second == kMySimDispatchMsgView1);
        CHECK(fixture.host.server.added[3].second == kMySimDispatchMsgView2);
        CHECK(fixture.host.registered.size() == 1);
        CHECK(fixture.host.pathFinder.refs == 1);
        CHECK(fixture.host.clock.refs == 1);
        CHECK(dispatch.GetState() == 3);
        CHECK(dispatch.GetTravelType() == 0xff);
        CHECK(dispatch.CheckFlags(2));
        CHECK(dispatch.GetCurrentLocation().x == -1000.0f && dispatch.GetCurrentLocation().hasOccupant);
        CHECK(dispatch.GetCurrentLocation().commuteLocation);
        CHECK(dispatch.GetCurrentDefaultDestination().x == -1000.0f);
        CHECK(dispatch.GetCurrentDefaultDestination().hasOccupant);
        CHECK(!dispatch.CheckFlags(0x80));
        CHECK(!dispatch.FollowCamera());

        CHECK(dispatch.Init());   // idempotent: no second registration
        CHECK(fixture.host.server.added.size() == 4);
        CHECK(fixture.host.pathFinder.refs == 1 && fixture.host.clock.refs == 1);

        CHECK(dispatch.Shutdown());
        CHECK(fixture.host.server.removed.size() == 4);
        CHECK(fixture.host.unregistered.size() == 1);
        CHECK(fixture.host.pathFinder.refs == 0 && fixture.host.clock.refs == 0);
        CHECK(dispatch.GetState() == -1);
        CHECK(dispatch.Flags() == 0);
        CHECK(dispatch.Shutdown());   // idempotent
    }

    // ---- cLocation serialization ----
    {
        Fixture fixture;
        Fixture* fixturePtr = &fixture;
        (void)fixturePtr;
        FakeOccupant& occupant = newOccupant();
        MySimLocation location = MakeLocation(&occupant, 21, -3, false);
        location.commuteLocation = true;
        location.lotCellX = 4;
        location.lotCellZ = 5;
        location.data24 = 77;
        location.data28 = 88;

        TestOStream out;
        CHECK(MySimLocationWrite(location, out));
        CHECK(fixture.host.written.size() == 1 && fixture.host.written[0] == &occupant);

        TestIStream in;
        in.bytes = out.bytes;
        FakeOccupant& replacement = newOccupant();
        MySimLocation read = MakeLocation(&replacement, 0, 0, true);
        fixture.host.readEntries.push_back({kGZIID_cISC4OccupantSaved, &occupant});
        CHECK(MySimLocationRead(read, in));
        CHECK(replacement.refs == 0);   // the old occupant was released
        CHECK(read.occupant.get() == &occupant);
        CHECK(read.x == 21.0f * 16.0f && read.z == -3.0f * 16.0f);
        CHECK(read.cellX == 21 && read.cellZ == -3);
        CHECK(read.lotCellX == 4 && read.lotCellZ == 5);
        CHECK(!read.hasOccupant && read.commuteLocation);
        CHECK(read.data24 == 77 && read.data28 == 88);
        CHECK(read == location);
        location.occupant = nullptr;
        read.occupant = nullptr;
    }

    // ---- destinations, default destination, tracking ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& building = newOccupant();
        building.isBuilding = true;
        FakeOccupant& other = newOccupant();

        MySimLocation locationA = MakeLocation(&building, 10, 10);
        MySimLocation locationB = MakeLocation(&other, 11, 11);
        MySimLocation locationC = MakeLocation(nullptr, 12, 12, false);

        CHECK(dispatch.AddDestinationLocation(locationA, 0));
        CHECK(dispatch.AddDestinationLocation(locationB, 0));
        CHECK(dispatch.DestinationCount() == 2);
        CHECK(dispatch.AddDestinationLocation(locationC, 1));   // push front
        CHECK(dispatch.DestinationCount() == 3);
        CHECK(dispatch.AddDestinationLocation(locationA, 4));   // clear first
        CHECK(dispatch.DestinationCount() == 1);

        CHECK(dispatch.Init());
        dispatch.SetDefaultDestination(locationB);   // commuteLocation false, occupant, valid cells
        CHECK(dispatch.CheckFlags(0x80));
        CHECK(dispatch.GetCurrentDefaultDestination() == locationB);

        MySimLocation commute = MakeLocation(&building, 20, 20);
        commute.commuteLocation = true;
        dispatch.SetDefaultDestination(commute);   // rejected: commute location
        CHECK(dispatch.GetCurrentDefaultDestination() == locationB);

        dispatch.SetModelBaseResKey(0, cGZPersistResourceKey{1, 2, 3}, 4);
        dispatch.SetModelBaseResKey(2, cGZPersistResourceKey{5, 6, 7}, 8);
        CHECK(dispatch.ModelKeyCount() == 2);
        dispatch.SetModelOffset(0, 9, false);   // no automaton: only the map entry changes
        dispatch.SetModelOffset(7, 1, true);    // missing model type: no-op

        // tracking: the current location occupant is a building (not a pylon), so it is the occupant to track
        dispatch.SetCurrentCommuteDestination(locationA, false);
        dispatch.TeleportToCommuteLocation();
        CHECK(dispatch.GetCurrentLocation() == locationA);
        dispatch.StartTracking(0, cRZString("Bob"));
        CHECK(dispatch.TrackHandle() == 5);
        CHECK(fixture.host.view.tracked.size() == 1);
        CHECK(fixture.host.view.tracked[0].occupant == &building);
        CHECK(fixture.host.view.tracked[0].trackerType == 3);
        CHECK(dispatch.CheckFlags(0x100));
        dispatch.StopTracking();
        CHECK(dispatch.TrackHandle() == -1);
        CHECK(fixture.host.view.untracked.size() == 1);
        CHECK(fixture.host.view.untracked[0].first == 5);
        CHECK(!dispatch.CheckFlags(0x100));
        dispatch.StartTracking(0, cRZString("Bob"));   // handle from the view
        CHECK(dispatch.TrackHandle() == 6);
        dispatch.StopTracking();
        locationA.occupant = nullptr;
        locationB.occupant = nullptr;
        locationC.occupant = nullptr;
        commute.occupant = nullptr;
    }

    // ---- idle -> travelling -> path found -> dispatch ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        residence.position[0] = 160.0f;
        residence.position[2] = 160.0f;

        FakeOccupant& job = newOccupant();
        job.isBuilding = true;
        MySimLocation current = MakeLocation(&residence, 10, 10);
        MySimLocation destination = MakeLocation(&job, 12, 12);
        fixture.host.occupantRects[&residence] = {10, 10, 10, 10};
        fixture.host.occupantRects[&job] = {12, 12, 12, 12};
        fixture.host.lotCellRectSets = true;   // cell-only locations get their own cell rect
        dispatch.Init();
        dispatch.SetModelBaseResKey(0, cGZPersistResourceKey{11, 22, 33}, 4);
        dispatch.SetCurrentCommuteDestination(current, false);
        dispatch.TeleportToCommuteLocation();
        CHECK(dispatch.GetCurrentLocation() == current);
        dispatch.SetDefaultDestination(destination);
        CHECK(dispatch.CheckFlags(0x80));
        dispatch.GoTo(destination);   // state != travelling: queue only
        CHECK(dispatch.DestinationCount() == 1);

        // simulate tick: idle branch picks the queue entry as the destination
        dispatch.updateState();
        CHECK(dispatch.GetState() == 2);
        CHECK(dispatch.GetCurrentDestination() == destination);
        CHECK(dispatch.GetCurrentDestination().data28 == 0);   // time 0 + data24 0

        // second update: the host's FindPath fills the route; the dispatch appends the walking leg
        fixture.host.findPathRoute = {RouteData{10, 10, 1}, RouteData{11, 10, 1}, RouteData{12, 12, 1}};
        dispatch.updateState();
        CHECK(dispatch.RouteSize() == 5);   // three path elements, the start corner and the walking leg
        CHECK(dispatch.CheckFlags(0x1000));
        CHECK(dispatch.CheckFlags(0x400));
        CHECK(!dispatch.CheckFlags(0x820));
        CHECK(fixture.host.pathFinder.destinationFilter == 3);
        CHECK(fixture.host.pathFinder.density == 1);
        CHECK(fixture.host.pathFinder.startRect.x0 == 10 && fixture.host.pathFinder.endRect.x0 == 12);
        bool neighborEvent = false;
        for (const auto& message : fixture.owner.messages) {
            if (message.data1 == kDispatchNotifyNeighbor) neighborEvent = true;
        }
        CHECK(neighborEvent);

        // third update: the switch-travel flag makes switchTravel() dispatch the leg on a new automaton
        FakeOccupant& automatonOccupant = newOccupant();
        automatonOccupant.group = 0x420b;
        fixture.host.vehicleManager.automatonOccupant = &automatonOccupant;
        dispatch.updateState();
        CHECK(fixture.host.vehicleManager.created.size() == 1);
        FakeAutomaton* automaton = fixture.host.vehicleManager.created.empty() ? nullptr
                                                                              : fixture.host.vehicleManager.created[0];
        CHECK(automaton != nullptr);
        if (automaton) {
            CHECK(automaton->priority == 0xff);
            CHECK(automaton->route.size() == 4);   // the car leg (the walking leg is the next segment)
            CHECK(automaton->anims.size() == 1 && automaton->anims[0] == 4);
            CHECK(automaton->flags & 0x1000000);
            CHECK(automaton->occupant == &automatonOccupant);
            CHECK(automaton->removeCalls == 0);
            // createAutomaton (i386 0x00480624): the leg's automata group and the model type's base key; SetState(0) runs
            // without a controller
            CHECK(fixture.host.vehicleManager.kindSeen == 0x420b);
            CHECK(fixture.host.vehicleManager.keySeen.type == 11 && fixture.host.vehicleManager.keySeen.instance == 33);
            CHECK(automaton->state == 0 && automaton->controllerSeen == nullptr && automaton->sfxSeen == 0);
        }
        bool requestedVehicleManager = false;
        for (std::uint32_t group : fixture.host.managerGroups) {
            if (group == 0x420b) requestedVehicleManager = true;
        }
        CHECK(requestedVehicleManager);
        CHECK(dispatch.GetTravelType() == 1);
        CHECK(dispatch.CheckFlags(0x200));
        CHECK(dispatch.AutomataOccupant() == &automatonOccupant);

        // switchTravel keeps the trip going: the sim stays in the travelling state on its car
        dispatch.updateState();
        CHECK(dispatch.GetState() == 2);
        CHECK(dispatch.AutomataOccupant() == &automatonOccupant);
        if (automaton) CHECK(automaton->removeCalls == 0);

        // the car reaches the end of its route at the job: the walking leg is the arrival (state 3, location notice)
        automatonOccupant.position[0] = 12 * 16 + 8;
        automatonOccupant.position[2] = 12 * 16 + 8;
        if (automaton) automaton->route = {RouteData{12, 12, 1}};
        fixture.owner.messages.clear();
        cRZMessage2Standard* reached = MakeMessage(kMySimDispatchMsgAutomaton, 0, kDispatchEventRouteReached);
        dispatch.DoMessage(reached);
        reached->Release();
        CHECK(dispatch.GetState() == 3);
        CHECK(dispatch.GetCurrentLocation() == destination);
        CHECK(dispatch.GetTravelType() == 0xfe);
        CHECK(!dispatch.CheckFlags(0x8460));
        CHECK(dispatch.CheckFlags(0x2));
        bool locationEvent = false, farEvent = false;
        for (const auto& message : fixture.owner.messages) {
            if (message.data1 == kDispatchNotifyLocation) locationEvent = true;
            if (message.data1 == kDispatchNotifyFarFromDestination) farEvent = true;
        }
        CHECK(locationEvent);
        CHECK(!farEvent);
        current.occupant = nullptr;
        destination.occupant = nullptr;
        job.refs = 0;
    }

    // ---- a trip to the commute location: it becomes the commute destination and keeps the work dwell time ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        FakeOccupant& job = newOccupant();
        job.isBuilding = true;
        MySimLocation home = MakeLocation(&residence, 10, 10);
        MySimLocation work = MakeLocation(&job, 12, 12);
        fixture.host.occupantRects[&residence] = {10, 10, 10, 10};
        fixture.host.occupantRects[&job] = {12, 12, 12, 12};
        dispatch.Init();
        dispatch.SetCurrentCommuteDestination(home, false);
        dispatch.TeleportToCommuteLocation();
        dispatch.SetDefaultDestination(home);
        work.commuteLocation = true;
        work.data24 = 540;   // the MySim's CheckCommute at 8:00: (17 - 8) * 60 minutes
        dispatch.AddDestinationLocation(work, 4);
        dispatch.updateState();
        CHECK(dispatch.GetState() == 2);
        CHECK(dispatch.GetCurrentDestination().data28 == 540);
        fixture.host.findPathRoute = {RouteData{10, 10, 1}, RouteData{11, 11, 1}, RouteData{12, 12, 1}};
        dispatch.updateState();
        CHECK(dispatch.CheckFlags(0x1000));
        CHECK(dispatch.GetCurrentDestination().data28 == 540);
        CHECK(dispatch.GetCurrentCommuteDestination() == work);
        CHECK(dispatch.GetCurrentCommuteDestination().data24 == 540);
        home.occupant = nullptr;
        work.occupant = nullptr;
    }

    // ---- switchTravel: the walking leg ends too far from the destination ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        FakeOccupant& job = newOccupant();
        job.isBuilding = true;
        MySimLocation current = MakeLocation(&residence, 10, 10);
        MySimLocation destination = MakeLocation(&job, 140, 140);
        fixture.host.occupantRects[&residence] = {10, 10, 10, 10};
        fixture.host.occupantRects[&job] = {140, 140, 140, 140};
        fixture.host.cityX = fixture.host.cityZ = 256;
        fixture.host.lotCellRectSets = true;
        dispatch.Init();
        dispatch.SetCurrentCommuteDestination(current, false);
        dispatch.TeleportToCommuteLocation();
        dispatch.SetDefaultDestination(destination);
        dispatch.GoTo(destination);
        dispatch.updateState();
        fixture.host.findPathRoute = {RouteData{10, 10, 1}, RouteData{11, 10, 1}, RouteData{12, 10, 1}};
        dispatch.updateState();
        FakeOccupant& automatonOccupant = newOccupant();
        automatonOccupant.group = 0x420b;
        fixture.host.vehicleManager.automatonOccupant = &automatonOccupant;
        dispatch.updateState();
        FakeAutomaton* automaton = fixture.host.vehicleManager.created.empty() ? nullptr
                                                                              : fixture.host.vehicleManager.created[0];
        CHECK(automaton != nullptr);
        automatonOccupant.position[0] = 12 * 16 + 8;
        automatonOccupant.position[2] = 10 * 16 + 8;
        if (automaton) automaton->route = {RouteData{12, 10, 1}};
        fixture.owner.messages.clear();
        cRZMessage2Standard* reached = MakeMessage(kMySimDispatchMsgAutomaton, 0, kDispatchEventRouteReached);
        dispatch.DoMessage(reached);
        reached->Release();
        // about 180 cells away (the original compares the distance in cells with the squared proximity minimum, 100): no arrival, the sim idles where the walk ended
        CHECK(dispatch.GetState() == 2);
        CHECK(dispatch.GetTravelType() == 0xfe);
        CHECK(dispatch.CheckFlags(0x1));
        CHECK(dispatch.CheckFlags(0x2400));
        CHECK(!dispatch.CheckFlags(0x40));
        CHECK(dispatch.GetCurrentLocation().cellX == 12);
        CHECK(dispatch.GetCurrentLocation().data28 == dispatch.Time() + 5);
        bool farEvent = false, locationEvent = false;
        for (const auto& message : fixture.owner.messages) {
            if (message.data1 == kDispatchNotifyFarFromDestination) farEvent = true;
            if (message.data1 == kDispatchNotifyLocation) locationEvent = true;
        }
        CHECK(farEvent);
        CHECK(!locationEvent);
        current.occupant = nullptr;
        destination.occupant = nullptr;
    }

    // ---- switchTravel: car leg to a bus leg (get off, find the stop, board the bus) ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        FakeOccupant& job = newOccupant();
        job.isBuilding = true;
        MySimLocation current = MakeLocation(&residence, 10, 10);
        MySimLocation destination = MakeLocation(&job, 14, 14);
        fixture.host.occupantRects[&residence] = {10, 10, 10, 10};
        fixture.host.occupantRects[&job] = {14, 14, 14, 14};
        fixture.host.lotCellRectSets = true;
        dispatch.Init();
        dispatch.SetCurrentCommuteDestination(current, false);
        dispatch.TeleportToCommuteLocation();
        dispatch.SetDefaultDestination(destination);
        dispatch.GoTo(destination);
        dispatch.updateState();
        fixture.host.findPathRoute = {RouteData{10, 10, 1}, RouteData{11, 10, 1}, RouteData{11, 11, 2},
                                      RouteData{12, 12, 2}, RouteData{14, 14, 2}};
        dispatch.updateState();
        FakeOccupant& carOccupant = newOccupant();
        carOccupant.group = 0x420b;
        fixture.host.vehicleManager.automatonOccupant = &carOccupant;
        dispatch.updateState();
        CHECK(dispatch.GetTravelType() == 1);
        FakeAutomaton* car = fixture.host.vehicleManager.created.empty() ? nullptr : fixture.host.vehicleManager.created[0];
        CHECK(car != nullptr);

        // the car leg ends at 11,10: the next leg is a bus leg, so the sim gets off and waits at the switch
        carOccupant.position[0] = 11 * 16 + 8;
        carOccupant.position[2] = 10 * 16 + 8;
        if (car) car->route = {RouteData{11, 10, 1}};
        cRZMessage2Standard* reached = MakeMessage(kMySimDispatchMsgAutomaton, 0, kDispatchEventRouteReached);
        dispatch.DoMessage(reached);
        reached->Release();
        CHECK(dispatch.GetState() == 2);
        CHECK(dispatch.CheckFlags(0x10));
        CHECK(!dispatch.CheckFlags(0x20));
        CHECK(dispatch.CheckFlags(0x400));
        CHECK(dispatch.AutomataOccupant() == nullptr);
        if (car) CHECK(car->removeCalls == 1);

        // next update: the stop (group 0x1301 for a bus leg) is found around the switch, then the bus of group 0x4200
        // standing on the stop's cell is boarded for the bus leg
        FakeOccupant& stop = newOccupant();
        stop.group = 0x1301;
        stop.position[0] = 11 * 16 + 4;
        stop.position[2] = 11 * 16 + 4;
        FakeOccupant& busOccupant = newOccupant();
        busOccupant.group = 0x4200;
        FakeAutomaton bus;
        bus.refs = 1;
        bus.occupant = &busOccupant;
        busOccupant.automaton = &bus;
        fixture.host.cellOccupants = {&residence, &stop, &busOccupant};
        fixture.owner.messages.clear();
        dispatch.updateState();
        CHECK(fixture.host.rectsSeen.size() == 2);
        if (fixture.host.rectsSeen.size() == 2) {
            const std::array<int, 4> around{10, 9, 12, 12};   // the switch cell and the next route cell, one cell wider
            CHECK(fixture.host.rectsSeen[0] == around);
            const std::array<int, 4> cell{11, 11, 11, 11};    // the stop's own cell
            CHECK(fixture.host.rectsSeen[1] == cell);
        }
        bool stopEvent = false;
        for (const auto& message : fixture.owner.messages) {
            if (message.data1 == kDispatchNotifyTransitStop && message.data2 == 0x1301) stopEvent = true;
        }
        CHECK(stopEvent);
        CHECK(stop.groupQueries.size() >= 1 && stop.groupQueries[0] == 0x1301);
        CHECK(bus.callback != nullptr);
        CHECK(bus.priority == 0xff);
        CHECK(bus.route.size() == 4);   // the bus leg 11,11 .. 14,14 plus the start corner
        CHECK(dispatch.GetTravelType() == 2);
        CHECK(dispatch.AutomataOccupant() == &busOccupant);
        CHECK(dispatch.CheckFlags(0x200));
        CHECK(!dispatch.CheckFlags(0x436));
        CHECK(dispatch.GetState() == 2);
        CHECK(fixture.host.vehicleManager.created.size() == 1);   // no new automaton: the bus was boarded
        fixture.host.cellOccupants.clear();
        current.occupant = nullptr;
        destination.occupant = nullptr;
    }

    // ---- switchTravel: the next leg cannot be dispatched and there is no further switch (trip fails) ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        FakeOccupant& job = newOccupant();
        job.isBuilding = true;
        MySimLocation current = MakeLocation(&residence, 10, 10);
        MySimLocation destination = MakeLocation(&job, 12, 12);
        fixture.host.occupantRects[&residence] = {10, 10, 10, 10};
        fixture.host.occupantRects[&job] = {12, 12, 12, 12};
        fixture.host.lotCellRectSets = true;
        dispatch.Init();
        dispatch.SetCurrentCommuteDestination(current, false);
        dispatch.TeleportToCommuteLocation();
        dispatch.SetDefaultDestination(destination);
        dispatch.GoTo(destination);
        dispatch.updateState();
        // no automata manager answers, so the car leg cannot be dispatched; the next leg is the walking one
        fixture.host.findPathRoute = {RouteData{10, 10, 1}, RouteData{11, 10, 1}};
        fixture.host.noManagers = true;
        dispatch.updateState();
        CHECK(dispatch.CheckFlags(0x400));
        fixture.owner.messages.clear();
        dispatch.updateState();
        CHECK(dispatch.GetState() == -1);
        CHECK(dispatch.GetTravelType() == 0xff);
        CHECK(fixture.host.vehicleManager.created.empty());
        bool pylonEvent = false;
        for (const auto& message : fixture.owner.messages) {
            if (message.data1 == kDispatchNotifyPylon) pylonEvent = true;
        }
        CHECK(pylonEvent);
        current.occupant = nullptr;
        destination.occupant = nullptr;
    }

    // ---- messages: follow camera, five-minute tick, deferred Init ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        dispatch.Init();

        cRZMessage2Standard* follow = MakeMessage(kMySimDispatchMsgFollow, 1);
        CHECK(dispatch.DoMessage(follow));
        follow->Release();
        CHECK(dispatch.FollowCamera());
        follow = MakeMessage(kMySimDispatchMsgFollow, 0);
        dispatch.DoMessage(follow);
        follow->Release();
        CHECK(!dispatch.FollowCamera());

        CHECK(dispatch.Time() == 0);
        for (int i = 0; i < 3; ++i) {
            cRZMessage2Standard* tick = MakeMessage(kMySimDispatchMsgFiveMinuteTick);
            dispatch.DoMessage(tick);
            tick->Release();
        }
        CHECK(dispatch.Time() == 15);

        cRZMessage2Standard* simulate = MakeMessage(kMySimDispatchMsgSimulate);
        CHECK(dispatch.DoMessage(simulate));
        simulate->Release();

        cRZMessage2Standard* other = MakeMessage(0x12345678);
        CHECK(dispatch.DoMessage(other));
        other->Release();
    }

    // ---- Write / Read roundtrip ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        FakeOccupant& other = newOccupant();
        MySimLocation current = MakeLocation(&residence, 10, 10);
        MySimLocation destination = MakeLocation(&other, 12, 12);
        fixture.host.occupantRects[&residence] = {10, 10, 10, 10};
        fixture.host.occupantRects[&other] = {12, 12, 12, 12};
        fixture.host.lotCellRectSets = true;
        dispatch.Init();
        dispatch.SetDefaultDestination(destination);
        dispatch.SetCurrentCommuteDestination(current, false);
        dispatch.TeleportToCommuteLocation();
        dispatch.GoTo(destination);
        dispatch.updateState();
        fixture.host.findPathRoute = {RouteData{10, 10, 1}, RouteData{11, 10, 1}};
        dispatch.updateState();
        dispatch.AddDestinationLocation(destination, 0);
        dispatch.SetModelBaseResKey(0, cGZPersistResourceKey{11, 22, 33}, 44);
        dispatch.StartTracking(0, cRZString("Bob"));
        const int savedHandle = dispatch.TrackHandle();

        TestOStream out;
        CHECK(dispatch.Write(out));
        CHECK(fixture.host.written.size() >= 6);   // four locations, tracked occupant, automaton occupant, owner, clock
        const std::int32_t savedState = dispatch.GetState();
        const std::uint32_t savedFlags = dispatch.Flags();
        const int savedTime = dispatch.Time();
        const std::size_t savedRoute = dispatch.RouteSize();
        const std::size_t savedModelKeys = dispatch.ModelKeyCount();
        const std::size_t savedDestinations = dispatch.DestinationCount();

        dispatch.Reset();
        CHECK(dispatch.RouteSize() == 0 && dispatch.DestinationCount() == 0);
        CHECK(dispatch.ModelKeyCount() == 1);   // Reset keeps the model keys
        CHECK(dispatch.GetState() == -1);

        TestIStream in;
        in.bytes = out.bytes;
        in.strings = out.strings;
        fixture.host.readEntries = {{kGZIID_cISC4OccupantSaved, &residence},
                                    {kGZIID_cISC4OccupantSaved, &other},
                                    {kGZIID_cISC4OccupantSaved, &other},
                                    {kGZIID_cISC4OccupantSaved, &residence},
                                    {kGZIID_cISC4OccupantSaved, nullptr},
                                    {kGZIID_cISC4OccupantSaved, nullptr},
                                    {kGZIID_cISC4MySimOwner, &fixture.owner},
                                    {kGZIID_cISC4GameClock, &fixture.host.clock}};
        CHECK(dispatch.Read(in));
        CHECK(dispatch.ReadPending());
        CHECK(dispatch.GetState() == savedState);
        CHECK(dispatch.Flags() == savedFlags);
        CHECK(dispatch.Time() == savedTime);
        CHECK(dispatch.RouteSize() == savedRoute);
        CHECK(dispatch.ModelKeyCount() == savedModelKeys);
        CHECK(dispatch.DestinationCount() == savedDestinations);
        CHECK(dispatch.GetCurrentLocation() == current);
        CHECK(dispatch.GetCurrentDefaultDestination() == destination);
        CHECK(dispatch.GetCurrentCommuteDestination() == current);
        CHECK(fixture.host.server.added.back().second == kMySimDispatchMsgReadPending);
        CHECK(fixture.owner.refs == 1);   // Read released the old reference and took the one from the stream
        CHECK(dispatch.TrackHandle() == savedHandle);

        cRZMessage2Standard* readIdle = MakeMessage(kMySimDispatchMsgReadPending);
        CHECK(dispatch.DoMessage(readIdle));
        readIdle->Release();
        CHECK(!dispatch.ReadPending());

        current.occupant = nullptr;
        destination.occupant = nullptr;
    }

    // ---- U-Drive-It ----
    {
        Fixture fixture;
        cSC4MySimDispatch& dispatch = *fixture.dispatch;
        FakeOccupant& residence = newOccupant();
        residence.isBuilding = true;
        MySimLocation current = MakeLocation(&residence, 10, 10);
        dispatch.Init();
        dispatch.SetDefaultDestination(current);
        CHECK(dispatch.CanUDriveIt());   // idle at the default destination
        CHECK(dispatch.EnterUDriveIt()); // going to the commute destination
        CHECK(dispatch.CheckFlags(0x20000));
        CHECK(dispatch.ExitUDriveIt());
        CHECK(!dispatch.CheckFlags(0x30000));
        current.occupant = nullptr;
    }

    if (failures == 0) std::printf("cSC4MySimDispatch: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
