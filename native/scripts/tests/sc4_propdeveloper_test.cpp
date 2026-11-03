// cSC4PropDeveloper — unit test. Drives the recovered COM object through a fake IPropDeveloperHost: the three
// views and their shared reference count, Init/Shutdown (services, the wealth requester, the tract rectangle,
// the 13 message registrations, the two simulator agents and the animation tick), the requester list, every
// DoMessage branch (lights on/off, minute, day/night, day, idle, power, lot state, occupant insert/remove,
// terrain, global effect, animation tick), the tract walk (DoIdle), UpdateTract on a real cSC4Lot and on a
// network lot (keep / remove / create the optional configuration props), SelectAndCreateProp (family
// expansion, requester selection, lot developer and network lot paths, the return-value quirk), the prop
// lists and their timers, the sign heights/visibility and the stream round trip.
//
// The lots are real cSC4Lot objects behind a fake ILotHost, the configuration objects are the recovered
// cSC4LotConfigurationObject records, the network lot is a real cSC4NetworkLot and the prop occupants are
// fakes implementing cISC4PropOccupant / cISC4Occupant (the two recovered classes are not linked here).

#include "sc4/PropDeveloper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/SCPropertyHelp.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (false)

using namespace sc4;

}  // namespace

// The two GZ serializable helpers need a DB-segment stream (the DB stack is not linked into this test), so the
// test supplies the same fallback behaviour the real ones show on a plain memory stream: nothing is written and
// a read returns no object. The stream test therefore covers the scalar layout and the counts only.
namespace sc4 {
namespace {
constexpr std::uint32_t kTestDBSegmentIStreamId = 0x4999a8f5;    // kGZIID_cISC4DBSegmentIStream
constexpr std::uint32_t kTestDBSegmentOStreamId = 0x6999a8fb;    // kGZIID_cISC4DBSegmentOStream
}  // namespace

bool SC4WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown*) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kTestDBSegmentOStreamId, &raw)) return false;
    if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    return false;
}
bool SC4ReadGZSerializable(cIGZIStream& stream, std::uint32_t, void** out, bool) {
    if (out) *out = nullptr;
    void* raw = nullptr;
    if (!stream.QueryInterface(kTestDBSegmentIStreamId, &raw)) return false;
    if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    return false;
}
}  // namespace sc4

namespace {

// ----------------------------------------------------------------------------------------- fake prop occupant
struct FakeOccupant;

struct FakeProp final : cISC4PropOccupant {
    FakeOccupant* owner = nullptr;
    std::uint32_t propType = 0x2977aa47;
    std::uint32_t state = 0;
    std::uint32_t timerBits = 0;
    float timeStart = 0.0f, timeEnd = 0.0f;
    std::uint8_t randomChance = 100;
    std::uint8_t gzclsidLow = 0;
    std::uint32_t datePropType = 0, dateSpan = 0;
    std::uint8_t dateMonth = 0, dateDay = 0;
    int dateTicks = 0;
    int refs = 1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4PropOccupant) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }   // owned by FakeOccupant
    cISC4Occupant* AsOccupant() override;
    void SetBoundingBox(const float*, const float*) override {}
    void* GetPropPlacementProperties() override { return nullptr; }
    void SetPropPlacementProperties(void*) override {}
    std::uint32_t GetPropType() const override { return propType; }
    void SetPropType(std::uint32_t type) override { propType = type; }
    std::int32_t GetOrientation() const override { return 0; }
    void SetOrientation(std::int32_t) override {}
    void SetState(std::uint32_t value) override { state = value; }
    std::uint32_t GetState() const override { return state; }
    void SetTimeOfDayRange(float start, float end) override { timeStart = start; timeEnd = end; }
    void GetTimeOfDayRange(float& start, float& end) const override { start = timeStart; end = timeEnd; }
    void SetSimulatorDateRange(std::uint32_t type, std::uint32_t span, std::uint8_t month,
                               std::uint8_t day) override {
        datePropType = type;
        dateSpan = span;
        dateMonth = month;
        dateDay = day;
    }
    void TickSimulatorDate(const cIGZDate&) override { ++dateTicks; }
    void SetRandomChance(std::uint8_t percent) override { randomChance = percent; }
    std::uint8_t GetRandomChance() const override { return randomChance; }
    std::uint8_t GetPropTimerBitfield() const override { return static_cast<std::uint8_t>(timerBits); }
    bool GetPropTimerState(std::uint32_t mask) const override { return (timerBits & mask) != 0; }
    void SetPropTimerState(std::uint32_t mask, bool on) override {
        if (on) timerBits |= mask;
        else timerBits &= ~mask;
    }
    std::uint8_t GetPropUsage() const override { return 0; }
    void SetPropUsage(std::uint8_t) override {}
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool Write(cIGZOStream&) const override { return true; }
    bool Read(cIGZIStream&) override { return true; }
    std::uint32_t GetGZCLSID() const override { return 0x2977aa47; }
    bool DoMessage(cIGZMessage2*) override { return true; }
    void GetPosition(float* xyz) override;
    void SetPosition(const float* xyz) override;
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        minXyz[0] = minXyz[1] = minXyz[2] = 0.0f;
        maxXyz[0] = maxXyz[1] = maxXyz[2] = 0.0f;
    }
    bool GetLotTag(std::uint32_t& tag) const override;
    void SetLotTag(std::uint32_t) override {}
};

struct FakeOccupant final : cISC4Occupant {
    FakeProp prop;
    cSCPropertyHolder holder;
    float position[3] = {0.0f, 0.0f, 0.0f};
    SC4CellRect cells{};
    std::uint32_t lotTag = 0;
    bool hasLotTag = false;
    bool visible = true;
    int visibilityCalls = 0;
    std::uint32_t flags = 0;
    int flagCalls = 0;
    std::uint32_t type = kOccupantTypeProp;
    int refs = 1;

    FakeOccupant() {
        prop.owner = this;
        type = kOccupantTypeProp;
    }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4PropOccupant) {
            prop.AddRef();
            *out = &prop;
            return true;
        }
        if (iid != 1 && iid != kGZIID_cISC4Occupant) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }   // owned by the fixture
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override {
        xyz[0] = position[0];
        xyz[1] = position[1];
        xyz[2] = position[2];
    }
    void SetPosition(const float* xyz) override {
        position[0] = xyz[0];
        position[1] = xyz[1];
        position[2] = xyz[2];
    }
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        minXyz[0] = minXyz[1] = minXyz[2] = 0.0f;
        maxXyz[0] = maxXyz[1] = maxXyz[2] = 0.0f;
    }
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = cells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool value, bool) override {
        visible = value;
        ++visibilityCalls;
    }
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t& tag) const override {
        tag = lotTag;
        return hasLotTag;
    }
    void SetLotTag(std::uint32_t tag) override {
        lotTag = tag;
        hasLotTag = true;
    }
    void SetFlag(std::uint32_t flag) override {
        flags |= flag;
        ++flagCalls;
    }
    void SetAllFlags(std::uint32_t value) override { flags = value; }
    void ClearFlag(std::uint32_t flag) override {
        flags &= ~flag;
        ++flagCalls;
    }
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

cISC4Occupant* FakeProp::AsOccupant() { return owner; }
void FakeProp::GetPosition(float* xyz) {
    xyz[0] = owner->position[0];
    xyz[1] = owner->position[1];
    xyz[2] = owner->position[2];
}
void FakeProp::SetPosition(const float* xyz) {
    owner->position[0] = xyz[0];
    owner->position[1] = xyz[1];
    owner->position[2] = xyz[2];
}
bool FakeProp::GetLotTag(std::uint32_t& tag) const {
    tag = owner->lotTag;
    return owner->hasLotTag;
}

// ----------------------------------------------------------------------------------------- fake requester
struct FakeRequester final : cISC4PropRequester {
    std::uint32_t share = 0;
    std::uint32_t satisfaction = 0;
    std::uint32_t selectResult = 0;
    bool customSelector = false;
    int refs = 1;
    int shareCalls = 0;
    std::vector<SC4Rect<int>> rects;
    std::vector<std::vector<std::uint32_t>> selections;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4PropRequester) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::uint32_t SelectProp(const std::vector<std::uint32_t>& candidates, int, int) override {
        selections.push_back(candidates);
        return selectResult != 0 ? selectResult : (candidates.empty() ? 0 : candidates.front());
    }
    int GetRequestedShare(const void* rect) const override {
        auto* self = const_cast<FakeRequester*>(this);
        ++self->shareCalls;
        if (rect) self->rects.push_back(*static_cast<const SC4Rect<int>*>(rect));
        return static_cast<int>(share);
    }
    std::uint32_t GetRequesterSatisfactionId() const override { return satisfaction; }
    bool UsesCustomPropSelector() const override { return customSelector; }
};

// ----------------------------------------------------------------------------------------- fake services
struct FakeLotManager final : IPropLotManagerApi {
    std::map<std::pair<int, int>, cSC4Lot*> lots;
    int refs = 0, releases = 0, calls = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    // exactCell asks for the lot whose *origin* cell is (x, z) (the tract walk); otherwise any cell of the
    // lot footprint answers (the creation path).
    cSC4Lot* GetLot(int x, int z, bool exactCell) override {
        ++calls;
        const auto it = lots.find({x, z});
        if (it == lots.end()) return nullptr;
        if (!exactCell) return it->second;
        int lotX = 0, lotZ = 0;
        it->second->GetLocation(lotX, lotZ);
        return lotX == x && lotZ == z ? it->second : nullptr;
    }
};

struct FakePropManager final : IPropManagerApi {
    std::map<std::uint32_t, PropProfile> profiles;
    std::map<std::uint32_t, std::vector<std::uint32_t>> families;
    std::vector<cISC4Occupant*> lotProps;
    std::vector<cISC4Occupant*> cellProps;
    std::vector<cISC4Occupant*> rectProps;
    std::vector<cISC4Occupant*> removed;
    std::vector<std::pair<int, int>> cellQueries;
    int refs = 0, releases = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    bool IsPropFamilyTypeValid(std::uint32_t type) override { return families.count(type) != 0; }
    int GetPropTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) override {
        const auto it = families.find(family);
        if (it == families.end()) return 0;
        out = it->second;
        return static_cast<int>(out.size());
    }
    const PropProfile* GetPropProfile(std::uint32_t type) override {
        const auto it = profiles.find(type);
        return it == profiles.end() ? nullptr : &it->second;
    }
    int PropsInLot(std::vector<cISC4Occupant*>& out, cSC4Lot*) override {
        out = lotProps;
        for (cISC4Occupant* occupant : out) occupant->AddRef();
        return static_cast<int>(out.size());
    }
    void RemoveProp(cISC4Occupant* occupant) override { removed.push_back(occupant); }
    int PropsInCell(std::vector<cISC4Occupant*>& out, int x, int z) override {
        cellQueries.emplace_back(x, z);
        out = cellProps;
        for (cISC4Occupant* occupant : out) occupant->AddRef();
        return static_cast<int>(out.size());
    }
    bool PropsInRect(std::vector<cISC4Occupant*>& out, const SC4Rect<int>&) override {
        out = rectProps;
        for (cISC4Occupant* occupant : out) occupant->AddRef();
        return true;
    }
};

struct FakePower final : IPropPowerApi {
    bool powered = true;
    int refs = 0, releases = 0, calls = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    bool IsPowered(int, int) override {
        ++calls;
        return powered;
    }
};

struct FakeNetworkLotManager final : IPropNetworkLotManagerApi {
    std::map<std::pair<int, int>, cSC4NetworkLot*> lots;
    std::vector<int> createdIndices;
    std::vector<std::uint32_t> createdTypes;
    int refs = 0, releases = 0, getCalls = 0;
    FakeOccupant* nextProp = nullptr;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    cSC4NetworkLot* GetNetworkLot(int x, int z) override {
        ++getCalls;
        const auto it = lots.find({x, z});
        return it == lots.end() ? nullptr : it->second;
    }
    cISC4Occupant* CreateProp(cSC4NetworkLot*, int objectIndex, std::uint32_t propType) override {
        createdIndices.push_back(objectIndex);
        createdTypes.push_back(propType);
        if (nextProp) nextProp->AddRef();
        return nextProp;
    }
};

struct FakeLotDeveloper final : IPropLotDeveloperApi {
    std::vector<int> indices;
    std::vector<std::uint32_t> types;
    std::vector<cSC4Lot*> lots;
    FakeOccupant* nextProp = nullptr;
    cISC4Occupant* CreateLotProp(cSC4Lot* lot, int objectIndex, std::uint32_t propType) override {
        lots.push_back(lot);
        indices.push_back(objectIndex);
        types.push_back(propType);
        if (nextProp) nextProp->AddRef();
        return nextProp;
    }
};

struct FakeBuildingDevelopment final : IPropBuildingDevelopmentApi {
    FakeLotDeveloper developer;
    int refs = 0, releases = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    IPropLotDeveloperApi* GetLotDeveloper() override { return &developer; }
};

struct FakeSimulator final : IPropSimulatorApi {
    cGZDate today{2024, 5, 1};
    int refs = 0, releases = 0, removals = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    cGZDate Date() override { return today; }
    void RemoveAgent(cIGZMessageTarget2*) override { ++removals; }
};

struct FakeClock final : IPropClockApi {
    int mode = 0;
    std::uint32_t hour = 0, minute = 0;
    int refs = 0, releases = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    int VisualTimeMode() const override { return mode; }
    std::uint32_t Hour() const override { return hour; }
    std::uint32_t Minute() const override { return minute; }
};

struct FakeLighting final : IPropLightingApi {
    bool night = false;
    int refs = 0, releases = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    bool IsNight() override { return night; }
};

struct FakeView3D final : IPropView3DApi {
    bool triggered = false;
    int refs = 0, releases = 0, calls = 0;
    std::vector<std::uint32_t> queried;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    bool GlobalEffectTriggered(std::uint32_t effectId) override {
        ++calls;
        queried.push_back(effectId);
        return triggered;
    }
};

struct FakeTerrain final : IPropTerrainApi {
    float height = 10.0f;
    float water = 0.0f;
    int refs = 0, releases = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    float Height(float, float) override { return height; }
    float WaterHeight(float, float) override { return water; }
};

struct FakeOccupantManager final : IPropOccupantManagerApi {
    int refs = 0, releases = 0, updates = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    void UpdateOccupant(cISC4Occupant*, bool) override { ++updates; }
};

struct FakeCity final : IPropCityApi {
    int cellsX = 16, cellsZ = 16;
    float cellX = 16.0f, cellZ = 16.0f;
    int refs = 0, releases = 0, positionCalls = 0;
    void AddRef() override { ++refs; }
    void Release() override { ++releases; }
    float CellSizeX() override { return cellX; }
    float CellSizeZ() override { return cellZ; }
    int CellCountX() override { return cellsX; }
    int CellCountZ() override { return cellsZ; }
    void PositionToCell(float, float, int& cellXOut, int& cellZOut) override {
        ++positionCalls;
        cellXOut = 0;
        cellZOut = 0;
    }
};

struct PostedMessage {
    std::uint32_t type = 0;
    std::uint64_t data1 = 0, data2 = 0, data3 = 0, data4 = 0;
    bool postFlag = false;
};

struct FakeMessageServer final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added;
    std::vector<std::uint32_t> removed;
    std::vector<PostedMessage> posts;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        --refs;
        if (refs <= 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refs);
    }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool flag) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        PostedMessage post;
        post.type = standard->GetType();
        post.data1 = standard->GetData1();
        post.data2 = standard->GetData2();
        post.data3 = standard->GetData3();
        post.data4 = standard->GetData4();
        post.postFlag = flag;
        posts.push_back(post);
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        added.push_back(type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        removed.push_back(type);
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

struct FakeWealthHost final : IWealthHost {
    bool hasLot = false;
    int wealth = 1;
    bool HasLot(int, int) override { return hasLot; }
    int LotWealth(int, int) override { return wealth; }
    int CityCellWealth(int, int) override { return wealth; }
    unsigned ZoneType(int, int) override { return 0; }
    int PropWealth(std::uint32_t) override { return wealth; }
};

// ----------------------------------------------------------------------------------------- the host
struct FakeHost final : IPropDeveloperHost {
    FakeLotManager lotManager;
    FakePropManager propManager;
    FakePower power;
    FakeNetworkLotManager networkLots;
    FakeBuildingDevelopment buildingDev;
    FakeSimulator simulator;
    FakeClock clock;
    FakeLighting lighting;
    FakeView3D view;
    FakeTerrain terrain;
    FakeOccupantManager occupants;
    FakeCity city;
    FakeMessageServer* messages = nullptr;
    FakeWealthHost wealthHost;
    cRZRandom requesterRandom{12345};
    std::map<cSC4Lot*, std::vector<cSC4LotConfigurationObject>*> lotObjects;
    cSC4Lot* messageLot = nullptr;
    std::vector<std::pair<int, const char*>> agents;
    std::vector<int> agentIntervals;
    int tickAdds = 0, tickRemoves = 0;

    IPropLotManagerApi* LotManager() override { return &lotManager; }
    IPropManagerApi* PropManager() override { return &propManager; }
    IPropPowerApi* PowerSimulator() override { return &power; }
    IPropNetworkLotManagerApi* NetworkLotManager() override { return &networkLots; }
    IPropBuildingDevelopmentApi* BuildingDevelopment() override { return &buildingDev; }
    IPropSimulatorApi* Simulator() override { return &simulator; }
    cIGZMessageServer2* MessageServer() override { return messages; }
    IPropClockApi* Clock() override { return &clock; }
    IPropLightingApi* Lighting() override { return &lighting; }
    IPropView3DApi* View3D() override { return &view; }
    IPropTerrainApi* Terrain() override { return &terrain; }
    IPropOccupantManagerApi* OccupantManager() override { return &occupants; }
    IPropCityApi* City() override { return &city; }
    void AddTickNotification(cIGZMessageTarget2*, int channel) override {
        CHECK(channel == static_cast<int>(kPropAnimationChannel));
        ++tickAdds;
    }
    void RemoveTickNotification(cIGZMessageTarget2*, int) override { ++tickRemoves; }
    void AddSimulatorAgent(cIGZMessageTarget2*, int interval, const char* name, std::uint32_t flags) override {
        CHECK(flags == kPropAgentFlags);
        CHECK(std::strcmp(name, "cSC4PropDeveloper") == 0);
        agents.emplace_back(interval, name);
        agentIntervals.push_back(interval);
    }
    IWealthHost* WealthRequesterHost() override { return &wealthHost; }
    std::uint32_t TimeReferenceJulianDay() override { return kPropTimeReferenceJulianDay; }
    cRZRandom* RequesterRandom() override { return &requesterRandom; }
    const std::vector<cSC4LotConfigurationObject>* LotConfigurationObjects(cSC4Lot* lot) override {
        const auto it = lotObjects.find(lot);
        return it == lotObjects.end() ? nullptr : it->second;
    }
    cSC4Lot* MessageLot(cIGZMessage2*) override { return messageLot; }
    bool WriteService(cIGZOStream&, IPropDeveloperService*) override { return false; }
    IPropDeveloperService* ReadService(cIGZIStream&, std::uint32_t) override { return nullptr; }
};

// ----------------------------------------------------------------------------------------- lot host
struct FakeLotHost final : ILotHost {
    LotBuildingProfile profile;
    std::uint32_t developerType = 0x1010;
    std::uint8_t ZoneTypeAt(int, int) override { return 0; }
    std::int32_t SimulatorDay() override { return 0; }
    float TerrainAltitude(float, float) override { return 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t) override { return &profile; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
    std::uint32_t DeveloperType(std::uint32_t, std::int32_t) override { return developerType; }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int, int) override { return 0; }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t) override { return nullptr; }
    void PostLotMessage(std::uint32_t, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::int32_t) override {}
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 0.0f; }
    float TravelVolumeFactor() override { return 0.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = out[1] = out[2] = 0.0f; }
    void JobsPerOccupant(float out[4], int) override { out[0] = out[1] = out[2] = out[3] = 0.0f; }
    void ExemplarJobs(const cSC4Lot&, float out[4]) override { out[0] = out[1] = out[2] = out[3] = 0.0f; }
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override {
        available = true;
        return false;
    }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    bool RoadAccessCells(const cSC4Lot&, int) override { return false; }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return nullptr; }
    void WriteSerializable(cIGZOStream&, cIGZUnknown*) override {}
    cIGZUnknown* ReadSerializable(cIGZIStream&, std::uint32_t) override { return nullptr; }
};

// ----------------------------------------------------------------------------------------- fixture
struct Fixture {
    FakeHost host;
    FakeMessageServer* messages = nullptr;
    FakeLotHost lotHost;
    FakeRequester requester;
    std::vector<std::unique_ptr<FakeOccupant>> props;
    std::vector<std::unique_ptr<cSC4Lot>> lots;

    Fixture() {
        messages = new FakeMessageServer();
        host.messages = messages;
        SetPropDeveloperHost(&host);
        SetLotHost(&lotHost);
    }
    ~Fixture() {
        SetPropDeveloperHost(nullptr);
        SetLotHost(nullptr);
        for (auto& lot : lots) lot->SetLotConfiguration(nullptr);
        messages->Release();
    }

    cSC4PropDeveloper* MakeDeveloper() {
        auto* developer = new cSC4PropDeveloper();
        developer->AddRef();
        return developer;
    }
    void Drop(cSC4PropDeveloper*& developer) {
        if (!developer) return;
        developer->Release();
        developer = nullptr;
    }

    FakeOccupant* MakeProp(std::uint32_t propType, float x, float y, float z) {
        auto occupant = std::make_unique<FakeOccupant>();
        occupant->type = kOccupantTypeProp;
        occupant->prop.propType = propType;
        occupant->position[0] = x;
        occupant->position[1] = y;
        occupant->position[2] = z;
        FakeOccupant* raw = occupant.get();
        props.push_back(std::move(occupant));
        return raw;
    }

    cSC4Lot* MakeLot(int x, int z, int sx, int sz, std::uint32_t buildingType) {
        auto lot = std::make_unique<cSC4Lot>(x, z, sx, sz, 0);
        lot->SetBuildingType(buildingType);
        lot->SetState(3, 0, 0);   // kLotStateLivable
        cSC4Lot* raw = lot.get();
        lots.push_back(std::move(lot));
        // The lot manager answers for every cell of the lot footprint (the origin cell depends on the facing).
        std::int32_t rect[4] = {0, 0, 0, 0};
        raw->GetBoundingRect(rect);
        for (int cx = rect[0]; cx <= rect[2]; ++cx) {
            for (int cz = rect[1]; cz <= rect[3]; ++cz) host.lotManager.lots[{cx, cz}] = raw;
        }
        return raw;
    }
};

// A configuration with one optional prop object (type 1, flags bit 0) whose `extra` ids are the candidates.
cSC4LotConfigurationObject MakeOptionalObject(std::uint32_t id, std::vector<std::uint32_t> extra) {
    cSC4LotConfigurationObject object;
    object.type = 1;
    object.flags = 1;
    object.id = id;
    object.extra = std::move(extra);
    return object;
}

void AddUint(cSCPropertyHolder& holder, std::uint32_t id, std::uint32_t value) {
    holder.AddProperty(id, value, true);
}
void AddByte(cSCPropertyHolder& holder, std::uint32_t id, std::uint8_t value) {
    holder.AddProperty(id, static_cast<std::uint32_t>(value), true);
}
void AddFloatArray(cSCPropertyHolder& holder, std::uint32_t id, const float* values, std::uint32_t count) {
    auto* property = new cSCProperty(id);
    property->AddRef();   // the test's reference (a fresh port object starts at 0)
    property->GetPropertyValue()->RefFloat32(values, count);
    holder.AddProperty(property, true);   // the holder takes its own reference
    property->Release();
}
void AddByteArray(cSCPropertyHolder& holder, std::uint32_t id, const std::uint8_t* values,
                  std::uint32_t count) {
    auto* property = new cSCProperty(id);
    property->AddRef();   // the test's reference (a fresh port object starts at 0)
    property->GetPropertyValue()->RefUint8(values, count);
    holder.AddProperty(property, true);   // the holder takes its own reference
    property->Release();
}

// =================================================================================================== tests
void TestConstructionAndViews() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->ReferenceCount() == 1);
    CHECK(!developer->IsInitialized());
    CHECK(!developer->IsNight());
    CHECK(developer->AreSignsAndLabelsVisible());
    CHECK(developer->Phase() == 0);
    CHECK(developer->CursorState() == 0);
    CHECK(developer->TractRect().x0 == static_cast<std::int32_t>(0x80000000u));
    CHECK(developer->TractRect().z0 == static_cast<std::int32_t>(0x80000000u));
    CHECK(developer->TractRect().x1 == 0x7fffffff);
    CHECK(developer->TractRect().z1 == 0x7fffffff);
    CHECK(developer->GetGZCLSID() == kGZCLSID_cSC4PropDeveloper);

    void* out = nullptr;
    CHECK(developer->QueryInterface(1, &out));
    CHECK(out == static_cast<cISC4PropDeveloper*>(developer));
    CHECK(developer->ReferenceCount() == 2);
    out = nullptr;
    CHECK(developer->QueryInterface(kGZIID_cISC4PropDeveloper, &out));
    CHECK(out == static_cast<cISC4PropDeveloper*>(developer));
    out = nullptr;
    CHECK(developer->QueryInterface(kGZIID_cIGZMessageTarget2PropDeveloper, &out));
    CHECK(out == static_cast<cIGZMessageTarget2*>(developer));
    out = nullptr;
    CHECK(developer->QueryInterface(kGZIID_cIGZSerializablePropDeveloper, &out));
    CHECK(out == static_cast<cIGZSerializable*>(developer));
    CHECK(!developer->QueryInterface(0x12345678, &out));
    CHECK(developer->ReferenceCount() == 5);
    developer->Release();
    developer->Release();
    developer->Release();
    developer->Release();
    CHECK(developer->ReferenceCount() == 1);
    fixture.Drop(developer);   // destroys the object
}

void TestInitAndShutdown() {
    Fixture fixture;
    fixture.host.city.cellsX = 128;   // 16 tracts
    fixture.host.city.cellsZ = 64;    // 8 tracts
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    CHECK(developer->IsInitialized());
    CHECK(developer->Phase() == 1);
    CHECK(developer->TractRect().x0 == 0 && developer->TractRect().z0 == 0);
    CHECK(developer->TractRect().x1 == 15 && developer->TractRect().z1 == 7);
    CHECK(developer->Requesters().size() == 1);   // the cSC4WealthRequester Init creates
    CHECK(fixture.host.lotManager.refs == 1);
    CHECK(fixture.host.propManager.refs == 1);
    CHECK(fixture.host.power.refs == 1);
    CHECK(fixture.host.networkLots.refs == 1);
    CHECK(fixture.host.agentIntervals.size() == 2);
    CHECK(fixture.host.agentIntervals[0] == 10 && fixture.host.agentIntervals[1] == 4);
    CHECK(fixture.host.tickAdds == 1);
    CHECK(fixture.messages->added.size() == 13);
    CHECK(fixture.messages->added.front() == kPropMessageOccupantInserted);
    CHECK(fixture.messages->added.back() == kPropMessageTerrainChanged2);

    // Clear the city size: the rectangle clamps to 0 (the +0x24 byte stays 0).
    CHECK(developer->Shutdown());
    CHECK(!developer->IsInitialized());
    CHECK(developer->Phase() == 0);
    CHECK(fixture.host.lotManager.releases == 1);
    CHECK(fixture.host.propManager.releases == 1);
    CHECK(fixture.host.power.releases == 1);
    CHECK(fixture.host.networkLots.releases == 1);
    CHECK(fixture.host.simulator.removals == 1);
    CHECK(fixture.host.tickRemoves == 1);
    CHECK(fixture.messages->removed.size() == 13);
    CHECK(developer->Requesters().empty());
    fixture.Drop(developer);
}

void TestEmptyCityRectangle() {
    Fixture fixture;
    fixture.host.city.cellsX = 7;   // < 8: (cells >> 3) - 1 == -1 clamped to 0
    fixture.host.city.cellsZ = 0;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    CHECK(developer->TractRect().x1 == 0 && developer->TractRect().z1 == 0);
    CHECK(developer->CursorState() == 0);
    fixture.Drop(developer);
}

void TestRequesters() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    CHECK(developer->Requesters().size() == 1);
    fixture.requester.share = 5;
    developer->AddPropRequester(&fixture.requester);
    CHECK(developer->Requesters().size() == 2);
    CHECK(fixture.requester.refs == 2);   // one from the test, one from the developer
    developer->RemovePropRequester(&fixture.requester);
    CHECK(developer->Requesters().size() == 1);
    CHECK(fixture.requester.refs == 1);
    developer->RemovePropRequester(&fixture.requester);   // not present: no release
    CHECK(fixture.requester.refs == 1);
    fixture.Drop(developer);
}

void TestOccupantInsertAndRemove() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    fixture.host.lighting.night = false;

    // A prop with no state/time/date/effect/power property: the no-shadow marker makes it ignored.
    FakeOccupant* ignored = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(ignored->holder, kPropPropertyNoShadow, 1);
    developer->HandleInsertOccupant(ignored);
    CHECK(developer->StateProps().empty());

    // A night-state prop: it enters the state list and gets state 0 (day).
    FakeOccupant* stateProp = fixture.MakeProp(0x2977aa47, 16.0f, 0.0f, 32.0f);
    AddByte(stateProp->holder, kPropPropertyNoShadow, 0);   // has the property -> ignored
    developer->HandleInsertOccupant(stateProp);
    CHECK(developer->StateProps().empty());
    stateProp->holder.RemoveProperty(kPropPropertyNoShadow, false);
    AddByte(stateProp->holder, kPropPropertyNightState, 1);
    developer->HandleInsertOccupant(stateProp);
    CHECK(developer->StateProps().size() == 1);
    CHECK(stateProp->prop.state == 0);
    CHECK(stateProp->prop.timerBits == 0);

    // At night the state follows the power grid.
    fixture.host.power.powered = true;
    developer->HandleRemoveOccupant(stateProp);   // clears the lists first
    CHECK(developer->StateProps().empty());
    fixture.host.lighting.night = true;
    developer->DoMessage(nullptr);   // no-op guard
    // The lights-on message sets night_ and the animation mode; then insert.
    cRZMessage2Standard lightsOn;
    lightsOn.SetType(kPropMessageLightsOn);
    CHECK(developer->DoMessage(&lightsOn));
    CHECK(developer->IsNight());
    CHECK(developer->AnimationMode() == 1);
    developer->HandleInsertOccupant(stateProp);
    CHECK(developer->StateProps().size() == 1);
    CHECK(stateProp->prop.state == 1);
    // A prop whose state is 0 does not light up when the cell has no power.
    fixture.host.power.powered = false;
    developer->HandleRemoveOccupant(stateProp);
    stateProp->prop.state = 0;
    developer->HandleInsertOccupant(stateProp);
    CHECK(stateProp->prop.state == 0);   // unpowered at night
    fixture.host.power.powered = true;
    developer->HandleRemoveOccupant(stateProp);
    stateProp->prop.state = 0;
    developer->HandleInsertOccupant(stateProp);
    CHECK(stateProp->prop.state == 1);   // powered at night
    fixture.host.power.powered = true;

    // A prop with a time window: timer bit 1 is set from the window, the pair is stored.
    FakeOccupant* timed = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(timed->holder, kPropPropertyNightState, 1);
    const float window[2] = {6.0f, 18.0f};
    AddFloatArray(timed->holder, kPropPropertyTimeWindow, window, 2);
    AddByte(timed->holder, kPropPropertyRandomChance, 42);
    developer->HandleInsertOccupant(timed);
    CHECK(developer->StateProps().size() == 2);
    CHECK(developer->TimeProps().size() == 1);
    CHECK(timed->prop.timeStart == 6.0f && timed->prop.timeEnd == 18.0f);
    CHECK(timed->prop.randomChance == 42);

    // A dated prop: slot 0x78 arguments come from the three properties.
    FakeOccupant* dated = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddUint(dated->holder, kPropPropertyDatePropType, 0x5001);
    const std::uint8_t monthDay[2] = {7, 14};
    AddByteArray(dated->holder, kPropPropertyDateMonthDay, monthDay, 2);
    AddUint(dated->holder, kPropPropertyDateSpan, 9);
    developer->HandleInsertOccupant(dated);
    CHECK(developer->DateProps().size() == 1);
    CHECK(dated->prop.datePropType == 0x5001);
    CHECK(dated->prop.dateSpan == 9);
    CHECK(dated->prop.dateMonth == 7 && dated->prop.dateDay == 14);
    CHECK((dated->prop.timerBits & kPropTimerBitDate) == 0);

    // A prop without the date property gets timer bit 2 set.
    FakeOccupant* noDate = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(noDate->holder, kPropPropertyPowerTimer, 1);
    developer->HandleInsertOccupant(noDate);
    CHECK((noDate->prop.timerBits & kPropTimerBitDate) != 0);
    CHECK(developer->PowerProps().size() == 1);
    CHECK((noDate->prop.timerBits & kPropTimerBitPower) != 0);   // the fake grid is powered

    // An effect prop: the map is keyed by the effect id and the timer bit comes from the view.
    FakeOccupant* effect = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddUint(effect->holder, kPropPropertyEffectId, 0x1234);
    fixture.host.view.triggered = true;
    developer->HandleInsertOccupant(effect);
    CHECK(developer->EffectProps().count(0x1234) == 1);
    CHECK((effect->prop.timerBits & kPropTimerBitEffect) != 0);
    CHECK(fixture.host.view.queried.back() == 0x1234);

    // A sign prop: kept in the sign list.
    FakeOccupant* sign = fixture.MakeProp(kPropTypeSignpost, 10.0f, 5.0f, 20.0f);
    AddByte(sign->holder, kPropPropertyNightState, 0);
    const float signWindow[2] = {6.0f, 6.0f};
    AddFloatArray(sign->holder, kPropPropertyTimeWindow, signWindow, 2);
    developer->HandleInsertOccupant(sign);
    CHECK(developer->SignProps().size() == 1);

    // Removing the timed prop takes it out of the time list and releases the list's reference.
    developer->HandleRemoveOccupant(timed);
    CHECK(developer->TimeProps().empty());
    developer->HandleRemoveOccupant(effect);
    CHECK(developer->EffectProps().empty());
    developer->HandleRemoveOccupant(dated);
    CHECK(developer->DateProps().empty());
    developer->HandleRemoveOccupant(noDate);
    CHECK(developer->PowerProps().empty());
    developer->HandleRemoveOccupant(sign);
    CHECK(developer->SignProps().empty());

    // Quirk kept: the sign check sits after the property block (0x100123e11), so a sign with none of the four
    // properties is listed as well.
    FakeOccupant* plainSign = fixture.MakeProp(kPropTypeNetworkSign, 0.0f, 0.0f, 0.0f);
    AddByte(plainSign->holder, kPropPropertyNightState, 1);
    developer->HandleInsertOccupant(plainSign);
    CHECK(developer->SignProps().size() == 1);
    CHECK((plainSign->prop.timerBits & kPropTimerBitTimeOfDay) == 0);   // the property block did not run
    fixture.Drop(developer);
}

void TestMinuteChanged() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    fixture.host.clock.hour = 9;
    fixture.host.clock.minute = 0;
    FakeOccupant* timed = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(timed->holder, kPropPropertyNightState, 1);
    const float window[2] = {6.0f, 18.0f};
    AddFloatArray(timed->holder, kPropPropertyTimeWindow, window, 2);
    developer->HandleInsertOccupant(timed);
    CHECK(developer->TimeProps().size() == 1);

    cRZMessage2Standard message;
    message.SetType(kPropMessageMinuteChanged);
    CHECK(developer->DoMessage(&message));
    CHECK((timed->prop.timerBits & kPropTimerBitTimeOfDay) != 0);   // 09:00 is inside 6..18
    // A change smaller than the 0.4 h threshold is ignored.
    fixture.host.clock.hour = 18;
    fixture.host.clock.minute = 12;   // 18.2 h
    developer->DoMessage(&message);
    CHECK((timed->prop.timerBits & kPropTimerBitTimeOfDay) == 0);   // 18.2 > 18
    // Close enough to the last time: no update.
    fixture.host.clock.hour = 18;
    fixture.host.clock.minute = 24;   // 18.4 h, exactly the threshold
    developer->DoMessage(&message);
    CHECK((timed->prop.timerBits & kPropTimerBitTimeOfDay) == 0);
    // A wrapping window (20:00 .. 06:00) is on outside its ends.
    timed->prop.timeStart = 20.0f;
    timed->prop.timeEnd = 6.0f;
    fixture.host.clock.hour = 23;
    fixture.host.clock.minute = 0;
    developer->DoMessage(&message);
    CHECK((timed->prop.timerBits & kPropTimerBitTimeOfDay) != 0);
    fixture.host.clock.hour = 12;
    developer->DoMessage(&message);
    CHECK((timed->prop.timerBits & kPropTimerBitTimeOfDay) == 0);
    fixture.Drop(developer);
}

void TestDayTickAndAnimation() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    FakeOccupant* dated = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddUint(dated->holder, kPropPropertyDatePropType, 0x5001);
    developer->HandleInsertOccupant(dated);
    cRZMessage2Standard day;
    day.SetType(kPropMessageDay);
    CHECK(developer->DoMessage(&day));
    CHECK(dated->prop.dateTicks == 1);

    // Eight props -> a batch of one; the tick needs 200 seconds and then moves one prop per call.
    for (int i = 0; i < 8; ++i) {
        FakeOccupant* prop = fixture.MakeProp(0x2977aa47, 16.0f * i, 0.0f, 0.0f);
        AddByte(prop->holder, kPropPropertyNightState, 1);
        developer->HandleInsertOccupant(prop);
    }
    cRZMessage2Standard lightsOff;
    lightsOff.SetType(kPropMessageLightsOff);
    CHECK(developer->DoMessage(&lightsOff));
    CHECK(!developer->IsNight());
    CHECK(developer->AnimationMode() == 2);
    CHECK(developer->AnimationBatch() == 1);   // the eight state props -> ceil(8/8)
    cRZMessage2Standard tick;
    tick.SetType(kPropMessageAnimationTick);
    tick.SetData2(100000);   // 100 s
    CHECK(developer->DoMessage(&tick));
    tick.SetData2(150000);   // 250 s -> the first batch runs
    CHECK(developer->DoMessage(&tick));
    // The idle-time message hands its data3 to the (ignored) DoIdle argument.
    cRZMessage2Standard idle;
    idle.SetType(kPropMessageIdleTime);
    idle.SetData3(5);
    CHECK(developer->DoMessage(&idle));
    fixture.Drop(developer);
}

void TestLotStateAndPower() {
    Fixture fixture;
    fixture.host.city.cellX = 16.0f;
    fixture.host.city.cellZ = 16.0f;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());

    // Lot state props: state = (property value != lot state).
    cSC4Lot* lot = fixture.MakeLot(0, 0, 2, 2, 0x1000);
    FakeOccupant* stateProp = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(stateProp->holder, kPropPropertyLotState, 3);
    fixture.host.propManager.lotProps = {stateProp};
    developer->UpdateLotStateProps(lot);
    CHECK(stateProp->prop.state == 0);   // 3 == lot state 3
    AddByte(stateProp->holder, kPropPropertyLotState, 1);
    developer->UpdateLotStateProps(lot);
    CHECK(stateProp->prop.state == 1);   // 1 != 3
    CHECK(stateProp->refs == 1);         // the list reference was released

    // Power changed: the state prop follows the grid, the power-timer prop gets timer bit 8.
    AddByte(stateProp->holder, kPropPropertyNightState, 1);
    FakeOccupant* powerProp = fixture.MakeProp(0x2977aa47, 16.0f, 0.0f, 16.0f);
    AddByte(powerProp->holder, kPropPropertyPowerTimer, 1);
    stateProp->position[0] = 16.0f;
    stateProp->position[2] = 16.0f;
    cRZMessage2Standard lightsOn;
    lightsOn.SetType(kPropMessageLightsOn);
    CHECK(developer->DoMessage(&lightsOn));
    CHECK(developer->IsNight());
    stateProp->prop.state = 1;
    fixture.host.propManager.rectProps = {stateProp};
    developer->HandleInsertOccupant(powerProp);
    CHECK(developer->PowerProps().size() == 1);
    fixture.host.power.powered = false;
    developer->DoPowerChanged(SC4Rect<int>(0, 0, 1, 1));
    CHECK(stateProp->prop.state == 0);   // at night the state follows the grid
    CHECK((powerProp->prop.timerBits & kPropTimerBitPower) == 0);
    fixture.host.power.powered = true;
    developer->DoPowerChanged(SC4Rect<int>(0, 0, 1, 1));
    CHECK(stateProp->prop.state == 1);
    // Outside the rectangle the power prop keeps its timer.
    developer->DoPowerChanged(SC4Rect<int>(4, 4, 5, 5));
    CHECK((powerProp->prop.timerBits & kPropTimerBitPower) != 0);
    fixture.host.propManager.rectProps.clear();
    fixture.Drop(developer);
}

void TestSigns() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    FakeOccupant* sign = fixture.MakeProp(kPropTypeSignpost, 32.0f, 1.0f, 48.0f);
    AddByte(sign->holder, kPropPropertyNightState, 1);
    const float flatWindow[2] = {6.0f, 6.0f};
    AddFloatArray(sign->holder, kPropPropertyTimeWindow, flatWindow, 2);   // present but empty: not a time prop
    developer->HandleInsertOccupant(sign);
    CHECK(developer->SignProps().size() == 1);
    // Terrain 10, water 12: the sign sits on the higher one.
    fixture.host.terrain.height = 10.0f;
    fixture.host.terrain.water = 12.0f;
    developer->UpdateSignpostHeights();
    CHECK(sign->position[1] == 12.0f);
    CHECK(fixture.host.occupants.updates == 1);
    // Already at the right height: no update.
    developer->UpdateSignpostHeights();
    CHECK(fixture.host.occupants.updates == 1);
    // Show / hide.
    CHECK(developer->AreSignsAndLabelsVisible());
    developer->ShowSignsAndLabels(false);
    CHECK(!developer->AreSignsAndLabelsVisible());
    CHECK((sign->flags & kPropSignVisibleFlag) == 0 && sign->flagCalls == 1);   // ClearFlag(2)
    developer->ShowSignsAndLabels(false);   // already hidden: no call
    CHECK(sign->flagCalls == 1);
    developer->ToggleSignsAndLabels();
    CHECK(developer->AreSignsAndLabelsVisible());
    CHECK((sign->flags & kPropSignVisibleFlag) != 0 && sign->flagCalls == 2);   // SetFlag(2)
    // A network sign at cell (0,0): removing a network occupant in that cell removes the sign.
    FakeOccupant* networkSign = fixture.MakeProp(kPropTypeNetworkSign, 0.0f, 0.0f, 0.0f);
    AddByte(networkSign->holder, kPropPropertyNightState, 1);
    AddFloatArray(networkSign->holder, kPropPropertyTimeWindow, flatWindow, 2);
    developer->HandleInsertOccupant(networkSign);
    FakeOccupant* removedOccupant = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    removedOccupant->cells = SC4CellRect{0, 0, 0, 0};
    developer->HandleRemoveNetworkOccupant(removedOccupant);
    CHECK(fixture.host.propManager.removed.size() == 1);
    CHECK(fixture.host.propManager.removed[0] == networkSign);
    fixture.Drop(developer);
}

void TestTractWalk() {
    Fixture fixture;
    fixture.host.city.cellsX = 16;
    fixture.host.city.cellsZ = 16;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    developer->AddPropRequester(&fixture.requester);
    CHECK(developer->Phase() == 1);
    developer->DoIdle(0);   // arms the walk
    CHECK(developer->Phase() == 2);
    CHECK(fixture.requester.shareCalls == 0);
    developer->DoIdle(0);
    CHECK(fixture.requester.shareCalls == 1);   // tract (0,0)
    developer->DoIdle(0);
    developer->DoIdle(0);
    developer->DoIdle(0);
    CHECK(fixture.requester.shareCalls == 4);   // the four tracts of a 16x16 city
    CHECK(fixture.requester.rects[0].x0 == 0 && fixture.requester.rects[0].x1 == 7);
    CHECK(fixture.requester.rects[1].x0 == 8 && fixture.requester.rects[1].x1 == 15);
    CHECK(fixture.requester.rects[2].z0 == 8 && fixture.requester.rects[2].z1 == 15);
    developer->DoIdle(0);   // wraps and restarts in the same call
    CHECK(fixture.requester.shareCalls == 5);
    CHECK(fixture.requester.rects.back().x0 == 0 && fixture.requester.rects.back().z0 == 0);
    fixture.Drop(developer);
}

void TestSelectAndCreateProp() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    developer->AddPropRequester(&fixture.requester);
    fixture.requester.customSelector = false;

    // The requester is index 1 in the vector.
    std::vector<cSC4LotConfigurationObject> objects;
    objects.push_back(MakeOptionalObject(0x777, {0x6000}));   // 0x6000 is a family
    fixture.host.propManager.families[0x6000] = {0x5001, 0x5002};
    fixture.host.propManager.profiles[0x5001].valueA = 0x4321;
    fixture.host.propManager.profiles[0x5002].valueA = 0x9999;
    fixture.requester.satisfaction = 0x4321;
    cSC4Lot* lot = fixture.MakeLot(0, 0, 2, 2, 0x1000);
    FakeOccupant* created = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    fixture.host.buildingDev.developer.nextProp = created;

    PropSlot slot;
    slot.x = 0;
    slot.z = 0;
    slot.objects = &objects;
    slot.objectIndex = 0;
    const std::uint32_t result = developer->SelectAndCreateProp(&slot, 1);
    CHECK(result == 1);
    CHECK(fixture.host.buildingDev.developer.lots.size() == 1);
    CHECK(fixture.host.buildingDev.developer.lots[0] == lot);
    CHECK(fixture.host.buildingDev.developer.indices[0] == 0);
    CHECK(fixture.host.buildingDev.developer.types[0] == 0x5001);
    std::uint32_t requesterIndex = 0;
    CHECK(cSCPropertyHelp::GetPropertyValue(created->AsPropertyHolder(), kPropPropertyRequester, requesterIndex));
    CHECK(requesterIndex == 1);

    // A custom selector picks the type itself; a mismatch removes nothing but returns 0 from this function.
    fixture.requester.customSelector = true;
    fixture.requester.selectResult = 0x5002;
    CHECK(developer->SelectAndCreateProp(&slot, 1) == 1);
    CHECK(fixture.host.buildingDev.developer.types.back() == 0x5002);
    // A custom selector's answer is used as-is even when it is not one of the candidates (the original only
    // checks for zero).
    fixture.requester.selectResult = 0x7777;
    CHECK(developer->SelectAndCreateProp(&slot, 1) == 1);
    CHECK(fixture.host.buildingDev.developer.types.back() == 0x7777);
    CHECK(fixture.requester.selections.back().size() == 1);
    CHECK(fixture.requester.selections.back()[0] == 0x5001);

    // No candidate satisfies the requester: 0.
    fixture.requester.satisfaction = 0x1234;
    CHECK(developer->SelectAndCreateProp(&slot, 1) == 0);
    fixture.requester.satisfaction = 0x4321;

    // No lot at all and no network lot: 0.
    fixture.host.lotManager.lots.clear();
    CHECK(developer->SelectAndCreateProp(&slot, 1) == 0);

    // A network lot: the prop manager's CreateProp is used.
    fixture.requester.customSelector = false;
    auto* configuration = new cSC4NetworkLotConfiguration();
    configuration->AddRef();   // the test's reference (a fresh port object starts at 0)
    configuration->GetObjectArray() = objects;
    cSC4NetworkLot networkLot;
    networkLot.SetConfiguration(configuration);
    configuration->Release();  // the lot keeps one, released with the lot
    fixture.host.networkLots.lots[{0, 0}] = &networkLot;
    fixture.host.networkLots.nextProp = created;
    CHECK(developer->SelectAndCreateProp(&slot, 1) == 1);
    CHECK(fixture.host.networkLots.createdIndices.back() == 0);
    CHECK(fixture.host.networkLots.createdTypes.back() == 0x5001);

    // The return value quirk: with more than 256 ids it returns (count & ~0xff) | 1.
    std::vector<cSC4LotConfigurationObject> many;
    cSC4LotConfigurationObject wide = MakeOptionalObject(0x778, {});
    wide.extra.assign(258, 0x5001);
    many.push_back(wide);
    PropSlot wideSlot = slot;
    wideSlot.objects = &many;
    CHECK(developer->SelectAndCreateProp(&wideSlot, 1) == 0x101);
    fixture.Drop(developer);
}

void TestUpdateTract() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    // Drop the wealth requester Init created so the fake requester is the only one (index 0).
    developer->RemovePropRequester(developer->Requesters()[0]);
    developer->AddPropRequester(&fixture.requester);
    fixture.requester.share = 2;
    fixture.requester.satisfaction = 0x4321;
    fixture.requester.customSelector = false;

    cSC4Lot* lot = fixture.MakeLot(0, 0, 2, 2, 0x1000);
    fixture.lotHost.profile.capacities.resize(1);
    fixture.lotHost.profile.capacities[0][0x1010] = 1;
    CHECK(lot->SetPopulation(0x1010, 1));
    CHECK(std::fabs(lot->GetOccupancy() - 1.0f) < 1e-6f);
    std::vector<cSC4LotConfigurationObject> objects;
    objects.push_back(MakeOptionalObject(0x777, {0x5001}));
    fixture.host.lotObjects[lot] = &objects;
    fixture.host.propManager.profiles[0x5001].valueA = 0x4321;
    FakeOccupant* created = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    fixture.host.buildingDev.developer.nextProp = created;

    developer->UpdateTract(0, 0);
    // One optional slot, occupancy 1.0 -> one prop wanted; the requester asks for 2, so the single slot's
    // remainder goes to it and exactly one prop is created.
    CHECK(fixture.host.buildingDev.developer.types.size() == 1);
    CHECK(fixture.host.buildingDev.developer.types[0] == 0x5001);
    CHECK(fixture.host.buildingDev.developer.lots[0] == lot);
    CHECK(created->refs == 2);   // the fixture's reference + the one CreateLotProp returned

    // Now the lot already holds the prop, tagged with the object id: it is kept, not re-created.
    created->lotTag = 0x777;
    created->hasLotTag = true;
    std::uint32_t index = 0;
    CHECK(cSCPropertyHelp::GetPropertyValue(created->AsPropertyHolder(), kPropPropertyRequester, index));
    fixture.host.propManager.lotProps = {created};
    const std::size_t before = fixture.host.buildingDev.developer.types.size();
    const int refsBefore = created->refs;
    developer->UpdateTract(0, 0);
    CHECK(fixture.host.buildingDev.developer.types.size() == before);
    CHECK(fixture.host.propManager.removed.empty());
    CHECK(created->refs == refsBefore);   // all the temporary references were released

    // An optional prop whose requester no longer selects it is removed from the lot.
    fixture.requester.customSelector = true;
    fixture.requester.selectResult = 0x9999;
    developer->UpdateTract(0, 0);
    CHECK(fixture.host.propManager.removed.size() == 1);
    CHECK(fixture.host.propManager.removed[0] == created);
    // ... and the removed prop is only removed once per tract (the bit is set again, a PropSlot is queued).
    // A non-optional object keeps its prop.
    fixture.requester.customSelector = false;
    fixture.host.propManager.removed.clear();
    objects[0].flags = 0;
    developer->UpdateTract(0, 0);
    CHECK(fixture.host.propManager.removed.empty());
    fixture.Drop(developer);
}

void TestRemoveOptionalPropsInLot() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    cSC4Lot* lot = fixture.MakeLot(0, 0, 2, 2, 0x1000);
    std::vector<cSC4LotConfigurationObject> objects;
    objects.push_back(MakeOptionalObject(0x777, {0x5001}));
    objects.push_back(MakeOptionalObject(0x778, {0x5001}));
    objects[1].flags = 0;   // not optional
    fixture.host.lotObjects[lot] = &objects;
    FakeOccupant* optional = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    optional->lotTag = 0x777;
    optional->hasLotTag = true;
    FakeOccupant* mandatory = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    mandatory->lotTag = 0x778;
    mandatory->hasLotTag = true;
    FakeOccupant* unknown = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    fixture.host.propManager.lotProps = {optional, mandatory, unknown};
    developer->RemoveOptionalPropsInLot(lot);
    CHECK(fixture.host.propManager.removed.size() == 1);
    CHECK(fixture.host.propManager.removed[0] == optional);
    CHECK(optional->refs == 1 && mandatory->refs == 1 && unknown->refs == 1);
    fixture.Drop(developer);
}

void TestLotStateMessage() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    cSC4Lot* lot = fixture.MakeLot(0, 0, 2, 2, 0x1000);
    std::vector<cSC4LotConfigurationObject> objects;
    objects.push_back(MakeOptionalObject(0x777, {0x5001}));
    fixture.host.lotObjects[lot] = &objects;
    FakeOccupant* optional = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    optional->lotTag = 0x777;
    optional->hasLotTag = true;
    fixture.host.propManager.lotProps = {optional};
    fixture.host.messageLot = lot;
    lot->SetState(1, 0, 0);   // under construction -> not livable
    cRZMessage2Standard message;
    message.SetType(kPropMessageLotStateChanged);
    message.SetData1(2);      // old state (occupied)
    message.SetData2(3);      // new state (livable)
    cRZMessage2Standard* heap = new cRZMessage2Standard(message);
    heap->AddRef();
    CHECK(developer->DoMessage(heap));
    CHECK(fixture.host.propManager.removed.size() == 1);   // the optional prop was removed
    CHECK(fixture.messages->posts.size() == 1);
    CHECK(fixture.messages->posts[0].type == kPropMessagePropsChanged);
    CHECK(fixture.messages->posts[0].data1 == 1);   // the lot's origin cell (facing 0 -> corner + size - 1)
    CHECK(fixture.messages->posts[0].data2 == 0);
    CHECK(fixture.messages->posts[0].data3 == 1);   // old 2 -> new 3
    heap->Release();

    // A different lot location is ignored.
    fixture.messages->posts.clear();
    fixture.host.messageLot = nullptr;
    CHECK(developer->DoMessage(&message));
    CHECK(fixture.messages->posts.empty());

    // The "props changed" reply runs UpdateLotStateProps for the lot at that cell.
    FakeOccupant* stateProp = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(stateProp->holder, kPropPropertyLotState, 0);
    fixture.host.propManager.lotProps = {stateProp};
    lot->SetState(3, 0, 0);
    cRZMessage2Standard reply;
    reply.SetType(kPropMessagePropsChanged);
    reply.SetData1(0);
    reply.SetData2(0);
    CHECK(developer->DoMessage(&reply));
    CHECK(stateProp->prop.state == 1);   // 0 != 3
    fixture.Drop(developer);
}

void TestGlobalEffectAndTerrainMessages() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    FakeOccupant* effect = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddUint(effect->holder, kPropPropertyEffectId, 0xBEEF);
    fixture.host.view.triggered = false;
    developer->HandleInsertOccupant(effect);
    CHECK(developer->EffectProps().size() == 1);
    CHECK((effect->prop.timerBits & kPropTimerBitEffect) == 0);
    cRZMessage2Standard message;
    message.SetType(kPropMessageGlobalEffectChanged);
    message.SetData1(0xBEEF);
    message.SetData2(1);
    CHECK(developer->DoMessage(&message));
    CHECK((effect->prop.timerBits & kPropTimerBitEffect) != 0);
    message.SetData2(0);
    developer->DoMessage(&message);
    CHECK((effect->prop.timerBits & kPropTimerBitEffect) == 0);

    // Terrain changed updates the sign height.
    FakeOccupant* sign = fixture.MakeProp(kPropTypeSignpost, 8.0f, 0.0f, 8.0f);
    AddByte(sign->holder, kPropPropertyNightState, 1);
    const float signWindow[2] = {6.0f, 6.0f};
    AddFloatArray(sign->holder, kPropPropertyTimeWindow, signWindow, 2);
    developer->HandleInsertOccupant(sign);
    fixture.host.terrain.height = 7.5f;
    fixture.host.terrain.water = 0.0f;
    message = cRZMessage2Standard();
    message.SetType(kPropMessageTerrainChanged);
    CHECK(developer->DoMessage(&message));
    CHECK(sign->position[1] == 7.5f);
    // Day/night message with a pinned clock.
    fixture.host.clock.mode = 2;   // midnight
    message = cRZMessage2Standard();
    message.SetType(kPropMessageDayNightChanged);
    message.SetData1(0);           // day, but the view is pinned to midnight
    CHECK(developer->DoMessage(&message));
    CHECK(fixture.host.power.calls > 0);
    // City init re-reads the lighting manager.
    fixture.host.lighting.night = true;
    message = cRZMessage2Standard();
    message.SetType(kPropMessageCityInit);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->IsNight());
    fixture.Drop(developer);
}

void TestInsertRemoveMessages() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    FakeOccupant* prop = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(prop->holder, kPropPropertyNightState, 1);
    cRZMessage2Standard message;
    message.SetType(kPropMessageOccupantInserted);
    message.SetData3(kPropOccupantType);
    message.SetVoid1(prop);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->StateProps().size() == 1);
    // A different occupant type is ignored.
    message.SetData3(0x12345678);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->StateProps().size() == 1);
    message.SetData3(kPropOccupantType);
    message.SetType(kPropMessageOccupantRemoved);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->StateProps().empty());
    // The animation tick while the mode is 0 is not handled.
    cRZMessage2Standard tick;
    tick.SetType(kPropMessageAnimationTick);
    CHECK(!developer->DoMessage(&tick));
    fixture.Drop(developer);
}

void TestUpdateAllOptionalProps() {
    Fixture fixture;
    fixture.host.city.cellsX = 8;
    fixture.host.city.cellsZ = 8;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    developer->AddPropRequester(&fixture.requester);
    developer->UpdateAllOptionalProps();
    CHECK(fixture.requester.shareCalls == 1);
    CHECK(fixture.requester.rects[0].x1 == 7 && fixture.requester.rects[0].z1 == 7);
    fixture.Drop(developer);
}

std::vector<unsigned char> MakeReadStream() {
    std::vector<unsigned char> bytes;
    auto u8 = [&](unsigned value) { bytes.push_back(static_cast<unsigned char>(value & 0xff)); };
    auto u16 = [&](unsigned value) {
        u8(value);
        u8(value >> 8);
    };
    auto u32 = [&](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) u8(value >> (8 * i));
    };
    u16(5);   // version
    u8(2);    // phase
    u8(0);    // scan cursor: state
    u32(0); u32(0); u32(0);                                      // x, z, remaining
    u32(0x80000000u); u32(0x7fffffffu); u32(0x80000000u); u32(0x7fffffffu);   // the cursor's rectangle
    u8(0);    // requester created
    u32(0);   // requesters
    u32(3);   // animation index
    u32(1);   // animation mode
    u32(2);   // animation batch
    u32(7);   // animation seconds
    u8(1);    // night
    u32(0);   // state props
    u32(0);   // effect map entries
    u32(0);   // time props
    u32(0);   // date props
    u32(0);   // power props
    u8(0);    // signs visible
    u32(0);   // sign props
    return bytes;
}

void TestStreams() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    {
        PortMemoryOStream stream;
        CHECK(developer->Write(stream));
        const std::vector<unsigned char> expected = {
            5, 0,           // version
            1,              // phase
            0,              // scan cursor: state
            0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,                // x, z, remaining
            0, 0, 0, 0x80,  0xff, 0xff, 0xff, 0x7f,  0, 0, 0, 0x80,  0xff, 0xff, 0xff, 0x7f,   // rectangle
            1,              // requester created
            1, 0, 0, 0,     // one requester (its own bytes are skipped on a plain stream)
            0, 0, 0, 0,     // animation index
            0, 0, 0, 0,     // animation mode
            0, 0, 0, 0,     // animation batch
            0, 0, 0, 0,     // animation seconds
            0,              // night
            0, 0, 0, 0,     // state props
            0, 0, 0, 0,     // effect props
            0, 0, 0, 0,     // time props
            0, 0, 0, 0,     // date props
            0, 0, 0, 0,     // power props
            1,              // signs visible
            0, 0, 0, 0,     // sign props
        };
        CHECK(stream.bytes() == expected);
    }
    {
        const std::vector<unsigned char> bytes = MakeReadStream();
        PortMemoryIStream stream(bytes.data(), bytes.size());
        CHECK(developer->Read(stream));
        CHECK(developer->Phase() == 2);
        CHECK(developer->AnimationMode() == 1);
        CHECK(developer->AnimationBatch() == 2);
        CHECK(developer->IsNight());
        CHECK(!developer->AreSignsAndLabelsVisible());
    }
    {
        std::vector<unsigned char> bytes = MakeReadStream();
        bytes[0] = 3;   // version 3 is refused
        PortMemoryIStream stream(bytes.data(), bytes.size());
        CHECK(!developer->Read(stream));
    }
    fixture.Drop(developer);
}

void TestShutdownReleasesProps() {
    Fixture fixture;
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init());
    FakeOccupant* prop = fixture.MakeProp(0x2977aa47, 0.0f, 0.0f, 0.0f);
    AddByte(prop->holder, kPropPropertyNightState, 1);
    AddByte(prop->holder, kPropPropertyPowerTimer, 1);
    developer->HandleInsertOccupant(prop);
    CHECK(developer->StateProps().size() == 1);
    CHECK(prop->prop.refs == 3);   // the fixture's + the state list's + the power list's
    CHECK(developer->Shutdown());
    CHECK(prop->prop.refs == 1);
    CHECK(developer->StateProps().empty());
    fixture.Drop(developer);
}

}  // namespace

int main() {
    TestConstructionAndViews();
    TestInitAndShutdown();
    TestEmptyCityRectangle();
    TestRequesters();
    TestOccupantInsertAndRemove();
    TestMinuteChanged();
    TestDayTickAndAnimation();
    TestLotStateAndPower();
    TestSigns();
    TestTractWalk();
    TestSelectAndCreateProp();
    TestUpdateTract();
    TestRemoveOptionalPropsInLot();
    TestLotStateMessage();
    TestGlobalEffectAndTerrainMessages();
    TestInsertRemoveMessages();
    TestUpdateAllOptionalProps();
    TestStreams();
    TestShutdownReleasesProps();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("cSC4PropDeveloper: all checks passed\n");
    return 0;
}
