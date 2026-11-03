// sc4_aurasimulator_test — drives cSC4AuraSimulator through built fakes of every neighbour interface:
// the city (position/cell size/disaster layer), the lot manager and its iterator, the budget, residential, the
// crime / traffic / pollution grids, the ordinance, the land value simulator, the history warehouse, the message
// server, the simulator agents, the exemplar and the game-data registry.
//
// Covered: construction defaults, the three COM views and the shared reference count, Init (the nine grids with
// their instance ids / tract sizes / initial values, five subscriptions, four agents, the three game-data entries),
// ReadSimulatorTunableValues (curves, smoothing/decay, base auras, transient pairs, hotspot settings, riot curve),
// the transient blob tool (preset and explicit), the whole DoIdle state machine (park map, the landmark map walk,
// the lot pass, the curve folds, the smoothing pass, the aura publish, the hotspots, the transient decay and the
// cycle message), UpdateAuraGrid (aura = smoothed + transient, 0 for "no data", best/worst tract, population
// weighted rating, history), ComputeHotspotList (merge + prune), GetLotBaseAura (population weighted current aura +
// land value curve), ComputeTaxRateChangeEffect, DoOccupantChanged (park / landmark / vehicle properties, add and
// remove, the funded landmark key), CheckForStartingRiot, BustStrike, DoMessage routing, Write/Read and the two
// game-data command handlers.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sc4/AuraSimulator.h"
#include "sc4/SimGridOps.h"

#include "gz/CommandParameterSet.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) Check((a) == (b), #a " == " #b, __LINE__)

using namespace sc4;

//--------------------------------------------------------------------------------------------------
// the grid city (cSC4SimGrid's city geometry)
//--------------------------------------------------------------------------------------------------
class FakeSimGridCity final : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 32; }
    std::int32_t CitySizeZInCells() const override { return 32; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

//--------------------------------------------------------------------------------------------------
// city
//--------------------------------------------------------------------------------------------------
class FakeDisaster final : public IAuraDisasterApi {
public:
    int calls = 0;
    int cellX = -1, cellZ = -1;
    void StartAtCell(int x, int z) override {
        ++calls;
        cellX = x;
        cellZ = z;
    }
};

class FakeCity final : public IAuraCityApi {
public:
    FakeDisaster disaster;
    int removeTargetCalls = 0;
    int disasterLookups = 0;
    std::uint32_t lastDisasterType = 0;
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x / 16.0f);
        cellZ = static_cast<int>(z / 16.0f);
    }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
    IAuraDisasterApi* Disaster(std::uint32_t type) override {
        ++disasterLookups;
        lastDisasterType = type;
        return &disaster;
    }
    void RemoveMessageTarget(cIGZMessageTarget2*) override { ++removeTargetCalls; }
};

//--------------------------------------------------------------------------------------------------
// lots
//--------------------------------------------------------------------------------------------------
class FakeOccupant final : public cSC4Occupant {
public:
    float position[3] = {0, 0, 0};
    explicit FakeOccupant(std::uint32_t type = 0x2781284f) { type_ = type; }
    void GetPosition(float* xyz) override {
        xyz[0] = position[0];
        xyz[1] = position[1];
        xyz[2] = position[2];
    }
};

class FakeLotIterator final : public IAuraLotIterator {
public:
    const std::vector<cSC4Lot*>* source = nullptr;   // the manager's live lot list
    std::vector<cSC4Lot*> lots;
    std::size_t index = 0;
    bool AtEnd() override { return index >= lots.size(); }
    void Advance() override { ++index; }
    void Restart() override {   // the original rewinds to end->next, i.e. re-reads the circular list
        if (source) lots = *source;
        index = 0;
    }
    cSC4Lot* Lot() override { return index < lots.size() ? lots[index] : nullptr; }
    void Release() override { delete this; }
};

class FakeLotManager final : public IAuraLotManagerApi {
public:
    FakeLotIterator iterator;
    int lotCount = 0;
    cSC4Lot* LotAtCell(int cellX, int cellZ, int) override {
        for (cSC4Lot* lot : iterator.lots) {
            std::int32_t rect[4] = {0, 0, 0, 0};
            lot->GetBoundingRect(rect);
            if (cellX >= rect[0] && cellX <= rect[2] && cellZ >= rect[1] && cellZ <= rect[3]) return lot;
        }
        return nullptr;
    }
    IAuraLotIterator* LotIterator() override {   // the original hands out a heap iterator (released by Shutdown)
        auto* copy = new FakeLotIterator();
        copy->source = &iterator.lots;
        copy->lots = iterator.lots;
        return copy;
    }
    int LotCount() override { return lotCount; }
};

// The lot host cSC4Lot needs (only FindBuildingOccupant and the capacity profile matter here).
class FakeLotHost final : public ILotHost {
public:
    LotBuildingProfile profile;
    cIGZUnknown* building = nullptr;
    int roadCalls = 0;
    std::uint8_t ZoneTypeAt(int, int) override { return 0; }
    std::int32_t SimulatorDay() override { return 1; }
    float TerrainAltitude(float, float) override { return 250.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t type) override { return type ? &profile : nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override {
        return purpose * 0x100 + static_cast<std::uint32_t>(wealth);
    }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int, int) override { return 0; }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t) override { return nullptr; }
    void PostLotMessage(std::uint32_t, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::int32_t) override {}
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 1.0f; }
    float TravelVolumeFactor() override { return 1.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = out[1] = out[2] = 0.0f; }
    void JobsPerOccupant(float out[4], int) override { out[0] = out[1] = out[2] = out[3] = 0.0f; }
    void ExemplarJobs(const cSC4Lot&, float*) override {}
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override {
        available = false;
        return false;
    }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    bool RoadAccessCells(const cSC4Lot&, int) override {
        ++roadCalls;
        return true;
    }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return building; }
    void WriteSerializable(cIGZOStream& s, cIGZUnknown* o) override { s.SetUint8(o ? 1 : 0); }
    cIGZUnknown* ReadSerializable(cIGZIStream& s, std::uint32_t) override {
        std::uint8_t b = 0;
        s.GetUint8(b);
        return nullptr;
    }
};

//--------------------------------------------------------------------------------------------------
// budget / residential / grids / misc services
//--------------------------------------------------------------------------------------------------
class FakeBudget final : public IAuraBudgetApi {
public:
    std::map<std::uint32_t, float> funding;
    float taxRate[12] = {};
    bool funded = false;
    int fundingCalls = 0;
    std::uint32_t lastFundingId = 0;
    float DepartmentFunding(std::uint32_t departmentId) override {
        ++fundingCalls;
        lastFundingId = departmentId;
        const auto it = funding.find(departmentId);
        return it == funding.end() ? 1.0f : it->second;
    }
    bool IsDepartmentFunded(cISCPropertyHolder*, std::uint32_t) override { return funded; }
    float TaxRate(std::uint32_t economyType) override {
        return economyType < 12 ? taxRate[economyType] : 0.0f;
    }
};

class FakeResidential final : public IAuraResidentialApi {
public:
    std::unique_ptr<cSC4SimGrid<float>> map138, map140;
    std::unique_ptr<cSC4SimGrid<std::uint16_t>> wealth[3];
    int schoolStrikeEnds = 0, healthStrikeEnds = 0;
    FakeResidential() {
        map138 = MakeFloatGrid();
        map140 = MakeFloatGrid();
        for (auto& grid : wealth) {
            grid = std::make_unique<cSC4SimGrid<std::uint16_t>>();
            grid->SetTractSize(4);
            grid->AddRef();   // the simulator owns one reference; PopulationGrids hands out another
        }
    }
    static std::unique_ptr<cSC4SimGrid<float>> MakeFloatGrid() {
        auto grid = std::make_unique<cSC4SimGrid<float>>();
        grid->SetTractSize(4);
        return grid;
    }
    cSC4SimGrid<float>* EQCandidateMap() override { return map138.get(); }
    cSC4SimGrid<float>* HQCandidateMap() override { return map140.get(); }
    void PopulationGrids(cSC4SimGrid<std::uint16_t>*& a, cSC4SimGrid<std::uint16_t>*& b,
                         cSC4SimGrid<std::uint16_t>*& c) override {
        a = wealth[0].get();
        b = wealth[1].get();
        c = wealth[2].get();
        a->AddRef();
        b->AddRef();
        c->AddRef();
    }
    void EndSchoolStrike() override { ++schoolStrikeEnds; }
    void EndHealthStrike() override { ++healthStrikeEnds; }
};

class FakeGridApi final : public IAuraGridApi {
public:
    std::unique_ptr<cSC4SimGrid<std::uint8_t>> map;
    FakeGridApi() {
        map = std::make_unique<cSC4SimGrid<std::uint8_t>>();
        map->SetTractSize(4);
    }
    cSC4SimGrid<std::uint8_t>* Uint8Map() override { return map.get(); }
};

class FakePollution final : public IAuraPollutionApi {
public:
    std::unique_ptr<cSC4SimGrid<std::int16_t>> maps[4];
    FakePollution() {
        for (auto& grid : maps) {
            grid = std::make_unique<cSC4SimGrid<std::int16_t>>();
            grid->SetTractSize(4);
        }
    }
    cSC4SimGrid<std::int16_t>* TypeMap(int type) override {
        return type >= 0 && type < 4 ? maps[type].get() : nullptr;
    }
};

class FakeOrdinance final : public IAuraOrdinanceApi {
public:
    int value = 0;
    int calls = 0;
    int OrdinanceValue(std::uint32_t, int) override {
        ++calls;
        return value;
    }
};

class FakeLandValue final : public IAuraLandValueApi {
public:
    int value = 0;
    int LandValueAtCell(int, int) override { return value; }
};

class FakeHistory final : public IAuraHistoryApi {
public:
    std::vector<std::pair<std::uint32_t, std::int64_t>> entries;
    void AddHistory(std::uint32_t id, std::int64_t value) override { entries.emplace_back(id, value); }
};

class FakeStrike final : public IAuraStrikeApi {
public:
    int ends = 0;
    void EndStrike() override { ++ends; }
};

//--------------------------------------------------------------------------------------------------
// message server
//--------------------------------------------------------------------------------------------------
class FakeMessageServer final : public cIGZMessageServer2 {
public:
    std::vector<std::uint32_t> added, removed;
    std::vector<std::uint32_t> posted;
    std::uint32_t refCount = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refCount; }
    std::uint32_t Release() override { return refCount ? --refCount : 0; }
    bool MessageSend(cIGZMessage2* message) override {
        if (message) posted.push_back(message->GetType());
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        if (message) posted.push_back(message->GetType());
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        added.push_back(messageType);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        removed.push_back(messageType);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return refCount; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

//--------------------------------------------------------------------------------------------------
// game-data registry (only the entry bookkeeping the aura simulator uses)
//--------------------------------------------------------------------------------------------------
class FakeRegistry final : public cISC4GameDataRegistry {
public:
    struct Entry {
        void* handle = nullptr;
        GameDataCommandFn command = nullptr;
        void* context = nullptr;
        std::string name;
    };
    std::map<EntryId, Entry> entries;
    EntryId nextId = 0x1000;
    std::vector<std::string> addedNodes;
    std::vector<std::string> addedCommands;
    int removed = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }

    EntryId AddChildEntry(cIGZUnknown* node, const char* name, EntryId, int, void* context) override {
        Entry entry;
        entry.handle = reinterpret_cast<void*>(node);
        entry.context = context;
        entry.name = name ? name : "";
        addedNodes.push_back(entry.name);
        const EntryId id = nextId++;
        entries[id] = entry;
        return id;
    }
    EntryId AddChildEntry(void* handle, const char* name, GameDataValueFn, EntryId, int, void*) override {
        Entry entry;
        entry.handle = handle;
        entry.name = name ? name : "";
        const EntryId id = nextId++;
        entries[id] = entry;
        return id;
    }
    EntryId AddChildEntry(void* handle, const char* name, GameDataCommandFn fn, EntryId, int, void* context) override {
        Entry entry;
        entry.handle = handle;
        entry.command = fn;
        entry.context = context;
        entry.name = name ? name : "";
        addedCommands.push_back(entry.name);
        const EntryId id = nextId++;
        entries[id] = entry;
        return id;
    }
    bool RemoveChildEntry(cIGZUnknown* node) override {
        ++removed;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.handle == reinterpret_cast<void*>(node)) {
                entries.erase(it);
                return true;
            }
        }
        return false;
    }
    bool RemoveChildEntry(void* handle) override {
        ++removed;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.handle == handle) {
                entries.erase(it);
                return true;
            }
        }
        return false;
    }
    EntryId GetEntryID(cIGZUnknown* node) const override {
        for (const auto& entry : entries) {
            if (entry.second.handle == reinterpret_cast<void*>(node)) return entry.first;
        }
        return 0;
    }
    EntryId GetEntryID(void* handle) const override {
        for (const auto& entry : entries) {
            if (entry.second.handle == handle) return entry.first;
        }
        return 0;
    }
    cIGZUnknown* GetNode(EntryId) const override { return nullptr; }
    void* GetValue(EntryId) const override { return nullptr; }
    bool IsValidEntry(EntryId entry) const override { return entries.count(entry) != 0; }
    bool IsNodeEntry(EntryId) const override { return true; }
    const cRZString* GetEntryName(EntryId) const override { return nullptr; }
    bool DoValueExchange(EntryId, cIGZVariant&, bool) override { return false; }
    bool DoValueCall(EntryId entry, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out) override {
        const auto it = entries.find(entry);
        if (it == entries.end() || !it->second.command) return false;
        return it->second.command(it->second.handle, in, out, this);
    }
    bool SetEntryContext(EntryId entry, void* context) override {
        const auto it = entries.find(entry);
        if (it == entries.end()) return false;
        it->second.context = context;
        return true;
    }
    void* GetEntryContext(EntryId entry) const override {
        const auto it = entries.find(entry);
        return it == entries.end() ? nullptr : it->second.context;
    }
    bool CheckEntryFlags(EntryId, int) const override { return false; }
    EntryId GetParentNodeEntry(EntryId) const override { return 0; }
    EntryId GetRootNodeEntry() const override { return 0x1; }
    bool VisitBreadthFirst(EntryId, EntryVisitFn, void*) override { return false; }
    bool VisitImmediateChildren(EntryId, EntryVisitFn, void*) override { return false; }
    bool AddNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return true; }
    bool AddNotificationTarget(cIGZMessageTarget2*) override { return true; }
    bool RemoveNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return true; }
    bool RemoveNotificationTarget(cIGZMessageTarget2*) override { return true; }
    void DebugDumpRecursive(EntryId) override {}
};

//--------------------------------------------------------------------------------------------------
// the host
//--------------------------------------------------------------------------------------------------
class FakeHost final : public IAuraHost {
public:
    FakeMessageServer messageServer;
    FakeCity city;
    FakeLotManager lotManager;
    FakeBudget budget;
    FakeResidential residential;
    FakeOrdinance ordinance;
    FakeLandValue landValue;
    FakeHistory history;
    FakeStrike police, fire;
    FakeGridApi crime, trafficDensity, trafficCongestion;
    FakePollution pollution;
    FakeRegistry registry;
    IAuraService buildingDevelopment;

    bool hasSimulator = true;
    std::int32_t simDate = 100;
    int dateCalls = 0;
    std::vector<std::pair<int, std::string>> agents;
    cISCPropertyHolder* exemplar = nullptr;
    cISC4Occupant* building = nullptr;
    std::vector<std::uint64_t> slices;
    std::size_t expiredCount = 0;
    bool sliceAlwaysExpired = false;
    int gridWrites = 0;
    std::vector<int> readGridKinds;   // 0 int8, 1 int16

    cIGZMessageServer2* MessageServer() override { return &messageServer; }
    IAuraCityApi* City() override { return &city; }
    IAuraLotManagerApi* LotManager() override { return &lotManager; }
    IAuraBudgetApi* Budget() override { return &budget; }
    IAuraResidentialApi* Residential() override { return &residential; }
    IAuraGridApi* Crime() override { return &crime; }
    IAuraGridApi* TrafficDensity() override { return &trafficDensity; }
    IAuraGridApi* TrafficCongestion() override { return &trafficCongestion; }
    IAuraPollutionApi* Pollution() override { return &pollution; }
    IAuraOrdinanceApi* Ordinance() override { return &ordinance; }
    IAuraLandValueApi* LandValue() override { return &landValue; }
    IAuraHistoryApi* History() override { return &history; }
    IAuraStrikeApi* PoliceStrike() override { return &police; }
    IAuraStrikeApi* FireStrike() override { return &fire; }
    IAuraService* BuildingDevelopment() override { return &buildingDevelopment; }
    IAuraService* Simulator() override { return hasSimulator ? &buildingDevelopment : nullptr; }
    std::int32_t SimDateNumber() override { return simDate; }
    void AddSimulatorAgent(cIGZMessageTarget2*, int interval, const char* name, std::uint32_t) override {
        agents.emplace_back(interval, name ? name : "");
    }
    bool SimulatorPresent() override { return hasSimulator; }
    bool GetTuningProperties(cISCPropertyHolder** out) override {
        *out = exemplar;
        return exemplar != nullptr;
    }
    cISC4GameDataRegistry* GameDataRegistry() override { return &registry; }
    cISC4Occupant* BuildingOccupant(cSC4Lot* lot) override {
        (void)lot;
        return building;
    }
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override {
        slices.push_back(milliseconds);
        return slices.size();
    }
    bool SliceExpired(std::uint64_t token) override {
        if (!sliceAlwaysExpired) return false;
        ++expiredCount;
        return token != 0;
    }
    void WriteGrid(cIGZOStream& stream, cIGZSerializable* grid) override {
        ++gridWrites;
        if (grid) {
            grid->Write(stream);
        } else {
            const std::uint16_t zero = 0;
            stream.SetUint16(zero);
        }
    }
    cIGZSerializable* ReadGrid(cIGZIStream& stream) override {
        const int kind = readGridKinds.empty() ? 0 : readGridKinds.front();
        if (!readGridKinds.empty()) readGridKinds.erase(readGridKinds.begin());
        cIGZSerializable* grid = nullptr;
        if (kind == 1) {
            auto* typed = new cSC4SimGrid<std::int16_t>();
            typed->AddRef();
            grid = typed;
        } else {
            auto* typed = new cSC4SimGrid<std::int8_t>();
            typed->AddRef();
            grid = typed;
        }
        grid->Read(stream);
        return grid;
    }
};

//--------------------------------------------------------------------------------------------------
// helpers
//--------------------------------------------------------------------------------------------------
void AddFloatProperty(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<float> values) {
    std::vector<float> data(values);
    cRZVariant variant;
    variant.RefFloat32(data.data(), static_cast<std::uint32_t>(data.size()));
    holder.AddProperty(id, &variant, false);
}

void AddSint32Property(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<std::int32_t> values) {
    std::vector<std::int32_t> data(values);
    cRZVariant variant;
    variant.RefSint32(data.data(), static_cast<std::uint32_t>(data.size()));
    holder.AddProperty(id, &variant, false);
}

void AddUint32Property(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<std::uint32_t> values) {
    std::vector<std::uint32_t> data(values);
    cRZVariant variant;
    variant.RefUint32(data.data(), static_cast<std::uint32_t>(data.size()));
    holder.AddProperty(id, &variant, false);
}

void AddFloatScalar(cSCPropertyHolder& holder, std::uint32_t id, float value) {
    cRZVariant variant(value);
    holder.AddProperty(id, &variant, false);
}

void AddIntScalar(cSCPropertyHolder& holder, std::uint32_t id, std::int32_t value) {
    cRZVariant variant;
    variant.SetValSint32(value);
    holder.AddProperty(id, &variant, false);
}

// A response curve property with two points: (0, 0) and (in, out).
void AddCurveProperty(cSCPropertyHolder& holder, std::uint32_t id, float in, float out) {
    AddFloatProperty(holder, id, {0.0f, 0.0f, in, out});
}

}  // namespace

int main() {
    static_assert(cSC4AuraSimulator::kClassSize == 0x208, "class size");
    static_assert(sizeof(tHotspot) == 0xc, "hotspot size");
    static_assert(cSC4AuraSimulator::kOffsetAuraGrid == 0x58, "layout");
    static_assert(cSC4AuraSimulator::kOffsetHotspotList == 0x130, "layout");
    static_assert(cSC4AuraSimulator::kOffsetMaxRating == 0x1f0, "layout");

    FakeSimGridCity gridCity;
    SetSimGridCity(&gridCity);
    FakeHost host;
    FakeLotHost lotHost;
    SetLotHost(&lotHost);
    SetAuraHost(&host);

    // An exemplar covering every tunable the class reads.
    cSCPropertyHolder exemplar;   // not reference counted by the class (the host only lends it)
    AddCurveProperty(exemplar, kAuraPropertyParkCurve, 10.0f, 10.0f);
    AddCurveProperty(exemplar, kAuraPropertyPollutionCurve0, 10.0f, -2.0f);
    AddCurveProperty(exemplar, kAuraPropertyPollutionCurve0 + 1, 10.0f, -3.0f);
    AddCurveProperty(exemplar, kAuraPropertyResidentialCurve138, 10.0f, 4.0f);
    AddCurveProperty(exemplar, kAuraPropertyResidentialCurve140, 10.0f, 5.0f);
    AddCurveProperty(exemplar, kAuraPropertyCrimeCurve, 10.0f, -6.0f);
    AddCurveProperty(exemplar, kAuraPropertyTrafficTripCurve, 10.0f, 7.0f);
    AddCurveProperty(exemplar, kAuraPropertyTrafficCurve, 10.0f, 8.0f);
    AddCurveProperty(exemplar, kAuraPropertyTaxChangeCurve, 3.0f, 9.0f);
    AddCurveProperty(exemplar, kAuraPropertyLandValueCurve, 100.0f, 50.0f);
    AddCurveProperty(exemplar, kAuraPropertyRiotCurve, 10.0f, 100.0f);
    AddFloatScalar(exemplar, kAuraPropertySmoothing, 0.5f);
    AddFloatScalar(exemplar, kAuraPropertyTransientDecay, 0.25f);
    AddIntScalar(exemplar, kAuraPropertyBaseAuraResidential, 5);
    AddIntScalar(exemplar, kAuraPropertyBaseAuraCommercial, 6);
    AddIntScalar(exemplar, kAuraPropertyBaseAuraIndustrial, 7);
    AddFloatProperty(exemplar, kAuraPropertyTransientEffects + 0, {0.0f, 0.0f});
    AddFloatProperty(exemplar, kAuraPropertyTransientEffects + 1, {32.0f, 9.0f});
    AddFloatProperty(exemplar, kAuraPropertyTransientEffects + 2, {0.0f, 0.0f});
    AddFloatProperty(exemplar, kAuraPropertyTransientEffects + 3, {0.0f, 0.0f});
    AddUint32Property(exemplar, kAuraPropertyHotspotSettings, {100u, 4u, 2u});
    host.exemplar = &exemplar;

    // ---- construction defaults -------------------------------------------------------------------
    auto* sim = new cSC4AuraSimulator();
    CHECK_EQ(sim->RefCount(), 0u);
    CHECK(!sim->IsInitialized());
    CHECK_EQ(sim->TractSize(), 4);
    CHECK(!sim->GetAuraGrid());
    CHECK_EQ(sim->IdleState(), 0);
    CHECK_EQ(sim->CurrentLandmarkKey(), 0xffffffffu);
    CHECK_EQ(sim->MayorRating(), 0);
    CHECK_EQ(sim->AuraSmoothing(), 0.6f);
    CHECK_EQ(sim->TransientDecay(), 0.4f);
    CHECK_EQ(sim->HotspotMergeDistance(), 8);
    CHECK_EQ(sim->HotspotValueThreshold(), 196);
    CHECK_EQ(sim->HotspotMinCount(), 16);
    CHECK_EQ(sim->LastTaxRate(0), -1.0f);
    CHECK_EQ(AuraTypeTable()[0].type, 0x1010u);
    CHECK_EQ(AuraTypeTable()[1].type, 0x1020u);
    CHECK_EQ(AuraTypeTable()[2].type, 0x1030u);
    CHECK_EQ(AuraTypeTable()[3].type, 0u);

    // ---- the three COM views and the shared count -------------------------------------------------
    sim->AddRef();                                     // count 1
    void* view = nullptr;
    CHECK(sim->QueryInterface(kGZIID_cISC4AuraSimulator, &view));
    CHECK(view == static_cast<cISC4AuraSimulator*>(sim));
    CHECK_EQ(sim->RefCount(), 2u);
    sim->Release();
    view = nullptr;
    CHECK(sim->QueryInterface(1, &view));
    CHECK(view == static_cast<cISC4AuraSimulator*>(sim));
    sim->Release();
    view = nullptr;
    CHECK(sim->QueryInterface(kGZIID_cIGZMessageTarget2, &view));
    CHECK(view == static_cast<void*>(static_cast<cIGZMessageTarget2*>(sim)));
    sim->Release();
    view = nullptr;
    CHECK(sim->QueryInterface(kGZIID_cISC4AuraSimulatorSerializable, &view));
    CHECK(view == static_cast<void*>(static_cast<cIGZSerializable*>(sim)));
    sim->Release();
    view = nullptr;
    CHECK(!sim->QueryInterface(0x12345678u, &view));
    CHECK_EQ(sim->RefCount(), 1u);

    // ---- Init ------------------------------------------------------------------------------------
    CHECK(sim->Init());
    CHECK(sim->IsInitialized());
    CHECK_EQ(sim->IdleState(), 1);
    CHECK_EQ(sim->TractSize(), 4);
    CHECK_EQ(sim->GetAuraGrid()->GetInstanceID(), kAuraGridInstance);
    CHECK_EQ(sim->GetAuraGrid()->GetTractSize(), 4);
    CHECK_EQ(sim->GetAuraGrid()->GetTractCountX(), 8);
    CHECK_EQ(sim->GetTransientAuraGrid()->GetInstanceID(), kAuraTransientGridInstance);
    CHECK_EQ(sim->GetTransientAuraGrid()->GetInstanceID(), kAuraTransientGridInstance);
    CHECK_EQ(sim->CellWorkGrid()->GetInstanceID(), kAuraCellGridInstance);
    CHECK_EQ(sim->CellWorkGrid()->GetTractSize(), 1);
    CHECK_EQ(sim->CellWorkGrid()->GetTractCountX(), 32);
    CHECK_EQ(sim->SmoothedGrid()->GetInstanceID(), kAuraSmoothedGridInstance);
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), -0x80);
    CHECK_EQ(sim->VehicleGrid()->GetInstanceID(), kAuraVehicleGridInstance);
    CHECK_EQ(sim->ParkEffectGrid()->GetInstanceID(), kAuraParkEffectGridInstance);
    CHECK_EQ(sim->GetParkMap()->GetInstanceID(), kAuraParkGridInstance);
    CHECK_EQ(sim->GetLandmarkMap()->GetInstanceID(), kAuraLandmarkGridInstance);
    CHECK_EQ(sim->LandmarkWorkGrid()->GetInstanceID(), kAuraLandmarkGridInstance);
    CHECK_EQ(host.messageServer.added.size(), 5u);
    CHECK_EQ(host.messageServer.added[0], kMessageOccupantInsertedAura);
    CHECK_EQ(host.messageServer.added[1], kMessageOccupantRemovedAura);
    CHECK_EQ(host.messageServer.added[2], kAuraMessageOrdinanceChanged);
    CHECK_EQ(host.messageServer.added[3], kAuraMessageReloadTunables);
    CHECK_EQ(host.messageServer.added[4], kAuraMessageLotsChanged);
    CHECK_EQ(host.agents.size(), 4u);
    CHECK_EQ(host.agents[0].first, 0);
    CHECK_EQ(host.agents[1].first, 10);
    CHECK_EQ(host.agents[2].first, 4);
    CHECK_EQ(host.agents[3].first, 6);
    CHECK_EQ(host.agents[0].second, std::string("cSC4AuraSimulator"));
    CHECK_EQ(host.registry.addedNodes.size(), 1u);
    CHECK_EQ(host.registry.addedNodes[0], std::string("aura"));
    CHECK_EQ(host.registry.addedCommands.size(), 2u);
    CHECK_EQ(host.registry.addedCommands[0], std::string("add_transient_effect"));
    CHECK_EQ(host.registry.addedCommands[1], std::string("end_strike"));
    CHECK(!sim->Init() || sim->IsInitialized());   // a second Init is a no-op

    // ---- tunables --------------------------------------------------------------------------------
    CHECK_EQ(sim->AuraSmoothing(), 0.5f);
    CHECK_EQ(sim->TransientDecay(), 0.25f);
    CHECK(sim->ParkCurve() != nullptr);
    CHECK_EQ(sim->ParkCurve()->GetOutputValue(10.0f), 10.0f);
    CHECK_EQ(sim->RiotCurve()->GetOutputValue(10.0f), 100.0f);
    CHECK_EQ(sim->PollutionCurves().size(), 4u);
    CHECK(sim->PollutionCurves()[0] != nullptr);
    CHECK(sim->PollutionCurves()[1] != nullptr);
    CHECK(!sim->PollutionCurves()[2]);
    CHECK_EQ(sim->TransientRadii()[1], 32.0f);
    CHECK_EQ(sim->TransientMagnitudes()[1], 9.0f);
    CHECK_EQ(sim->TransientRadii()[0], 0.0f);
    CHECK_EQ(sim->HotspotValueThreshold(), 100);
    CHECK_EQ(sim->HotspotMergeDistance(), 4);
    CHECK_EQ(sim->HotspotMinCount(), 2);
    CHECK_EQ(AuraTypeTable()[0].baseAura, 5);
    CHECK_EQ(AuraTypeTable()[1].baseAura, 6);
    CHECK_EQ(AuraTypeTable()[2].baseAura, 7);

    // ---- transient blobs -------------------------------------------------------------------------
    host.sliceAlwaysExpired = false;
    sim->AddTransientEffect(32.0f, 32.0f, kTransientAuraEffect1);   // radius 32 m / tract 64 m -> centre only
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(0, 0), 9);
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(1, 0), 0);
    sim->AddTransientEffect(32.0f, 32.0f, kTransientAuraEffect0);   // magnitude 0 -> not added
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(0, 0), 9);
    sim->AddTransientEffect(96.0f, 96.0f, 64.0f, 20.0f);           // radius 1 tract at (1,1)
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(1, 1), 20);
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(1, 0), 0);
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(2, 1), 0);
    sim->GetTransientAuraGrid()->SetTractValues(0);

    // ---- DoIdle stage 1..4 -----------------------------------------------------------------------
    sim->SetIdleState(1);
    host.simDate = 250;
    sim->DoIdle(7);
    CHECK_EQ(sim->IdleState(), 2);
    CHECK_EQ(host.slices.size(), 0u);   // only the lot pass starts a slice
    sim->ParkEffectGrid()->SetTractValue(0, 0, 10);
    host.budget.funding[kAuraParkDepartment] = 0.5f;
    sim->DoIdle(7);
    CHECK_EQ(sim->IdleState(), 3);
    CHECK_EQ(sim->GetParkMap()->GetTractValue(0, 0), 5);   // 10 * 0.5 assigned
    CHECK_EQ(host.budget.lastFundingId, kAuraParkDepartment);
    host.lotManager.lotCount = 0;
    sim->DoIdle(7);
    CHECK_EQ(sim->IdleState(), 4);   // no landmark maps
    sim->DoIdle(7);
    CHECK_EQ(sim->IdleState(), 0);   // no lots
    CHECK_EQ(sim->CellWorkGrid()->GetTractValue(0, 0), -0x80);

    // ---- GetLotBaseAura --------------------------------------------------------------------------
    FakeOccupant building(0x2781284f);
    building.AddRef();
    lotHost.building = static_cast<cISC4Occupant*>(&building);
    lotHost.profile.capacities.resize(1);
    lotHost.profile.allowPartialCapacity = true;
    lotHost.profile.capacities[0][0x1010] = 100;
    lotHost.profile.capacities[0][0x1030] = 100;

    cSC4Lot lot(0, 0, 4, 4, 0);
    lot.SetBuildingType(0x1010);
    lot.SetState(kLotStateLivable, 10, 0);
    CHECK(lot.SetPopulation(0x1010, 10));
    CHECK(lot.SetPopulation(0x1030, 30));
    // The table's current auras are reset to the base auras by ComputeTaxRateChangeEffect; set them by hand.
    AuraTypeTable()[0].currentAura = 5;
    AuraTypeTable()[1].currentAura = 6;
    AuraTypeTable()[2].currentAura = 7;
    CHECK_EQ(static_cast<int>(sim->GetLotBaseAura(&lot)), 6);   // (5*10 + 7*30) / 40 = 6.5 -> 6
    CHECK_EQ(static_cast<int>(sim->GetLotBaseAura(nullptr)), -0x80);

    // With the land value curve: land value 100 - occupant wealth 10 = 90 -> curve(90) = 45.
    host.landValue.value = 100;
    CHECK_EQ(static_cast<int>(sim->GetLotBaseAura(&lot)), 6 + 45);

    // ---- the smoothing pass (stage 14) -----------------------------------------------------------
    sim->SmoothedGrid()->SetTractValue(0, 0, 10);
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) sim->CellWorkGrid()->SetTractValue(x, z, 20);
    sim->SetIdleState(14);
    sim->DoIdle(0);
    // delta 10, scaled 10 * (1 - 0.5) = 5 -> round-away 5, 10 + 5 = 15
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), 15);
    CHECK_EQ(sim->IdleState(), 15);
    // A negative delta moves down by the same rule: source 0, previous 15 -> -15 * 0.5 = -7.5 -> -8.
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) sim->CellWorkGrid()->SetTractValue(x, z, 0);
    sim->SetIdleState(14);
    sim->DoIdle(0);
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), 15 - 8);
    // A one unit delta still moves one unit (the round-away-from-zero quirk): 7 -> 8.
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) sim->CellWorkGrid()->SetTractValue(x, z, 8);
    sim->SetIdleState(14);
    sim->DoIdle(0);
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), 8);
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) sim->CellWorkGrid()->SetTractValue(x, z, 9);
    sim->SetIdleState(14);
    sim->DoIdle(0);
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), 9);   // delta 1 * 0.5 = 0.5 -> 1
    // A tract without any valid source cell becomes "no data".
    sim->CellWorkGrid()->SetTractValues(-0x80);
    sim->SetIdleState(14);
    sim->DoIdle(0);
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), -0x80);
    sim->SmoothedGrid()->SetTractValue(0, 0, 10);

    // ---- the lot pass (stage 5) ------------------------------------------------------------------
    host.landValue.value = 0;   // no land-value bonus: the lot base aura is the table average
    host.lotManager.iterator.lots = {&lot};
    host.lotManager.lotCount = 1;
    sim->CellWorkGrid()->SetTractValues(-0x80);
    sim->SetIdleState(4);
    sim->DoIdle(0);                 // restarts the lot iterator and calls ComputeTaxRateChangeEffect
    CHECK_EQ(sim->IdleState(), 5);
    sim->DoIdle(3);                 // the pass itself
    CHECK_EQ(host.slices.size(), 1u);
    CHECK_EQ(host.slices[0], 3u);
    CHECK_EQ(sim->CellWorkGrid()->GetTractValue(0, 0), 6);   // the lot base aura, clamped into its cells
    CHECK_EQ(sim->CellWorkGrid()->GetTractValue(3, 3), 6);
    CHECK_EQ(sim->CellWorkGrid()->GetTractValue(4, 4), -0x80);
    // The pass also restarts the iterator on the next stage 4 and times out (state stays 5).
    host.sliceAlwaysExpired = true;
    sim->SetIdleState(4);
    sim->DoIdle(0);
    CHECK_EQ(sim->IdleState(), 5);
    sim->DoIdle(0);
    CHECK_EQ(sim->IdleState(), 5);   // timed out before the list was exhausted
    host.sliceAlwaysExpired = false;

    // ---- UpdateAuraGrid --------------------------------------------------------------------------
    sim->SmoothedGrid()->SetTractValues(-0x80);
    sim->GetTransientAuraGrid()->SetTractValues(0);
    sim->SmoothedGrid()->SetTractValue(0, 0, 10);
    sim->SmoothedGrid()->SetTractValue(1, 1, -5);
    sim->GetTransientAuraGrid()->SetTractValue(0, 0, 3);
    sim->GetTransientAuraGrid()->SetTractValue(1, 1, 2);
    host.residential.wealth[0]->SetTractValue(0, 0, 100);
    host.history.entries.clear();
    sim->UpdateAuraGrid();
    CHECK_EQ(sim->GetAuraGrid()->GetTractValue(0, 0), 13);
    CHECK_EQ(sim->GetAuraGrid()->GetTractValue(1, 1), -3);
    CHECK_EQ(sim->GetAuraGrid()->GetTractValue(2, 2), 0);      // no smoothed data -> 0
    AuraPoint best;
    CHECK_EQ(sim->GetMaxMayorRating(best), 13);
    CHECK_EQ(best.x, 2);   // tract (0,0) centre: 0*4 + 4/2
    CHECK_EQ(best.z, 2);
    AuraPoint worst;
    CHECK_EQ(sim->GetMinMayorRating(worst), -3);
    CHECK_EQ(worst.x, 6);  // tract (1,1) centre
    CHECK_EQ(worst.z, 6);
    CHECK_EQ(sim->MayorRating(), 13);   // (100 * 13) / 100
    CHECK_EQ(host.history.entries.size(), 1u);
    CHECK_EQ(host.history.entries[0].first, kAuraHistoryMayorRating);
    CHECK_EQ(host.history.entries[0].second, 13);
    // Every published tract whose value is below the threshold is a raw hotspot; (2,2) publishes 0 as well.
    CHECK_EQ(sim->RawHotspots().size(), 2u);

    // ---- ComputeHotspotList ----------------------------------------------------------------------
    sim->SetHotspotTunables(1000, 4, 1);
    sim->SetIdleState(16);
    // Two raw hotspots two tracts apart merge into their average.
    sim->UpdateAuraGrid();
    sim->ComputeHotspotList();
    CHECK_EQ(sim->HotspotList().size() <= 1u, true);
    // Build the merge case directly through the public hotspot list helper: use a fresh state.
    sim->GetAuraGrid()->SetTractValue(0, 0, 0);
    {
        // (0,0) and (2,0) are 2 apart (< 4): merged to (1,0) with count 2; min count 2 keeps it.
        sim->SetIdleState(16);
        sim->UpdateAuraGrid();   // fills hotspots_ with every tract (all values are below 1000)
        sim->SetHotspotTunables(1000, 4, 2);
        sim->ComputeHotspotList();
        const auto& list = sim->HotspotList();
        CHECK(!list.empty());
        for (const tHotspot& hotspot : list) CHECK(hotspot.count >= 2);
    }
    sim->SetHotspotTunables(196, 8, 16);

    // ---- DoOccupantChanged: the park property ---------------------------------------------------
    sim->ParkEffectGrid()->SetTractValues(0);
    AddSint32Property(exemplar, 0xdeadbeef, {0, 0});   // (unrelated property)
    FakeOccupant park(0x2781284f);
    AddSint32Property(*static_cast<cSCPropertyHolder*>(park.AsPropertyHolder()), kAuraPropertyParkEffect, {10, 8});
    park.position[0] = 32.0f;
    park.position[2] = 32.0f;
    sim->DoOccupantChanged(&park, true);
    CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(0, 0), 10);
    CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(1, 0), 5);
    CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(0, 1), 5);
    CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(2, 0), 0);   // at the radius
    sim->DoOccupantChanged(&park, false);
    CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(0, 0), 0);
    CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(1, 0), 0);

    // ---- DoOccupantChanged: the landmark property (unfunded key 0, funded key = the lot's x0) ----
    host.budget.funded = false;
    cSC4Lot eastLot(8, 8, 4, 4, 0);
    eastLot.SetBuildingType(0x1010);
    FakeOccupant landmark(0x2781284f);
    AddSint32Property(*static_cast<cSCPropertyHolder*>(landmark.AsPropertyHolder()), kAuraPropertyLandmarkEffect,
                      {20, 8});
    landmark.position[0] = 160.0f;   // cell (10,10), inside the (8..11) lot
    landmark.position[2] = 160.0f;
    host.lotManager.iterator.lots = {&eastLot};
    sim->DoOccupantChanged(&landmark, true);
    CHECK_EQ(sim->LandmarkMaps().size(), 1u);
    CHECK(sim->LandmarkMaps().count(0) == 1);
    // The effect is centred on the (8..11) lot: cell (10,10) -> tract (2,2).
    CHECK_EQ(sim->LandmarkMaps().at(0)->GetTractValue(2, 2), 20);
    host.budget.funded = true;
    sim->DoOccupantChanged(&landmark, true);
    CHECK_EQ(sim->LandmarkMaps().size(), 2u);
    CHECK(sim->LandmarkMaps().count(8) == 1);
    CHECK_EQ(sim->LandmarkMaps().at(8)->GetTractValue(2, 2), 20);
    // Removing the unfunded entry takes the value back out of map 0.
    host.budget.funded = false;
    sim->DoOccupantChanged(&landmark, false);
    CHECK_EQ(sim->LandmarkMaps().at(0)->GetTractValue(2, 2), 0);

    // ---- DoOccupantChanged: the vehicle aura ----------------------------------------------------
    FakeOccupant vehicle(0x2781284f);
    AddSint32Property(*static_cast<cSCPropertyHolder*>(vehicle.AsPropertyHolder()), kAuraPropertyVehicleAura, {7, 4});
    vehicle.position[0] = 96.0f;   // tract (1,1)
    vehicle.position[2] = 96.0f;
    sim->VehicleGrid()->SetTractValues(0);
    sim->DoOccupantChanged(&vehicle, true);
    CHECK_EQ(sim->VehicleGrid()->GetTractValue(1, 1), 7);
    // A moving vehicle occupant of type 0xc772bf98 adds the preset blob 3 instead.
    FakeOccupant moving(kAuraVehicleOccupantType);
    sim->SetTransientTunable(3, 64.0f, 4.0f);
    moving.position[0] = 32.0f;
    moving.position[2] = 32.0f;
    sim->GetTransientAuraGrid()->SetTractValues(0);
    sim->DoOccupantChanged(&moving, true);
    CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(0, 0), 4);

    // ---- ComputeTaxRateChangeEffect --------------------------------------------------------------
    // The lot pass above ran the first reading: every economy type's rate was 0 and is now recorded.
    CHECK_EQ(sim->LastTaxRate(0), 0.0f);
    for (int i = 0; i < 12; ++i) host.budget.taxRate[i] = 0.0f;
    host.budget.taxRate[0] = 3.0f;   // +3 for the residential type -> the tax change curve returns 9
    sim->ComputeTaxRateChangeEffect();
    CHECK_EQ(AuraTypeTable()[0].currentAura, AuraTypeTable()[0].baseAura + 9);
    CHECK_EQ(AuraTypeTable()[1].currentAura, AuraTypeTable()[1].baseAura);
    CHECK_EQ(AuraTypeTable()[2].currentAura, AuraTypeTable()[2].baseAura);
    // The stored rate is *not* updated after the first reading (quirk): a second identical call still adds 9.
    sim->ComputeTaxRateChangeEffect();
    CHECK_EQ(AuraTypeTable()[0].currentAura, AuraTypeTable()[0].baseAura + 9);
    for (int i = 0; i < 12; ++i) host.budget.taxRate[i] = 0.0f;
    sim->ComputeTaxRateChangeEffect();
    CHECK_EQ(AuraTypeTable()[0].currentAura, AuraTypeTable()[0].baseAura);

    // ---- CheckForStartingRiot -------------------------------------------------------------------
    sim->SetHotspotTunables(100, 8, 1);
    sim->SmoothedGrid()->SetTractValues(-0x80);
    sim->SmoothedGrid()->SetTractValue(1, 1, 50);
    sim->GetTransientAuraGrid()->SetTractValues(0);
    sim->UpdateAuraGrid();
    sim->ComputeHotspotList();
    CHECK_EQ(sim->HotspotList().size(), 1u);
    sim->Random().Seed(12345);
    // The riot curve gives 10 for a hotspot count of 1, so the roll only passes about one time in ten.
    for (int attempt = 0; attempt < 500 && host.city.disaster.calls == 0; ++attempt) sim->CheckForStartingRiot();
    CHECK_EQ(host.city.disaster.calls, 1);
    CHECK_EQ(host.city.disaster.cellX, 6);   // tract (1,1) centre = 1*4 + 2
    CHECK_EQ(host.city.disaster.cellZ, 6);
    CHECK_EQ(host.city.lastDisasterType, kAuraRiotType);

    // ---- BustStrike -------------------------------------------------------------------------------
    struct {
        eStrikeBuster which;
        int police, fire, health, school;
    } strikes[] = {
        {kStrikeBusterAll, 1, 1, 1, 1},
        {kStrikeBusterPolice, 1, 0, 0, 0},
        {kStrikeBusterFire, 0, 1, 0, 0},
        {kStrikeBusterHealth, 0, 0, 1, 0},
        {kStrikeBusterEducation, 0, 0, 0, 1},
    };
    for (const auto& entry : strikes) {
        const int police = host.police.ends, fire = host.fire.ends;
        const int health = host.residential.healthStrikeEnds, school = host.residential.schoolStrikeEnds;
        sim->BustStrike(entry.which);
        CHECK_EQ(host.police.ends - police, entry.police);
        CHECK_EQ(host.fire.ends - fire, entry.fire);
        CHECK_EQ(host.residential.healthStrikeEnds - health, entry.health);
        CHECK_EQ(host.residential.schoolStrikeEnds - school, entry.school);
    }

    // ---- the full cycle --------------------------------------------------------------------------
    host.lotManager.lotCount = 1;
    host.lotManager.iterator.lots = {&lot};
    host.ordinance.value = 2;
    sim->SmoothedGrid()->SetTractValues(-0x80);
    sim->GetTransientAuraGrid()->SetTractValues(0);
    sim->SetIdleState(1);
    int guard = 0;
    while (sim->IdleState() != 0 && guard < 40) {
        sim->DoIdle(1);
        ++guard;
    }
    CHECK_EQ(sim->IdleState(), 0);
    CHECK(guard < 40);
    // The lot's base aura (6) went through the park curve (10 -> 10) and the smoothing pass, which also adds
    // the ordinance value 2: target 6 + 2 = 8, first write so no step -> 8.
    CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), 8);
    CHECK_EQ(sim->GetAuraGrid()->GetTractValue(0, 0), 8);
    CHECK_EQ(sim->OrdinanceValue(), 2);
    CHECK_EQ(host.messageServer.posted.back(), kAuraMessageCycleEnd);

    // ---- DoMessage --------------------------------------------------------------------------------
    {
        cRZMessage2Standard month;
        month.SetType(kAuraMessageMonth);
        sim->SetIdleState(0);
        CHECK(sim->DoMessage(&month));
        CHECK_EQ(sim->IdleState(), 1);
        cRZMessage2Standard idle;
        idle.SetType(kAuraMessageIdleTime);
        idle.SetData3(4);
        CHECK(sim->DoMessage(&idle));
        CHECK_EQ(sim->IdleState(), 2);
        host.ordinance.value = 11;
        cRZMessage2Standard ordinance;
        ordinance.SetType(kAuraMessageOrdinanceChanged);
        CHECK(sim->DoMessage(&ordinance));
        CHECK_EQ(sim->OrdinanceValue(), 11);
        // A reload message with the exemplar key re-reads the tunables.
        host.exemplar = nullptr;
        cRZMessage2Standard reload;
        reload.SetType(kAuraMessageReloadTunables);
        reload.SetData1(kAuraExemplarGroup);
        reload.SetData2(kAuraExemplarInstance);
        host.budget.taxRate[0] = 5.0f;
        CHECK(sim->DoMessage(&reload));
        host.exemplar = &exemplar;
        // An occupant inserted message runs DoOccupantChanged (the park grid gets the value again).
        sim->ParkEffectGrid()->SetTractValues(0);
        cRZMessage2Standard inserted;
        inserted.SetType(kMessageOccupantInsertedAura);
        inserted.SetVoid1(static_cast<void*>(static_cast<cISC4Occupant*>(&park)));
        CHECK(sim->DoMessage(&inserted));
        CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(0, 0), 10);
        // The day message runs one idle stage and reports "not handled" when the machine lags behind.
        sim->SetIdleState(1);
        host.simDate = 200;
        cRZMessage2Standard day;
        day.SetType(kAuraMessageDay);
        CHECK(sim->DoMessage(&day));            // 1 -> 2, idle start date 200
        CHECK_EQ(sim->IdleState(), 2);
        host.simDate = 210;                     // 10 days later: state 3 < 10 -> not handled
        CHECK(!sim->DoMessage(&day));
        // The lots-changed message re-adds every lot's landmark effect once Read asked for it.
        sim->SetNeedsLandmarkRefresh(true);
        host.lotManager.iterator.lots = {&eastLot};
        host.building = nullptr;                // cSC4Lot::GetBuilding needs the lot host's occupant
        host.lotManager.lotCount = 1;
        // A loaded save already holds the value: the remove/add pair leaves it unchanged.
        sim->LandmarkMaps().at(0)->SetTractValue(2, 2, 20);
        host.building = static_cast<cISC4Occupant*>(&landmark);
        lotHost.building = static_cast<cISC4Occupant*>(&landmark);
        cRZMessage2Standard lotsChanged;
        lotsChanged.SetType(kAuraMessageLotsChanged);
        CHECK(sim->DoMessage(&lotsChanged));
        CHECK_EQ(sim->LandmarkMaps().at(0)->GetTractValue(2, 2), 20);
        // On an empty map the same pair cancels out (-20 then +20), which is what the original does.
        sim->LandmarkMaps().at(0)->SetTractValue(2, 2, 0);
        CHECK(sim->DoMessage(&lotsChanged));
        CHECK_EQ(sim->LandmarkMaps().at(0)->GetTractValue(2, 2), 0);
        host.building = nullptr;
        sim->SetNeedsLandmarkRefresh(false);
        // An occupant with the "no aura" property is skipped.
        FakeOccupant muted(0x2781284f);
        AddSint32Property(*static_cast<cSCPropertyHolder*>(muted.AsPropertyHolder()), kAuraPropertyNoAuraBuilding, {1});
        AddSint32Property(*static_cast<cSCPropertyHolder*>(muted.AsPropertyHolder()), kAuraPropertyParkEffect,
                          {99, 8});
        sim->ParkEffectGrid()->SetTractValues(0);
        cRZMessage2Standard mutedMessage;
        mutedMessage.SetType(kMessageOccupantInsertedAura);
        mutedMessage.SetVoid1(static_cast<void*>(static_cast<cISC4Occupant*>(&muted)));
        CHECK(sim->DoMessage(&mutedMessage));
        CHECK_EQ(sim->ParkEffectGrid()->GetTractValue(0, 0), 0);
    }

    // ---- game-data command handlers ---------------------------------------------------------------
    {
        const auto& entries = host.registry.entries;
        EntryId transientId = 0, strikeId = 0;
        for (const auto& entry : entries) {
            if (entry.second.name == "add_transient_effect") transientId = entry.first;
            if (entry.second.name == "end_strike") strikeId = entry.first;
        }
        CHECK(transientId != 0);
        CHECK(strikeId != 0);
        CHECK(entries.at(transientId).context == sim);
        sim->GetTransientAuraGrid()->SetTractValues(0);
        cGZCommandParameterSet parameters;
        parameters.SetParameterCount(4);
        cRZVariant x(32.0f), z(32.0f), radius(64.0f), magnitude(6.0f);
        parameters.SetParameter(0, x);
        parameters.SetParameter(1, z);
        parameters.SetParameter(2, radius);
        parameters.SetParameter(3, magnitude);
        CHECK(host.registry.DoValueCall(transientId, &parameters, nullptr));
        CHECK_EQ(sim->GetTransientAuraGrid()->GetTractValue(0, 0), 6);
        cGZCommandParameterSet strikeParameters;
        strikeParameters.SetParameterCount(1);
        cRZVariant which(static_cast<std::uint32_t>(kStrikeBusterFire));
        strikeParameters.SetParameter(0, which);
        const int fireEnds = host.fire.ends;
        CHECK(host.registry.DoValueCall(strikeId, &strikeParameters, nullptr));
        CHECK_EQ(host.fire.ends, fireEnds + 1);
    }

    // ---- Write / Read -----------------------------------------------------------------------------
    {
        sim->SetHotspotTunables(100, 8, 1);
        sim->UpdateAuraGrid();
        sim->ComputeHotspotList();
        const std::size_t hotspotCount = sim->HotspotList().size();
        const std::int32_t rating = sim->MayorRating();
        const std::uint32_t landmarkCount = static_cast<std::uint32_t>(sim->LandmarkMaps().size());
        PortMemoryOStream out;
        CHECK(sim->Write(out));
        CHECK_EQ(host.gridWrites, 7 + static_cast<int>(landmarkCount));

        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        host.readGridKinds = {0, 1, 1, 0, 0, 0, 1};
        for (std::uint32_t i = 0; i < landmarkCount; ++i) host.readGridKinds.push_back(1);
        CHECK(sim->Read(in));
        CHECK_EQ(sim->MayorRating(), rating);
        CHECK_EQ(sim->HotspotList().size(), hotspotCount);
        CHECK_EQ(sim->LandmarkMaps().size(), landmarkCount);
        CHECK_EQ(sim->SmoothedGrid()->GetTractValue(0, 0), 8);
        // A version below 8 is refused.
        const unsigned char bad[2] = {7, 0};
        PortMemoryIStream shortStream(bad, sizeof(bad));
        CHECK(!sim->Read(shortStream));
    }

    // ---- Shutdown ----------------------------------------------------------------------------------
    CHECK(sim->Shutdown());
    CHECK(!sim->IsInitialized());
    CHECK_EQ(host.messageServer.removed.size(), 5u);
    CHECK_EQ(host.city.removeTargetCalls, 1);
    CHECK(host.registry.removed >= 1);
    CHECK(!sim->GetAuraGrid());
    CHECK_EQ(host.messageServer.refCount, 0u);

    // Release destroys the object (the reference count is below 2).
    sim->Release();

    SetLotHost(nullptr);
    SetAuraHost(nullptr);
    SetSimGridCity(nullptr);

    if (g_failures == 0) {
        std::printf("sc4_aurasimulator_test: ok\n");
        return 0;
    }
    std::printf("sc4_aurasimulator_test: %d failure(s)\n", g_failures);
    return 1;
}
