// cSC4LandfillDeveloper — unit test. Drives the recovered object through a fake ILandfillDeveloperHost
// (whose ILandfillZoneHost half builds real cSC4LandfillZone objects) and checks the decompiled formulas of
// 0x100060940..0x1000622b8 by hand.
//
// Covered: the three COM views and the single reference count (Release deletes below 2), Init (the simulator
// agent "cSC4LandfillDeveloper"/interval 6/flags 2, the five services with their AddRef, the five
// notifications, the tuning exemplar), Shutdown (notifications, zones, services, the agent through the global
// city), ReadTuningParams (all eight properties of exemplar {0x6534284a, 0x67cd5fa1, 0x6a2806af}), the
// landfill zone bookkeeping (DoZoneChange creating/merging a zone, the inflated overlap window, the exclusive
// upper bounds of GetLandfillZoneFromCell), the empty-cell texture path, DezoneArea/HandleZoneOverlap, the
// monthly simulation (garbage supply clamp, the budget/maintenance scaling, per-heap intake, the capacity
// publish, the two history records), UpdateBudgetRequirements (line item 30), the funds-changed message
// (generator SetEnabled), the view-level message (seagull flock Start/Stop), DoTerrainChanged, the stream round
// trip plus the crafted streams for the capacity saturation and the min(garbage, capacity) usage quirk.
//
// Real game data: the zones are real cSC4LandfillZone objects, the messages are real cRZMessage2Standard
// objects, the tunables are a real cSCPropertyHolder + cSCProperty + cRZVariant, and the streams are the port
// memory streams.

#include "sc4/LandfillDeveloper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/SCProperty.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using namespace sc4;

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

//----------------------------------------------------------------------------------------------- seams

struct FakeEffect final : ILandfillEffect {
    int refs = 0;
    bool enabled = true, running = false;
    int starts = 0, stops = 0;
    std::map<std::uint32_t, bool> flags;
    float matrix[13] = {};

    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetFlag(std::uint32_t flag, bool on) override { flags[flag] = on; }
    void SetEnabled(bool on) override { enabled = on; }
    void SetTransform(const float m[13]) override {
        for (int i = 0; i < 13; ++i) matrix[i] = m[i];
    }
    void Start(int) override {
        running = true;
        ++starts;
    }
    void Stop(bool) override {
        running = false;
        ++stops;
    }
    bool IsRunning() override { return running; }
};

struct FakeZotProp final : ILandfillProp {
    int refs = 1;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetState(int) override {}
};

struct FakeCity final : ILandfillCityApi {
    int refs = 0;
    int cellsX = 64, cellsZ = 64;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    int CellCountX() const override { return cellsX; }
    int CellCountZ() const override { return cellsZ; }
    void CellToWorld(int x, int z, float& wx, float& wz) override {
        wx = static_cast<float>(x) * 16.0f;
        wz = static_cast<float>(z) * 16.0f;
    }
};

struct FakeTerrain final : ILandfillTerrainApi {
    int refs = 0;
    float base = 100.0f, slope = 2.0f, seaLevel = 0.0f;
    std::vector<LandfillRect> refreshed;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    float GetAltitude(float x, float) const override { return base + slope * (x / 16.0f); }
    float GetSeaLevel() const override { return seaLevel; }
    void RefreshRect(const LandfillRect& rect) override { refreshed.push_back(rect); }
};

struct FakePollution final : ILandfillPollutionApi {
    int refs = 0;
    int supply = 0, uncollected = 0, capacity = -1, capacityCalls = 0, landfilled = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    int GetGarbageSupply() override { return supply; }
    int GetTotalGarbageUncollected() const override { return uncollected; }
    void SetGarbageCapacity(int value) override {
        capacity = value;
        ++capacityCalls;
    }
};

struct FakeLine final : ILandfillBudgetLine {
    int refs = 0, setCalls = 0;
    std::int64_t expenses = 0, fullExpenses = -1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::int64_t GetCurrentExpenses() const override { return expenses; }
    void SetFullExpenses(std::int64_t value) override {
        fullExpenses = value;
        ++setCalls;
    }
};

struct FakeDepartment final : ILandfillDepartmentBudget {
    int refs = 0, lineRequests = 0;
    float funding = 0.0f;
    std::uint32_t lastLine = 0;
    FakeLine line;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    const float& GetFundingPercentage() const override { return funding; }
    ILandfillBudgetLine* GetLineItem(std::uint32_t id) override {
        lastLine = id;
        ++lineRequests;
        return &line;
    }
};

struct FakeBudget final : ILandfillBudgetApi {
    int refs = 0, requests = 0;
    std::uint32_t lastDepartment = 0;
    FakeDepartment* department = nullptr;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    ILandfillDepartmentBudget* GetDepartmentBudget(std::uint32_t id) override {
        lastDepartment = id;
        ++requests;
        return department;
    }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    struct Sent {
        std::uint32_t type;
        std::uint64_t data1, data2;
    };
    std::vector<Sent> sent;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back({standard->GetType(), standard->GetData1(), standard->GetData2()});
        return true;
    }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.push_back({target, type});
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.push_back({target, type});
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

// The zone half of the host: every landfill cell is zone type 0xe with no network, no tile occupant and no
// base texture, so PopulateZone takes the empty-cell texture path. The zones come out connected because the
// city reports network id 0 with flag 1 for every developer type.
struct FakeZoneHost final : ILandfillZoneHost {
    int cellsX = 64, cellsZ = 64;
    int tileCapacity = 100;
    std::map<std::pair<int, int>, int> zoneTypes;
    std::vector<std::pair<int, int>> dezoned, emptyTextures, activeOn, activeOff;
    FakePollution* pollution = nullptr;    // the zone host and the pollution simulator are the same object

    int landfilled = 0;
    std::vector<LandfillRect> developmentRects, levelRects;
    FakeEffect generator, attractor, flock;
    FakeZotProp zot;
    bool hasNetwork = false;

    const LandfillTunables& Tunables() override { return LandfillDeveloperTunables(); }
    int CityCellsX() override { return cellsX; }
    int CityCellsZ() override { return cellsZ; }
    void CellToWorld(int x, int z, float& wx, float& wz) override {
        wx = static_cast<float>(x) * 16.0f;
        wz = static_cast<float>(z) * 16.0f;
    }
    bool HasNetworkAt(int, int) override { return hasNetwork; }
    float AltitudeAt(float wx, float) override { return 100.0f + 2.0f * (wx / 16.0f); }
    float TerrainHeightAt(float, float) override { return 100.0f; }
    float AdjustedHeight(float, float) override { return 100.0f; }
    void LevelTerrainRect(const LandfillRect& rect, float) override { levelRects.push_back(rect); }
    void RefreshBuildingDevelopmentRect(const LandfillRect& rect) override { developmentRects.push_back(rect); }
    int ZoneTypeAt(int x, int z) override {
        const auto it = zoneTypes.find({x, z});
        return it == zoneTypes.end() ? 0x0e : it->second;   // the whole city is zoned landfill
    }
    void DezoneCell(int x, int z) override { dezoned.push_back({x, z}); }
    int LandfillTileCapacity() override { return tileCapacity; }
    void AddToGarbageSupply(int amount) override {
        if (pollution) pollution->supply += amount;
    }
    void GarbagePutInLandfill(int amount) override {
        landfilled += amount;
        if (pollution) pollution->landfilled += amount;
    }
    void SetActiveLandfill(int x, int z, bool on) override {
        (on ? activeOn : activeOff).push_back({x, z});
    }
    std::vector<std::uint32_t> NetworkIdsInRegion(const LandfillCellRegion&) override { return {0}; }
    std::vector<int> NetworkFlagsForDeveloperType(std::uint32_t) override { return {1}; }
    ILandfillTile* FindLandfillTile(int, int) override { return nullptr; }
    bool HasBaseTexture(int, int) override { return false; }
    std::vector<ILandfillOccupantRef*> ListBaseTextures(int, int) override { return {}; }
    std::vector<ILandfillOccupantRef*> ListIncompatibleOccupants(int, int) override { return {}; }
    ILandfillTile* LoadLandfillTile(std::uint32_t, int) override { return nullptr; }
    void AddOccupant(ILandfillTile*) override {}
    void RefreshOccupant(ILandfillTile*) override {}
    void PlaceEmptyCellTexture(int x, int z) override { emptyTextures.push_back({x, z}); }
    ILandfillProp* CreateZotProp(const float[3]) override { return &zot; }
    void RemoveProp(ILandfillProp*) override {}
    bool WriteProp(cIGZOStream& stream, ILandfillProp* prop) override {
        stream.SetUint8(prop ? 1 : 0);
        return true;
    }
    ILandfillProp* ReadProp(cIGZIStream& stream) override {
        std::uint8_t present = 0;
        stream.GetUint8(present);
        return present ? &zot : nullptr;
    }
    ILandfillEffect* CreateEffect(const char*, const float[3]) override {
        ++effectCalls;
        return effectCalls == 1 ? &generator : &attractor;
    }
    void RemoveEffect(ILandfillEffect*) override { ++removedEffects; }
    ILandfillEffect* CreateAmbientEffect(const char*) override {
        ++ambientCalls;
        return &flock;
    }
    int Rand() override { return 0; }

    int effectCalls = 0, ambientCalls = 0, removedEffects = 0;
};

//----------------------------------------------------------------------------------------------- fixture

constexpr std::uint32_t kTuningDecay = 0x4a280510;
constexpr std::uint32_t kTuningTolerance = 0x4a280513;
constexpr std::uint32_t kTuningMaxTake = 0x4a677e5b;
constexpr std::uint32_t kTuningTakePerTile = 0x4a677e5c;
constexpr std::uint32_t kTuningDecayPerZone = 0x4a677e5e;
constexpr std::uint32_t kTuningTileCost = 0x4a677e5f;
constexpr std::uint32_t kTuningCostPerHeight = 0x2aa00fe7;
constexpr std::uint32_t kTuningMoveWithTerrain = 0x4a677e5d;

struct Fixture final : ILandfillDeveloperHost {
    FakeMessageServer messages;
    FakeCity city;
    FakeTerrain terrain;
    FakePollution pollution;
    FakeBudget budget;
    FakeDepartment department;
    FakeZoneHost zoneHost;
    cSCPropertyHolder holder;      // the real tuning exemplar

    cISCPropertyHolder* exemplar = nullptr;
    int exemplarCalls = 0;
    struct AgentCall {
        cIGZMessageTarget2* target;
        int interval;
        std::string name;
        std::uint32_t flags;
    };
    std::vector<AgentCall> agents;
    std::vector<cIGZMessageTarget2*> removedAgents;
    std::uint32_t viewLevel = 1;
    std::int32_t tileCount = 0;

    Fixture() {
        budget.department = &department;
        zoneHost.pollution = &pollution;
        exemplar = &holder;
        SetLandfillDeveloperHost(this);
        ResetLandfillDeveloperTunables();
    }
    ~Fixture() override { SetLandfillDeveloperHost(nullptr); }

    // The eight tuning properties the landfill exemplar carries.
    void PublishTunables(float tolerance = 5.0f, float costPerHeight = 10.0f,
                         std::uint32_t maxTake = 240, std::uint32_t takePerTile = 100,
                         std::int64_t tileCost = 100, std::uint32_t decayPerZone = 0,
                         float decayRate = 0.0f, bool moveWithTerrain = false) {
        Float(kTuningDecay, decayRate);
        Float(kTuningTolerance, tolerance);
        Uint(kTuningMaxTake, maxTake);
        Uint(kTuningTakePerTile, takePerTile);
        Uint(kTuningDecayPerZone, decayPerZone);
        Int64(kTuningTileCost, tileCost);
        Float(kTuningCostPerHeight, costPerHeight);
        Bool(kTuningMoveWithTerrain, moveWithTerrain);
    }

    void Float(std::uint32_t id, float value) {
        cRZVariant variant;
        variant.SetValFloat32(value);
        holder.AddProperty(id, &variant, true);
    }
    void Uint(std::uint32_t id, std::uint32_t value) {
        cRZVariant variant;
        variant.SetValUint32(value);
        holder.AddProperty(id, &variant, true);
    }
    void Int64(std::uint32_t id, std::int64_t value) {
        cRZVariant variant;
        variant.SetValSint64(value);
        holder.AddProperty(id, &variant, true);
    }
    void Bool(std::uint32_t id, bool value) {
        cRZVariant variant;
        variant.SetValBool(value);
        holder.AddProperty(id, &variant, true);
    }

    // ILandfillDeveloperHost
    cIGZMessageServer2* MessageServer() override { return &messages; }
    ILandfillCityApi* City() override { return &city; }
    ILandfillTerrainApi* Terrain() override { return &terrain; }
    ILandfillPollutionApi* Pollution() override { return &pollution; }
    ILandfillBudgetApi* Budget() override { return &budget; }
    cISCPropertyHolder* TuningExemplar() override {
        ++exemplarCalls;
        return exemplar;
    }
    ILandfillZoneHost* ZoneHost() override { return &zoneHost; }
    void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name,
                           std::uint32_t flags) override {
        agents.push_back({target, interval, name ? name : "", flags});
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { removedAgents.push_back(target); }
    std::uint32_t ViewLevel() override { return viewLevel; }
    std::int32_t LandfillTileCount() override { return tileCount; }
};

//----------------------------------------------------------------------------------------------- helpers

LandfillCellRegion MakeRegion(int x0, int z0, int x1, int z1) {
    return LandfillCellRegion::Make(x0, z0, x1, z1, true);
}

// The zone manager's message carries its own SC4CellRegion<int> (cSC4ZoneManager::PlaceZone), as in the game.
void SendZoneChange(cSC4LandfillDeveloper* dev, LandfillCellRegion& region, std::uint32_t zoneType) {
    SC4CellRegion<int> changed;
    changed.bounds = SC4Rect<int>{region.bounds.x0, region.bounds.z0, region.bounds.x1, region.bounds.z1};
    changed.rowCount = region.rowCount;
    changed.bitCount = region.bitCount;
    changed.wordCount = region.wordCount;
    changed.rows = region.rows;
    cRZMessage2Standard message;
    message.SetType(kLandfillMessageZoneChange);
    message.SetVoid2(&changed);
    message.SetData3(zoneType);
    CHECK(dev->DoMessage(&message));
}

void SendType(cSC4LandfillDeveloper* dev, std::uint32_t type) {
    cRZMessage2Standard message;
    message.SetType(type);
    CHECK(dev->DoMessage(&message));
}

void SendTerrainChanged(cSC4LandfillDeveloper* dev, int x0, int z0, int x1, int z1, std::uint32_t reason) {
    cRZMessage2Standard message;
    message.SetType(kLandfillMessageTerrainChanged);
    const std::uint64_t data1 = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x0 + 1)) << 16) |
                                static_cast<std::uint32_t>(x1);
    const std::uint64_t data2 = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z0 + 1)) << 16) |
                                static_cast<std::uint32_t>(z1);
    message.SetData1(data1);
    message.SetData2(data2);
    message.SetData4(reason);
    CHECK(dev->DoMessage(&message));
}

std::vector<std::uint8_t> Bytes() {
    return {};
}
void Put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}
void Put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}
void Put8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

// One zone record: version 4, the four bounds, the (exclusive) cell bits, columns/rows, the heaps, the
// connected flag and a "no zot prop" byte.
void PutZone(std::vector<std::uint8_t>& out, int x0, int z0, int x1, int z1, int columns, int rows,
             const std::vector<std::pair<std::uint32_t, std::uint32_t>>& heaps, bool connected) {
    Put16(out, 4);
    Put32(out, static_cast<std::uint32_t>(x0));
    Put32(out, static_cast<std::uint32_t>(z0));
    Put32(out, static_cast<std::uint32_t>(x1));
    Put32(out, static_cast<std::uint32_t>(z1));
    for (int x = x0; x < x1; ++x)
        for (int z = z0; z < z1; ++z) Put8(out, 1);
    Put32(out, static_cast<std::uint32_t>(columns));
    Put32(out, static_cast<std::uint32_t>(rows));
    for (const auto& heap : heaps) {
        Put16(out, 2);
        Put32(out, 0);                    // x
        Put32(out, 0);                    // z
        Put16(out, 1);                    // valid
        Put16(out, static_cast<std::uint16_t>(heap.first != 0 ? 1 : 0));   // hasGarbage
        Put32(out, heap.first);           // garbage
        Put32(out, heap.second);          // capacity
    }
    Put8(out, connected ? 1 : 0);
    Put8(out, 0);                         // ReadProp: no zot prop
}

//----------------------------------------------------------------------------------------------- tests

// GetLandfillTileCount is const; call it through the interface view like the game does.
int GetLandfillTileCountViaInterface(cSC4LandfillDeveloper* dev) {
    return static_cast<const cISC4LandfillDeveloper*>(dev)->GetLandfillTileCount();
}

void TestCom() {
    CHECK(sizeof(cSC4LandfillDeveloper) == cSC4LandfillDeveloper::kClassSize);
    CHECK(cSC4LandfillDeveloper::kClassSize == 0x88);

    auto* dev = new cSC4LandfillDeveloper();
    // base offsets: +0 primary, +8 cIGZMessageTarget2, +0x10 cIGZSerializable
    auto* asTarget = static_cast<cIGZMessageTarget2*>(dev);
    auto* asSerial = static_cast<cIGZSerializable*>(dev);
    CHECK(reinterpret_cast<const char*>(asTarget) - reinterpret_cast<const char*>(dev) == 8);
    CHECK(reinterpret_cast<const char*>(asSerial) - reinterpret_cast<const char*>(dev) == 0x10);
    CHECK(dev->RefCount() == 0);

    void* out = nullptr;
    CHECK(dev->QueryInterface(1, &out));
    CHECK(out == static_cast<cISC4LandfillDeveloper*>(dev));
    CHECK(dev->RefCount() == 1);
    out = nullptr;
    CHECK(dev->QueryInterface(kGZIID_cISC4LandfillDeveloper, &out));       // 0x8a09d502 -> the primary
    CHECK(out == static_cast<cISC4LandfillDeveloper*>(dev));
    out = nullptr;
    CHECK(dev->QueryInterface(kLandfillMessageTargetIid, &out));
    CHECK(out == static_cast<cIGZMessageTarget2*>(dev));
    out = nullptr;
    CHECK(dev->QueryInterface(kLandfillSerializableIid, &out));
    CHECK(out == static_cast<cIGZSerializable*>(dev));
    out = reinterpret_cast<void*>(0x1234);
    CHECK(!dev->QueryInterface(0xdeadbeef, &out));
    CHECK(out == reinterpret_cast<void*>(0x1234));
    CHECK(dev->RefCount() == 4);

    // the thunks reach the same count / the same object
    asTarget->AddRef();
    CHECK(dev->RefCount() == 5);
    out = nullptr;
    CHECK(asTarget->QueryInterface(1, &out));
    CHECK(out == static_cast<cISC4LandfillDeveloper*>(dev));
    CHECK(dev->RefCount() == 6);
    out = nullptr;
    CHECK(asSerial->QueryInterface(kLandfillSerializableIid, &out));
    CHECK(out == static_cast<cIGZSerializable*>(dev));
    CHECK(dev->RefCount() == 7);

    CHECK(dev->GetGZCLSID() == 0xea09d4ae);
    CHECK(dev->GetSimulatorType() == 0x2a398ed6);
    CHECK(dev->GetMaintenanceCosts() == 0);
    CHECK(dev->ZoneCount() == 0);
    CHECK(dev->TotalCapacity() == 0 && dev->StoredGarbage() == 0);
    CHECK(GetLandfillTileCountViaInterface(dev) == 0);

    delete dev;   // not Release: no Shutdown needed yet

    // Release destroys as soon as the count is below 2 (0x100060b1e) and does not decrement it.
    auto* victim = new cSC4LandfillDeveloper();
    victim->AddRef();             // 1
    CHECK(victim->Release() == 0);  // below 2 -> FinalRelease
}

void TestInitShutdown() {
    Fixture fixture;
    fixture.PublishTunables();
    fixture.tileCount = 42;
    auto* dev = new cSC4LandfillDeveloper();
    CHECK(dev->Init());
    CHECK(dev->IsInitialized());
    CHECK(dev->TunablesWereRead());

    // the simulator agent: name, interval 6, flags 2, target = the +8 view
    CHECK(fixture.agents.size() == 1);
    if (fixture.agents.size() == 1) {
        CHECK(fixture.agents[0].target == static_cast<cIGZMessageTarget2*>(dev));
        CHECK(fixture.agents[0].interval == 6);
        CHECK(fixture.agents[0].flags == 2);
        CHECK(fixture.agents[0].name == "cSC4LandfillDeveloper");
    }

    // the five services, each with one reference
    CHECK(dev->MessageServerForTest() == &fixture.messages);
    CHECK(dev->CityForTest() == &fixture.city);
    CHECK(dev->TerrainForTest() == &fixture.terrain);
    CHECK(dev->PollutionForTest() == &fixture.pollution);
    CHECK(dev->BudgetForTest() == &fixture.budget);
    CHECK(dev->DepartmentForTest() == nullptr);
    CHECK(fixture.messages.refs == 1 && fixture.city.refs == 1 && fixture.terrain.refs == 1 &&
          fixture.pollution.refs == 1 && fixture.budget.refs == 1);

    // the five notifications, in the original order
    CHECK(fixture.messages.added.size() == 5);
    if (fixture.messages.added.size() == 5) {
        const std::uint32_t expected[5] = {0x07699455, 0x26d31ec1, 0x0772fad4, 0x278e6003, 0xc6cdbf1a};
        for (int i = 0; i < 5; ++i) {
            CHECK(fixture.messages.added[static_cast<std::size_t>(i)].second == expected[i]);
            CHECK(fixture.messages.added[static_cast<std::size_t>(i)].first ==
                  static_cast<cIGZMessageTarget2*>(dev));
        }
    }

    // the tunables came from the exemplar
    const LandfillTunables& t = LandfillDeveloperTunables();
    CHECK(Near(t.levelToleranceThreshold, 5.0f));
    CHECK(Near(t.costPerCellHeightChange, 10.0f));
    CHECK(t.maxGarbageToTakePerMonth == 240);
    CHECK(t.maxGarbageToTakePerMonthPerTile == 100);
    CHECK(t.defaultLandfillTileCost == 100);
    CHECK(t.maxGarbageToDecayPerMonthPerZone == 0);
    CHECK(!t.moveWithTerrain);
    CHECK(fixture.exemplarCalls == 1);

    // a second Init does nothing
    CHECK(dev->Init());
    CHECK(fixture.agents.size() == 1);
    CHECK(fixture.messages.added.size() == 5);
    CHECK(fixture.exemplarCalls == 1);

    CHECK(dev->GetLandfillTileCount() == 42);
    CHECK(dev->GetSimulatorType() == kSimulatorTypeLandfill);
    CHECK(dev->GetGZCLSID() == kGZCLSID_cSC4LandfillDeveloper);

    // Shutdown: notifications removed, services released, the agent removed through the global city
    CHECK(dev->Shutdown());
    CHECK(!dev->IsInitialized());
    CHECK(fixture.messages.removed.size() == 5);
    if (fixture.messages.removed.size() == 5) {
        const std::uint32_t expected[5] = {0x07699455, 0x26d31ec1, 0x0772fad4, 0x278e6003, 0xc6cdbf1a};
        for (int i = 0; i < 5; ++i) {
            CHECK(fixture.messages.removed[static_cast<std::size_t>(i)].second == expected[i]);
        }
    }
    CHECK(fixture.messages.refs == 0 && fixture.city.refs == 0 && fixture.terrain.refs == 0 &&
          fixture.pollution.refs == 0 && fixture.budget.refs == 0);
    CHECK(fixture.removedAgents.size() == 1);
    CHECK(dev->MessageServerForTest() == nullptr && dev->CityForTest() == nullptr &&
          dev->TerrainForTest() == nullptr && dev->PollutionForTest() == nullptr &&
          dev->BudgetForTest() == nullptr && dev->DepartmentForTest() == nullptr);

    // a second Shutdown is a no-op
    CHECK(dev->Shutdown());
    CHECK(fixture.removedAgents.size() == 1);

    // Init again re-registers everything
    CHECK(dev->Init());
    CHECK(fixture.agents.size() == 2);
    CHECK(fixture.messages.added.size() == 10);
    CHECK(dev->Shutdown());

    // no exemplar: the flag is still set but nothing is overwritten
    fixture.PublishTunables(9.0f);
    ResetLandfillDeveloperTunables();
    fixture.exemplar = nullptr;
    auto* bare = new cSC4LandfillDeveloper();
    CHECK(bare->Init());
    CHECK(bare->TunablesWereRead());
    CHECK(Near(LandfillDeveloperTunables().levelToleranceThreshold, 0.0f));   // untouched
    CHECK(fixture.exemplarCalls == 2);
    CHECK(bare->Shutdown());
    delete bare;

    dev->Release();
}

void TestZoneChange() {
    Fixture fixture;
    fixture.PublishTunables();
    auto* dev = new cSC4LandfillDeveloper();
    CHECK(dev->Init());

    LandfillCellRegion region = MakeRegion(10, 10, 11, 11);
    SendZoneChange(dev, region, kLandfillZoneTypeLandfill);
    CHECK(dev->ZoneCount() == 1);
    if (dev->ZoneCount() != 1) {
        dev->Release();
        return;
    }
    cSC4LandfillZone* zone = dev->Zones().front();
    CHECK(zone->Region() != nullptr);
    if (!zone->Region()) {
        dev->Release();
        return;
    }
    LandfillRect bounds = zone->Region()->bounds;
    CHECK(bounds.x0 == 10 && bounds.z0 == 10 && bounds.x1 == 11 && bounds.z1 == 11);

    // the level height is the mean altitude (100 + 2x): (120 + 120 + 122 + 122) / 4 = 121
    CHECK(Near(zone->LevelHeight(), 121.0f));
    // the empty-cell texture path: four cells, no tile occupant
    CHECK(fixture.zoneHost.emptyTextures.size() == 4);
    // the zone is not connected yet, so the developer's totals stay 0
    CHECK(dev->TotalCapacity() == 0 && dev->StoredGarbage() == 0);
    // one heap for a 2x2 cell region; 100 garbage per cell over 4 cells
    CHECK(zone->TotalCapacity() == 400);
    CHECK(zone->Connected() == false);

    // the flock exists and was started by InitZoneEffects
    CHECK(zone->Seagulls() == &fixture.zoneHost.flock);
    CHECK(fixture.zoneHost.flock.running);

    // GetLandfillZoneFromCell quirk: exclusive upper bounds, only (10,10) answers
    CHECK(dev->GetLandfillZoneFromCell(10, 10) == zone);
    CHECK(dev->GetLandfillZoneFromCell(10, 11) == nullptr);
    CHECK(dev->GetLandfillZoneFromCell(11, 10) == nullptr);
    CHECK(dev->GetLandfillZoneFromCell(11, 11) == nullptr);
    CHECK(dev->GetLandfillZoneFromCell(9, 9) == nullptr);

    // GetZoningCost over the same cells: level 121, |121-120|*2 + |121-122|*2 = 4, * 10 = 40
    CHECK(dev->GetZoningCost(&region) == 40);

    // a single flat cell: the variance is 0, so the zoning cost is 0
    LandfillCellRegion flat = MakeRegion(30, 30, 30, 30);
    CHECK(dev->GetZoningCost(&flat) == 0);
    // a sloped 4x4 region with no zone: variance 6 > 5, level = 143, sum |143-alt| = 32, * 10 = 320
    LandfillCellRegion sloped = MakeRegion(20, 20, 23, 23);
    CHECK(dev->GetZoningCost(&sloped) == 320);

    // a second landfill change that only touches the inflated window merges into the existing zone: the bounds
    // grow to 10..13 but only the two 2x2 blocks stay set (8 cells); the unset cells invalidate their heaps,
    // leaving 2 valid ones, so 100*8/2 = 400 per heap and 800 in total.
    LandfillCellRegion adjacent = MakeRegion(12, 12, 13, 13);
    SendZoneChange(dev, adjacent, kLandfillZoneTypeLandfill);
    CHECK(dev->ZoneCount() == 1);
    bounds = zone->Region()->bounds;
    CHECK(bounds.x0 == 10 && bounds.z0 == 10 && bounds.x1 == 13 && bounds.z1 == 13);
    CHECK(zone->TotalCapacity() == 800);
    CHECK(fixture.zoneHost.developmentRects.size() == 4);      // LevelRect calls for the four added cells
    CHECK(fixture.zoneHost.emptyTextures.size() == 12);        // PopulateZone re-places the 8 set cells
    // the merge ran ShutdownZoneEffects first and then InitZoneEffects again
    CHECK(fixture.zoneHost.flock.stops == 1);
    CHECK(fixture.zoneHost.effectCalls == 4);                  // generator + attractor twice
    CHECK(fixture.zoneHost.removedEffects == 2);
    CHECK(Near(zone->LevelHeight(), 121.0f));                  // LevelRect kept the old height

    // a non-landfill zone change over the original cells dezones them (removeTile = true, no zone manager
    // update: the caller is the one that dezones the city cells) and refreshes the terrain rectangle.
    fixture.zoneHost.dezoned.clear();
    fixture.terrain.refreshed.clear();
    LandfillCellRegion dezone = MakeRegion(10, 10, 11, 11);
    SendZoneChange(dev, dezone, 1 /* residential */);
    CHECK(dev->ZoneCount() == 1);
    CHECK(fixture.zoneHost.dezoned.empty());
    CHECK(fixture.terrain.refreshed.size() == 1);
    if (!fixture.terrain.refreshed.empty()) {
        const LandfillRect& refreshed = fixture.terrain.refreshed[0];
        CHECK(refreshed.x0 == 10 && refreshed.z0 == 10 && refreshed.x1 == 11 && refreshed.z1 == 11);
    }
    // 4 of the 8 cells are left; the dezoned cells invalidated their heaps: 1 valid heap, 100*4 = 400
    CHECK(zone->TotalCapacity() == 400);

    // the same change again: nothing left to dezone, the zone survives
    SendZoneChange(dev, dezone, 1);
    CHECK(dev->ZoneCount() == 1);
    CHECK(zone->TotalCapacity() > 0);

    // dezone everything: the zone is completely dezoned and removed. Quirk: the dezone path of DoZoneChange
    // passes updateZoneManager = false, so the zone manager is never told; RemoveZone finds no cell left, so
    // this developer never calls the zone manager for a dezone (the caller of the zone-change message does).
    LandfillCellRegion all = MakeRegion(10, 10, 13, 13);
    fixture.zoneHost.dezoned.clear();
    SendZoneChange(dev, all, 1);
    CHECK(dev->ZoneCount() == 0);
    CHECK(fixture.zoneHost.dezoned.empty());

    CHECK(dev->Shutdown());
    dev->Release();
}

void TestMonthly() {
    Fixture fixture;
    fixture.PublishTunables();
    auto* dev = new cSC4LandfillDeveloper();
    CHECK(dev->Init());

    LandfillCellRegion region = MakeRegion(10, 10, 11, 11);
    SendZoneChange(dev, region, kLandfillZoneTypeLandfill);
    CHECK(dev->ZoneCount() == 1);
    if (dev->ZoneCount() != 1) {
        dev->Release();
        return;
    }
    CHECK(dev->MaintenanceCost() == 0);      // DoZoneChange only updates the budget on the merge path

    // tick 1: no department yet, so the budget scaling is skipped (maintenance is 0 at this point)
    fixture.pollution.supply = 150;
    fixture.pollution.uncollected = 7;
    fixture.messages.sent.clear();
    SendType(dev, kLandfillMessageMonthlySimulation);
    CHECK(fixture.zoneHost.landfilled == 150);
    CHECK(fixture.pollution.supply == 0);
    CHECK(fixture.pollution.capacity == 250);                 // 400 - 150
    CHECK(fixture.pollution.capacityCalls == 1);
    CHECK(dev->TotalCapacity() == 400);
    CHECK(dev->StoredGarbage() == 150);
    CHECK(dev->MaintenanceCost() == 400);                     // 4 cells * 100
    CHECK(fixture.messages.sent.size() == 2);
    if (fixture.messages.sent.size() == 2) {
        CHECK(fixture.messages.sent[0].type == 0x89efa536);
        CHECK(fixture.messages.sent[0].data1 == 0x2a8c4696);
        CHECK(fixture.messages.sent[0].data2 == 400);
        CHECK(fixture.messages.sent[1].data1 == 0x2a8c4697);
        CHECK(fixture.messages.sent[1].data2 == 157);         // supply 0 + stored 150 + uncollected 7
    }
    // GetEfficiency: no budget department -> budget 0, maintenance 400 -> 0
    CHECK(Near(dev->GetEfficiency(), 0.0f));
    CHECK(dev->GetCurrentBudget() == 0);

    // the city-init message takes the garbage department from the budget simulator
    SendType(dev, kLandfillMessageCityInit);
    CHECK(fixture.budget.requests == 1);
    CHECK(fixture.budget.lastDepartment == 0x6a42a8f5);
    CHECK(dev->DepartmentForTest() == &fixture.department);
    CHECK(fixture.department.refs == 1);
    CHECK(dev->PendingZoneInit() == false);                   // nothing was read from a stream

    // tick 2: budget 800 over maintenance 400 -> no scaling; the heap decays by one first
    fixture.department.line.expenses = 800;
    fixture.pollution.supply = 150;
    fixture.messages.sent.clear();
    SendType(dev, kLandfillMessageMonthlySimulation);
    CHECK(fixture.zoneHost.landfilled == 300);                // +150
    CHECK(fixture.pollution.supply == 0);
    CHECK(fixture.department.lastLine == 30);
    CHECK(fixture.department.lineRequests == 2);              // GetCurrentBudget + UpdateBudgetRequirements
    CHECK(fixture.department.line.fullExpenses == 400);
    CHECK(fixture.department.line.setCalls == 1);
    CHECK(fixture.department.line.refs == 0);                 // AddRef/Release balanced
    CHECK(fixture.pollution.capacity == 101);                 // 400 - 299
    CHECK(dev->TotalCapacity() == 400);
    CHECK(dev->StoredGarbage() == 299);                       // 150 - 1 decay + 150
    CHECK(Near(dev->GetEfficiency(), 2.0f));                  // 800 / 400
    CHECK(dev->GetCurrentBudget() == 800);
    if (fixture.messages.sent.size() == 2) {
        CHECK(fixture.messages.sent[1].data2 == 306);         // supply 0 + stored 299 + 7
    } else {
        CHECK(false);
    }

    // tick 3: budget 200 < maintenance 400 -> the supply is scaled by 1/2 before the zones see it
    fixture.department.line.expenses = 200;
    fixture.pollution.supply = 150;
    fixture.messages.sent.clear();
    SendType(dev, kLandfillMessageMonthlySimulation);
    CHECK(fixture.pollution.supply == 75);                    // 150 - 75
    CHECK(fixture.zoneHost.landfilled == 375);                // +75
    CHECK(dev->StoredGarbage() == 373);                       // 299 - 1 decay + 75
    CHECK(dev->TotalCapacity() == 400);
    CHECK(fixture.pollution.capacity == 27);                  // 400 - 373
    CHECK(dev->MaintenanceCost() == 400);
    if (fixture.messages.sent.size() == 2) {
        CHECK(fixture.messages.sent[0].data1 == 0x2a8c4696 && fixture.messages.sent[0].data2 == 400);
        CHECK(fixture.messages.sent[1].data1 == 0x2a8c4697 && fixture.messages.sent[1].data2 == 455);
    } else {
        CHECK(false);
    }

    std::uint32_t used = 0, capacity = 0;
    dev->GetLandfillUsage(used, capacity);
    CHECK(used == 373 && capacity == 400);

    // the funds-changed message drives the garbage truck generator
    fixture.department.funding = 0.5f;
    SendType(dev, kLandfillMessageFundsChanged);
    cSC4LandfillZone* zone = dev->Zones().front();
    CHECK(zone->TruckGenerator() == &fixture.zoneHost.generator);
    CHECK(fixture.zoneHost.generator.enabled);
    fixture.department.funding = 0.0f;
    SendType(dev, kLandfillMessageFundsChanged);
    CHECK(!fixture.zoneHost.generator.enabled);

    // the view-level message starts and stops the seagull flock (level 1 = above ground)
    fixture.viewLevel = 1;
    SendType(dev, kLandfillMessageViewLevel);
    CHECK(fixture.zoneHost.flock.running);
    const int startsBefore = fixture.zoneHost.flock.starts;
    SendType(dev, kLandfillMessageViewLevel);
    CHECK(fixture.zoneHost.flock.starts == startsBefore);      // already running: no new Start
    fixture.viewLevel = 3;                                     // underground: the flocks stop
    SendType(dev, kLandfillMessageViewLevel);
    CHECK(!fixture.zoneHost.flock.running);
    fixture.viewLevel = 1;
    SendType(dev, kLandfillMessageViewLevel);
    CHECK(fixture.zoneHost.flock.running);

    // Shutdown releases the department and the zones
    CHECK(dev->Shutdown());
    CHECK(fixture.department.refs == 0);
    CHECK(fixture.zoneHost.flock.stops == 2);                 // view-level stop + ShutdownZoneEffects
    dev->Release();
}

void TestTerrainChanged() {
    Fixture fixture;
    fixture.PublishTunables();
    auto* dev = new cSC4LandfillDeveloper();
    CHECK(dev->Init());

    LandfillCellRegion region = MakeRegion(10, 10, 11, 11);
    SendZoneChange(dev, region, kLandfillZoneTypeLandfill);
    CHECK(dev->ZoneCount() == 1);
    if (dev->ZoneCount() != 1) {
        dev->Release();
        return;
    }

    // reason 4 is outside 1..3: the message is ignored
    SendTerrainChanged(dev, 10, 10, 11, 11, 4);
    CHECK(dev->ZoneCount() == 1);
    // reason 1: every cell of the rect is checked, and with no tile occupant the cells are dezoned
    fixture.zoneHost.dezoned.clear();
    SendTerrainChanged(dev, 10, 10, 11, 11, 1);
    CHECK(dev->ZoneCount() == 0);                             // the zone had no cell left
    CHECK(fixture.zoneHost.dezoned.size() == 4);

    // a terrain change with no zones at all is harmless
    SendTerrainChanged(dev, 0, 0, 63, 63, 2);
    CHECK(dev->ZoneCount() == 0);

    // a message with the rect clamped to the city: x1/z1 are clamped, x0/z0 only when zero
    LandfillCellRegion far = MakeRegion(60, 60, 62, 62);
    SendZoneChange(dev, far, kLandfillZoneTypeLandfill);
    CHECK(dev->ZoneCount() == 1);
    fixture.zoneHost.dezoned.clear();
    SendTerrainChanged(dev, 61, 61, 100, 100, 3);             // x1/z1 clamp to 63, x0/z0 stay 61
    CHECK(dev->ZoneCount() == 1);                             // the (60,60) cell is outside the rect
    CHECK(fixture.zoneHost.dezoned.size() == 4);              // (61..62) x (61..62)
    SendTerrainChanged(dev, 59, 59, 63, 63, 2);               // now the last cell goes too
    CHECK(dev->ZoneCount() == 0);

    CHECK(dev->Shutdown());
    dev->Release();
}

void TestStreams() {
    Fixture fixture;
    fixture.PublishTunables();
    auto* dev = new cSC4LandfillDeveloper();
    CHECK(dev->Init());
    LandfillCellRegion region = MakeRegion(10, 10, 11, 11);
    SendZoneChange(dev, region, kLandfillZoneTypeLandfill);
    SendType(dev, kLandfillMessageMonthlySimulation);          // make the zone connected + fill the heaps
    CHECK(dev->ZoneCount() == 1);

    PortMemoryOStream out;
    CHECK(dev->Write(out));
    const std::vector<std::uint8_t>& bytes = out.bytes();
    CHECK(bytes.size() > 6);
    if (bytes.size() >= 6) {
        CHECK(bytes[0] == 3 && bytes[1] == 0);                 // uint16 version 3
        CHECK(bytes[2] == 1 && bytes[3] == 0 && bytes[4] == 0 && bytes[5] == 0);   // int32 count 1
    }

    auto* loaded = new cSC4LandfillDeveloper();
    PortMemoryIStream in(bytes.data(), bytes.size());
    CHECK(loaded->Read(in));
    CHECK(loaded->PendingZoneInit());                          // armed before the version check
    CHECK(loaded->ZoneCount() == 1);
    if (loaded->ZoneCount() == 1) {
        const LandfillRect loadedBounds = loaded->Zones().front()->Region()->bounds;
        CHECK(loadedBounds.x0 == 10 && loadedBounds.z0 == 10 && loadedBounds.x1 == 11 &&
              loadedBounds.z1 == 11);
    }

    // a bad version fails but still arms the zone Init pass
    const std::uint8_t bad[] = {2, 0, 0, 0, 0, 0};
    auto* rejected = new cSC4LandfillDeveloper();
    PortMemoryIStream badIn(bad, sizeof bad);
    CHECK(!rejected->Read(badIn));
    CHECK(rejected->PendingZoneInit());
    CHECK(rejected->ZoneCount() == 0);

    // a failed zone Read leaks the zone but does not stop the loop
    std::vector<std::uint8_t> truncated = Bytes();
    Put16(truncated, 3);
    Put32(truncated, 2);                                       // two zones
    PutZone(truncated, 10, 10, 11, 11, 1, 1, {{0, 100}}, true);
    // the second zone record is missing: its Read fails
    auto* partial = new cSC4LandfillDeveloper();
    PortMemoryIStream partialIn(truncated.data(), truncated.size());
    CHECK(partial->Read(partialIn));
    CHECK(partial->ZoneCount() == 1);

    // crafted stream: the capacity sums saturate at 0xffffffff (two zones of 0xffffffff each)
    std::vector<std::uint8_t> saturated = Bytes();
    Put16(saturated, 3);
    Put32(saturated, 2);
    PutZone(saturated, 10, 10, 11, 11, 1, 1, {{0, 0xffffffffu}}, true);
    PutZone(saturated, 20, 20, 21, 21, 1, 1, {{0, 0xffffffffu}}, true);
    auto* sat = new cSC4LandfillDeveloper();
    PortMemoryIStream satIn(saturated.data(), saturated.size());
    CHECK(sat->Read(satIn));
    CHECK(sat->ZoneCount() == 2);
    SendType(sat, kLandfillMessageCityInit);                   // Init the loaded zone, then budget + capacity
    CHECK(sat->TotalCapacity() == 0xffffffffu);
    CHECK(sat->StoredGarbage() == 0);

    // crafted stream: a heap holding more garbage than its capacity, GetLandfillUsage reports
    // min(garbage, capacity) as the used value
    std::vector<std::uint8_t> overfull = Bytes();
    Put16(overfull, 3);
    Put32(overfull, 1);
    PutZone(overfull, 10, 10, 11, 11, 1, 1, {{50, 10}}, true);
    auto* over = new cSC4LandfillDeveloper();
    PortMemoryIStream overIn(overfull.data(), overfull.size());
    CHECK(over->Read(overIn));
    SendType(over, kLandfillMessageCityInit);
    std::uint32_t used = 0, capacity = 0;
    over->GetLandfillUsage(used, capacity);
    CHECK(capacity == 10);
    CHECK(used == 10);                                         // min(50, 10)
    CHECK(over->StoredGarbage() == 50);

    CHECK(dev->Shutdown());
    dev->Release();
    // A zone only frees its region in Shutdown once it was initialised, which the city-init message does.
    SendType(loaded, kLandfillMessageCityInit);
    SendType(partial, kLandfillMessageCityInit);
    for (cSC4LandfillDeveloper* other : {loaded, rejected, partial, sat, over}) {
        CHECK(other->Init());       // Shutdown only releases the zones once the object is initialised
        CHECK(other->Shutdown());
        other->Release();
    }
}

void TestQueries() {
    Fixture fixture;
    fixture.PublishTunables();
    auto* dev = new cSC4LandfillDeveloper();
    CHECK(dev->Init());
    CHECK(dev->GetMaintenanceCosts() == 0);
    CHECK(Near(dev->GetEfficiency(), 1.0f));                   // no maintenance cost -> 1.0
    CHECK(dev->GetCurrentBudget() == 0);                       // no department yet

    // no zones: every query is inert
    LandfillCellRegion region = MakeRegion(0, 0, 1, 1);
    CHECK(dev->GetZoningCost(&region) == 0);                   // flat? no: sloped, but no zone...
    CHECK(dev->GetZoningCost(nullptr) == 0);
    CHECK(dev->GetLandfillZoneFromCell(0, 0) == nullptr);
    std::uint32_t used = 7, capacity = 7;
    dev->GetLandfillUsage(used, capacity);
    CHECK(used == 0 && capacity == 0);
    CHECK(fixture.messages.sent.empty());

    CHECK(dev->Shutdown());
    dev->Release();
}

}  // namespace

int main() {
    TestCom();
    TestInitShutdown();
    TestZoneChange();
    TestMonthly();
    TestTerrainChanged();
    TestStreams();
    TestQueries();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("cSC4LandfillDeveloper: all checks passed\n");
    return 0;
}
