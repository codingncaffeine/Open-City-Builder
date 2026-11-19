// cSC4NetworkLotManager (37 functions) — the object and its three views, the lot registry, the
// configuration exemplars and their serialisation, the six message handlers, the cell/style passes and
// every query the developers and the network tools use.
//
// Everything outside the object is supplied through INetworkLotManagerHost: the message server, the
// city/terrain/zone/lot lookups, the traffic-network map, the occupant manager and its OR filter, the
// prop manager, the flora simulator, the lot-configuration object ids, the lot developer's slope, the
// GZCOM visual-effect factory, the resource manager (enumerator / exemplar holder / DB segment / store)
// and the two SC4Write/ReadGZSerializable helpers. Configuration exemplars are real cSCPropertyHolder
// objects driven through cSCPropertyHelp, exactly like the game's resources.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/NetworkLotManager.h"
#include "sc4/SCPropertyHelp.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace sc4;

// -----------------------------------------------------------------------------------------------------
// introspection: the original field offsets and the private state
// -----------------------------------------------------------------------------------------------------
namespace sc4 {
struct NetworkLotManagerTestAccess {
    struct Offsets {
        std::size_t lots, configurations, nextConfiguration, dimensionFlag, reserved74, maxCellX, maxCellZ,
            potholes, refCount, inited, updateCells;
    };
    static Offsets Get(const cSC4NetworkLotManager& m) {
        Offsets o{};
        o.lots = Off(m, &m.networkLots_);
        o.configurations = Off(m, &m.configurations_);
        o.nextConfiguration = Off(m, &m.nextPieceConfiguration_);
        o.dimensionFlag = Off(m, &m.dimensionFlag_);
        o.reserved74 = Off(m, &m.reserved74_);
        o.maxCellX = Off(m, &m.maxCellX_);
        o.maxCellZ = Off(m, &m.maxCellZ_);
        o.potholes = Off(m, &m.potholes_);
        o.refCount = Off(m, &m.refCount_);
        o.inited = Off(m, &m.inited_);
        o.updateCells = Off(m, &m.updateCells_);
        return o;
    }
    static std::size_t Off(const cSC4NetworkLotManager& m, const void* member) {
        return static_cast<std::size_t>(static_cast<const unsigned char*>(member) -
                                        reinterpret_cast<const unsigned char*>(&m));
    }
    static std::uint32_t RefCount(const cSC4NetworkLotManager& m) { return m.refCount_; }
    static bool Inited(const cSC4NetworkLotManager& m) { return m.inited_; }
    static std::size_t LotCount(const cSC4NetworkLotManager& m) { return m.networkLots_.size(); }
    static std::size_t ConfigBuckets(const cSC4NetworkLotManager& m) { return m.configurations_.size(); }
    static std::size_t Configs(const cSC4NetworkLotManager& m, std::uint32_t pieceId) {
        auto it = m.configurations_.find(pieceId);
        return it == m.configurations_.end() ? 0 : it->second.size();
    }
    static cSC4NetworkLotConfiguration* Config(const cSC4NetworkLotManager& m, std::uint32_t pieceId,
                                               std::size_t index) {
        auto it = m.configurations_.find(pieceId);
        if (it == m.configurations_.end() || index >= it->second.size()) return nullptr;
        return it->second[index];
    }
    static cSC4NetworkLotConfiguration* NextConfiguration(const cSC4NetworkLotManager& m) {
        return m.nextPieceConfiguration_;
    }
    static std::uint8_t DimensionFlag(const cSC4NetworkLotManager& m) { return m.dimensionFlag_; }
    static std::int32_t Reserved74(const cSC4NetworkLotManager& m) { return m.reserved74_; }
    static std::int32_t Reserved78(const cSC4NetworkLotManager& m) { return m.reserved78_; }
    static std::int32_t Reserved7c(const cSC4NetworkLotManager& m) { return m.reserved7c_; }
    static std::int32_t Reserved80(const cSC4NetworkLotManager& m) { return m.reserved80_; }
    static std::int32_t Reserved84(const cSC4NetworkLotManager& m) { return m.reserved84_; }
    static std::int32_t MaxCellX(const cSC4NetworkLotManager& m) { return m.maxCellX_; }
    static std::int32_t MaxCellZ(const cSC4NetworkLotManager& m) { return m.maxCellZ_; }
    static std::size_t Potholes(const cSC4NetworkLotManager& m) { return m.potholes_.size(); }
    static INetworkLotVisualEffect* Pothole(const cSC4NetworkLotManager& m, std::uint16_t key) {
        auto it = m.potholes_.find(key);
        return it == m.potholes_.end() ? nullptr : it->second;
    }
    static std::size_t UpdateCells(const cSC4NetworkLotManager& m) { return m.updateCells_.size(); }
    static bool HasUpdateCell(const cSC4NetworkLotManager& m, std::uint16_t key) {
        return m.updateCells_.count(key) != 0;
    }
    static cSC4NetworkLot* Lot(const cSC4NetworkLotManager& m, std::uint16_t key) {
        auto it = m.networkLots_.find(key);
        return it == m.networkLots_.end() ? nullptr : it->second;
    }
    // Shutdown releases lots and configurations only once the manager is initialized; tests that never call Init arm it here.
    static void MarkInited(cSC4NetworkLotManager& m) { m.inited_ = true; }
    static void AddUpdateCell(cSC4NetworkLotManager& m, std::uint16_t key) { m.updateCells_.insert(key); }
};
}  // namespace sc4

namespace {

// -----------------------------------------------------------------------------------------------------
// fakes
// -----------------------------------------------------------------------------------------------------
struct FakeOccupantObject final : INetworkLotOccupantObject {
    float position[3] = {0.0f, 0.0f, 0.0f};
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<INetworkLotOccupantObject*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (--refs <= 0) { delete this; return 0; }
        return static_cast<std::uint32_t>(refs);
    }
    void GetPosition(float xyz[3]) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float xyz[3]) override { std::memcpy(position, xyz, sizeof position); }
};

struct FakeNetworkOccupant final : INetworkLotNetworkOccupant {
    int refs = 1;
    std::uint32_t networkFlags = 0x1000;   // one network class; 0x1f4f is the "any network piece" mask
    std::uint32_t anyFlags = 0x1000;
    bool usable = true;
    std::uint32_t pieceId = 0;
    std::uint8_t rotation = 0;
    bool flipped = false;
    std::uint8_t variation = 0xff;
    std::uint32_t underTexture = 0xffffffffu;
    std::uint32_t edgeTypes = 0;
    std::uint32_t activeEdges = 0;
    bool intersection = false;
    float altitude = 0.0f;
    std::uint32_t cellX = 0, cellZ = 0;
    FakeOccupantObject* object = nullptr;
    int setNetworkFlagCalls = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4NetworkOccupant) return false;
        *out = static_cast<INetworkLotNetworkOccupant*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (--refs <= 0) { delete this; return 0; }
        return static_cast<std::uint32_t>(refs);
    }
    bool HasNetworkFlag(std::uint32_t flag) override { return (networkFlags & flag) == flag; }
    bool HasAnyNetworkFlag(std::uint32_t flag) override { return (anyFlags & flag) != 0; }
    void SetNetworkFlag(std::uint32_t flag) override { networkFlags |= flag; ++setNetworkFlagCalls; }
    std::uint32_t GetNetworkFlag() override { return networkFlags; }
    bool IsUsable() override { return usable; }
    std::uint32_t PieceId() override { return pieceId; }
    std::uint8_t GetRotation() override { return rotation; }
    bool GetFlip() override { return flipped; }
    void SetVariation(std::uint8_t value) override { variation = value; }
    void SetUnderTexture(std::uint32_t texture) override { underTexture = texture; }
    std::uint32_t GetEdgeTypesFlag(std::uint32_t) override { return edgeTypes; }
    std::uint32_t GetActiveEdges() override { return activeEdges; }
    INetworkLotOccupantObject* AsOccupant() override { return object; }
    bool IsIntersection() override { return intersection; }
    float GetAltitude(float, float) override { return altitude; }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) override { x = cellX; z = cellZ; }
};

struct FakeProp final : INetworkLotPropOccupant {
    int refs = 0;
    bool destroyed = false;
    std::uint32_t lotTag = 0xffffffffu;
    std::uint32_t flag = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<INetworkLotPropOccupant*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (--refs <= 0) { destroyed = true; return 0; }   // the host frees it
        return static_cast<std::uint32_t>(refs);
    }
    void SetLotTag(std::uint32_t tag) override { lotTag = tag; }
    void SetFlag(std::uint32_t value) override { flag |= value; }
};

struct FakeEffect final : INetworkLotVisualEffect {
    ~FakeEffect() { delete object; }   // the game never releases the effect's occupant object
    int refs = 0;                 // the original factory leaves a fresh object at 0
    bool destroyed = false;
    std::string effectName;
    int starts = 0, stops = 0;
    FakeOccupantObject* object = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<INetworkLotVisualEffect*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (--refs <= 0) { destroyed = true; return 0; }   // kept alive for the test's inspection
        return static_cast<std::uint32_t>(refs);
    }
    INetworkLotOccupantObject* AsOccupant() override { return object; }
    void SetEffectType(const char* name) override { effectName = name ? name : ""; }
    void StartEffect(bool) override { ++starts; }
    void StopEffect(bool) override { ++stops; }
};

struct FakeLot final : INetworkLot {
    int refs = 1;
    std::int32_t rect[4] = {0, 0, 0, 0};
    bool livable = true;
    std::uint32_t wealth = 0;
    std::uint32_t buildingType = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4LotNetworkLot) return false;
        *out = static_cast<INetworkLot*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (--refs <= 0) { delete this; return 0; }
        return static_cast<std::uint32_t>(refs);
    }
    void GetBoundingRect(std::int32_t out[4]) override { std::memcpy(out, rect, sizeof rect); }
    bool IsInLivableState() override { return livable; }
    std::uint32_t GetOccupantWealth() override { return wealth; }
    std::uint32_t GetBuildingType(bool) override { return buildingType; }
};

struct FakeFilter final : INetworkLotOccupantFilter {
    int refs = 0;                 // the original constructor leaves a fresh filter at 0
    bool destroyed = false;
    void AddRef() override { ++refs; }
    void Release() override { if (--refs <= 0) destroyed = true; }
};

struct FakeSegment final : INetworkLotDBSegment {
    int refs = 1;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
};

struct FakeEnumerator final : INetworkLotResourceEnumerator {
    std::vector<cGZPersistResourceKey> keys;
    int released = 0;
    void Enumerate(void (*callback)(const cGZPersistResourceKey&, void*), void* context) override {
        for (const auto& key : keys) callback(key, context);
    }
    void Release() override { ++released; }
};

struct FakeMessages final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZMessageServer2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
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
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// -----------------------------------------------------------------------------------------------------
// host
// -----------------------------------------------------------------------------------------------------
struct Host final : INetworkLotManagerHost {
    ~Host() {
        for (auto& entry : exemplars) entry.second->Release();
        for (auto& entry : ownedOccupants) delete entry;
        for (auto& entry : ownedLots) delete entry;
        for (auto* effect : effects) delete effect;
        for (auto* filter : filters) delete filter;
        for (auto* prop : props) delete static_cast<FakeProp*>(prop);
        for (auto* enumerator : enumerators) delete enumerator;
        for (auto& entry : segments) delete entry.second;
    }

    FakeMessages messages;
    int cellsX = 8, cellsZ = 6;
    std::map<std::pair<int, int>, int> zones;
    std::map<std::pair<int, int>, FakeNetworkOccupant*> networkOccupants;
    std::map<std::pair<int, int>, FakeNetworkOccupant*> firstOccupants;
    std::map<std::pair<int, int>, FakeLot*> lots;
    std::map<std::uint32_t, cSCPropertyHolder*> exemplars;
    std::vector<cGZPersistResourceKey> enumeratorKeys;
    std::map<std::pair<int, int>, std::vector<std::uint32_t>> objectIds;
    std::map<std::uint32_t, std::uint32_t> buildingWealth;
    float areaSlope = 0.0f;
    float terrainAltitude = 0.0f;
    bool hasResourceManager = true;
    bool storeResult = true;
    std::vector<FakeNetworkOccupant*> ownedOccupants;
    std::vector<FakeLot*> ownedLots;
    std::vector<FakeEffect*> effects;               // handed out by CreateVisualEffect; freed with the host
    std::vector<FakeEnumerator*> enumerators;

    std::map<const cIGZUnknown*, std::uint32_t> tags;
    std::map<std::uint32_t, cIGZUnknown*> readByTag;

    struct FindCall { int x, z; std::uint32_t flags; };
    std::vector<FindCall> findCalls;
    std::vector<std::pair<int, int>> avenueCalls;
    std::vector<INetworkLotNetworkOccupant*> intersections;
    std::vector<std::pair<int, int>> propRemovals;
    std::vector<std::vector<std::int32_t>> floraRemovals;
    std::vector<std::pair<std::uint32_t, std::vector<float>>> propCreates;
    std::vector<int> propOrientations;
    std::vector<INetworkLotPropOccupant*> props;
    std::vector<std::pair<std::uint32_t, std::vector<float>>> floraCreates;
    std::vector<INetworkLotOccupantObject*> inserted;
    std::vector<std::pair<INetworkLotOccupantObject*, std::uint32_t>> removed;
    std::vector<std::uint32_t> filterFlags;
    std::vector<int> filterRefs;
    std::vector<FakeFilter*> filters;
    std::vector<std::pair<cGZPersistResourceKey, FakeSegment*>> segments;
    std::vector<std::pair<cGZPersistResourceKey, cISCPropertyHolder*>> stores;
    std::vector<std::uint32_t> buildingWealthQueries;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> serialized;   // (iid, tag)
    int releasedExemplars = 0;
    int createVisualEffectCalls = 0;

    FakeNetworkOccupant* AddOccupant(int x, int z, FakeOccupantObject* object, std::uint32_t pieceId = 0) {
        auto* occupant = new FakeNetworkOccupant();
        occupant->object = object;
        occupant->pieceId = pieceId;
        occupant->cellX = static_cast<std::uint32_t>(x);
        occupant->cellZ = static_cast<std::uint32_t>(z);
        networkOccupants[{x, z}] = occupant;
        ownedOccupants.push_back(occupant);
        return occupant;
    }
    FakeLot* AddLot(int x, int z) {
        auto* lot = new FakeLot();
        lots[{x, z}] = lot;
        ownedLots.push_back(lot);
        return lot;
    }

    cIGZMessageServer2* MessageServer() override { return &messages; }
    int CityCellCountX() override { return cellsX; }
    int CityCellCountZ() override { return cellsZ; }
    void CityPositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x) / 16;
        cellZ = static_cast<int>(z) / 16;
    }
    void CityCellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        x = static_cast<float>(cellX) * 16.0f + 8.0f;
        z = static_cast<float>(cellZ) * 16.0f + 8.0f;
    }
    float TerrainAltitude(float, float) override { return terrainAltitude; }
    int ZoneTypeAt(int x, int z) override {
        auto it = zones.find({x, z});
        return it == zones.end() ? 0 : it->second;
    }
    INetworkLot* LotAt(int x, int z, bool) override {
        auto it = lots.find({x, z});
        return it == lots.end() ? nullptr : it->second;
    }
    INetworkLotNetworkOccupant* FindNetworkOccupant(int x, int z, std::uint32_t flags, bool includeUnusable) override {
        findCalls.push_back({x, z, flags});
        if (flags == kNetworkLotAvenueFlag) avenueCalls.emplace_back(x, z);
        auto it = networkOccupants.find({x, z});
        if (it == networkOccupants.end()) return nullptr;
        if (!includeUnusable && !it->second->usable) return nullptr;
        if ((it->second->anyFlags & flags) == 0) return nullptr;
        return it->second;
    }
    void UpdateIntersection(INetworkLotNetworkOccupant* occupant) override { intersections.push_back(occupant); }
    INetworkLotNetworkOccupant* GetFirstNetworkOccupant(int x, int z, INetworkLotOccupantFilter* filter) override {
        if (filter) filterRefs.push_back(static_cast<FakeFilter*>(filter)->refs);
        auto it = firstOccupants.find({x, z});
        if (it == firstOccupants.end()) return nullptr;
        return it->second;
    }
    void InsertOccupant(INetworkLotOccupantObject* occupant) override { inserted.push_back(occupant); }
    void RemoveOccupant(INetworkLotOccupantObject* occupant, bool, std::uint32_t reason) override {
        removed.emplace_back(occupant, reason);
    }
    INetworkLotOccupantFilter* CreateNetworkOrFilter(std::uint32_t flags) override {
        auto* filter = new FakeFilter();
        filters.push_back(filter);
        filterFlags.push_back(flags);
        return filter;
    }
    INetworkLotPropOccupant* AddStreetProp(std::uint32_t type, const float* position, int orientation) override {
        propCreates.push_back({type, {position[0], position[1], position[2]}});
        propOrientations.push_back(orientation);
        auto* prop = new FakeProp();
        props.push_back(prop);
        return prop;
    }
    void RemoveStreetPropsFromTile(int x, int z) override { propRemovals.emplace_back(x, z); }
    INetworkLotPropOccupant* AddFlora(std::uint32_t type, const float* position, bool) override {
        floraCreates.push_back({type, {position[0], position[1], position[2]}});
        auto* prop = new FakeProp();
        props.push_back(prop);
        return prop;
    }
    void DemolishFloraInCellArea(const std::int32_t rect[4], int) override {
        floraRemovals.push_back({rect[0], rect[1], rect[2], rect[3]});
    }
    void GetObjectIDArray(const cSC4LotConfigurationObject* object, std::vector<std::uint32_t>& out) override {
        auto it = objectIds.find({static_cast<int>(object->type), static_cast<int>(object->value)});
        if (it != objectIds.end()) out = it->second;
    }
    float GetAreaSlope(const std::int32_t*, bool) override { return areaSlope; }
    std::uint32_t GetBuildingWealth(std::uint32_t buildingType) override {
        buildingWealthQueries.push_back(buildingType);
        auto it = buildingWealth.find(buildingType);
        return it == buildingWealth.end() ? 0 : it->second;
    }
    INetworkLotVisualEffect* CreateVisualEffect() override {
        ++createVisualEffectCalls;
        auto* effect = new FakeEffect();
        effect->object = new FakeOccupantObject();
        effects.push_back(effect);
        return effect;
    }
    bool HasResourceManager() override { return hasResourceManager; }
    INetworkLotResourceEnumerator* GetExemplarEnumerator(std::uint32_t) override {
        auto* enumerator = new FakeEnumerator();
        enumerator->keys = enumeratorKeys;
        enumerators.push_back(enumerator);
        return enumerator;
    }
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key, std::uint32_t) override {
        auto it = exemplars.find(key.instance);
        if (it == exemplars.end()) {
            auto* holder = new cSCPropertyHolder();
            holder->AddRef();                  // the fake resource manager keeps one reference
            holder->AddRef();                  // the caller's reference
            exemplars[key.instance] = holder;
            return holder;
        }
        it->second->AddRef();
        return it->second;
    }
    void ReleaseExemplar(cISCPropertyHolder* holder) override {
        ++releasedExemplars;
        holder->Release();
    }
    INetworkLotDBSegment* GetConfigDBSegment(const cGZPersistResourceKey& key) override {
        auto* segment = new FakeSegment();
        segments.push_back({key, segment});
        return segment;
    }
    bool StoreExemplar(cISCPropertyHolder* holder, const cGZPersistResourceKey& key,
                       INetworkLotDBSegment*) override {
        stores.emplace_back(key, holder);
        return storeResult;
    }
    bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) override {
        std::uint32_t tag = 0;
        if (object) {
            auto it = tags.find(object);
            if (it != tags.end()) tag = it->second;
        }
        stream.SetUint32(tag);
        serialized.emplace_back(0, tag);
        return true;
    }
    bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override {
        std::uint32_t tag = 0;
        stream.GetUint32(tag);
        auto it = readByTag.find(tag);
        if (it != readByTag.end()) {
            *out = it->second;
            static_cast<cIGZUnknown*>(it->second)->AddRef();
        } else if (iid == kGZIID_cISC4PropOccupantVisualEffect) {
            auto* effect = new FakeEffect();
            effect->object = new FakeOccupantObject();
            *out = effect;
        } else {
            *out = nullptr;
        }
        return true;
    }
};

// ---- helpers ----------------------------------------------------------------------------------------
cSCPropertyHolder* MakeExemplar(std::uint32_t pieceId, std::uint32_t id, const char* name, std::uint8_t version,
                                std::uint32_t exemplarType, std::vector<std::uint8_t> zoneTypes,
                                std::vector<std::uint8_t> wealthTypes, std::uint32_t neighborhood,
                                std::uint8_t size, float minSlope, float maxSlope) {
    auto* holder = new cSCPropertyHolder();
    holder->AddRef();
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigExemplarTypeProperty, exemplarType);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigVersion, version);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigPieceId, pieceId);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigId, id);
    cRZString label(name);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigName, static_cast<const cIGZString&>(label));
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigZoneTypes, zoneTypes);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigWealthTypes, wealthTypes);
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(neighborhood), static_cast<std::uint8_t>(neighborhood >> 8),
                                   static_cast<std::uint8_t>(neighborhood >> 16),
                                   static_cast<std::uint8_t>(neighborhood >> 24)};
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigNeighborhood,
                                      std::vector<std::uint8_t>(bytes, bytes + 4));
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigSize, size);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigMinSlope, minSlope);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigMaxSlope, maxSlope);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigRotationMask, std::uint8_t{0x0f});
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigFlipMode, std::uint8_t{0});
    return holder;
}

cGZPersistResourceKey ExemplarKey(std::uint32_t instance) {
    cGZPersistResourceKey key;
    key.type = kNetworkLotConfigKeyType;
    key.group = kNetworkLotConfigKeyGroup;
    key.instance = instance;
    return key;
}

cSC4NetworkLotConfiguration* NewConfiguration(std::uint32_t id, std::uint32_t pieceId) {
    auto* configuration = new cSC4NetworkLotConfiguration();
    configuration->AddRef();
    configuration->SetID(id);
    configuration->SetNetworkPieceId(pieceId);
    configuration->ZoneTypeBits()[1] = true;
    configuration->ZoneTypeBits()[5] = true;
    configuration->WealthTypeBits()[0] = true;
    configuration->WealthTypeBits()[2] = true;
    configuration->MinimumSlope() = 0.0f;
    configuration->MaximumSlope() = 32.0f;
    return configuration;
}

cRZMessage2Standard* NewMessage(std::uint32_t type) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    return message;
}

std::set<std::pair<int, int>> RefreshCells(const Host& host, std::uint32_t flags) {
    std::set<std::pair<int, int>> cells;
    for (const auto& call : host.findCalls) {
        if (call.flags == flags) cells.insert({call.x, call.z});
    }
    return cells;
}

}  // namespace

int main() {
    using A = NetworkLotManagerTestAccess;

    // ================================================================================================
    // 1. the object: layout, constructor values, reference counting, QueryInterface
    // ================================================================================================
    Host host;
    SetNetworkLotManagerHost(&host);
    auto* manager = new cSC4NetworkLotManager();
#if defined(_LIBCPP_VERSION)   // the game's library: the members' containers have the Mac build's sizes only under libc++
    const A::Offsets offsets = A::Get(*manager);
    CHECK(offsets.lots == cSC4NetworkLotManager::kOffsetLots);
    CHECK(offsets.configurations == cSC4NetworkLotManager::kOffsetConfigurations);
    CHECK(offsets.nextConfiguration == cSC4NetworkLotManager::kOffsetNextConfiguration);
    CHECK(offsets.dimensionFlag == cSC4NetworkLotManager::kOffsetDimensionFlag);
    CHECK(offsets.reserved74 == cSC4NetworkLotManager::kOffsetReserved74);
    CHECK(offsets.maxCellX == cSC4NetworkLotManager::kOffsetMaxCellX);
    CHECK(offsets.maxCellZ == cSC4NetworkLotManager::kOffsetMaxCellZ);
    CHECK(offsets.potholes == cSC4NetworkLotManager::kOffsetPotholes);
    CHECK(offsets.refCount == cSC4NetworkLotManager::kOffsetRefCount);
    CHECK(offsets.inited == cSC4NetworkLotManager::kOffsetInited);
    CHECK(offsets.updateCells == cSC4NetworkLotManager::kOffsetUpdateCells);
    CHECK(sizeof(cSC4NetworkLotManager) <= cSC4NetworkLotManager::kClassSize);
#endif

    CHECK(A::RefCount(*manager) == 0);                    // quirk: a fresh object counts 0
    CHECK(A::Reserved7c(*manager) == 1);
    CHECK(A::Reserved74(*manager) == 0 && A::Reserved78(*manager) == 0);
    CHECK(A::Reserved80(*manager) == static_cast<std::int32_t>(0x80000000u));
    CHECK(A::Reserved84(*manager) == static_cast<std::int32_t>(0x80000000u));
    CHECK(A::MaxCellX(*manager) == 0x7fffffff && A::MaxCellZ(*manager) == 0x7fffffff);
    CHECK(A::LotCount(*manager) == 0 && A::ConfigBuckets(*manager) == 0 && A::Potholes(*manager) == 0);
    CHECK(!A::Inited(*manager) && A::NextConfiguration(*manager) == nullptr);
    CHECK(manager->GetGZCLSID() == kGZCLSID_cSC4NetworkLotManager);
    CHECK(manager->AddRef() == 1);
    CHECK(manager->AddRef() == 2);
    CHECK(manager->Release() == 1);

    void* out = nullptr;
    CHECK(manager->QueryInterface(kGZIID_cISC4NetworkLotManager, &out) &&
          out == static_cast<cISC4NetworkLotManager*>(manager));
    CHECK(manager->QueryInterface(1, &out) && out == static_cast<cISC4NetworkLotManager*>(manager));
    CHECK(manager->QueryInterface(kGZIID_cIGZMessageTarget2NetworkLot, &out) &&
          out == static_cast<cIGZMessageTarget2*>(manager));
    CHECK(manager->QueryInterface(kGZIID_cIGZSerializableNetworkLot, &out) &&
          out == static_cast<cIGZSerializable*>(manager));
    CHECK(manager->QueryInterface(0, &out) == false);
    CHECK(manager->QueryInterface(0xdeadbeefu, &out) == false);
    CHECK(A::RefCount(*manager) == 5);                    // 1 + four successful QIs
    for (int i = 0; i < 4; ++i) manager->Release();
    CHECK(A::RefCount(*manager) == 1);

    // ================================================================================================
    // 2. Init: the six subscriptions, the city geometry, the configuration reattachment, the flag
    // ================================================================================================
    auto* messageTarget = static_cast<cIGZMessageTarget2*>(manager);
    host.cellsX = 8;
    host.cellsZ = 6;
    CHECK(manager->Init());
    CHECK(manager->Init());                               // inited guard
    CHECK(A::Inited(*manager));
    CHECK(host.messages.added.size() == 6);
    for (std::size_t i = 0; i < host.messages.added.size() && i < 6; ++i) {
        CHECK(host.messages.added[i].first == messageTarget);
        CHECK(host.messages.added[i].second == kNetworkLotManagerMessages[i]);
    }
    CHECK(A::MaxCellX(*manager) == 7 && A::MaxCellZ(*manager) == 5);
    CHECK(A::Reserved74(*manager) == 0 && A::Reserved78(*manager) == 0);
    CHECK(A::Reserved80(*manager) == 0 && A::Reserved84(*manager) == 0);
    CHECK(A::DimensionFlag(*manager) == 0);

    // The +0x70 flag is ((maxCellX | maxCellZ) >> 30) & 2, i.e. it needs bit 31 of a cell count - 1:
    // a negative dimension is clamped to 0 first, so the flag is always 0 in practice (dead field).
    {
        Host bigHost;
        bigHost.cellsX = static_cast<int>(0x80000001u);   // negative as an int: the guard clamps it
        bigHost.cellsZ = 4;
        SetNetworkLotManagerHost(&bigHost);
        auto* big = new cSC4NetworkLotManager();
        big->AddRef();
        CHECK(big->Init());
        CHECK(A::DimensionFlag(*big) == 0);
        CHECK(A::MaxCellX(*big) == 0);
        CHECK(A::MaxCellZ(*big) == 3);
        bigHost.cellsX = 0;
        bigHost.cellsZ = 0;
        auto* empty = new cSC4NetworkLotManager();
        empty->AddRef();
        CHECK(empty->Init());
        CHECK(A::MaxCellX(*empty) == 0 && A::MaxCellZ(*empty) == 0);
        empty->Shutdown();
        empty->Release();
        big->Shutdown();
        big->Release();
        SetNetworkLotManagerHost(&host);
    }

    // Init reattaches a lot's configuration by the id stored at lot +0x18 and drops lots without one.
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* second = new cSC4NetworkLotManager();
        second->AddRef();
        auto* configuration = NewConfiguration(0x1234, 0x70);
        second->AddLotConfiguration(configuration);
        // Two lots through Read: one whose configuration exists, one whose does not.
        PortMemoryOStream stream;
        stream.SetUint16(2);
        stream.SetUint32(2);
        stream.SetUint16(NetworkLotCellKey(1, 1));
        stream.SetUint32(0x1111);
        stream.SetUint32(0x1234);
        stream.SetUint16(NetworkLotCellKey(2, 2));
        stream.SetUint32(0x2222);
        stream.SetUint32(0x9999);
        stream.SetUint32(0);
        PortMemoryIStream input(stream.bytes().data(), stream.bytes().size());
        CHECK(second->Read(input));
        CHECK(A::LotCount(*second) == 2);
        cSC4NetworkLot* kept = A::Lot(*second, NetworkLotCellKey(1, 1));
        cSC4NetworkLot* dropped = A::Lot(*second, NetworkLotCellKey(2, 2));
        CHECK(kept && dropped);
        CHECK(kept->GetConfiguration() == nullptr);        // Read only restores the id
        CHECK(kept->GetConfigurationId() == 0x1234);
        CHECK(dropped->GetConfigurationId() == 0x9999);
        CHECK(second->Init());
        CHECK(A::LotCount(*second) == 1);
        CHECK(A::Lot(*second, NetworkLotCellKey(1, 1)) == kept);
        CHECK(kept->GetConfiguration() == configuration);
        second->Shutdown();
        second->Release();
        configuration->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 3. LoadAllConfigurations + ReadLotConfiguration
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* exemplar = MakeExemplar(0x70, 0x2001, "RoadLot", 2, kNetworkLotConfigExemplarType, {1, 5}, {0, 2},
                                      0x0f0f0f0f, 4, 1.0f, 9.0f);
        local.exemplars[0x5001] = exemplar;
        // Add one object to the configuration through the recovered static converter.
        {
            std::vector<cSC4LotConfigurationObject> objects(1);
            objects[0].type = 1;
            objects[0].flags = 0;
            objects[0].orientation = 1;
            objects[0].x = 2.0f;
            objects[0].y = 0.5f;
            objects[0].z = 3.0f;
            objects[0].value = 0x77;
            objects[0].id = 0x88;
            cSC4LotConfiguration::ConvertObjectArrayToProperties(*exemplar, objects);
        }
        // A second key of the wrong exemplar type is ignored.
        auto* wrong = MakeExemplar(0x71, 0x2002, "Wrong", 2, 0x1e, {1}, {0}, 0x0f0f0f0f, 4, 0.0f, 8.0f);
        local.exemplars[0x5002] = wrong;
        local.enumeratorKeys = {ExemplarKey(0x5001), ExemplarKey(0x5002)};

        auto* third = new cSC4NetworkLotManager();
        third->AddRef();
        CHECK(third->Init());
        CHECK(A::ConfigBuckets(*third) == 1);
        CHECK(A::Configs(*third, 0x70) == 1);
        cSC4NetworkLotConfiguration* configuration = A::Config(*third, 0x70, 0);
        CHECK(configuration != nullptr);
        if (configuration) {
            CHECK(configuration->GetID() == 0x2001);
            CHECK(configuration->GetNetworkPieceId() == 0x70);
            CHECK(configuration->ZoneTypeBits()[1] && configuration->ZoneTypeBits()[5]);
            CHECK(configuration->WealthTypeBits()[0] && configuration->WealthTypeBits()[2]);
            CHECK(configuration->TileNeighborhood() == 0x0f0f0f0f);
            CHECK(configuration->Field58() == 4);
            CHECK(configuration->MinimumSlope() == 1.0f && configuration->MaximumSlope() == 9.0f);
            CHECK(configuration->RotationMask() == 0x0f && configuration->FlipMode() == 0);
            CHECK(configuration->GetObjectArray().size() == 1);
            if (configuration->GetObjectArray().size() == 1) {
                const cSC4LotConfigurationObject& object = configuration->GetObjectArray()[0];
                CHECK(object.type == 1 && object.orientation == 1 && object.id == 0x88);
                CHECK(object.x == 2.0f && object.y == 0.5f && object.z == 3.0f);
            }
            CHECK(local.releasedExemplars == 2);           // both exemplars are released
        }
        third->Shutdown();
        third->Release();
        SetNetworkLotManagerHost(&host);
    }
    // A configuration without the piece-id property is rejected (ReadLotConfiguration returns false).
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* exemplar = new cSCPropertyHolder();
        exemplar->AddRef();
        cSCPropertyHelp::SetPropertyValue(exemplar, kNetworkLotConfigExemplarTypeProperty,
                                          kNetworkLotConfigExemplarType);
        local.exemplars[0x6001] = exemplar;
        local.enumeratorKeys = {ExemplarKey(0x6001)};
        auto* fourth = new cSC4NetworkLotManager();
        fourth->AddRef();
        CHECK(fourth->Init());
        CHECK(A::ConfigBuckets(*fourth) == 0);
        fourth->Shutdown();
        fourth->Release();
        SetNetworkLotManagerHost(&host);
    }
    // A version-1 configuration is upgraded and written back to the database during the load.
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* exemplar = MakeExemplar(0x72, 0x2003, "Old", 1, kNetworkLotConfigExemplarType, {1}, {0},
                                      0x0f0f0f0f, 4, 0.0f, 8.0f);
        local.exemplars[0x5003] = exemplar;
        local.enumeratorKeys = {ExemplarKey(0x5003)};
        auto* fifth = new cSC4NetworkLotManager();
        fifth->AddRef();
        CHECK(fifth->Init());
        CHECK(local.segments.size() == 1);
        if (!local.segments.empty()) {
            CHECK(local.segments[0].first.instance == 0x2003);
            CHECK(local.segments[0].first.group == kNetworkLotConfigKeyGroup);
            CHECK(local.segments[0].first.type == kNetworkLotConfigKeyType);
            CHECK(local.stores.size() == 1);
        }
        // Quirk: the version-1 upgrade writes the configuration into the piece table and then the
        // caller's AddLotConfiguration appends it again, so the table ends up with two entries.
        CHECK(A::Configs(*fifth, 0x72) == 2);
        CHECK(A::Config(*fifth, 0x72, 0) == A::Config(*fifth, 0x72, 1));
        fifth->Shutdown();
        fifth->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 4. AddLotConfiguration / GetNetworkLotConfigurationSet / GetNetworkLotConfigurationById
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* six = new cSC4NetworkLotManager();
        six->AddRef();
        auto* a = NewConfiguration(0x0101, 0x30);
        auto* b = NewConfiguration(0x0102, 0x30);
        auto* c = NewConfiguration(0x0103, 0x31);
        six->AddLotConfiguration(a);
        six->AddLotConfiguration(b);
        six->AddLotConfiguration(c);
        CHECK(A::ConfigBuckets(*six) == 2);
        CHECK(A::Configs(*six, 0x30) == 2 && A::Configs(*six, 0x31) == 1);
        CHECK(six->GetNetworkLotConfigurationById(0x0101));
        CHECK(six->GetNetworkLotConfigurationById(0x0103));
        CHECK(!six->GetNetworkLotConfigurationById(0x0104));
        std::set<cSC4NetworkLotConfiguration*> configurations;
        six->GetNetworkLotConfigurationSet(configurations);
        CHECK(configurations.size() == 3);
        CHECK(configurations.count(a) && configurations.count(b) && configurations.count(c));
        for (cSC4NetworkLotConfiguration* configuration : configurations) configuration->Release();   // the set's references; a second call only clears
        six->GetNetworkLotConfigurationSet(configurations);   // clears first
        CHECK(configurations.size() == 3);
        for (cSC4NetworkLotConfiguration* configuration : configurations) configuration->Release();
        a->Release();
        b->Release();
        c->Release();
        six->Init();   // Shutdown releases the lots only once initialized
        six->Shutdown();
        six->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 5. WriteLotConfiguration: properties, database store, piece-table update
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* seven = new cSC4NetworkLotManager();
        seven->AddRef();
        auto* configuration = NewConfiguration(0x4001, 0x40);
        cRZString writtenName("Written");
        configuration->SetName(writtenName);
        configuration->Field58() = 5;
        configuration->RotationMask() = 0x05;
        configuration->FlipMode() = 2;
        configuration->Neighborhood() = 0x03020100u;
        FakeSegment segment;
        CHECK(seven->WriteLotConfiguration(configuration, &segment));
        CHECK(local.stores.size() == 1);
        if (!local.stores.empty()) {
            CHECK(local.stores[0].first.instance == 0x4001);
            cISCPropertyHolder* holder = local.stores[0].second;
            std::uint32_t pieceId = 0, id = 0, exemplarType = 0;
            std::uint8_t version = 0, size = 0;
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigPieceId, pieceId) && pieceId == 0x40);
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigId, id) && id == 0x4001);
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigExemplarTypeProperty, exemplarType) &&
                  exemplarType == 0x21);
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigVersion, version) && version == 2);
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigSize, size) && size == 5);
            std::uint8_t rotation = 0, flip = 0;
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigRotationMask, rotation) && rotation == 0x05);
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigFlipMode, flip) && flip == 2);
            std::vector<std::uint8_t> bytes;
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigNeighborhood, bytes) &&
                  bytes.size() == 4 && bytes[0] == 0 && bytes[1] == 1 && bytes[2] == 2 && bytes[3] == 3);
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigZoneTypes, bytes) &&
                  (bytes == std::vector<std::uint8_t>{1, 5}));
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigWealthTypes, bytes) &&
                  (bytes == std::vector<std::uint8_t>{0, 2}));
            cRZString name;
            CHECK(cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigName, name) &&
                  std::strcmp(name.Data(), "Written") == 0);
        }
        CHECK(A::Configs(*seven, 0x40) == 1);
        // Writing a configuration with the same id replaces the table entry instead of appending.
        auto* replacement = NewConfiguration(0x4001, 0x40);
        CHECK(seven->WriteLotConfiguration(replacement, &segment));
        CHECK(A::Configs(*seven, 0x40) == 1);
        CHECK(A::Config(*seven, 0x40, 0) == replacement);
        // A new id for the same piece appends.
        auto* secondStyle = NewConfiguration(0x4002, 0x40);
        CHECK(seven->WriteLotConfiguration(secondStyle, &segment));
        CHECK(A::Configs(*seven, 0x40) == 2);
        // A failed store leaves the table alone.
        auto* failed = NewConfiguration(0x4003, 0x41);
        local.storeResult = false;
        CHECK(!seven->WriteLotConfiguration(failed, &segment));
        CHECK(A::Configs(*seven, 0x41) == 0);
        local.storeResult = true;
        // No resource manager: the call fails and takes no table entry.
        local.hasResourceManager = false;
        CHECK(!seven->WriteLotConfiguration(failed, &segment));
        local.hasResourceManager = true;
        // A null configuration fails.
        CHECK(!seven->WriteLotConfiguration(nullptr, &segment));
        failed->Release();
        secondStyle->Release();
        replacement->Release();
        configuration->Release();
        seven->Init();   // Shutdown releases the lots only once initialized
        seven->Shutdown();
        seven->Release();
        SetNetworkLotManagerHost(&host);
    }


    // ================================================================================================
    // 6. IterateAllNetworkLots, GetNetworkLot and SetLotConfigurationForNextPiece
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* eight = new cSC4NetworkLotManager();
        eight->AddRef();
        auto* configuration = NewConfiguration(0x7001, 0x55);
        eight->AddLotConfiguration(configuration);
        // Two lots through the stream path (no occupants needed for the iteration).
        PortMemoryOStream stream;
        stream.SetUint16(2);
        stream.SetUint32(2);
        stream.SetUint16(NetworkLotCellKey(3, 4));
        stream.SetUint32(0);
        stream.SetUint32(0x7001);
        stream.SetUint16(NetworkLotCellKey(5, 6));
        stream.SetUint32(0);
        stream.SetUint32(0x7001);
        stream.SetUint32(0);
        PortMemoryIStream input(stream.bytes().data(), stream.bytes().size());
        CHECK(eight->Read(input));
        CHECK(eight->GetNetworkLot(3, 4) != nullptr && eight->GetNetworkLot(5, 6) != nullptr);
        CHECK(eight->GetNetworkLot(1, 2) == nullptr);
        CHECK(eight->GetNetworkLot(3, 5) == nullptr);

        struct IterateContext { std::vector<std::pair<std::uint32_t, std::uint32_t>> cells; std::size_t stopAfter; };
        IterateContext context{{}, 0};
        auto callback = [](std::uint32_t x, std::uint32_t z, cSC4NetworkLot*, void* raw) {
            auto* c = static_cast<IterateContext*>(raw);
            c->cells.emplace_back(x, z);
            return c->stopAfter == 0 || c->cells.size() < c->stopAfter;
        };
        CHECK(eight->IterateAllNetworkLots(callback, &context));
        CHECK(context.cells.size() == 2);
        std::set<std::pair<std::uint32_t, std::uint32_t>> cells(context.cells.begin(), context.cells.end());
        CHECK(cells.count({3, 4}) && cells.count({5, 6}));
        context = IterateContext{{}, 1};
        CHECK(!eight->IterateAllNetworkLots(callback, &context));
        CHECK(context.cells.size() == 1);

        // SetLotConfigurationForNextPiece: same pointer is a no-op, a new one is referenced, the old
        // one is released, and null clears it.
        auto* forced = NewConfiguration(0x7002, 0x55);
        eight->SetLotConfigurationForNextPiece(forced);
        CHECK(A::NextConfiguration(*eight) == forced);
        eight->SetLotConfigurationForNextPiece(nullptr);
        CHECK(A::NextConfiguration(*eight) == nullptr);
        forced->Release();
        configuration->Release();
        eight->Init();   // Shutdown releases the lots only once initialized
        eight->Shutdown();
        eight->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 7. Write/Read round trip (version 2 only; Read restores the configuration id, not the pointer)
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* nine = new cSC4NetworkLotManager();
        nine->AddRef();
        auto* configuration = NewConfiguration(0x8001, 0x56);
        nine->AddLotConfiguration(configuration);
        FakeOccupantObject object;
        object.position[0] = 2.0f * 16.0f;
        object.position[2] = 3.0f * 16.0f;
        auto* occupant = local.AddOccupant(2, 3, &object, 0x56);
        local.tags[occupant] = 0xabcdef01u;
        FakeOccupantObject floraObject;
        auto* flora = local.AddOccupant(4, 4, &floraObject, 0);
        (void)flora;
        nine->DoNetworkOccupantInserted(occupant, 1, 2);
        CHECK(A::LotCount(*nine) == 1);

        PortMemoryOStream stream;
        CHECK(nine->Write(stream));
        const std::vector<unsigned char>& bytes = stream.bytes();
        // version 2, lot count 1, key, occupant tag 0xabcdef01, configuration id, pothole count 0.
        CHECK(bytes.size() == 2 + 4 + 2 + 4 + 4 + 4);
        CHECK(bytes[0] == 2 && bytes[1] == 0);
        CHECK(bytes[2] == 1 && bytes[3] == 0 && bytes[4] == 0 && bytes[5] == 0);
        CHECK(bytes[6] == static_cast<unsigned char>(NetworkLotCellKey(2, 3) & 0xff));
        CHECK(bytes[7] == static_cast<unsigned char>(NetworkLotCellKey(2, 3) >> 8));
        CHECK(bytes[8] == 0x01 && bytes[9] == 0xef && bytes[10] == 0xcd && bytes[11] == 0xab);

        auto* reader = new cSC4NetworkLotManager();
        reader->AddRef();
        FakeOccupantObject readObject;
        auto* readOccupant = local.AddOccupant(2, 3, &readObject, 0x56);
        local.readByTag[0xabcdef01u] = readOccupant;
        PortMemoryIStream input(bytes.data(), bytes.size());
        CHECK(reader->Read(input));
        CHECK(A::LotCount(*reader) == 1);
        cSC4NetworkLot* readLot = A::Lot(*reader, NetworkLotCellKey(2, 3));
        CHECK(readLot != nullptr);
        if (readLot) {
            CHECK(readLot->GetNetworkOccupant() == readOccupant);
            CHECK(readLot->GetConfigurationId() == 0x8001);
            CHECK(readLot->GetConfiguration() == nullptr);
        }
        // Write after Read uses the stored id (the binary would dereference the null configuration).
        PortMemoryOStream second;
        CHECK(reader->Write(second));
        PortMemoryIStream again(second.bytes().data(), second.bytes().size());
        auto* third = new cSC4NetworkLotManager();
        third->AddRef();
        CHECK(third->Read(again));
        CHECK(A::Lot(*third, NetworkLotCellKey(2, 3))->GetConfigurationId() == 0x8001);
        // A wrong version is rejected.
        const unsigned char badVersion[2] = {3, 0};
        PortMemoryIStream bad(badVersion, sizeof badVersion);
        CHECK(!third->Read(bad));
        A::MarkInited(*third);
        third->Shutdown();
        third->Release();
        A::MarkInited(*reader);
        reader->Shutdown();
        reader->Release();
        A::MarkInited(*nine);
        nine->Shutdown();
        nine->Release();
        configuration->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 8. IsLotCompatible: tile mask, zone, wealth, rotation, flip mode and slope
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* ten = new cSC4NetworkLotManager();
        ten->AddRef();
        auto* configuration = NewConfiguration(0x9001, 0x60);
        // zone {1,5}, wealth {0,2}, rotation 0xf, flip mode 0, slope 0..32, tile mask 0x0f0f0f0f.
        configuration->Neighborhood() = 0x0f0f0f0f;
        configuration->Field58() = 4;
        local.areaSlope = 4.0f;
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        CHECK(ten->IsLotCompatible(configuration, 3, 3, 5, 2, 3, true));
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 2, 0, 0, false));    // zone 2 not allowed
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 1, 0, false));    // wealth 1 not allowed
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 16, 0, 0, false));   // zone out of the bit set
        configuration->RotationMask() = 0x05;                                 // rotations 0 and 2 only
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 0, 1, false));
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 2, false));
        configuration->RotationMask() = 0x0f;
        configuration->FlipMode() = 1;              // requires flipped
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, true));
        configuration->FlipMode() = 2;              // requires not flipped
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, true));
        configuration->FlipMode() = 3;              // any other value accepts both
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, true));
        configuration->MinimumSlope() = 1.0f;
        configuration->MaximumSlope() = 4.0f;
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));     // 4 is inclusive
        local.areaSlope = 4.5f;
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        local.areaSlope = 0.5f;
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        // The 4x4 tile mask: only the (0,0) tile is allowed.
        configuration->Neighborhood() = 0x00000001;
        local.areaSlope = 2.0f;
        CHECK(ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        CHECK(!ten->IsLotCompatible(configuration, 1, 0, 1, 0, 0, false));
        CHECK(!ten->IsLotCompatible(configuration, 0, 1, 1, 0, 0, false));
        CHECK(ten->IsLotCompatible(configuration, 4, 4, 1, 0, 0, false));     // wraps to tile (0,0)
        configuration->Field58() = 0;                                         // the original would divide by 0
        CHECK(!ten->IsLotCompatible(configuration, 0, 0, 1, 0, 0, false));
        CHECK(!ten->IsLotCompatible(nullptr, 0, 0, 1, 0, 0, false));
        configuration->Release();
        ten->Init();   // Shutdown releases the lots only once initialized
        ten->Shutdown();
        ten->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 9. GetPositionForLotConfigurationObject, CreateProp, CreateLotObjects, RedevelopLot
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* eleven = new cSC4NetworkLotManager();
        eleven->AddRef();
        auto* configuration = NewConfiguration(0xA001, 0x61);
        eleven->AddLotConfiguration(configuration);
        FakeOccupantObject object;
        object.position[0] = 2.0f * 16.0f;   // cell (2, 3)
        object.position[2] = 3.0f * 16.0f;
        auto* occupant = local.AddOccupant(2, 3, &object, 0x61);
        occupant->altitude = 2.0f;
        eleven->DoNetworkOccupantInserted(occupant, 1, 0);
        cSC4NetworkLot* lot = eleven->GetNetworkLot(2, 3);
        CHECK(lot != nullptr);
        CHECK(lot->GetConfiguration() == configuration);
        CHECK(local.intersections.size() == 1 && local.intersections[0] == occupant);

        cSC4LotConfigurationObject configObject;
        configObject.type = 1;
        configObject.orientation = 1;
        configObject.x = 2.0f;
        configObject.y = 0.5f;
        configObject.z = 3.0f;
        configObject.id = 0x88;
        configObject.value = 0x77;
        configuration->GetObjectArray().push_back(configObject);
        int rotation = -1;
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->rotation = 0;
        eleven->GetPositionForLotConfigurationObject(lot, &configObject, position, rotation);
        CHECK(position[0] == 46.0f && position[2] == 61.0f && position[1] == 2.5f);
        CHECK(rotation == 1);                                   // (orientation 1 + rotation 0) & 3
        occupant->rotation = 1;
        eleven->GetPositionForLotConfigurationObject(lot, &configObject, position, rotation);
        CHECK(position[0] == 35.0f && position[2] == 62.0f);
        CHECK(rotation == 2);
        occupant->rotation = 2;
        eleven->GetPositionForLotConfigurationObject(lot, &configObject, position, rotation);
        CHECK(position[0] == 34.0f && position[2] == 51.0f);
        CHECK(rotation == 3);
        occupant->rotation = 3;
        eleven->GetPositionForLotConfigurationObject(lot, &configObject, position, rotation);
        CHECK(position[0] == 45.0f && position[2] == 50.0f);
        CHECK(rotation == 0);
        // Flip mirrors the position inside the cell and uses the flipped-rotation table.
        occupant->rotation = 0;
        occupant->flipped = true;
        eleven->GetPositionForLotConfigurationObject(lot, &configObject, position, rotation);
        CHECK(position[0] == 34.0f && position[2] == 61.0f);
        CHECK(rotation == kNetworkLotFlippedRotation[1 * 4 + 0]);
        occupant->flipped = false;
        // An occupant rotation above 3 puts the object at the cell origin.
        occupant->rotation = 4;
        eleven->GetPositionForLotConfigurationObject(lot, &configObject, position, rotation);
        CHECK(position[0] == 0.0f && position[2] == 0.0f);

        // CreateProp: places the object, tags it and flags the flipped variant.
        occupant->rotation = 0;
        local.propCreates.clear();
        local.propOrientations.clear();
        INetworkLotPropOccupant* prop = eleven->CreateProp(lot, 0, 0x1234);
        CHECK(prop != nullptr);
        CHECK(local.propCreates.size() == 1 && local.propCreates[0].first == 0x1234);
        CHECK(local.propOrientations.size() == 1 && local.propOrientations[0] == 1);
        if (prop) {
            auto* fakeProp = static_cast<FakeProp*>(prop);
            CHECK(fakeProp->lotTag == 0x88);
            CHECK(fakeProp->flag == 0);
        }
        occupant->flipped = true;
        prop = eleven->CreateProp(lot, 0, 0x1234);
        if (prop) CHECK(static_cast<FakeProp*>(prop)->flag == 0x80);
        occupant->flipped = false;
        CHECK(eleven->CreateProp(lot, 0, 0) == nullptr);
        CHECK(eleven->CreateProp(lot, 9, 0x1234) == nullptr);

        // CreateLotObjects: a flora object (type 4), a prop (type 1, flags 0) and a mirrored prop
        // (type 1, flags bit 0 set) which must be skipped.
        auto* builder = NewConfiguration(0xA002, 0x62);
        cSC4LotConfigurationObject floraObject;
        floraObject.type = 4;
        floraObject.value = 0x44;
        cSC4LotConfigurationObject propObject;
        propObject.type = 1;
        propObject.value = 0x77;
        propObject.id = 0x99;
        cSC4LotConfigurationObject mirroredObject;
        mirroredObject.type = 1;
        mirroredObject.flags = 1;
        mirroredObject.value = 0x78;
        builder->GetObjectArray() = {floraObject, propObject, mirroredObject};
        local.objectIds[{4, 0x44}] = {0x11};
        local.objectIds[{1, 0x77}] = {0x33};
        local.objectIds[{1, 0x78}] = {0x44};
        auto* builderOccupantObject = new FakeOccupantObject();
        builderOccupantObject->position[0] = 4.0f * 16.0f;
        builderOccupantObject->position[2] = 4.0f * 16.0f;
        auto* builderOccupant = local.AddOccupant(4, 4, builderOccupantObject, 0x62);
        eleven->SetLotConfigurationForNextPiece(builder);
        local.floraCreates.clear();
        local.propCreates.clear();
        eleven->DoNetworkOccupantInserted(builderOccupant, 1, 0);
        cSC4NetworkLot* builtLot = eleven->GetNetworkLot(4, 4);
        CHECK(builtLot != nullptr);
        if (builtLot) {
            CHECK(builtLot->UsesNextPieceConfiguration());       // the forced configuration was used
        }
        CHECK(local.floraCreates.size() == 1 && local.floraCreates[0].first == 0x11);
        CHECK(local.propCreates.size() == 1 && local.propCreates[0].first == 0x33);
        CHECK(A::NextConfiguration(*eleven) == nullptr);         // released after the piece was placed
        // RedevelopLot clears the cell's street props/flora (masked to a byte) and rebuilds the objects.
        local.propRemovals.clear();
        local.floraRemovals.clear();
        local.floraCreates.clear();
        eleven->RedevelopLot(0x104, 0x205, builtLot);
        CHECK(local.propRemovals.size() == 1 && local.propRemovals[0] == std::make_pair(4, 5));
        CHECK(local.floraRemovals.size() == 1 &&
              local.floraRemovals[0] == std::vector<std::int32_t>({0x104, 0x205, 0x104, 0x205}));
        CHECK(local.floraCreates.size() == 1);
        builder->Release();
        configuration->Release();
        A::MarkInited(*eleven);
        eleven->Shutdown();
        eleven->Release();
        delete builderOccupantObject;
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 10. UpdateRoadStyle and UpdateLot
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* twelve = new cSC4NetworkLotManager();
        twelve->AddRef();
        local.cellsX = 8;
        local.cellsZ = 6;
        FakeOccupantObject object;
        auto* occupant = local.AddOccupant(5, 5, &object, 0);
        local.zones[{5, 5}] = 3;
        // A neighbour lot supplies wealth 2 (not livable, so the wealth is the occupant wealth).
        FakeLot* neighbour = local.AddLot(4, 5);
        neighbour->livable = false;
        neighbour->wealth = 2;

        // Pieces carrying the "no style" mask are skipped.
        occupant->anyFlags = kNetworkLotNoStyleFlagMask;
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(occupant->underTexture == 0xffffffffu);
        // Unusable pieces are skipped.
        occupant->anyFlags = 0x1000u;
        occupant->underTexture = 0x12345678u;
        occupant->usable = false;
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(occupant->underTexture == 0x12345678u);
        occupant->usable = true;
        // Avenue pieces take the avenue style pass.
        occupant->networkFlags = 0x1000u | kNetworkLotAvenueFlag;
        local.avenueCalls.clear();
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(local.avenueCalls.size() == 1 && local.avenueCalls[0] == std::make_pair(5, 5));
        occupant->networkFlags = 0x1000u;
        // The normal style: zone 3 and wealth 2.
        occupant->underTexture = 0;
        occupant->variation = 0xff;
        occupant->setNetworkFlagCalls = 0;
        occupant->anyFlags = 0x1000u;
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(occupant->underTexture == kNetworkLotUnderTextureId[3 * 4 + 2]);
        CHECK(occupant->variation == kNetworkLotVariation[3 * 3 + 1]);
        CHECK(occupant->setNetworkFlagCalls == 1);
        CHECK((occupant->networkFlags & kNetworkLotUnderTextureFlag) != 0);
        // A piece with the "style 2" flag but not the dirt-road flag takes the avenue under-texture.
        occupant->anyFlags = 0x1000u | kNetworkLotStyleTwoFlag;
        occupant->underTexture = 0;
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(occupant->underTexture == 0x8400000u);
        // Quirk: 0xd shares bit 2 with the "no style" mask 0x40280304, so a dirt-road piece is skipped
        // by the first test and never reaches the table branch (the 0xd disjunct is dead).
        occupant->anyFlags = 0x1000u | kNetworkLotStyleTwoFlag | kNetworkLotStyleDRoadFlag;
        occupant->underTexture = 0x55;
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(occupant->underTexture == 0x55);
        // Zone 0 with the "style 2" path gives no under-texture and no flag.
        local.zones[{5, 5}] = 0;
        occupant->anyFlags = 0x1000u | kNetworkLotStyleTwoFlag;
        occupant->underTexture = 1;
        occupant->setNetworkFlagCalls = 0;
        twelve->UpdateRoadStyle(5, 5, occupant);
        CHECK(occupant->underTexture == 0);
        CHECK(occupant->setNetworkFlagCalls == 0);
        local.zones[{5, 5}] = 3;
        occupant->anyFlags = 0x1000u;

        // UpdateLot: no lot yet and the piece is usable -> insert.
        FakeOccupantObject lotObject;
        lotObject.position[0] = 5.0f * 16.0f;
        lotObject.position[2] = 5.0f * 16.0f;
        occupant->object = &lotObject;
        occupant->pieceId = 0x63;
        twelve->UpdateLot(occupant, 5, 5, 1, 2);
        CHECK(twelve->GetNetworkLot(5, 5) == nullptr);      // no configuration for piece 0x63
        occupant->usable = false;
        twelve->UpdateLot(occupant, 5, 5, 1, 2);
        CHECK(twelve->GetNetworkLot(5, 5) == nullptr);
        occupant->usable = true;
        // With a configuration the insert creates the lot.
        auto* configuration = NewConfiguration(0xB001, 0x63);
        twelve->AddLotConfiguration(configuration);
        twelve->UpdateLot(occupant, 5, 5, 1, 2);
        cSC4NetworkLot* lot = twelve->GetNetworkLot(5, 5);
        CHECK(lot != nullptr);
        if (lot) CHECK(lot->GetConfiguration() == configuration);
        // A compatible piece leaves the lot alone.
        twelve->UpdateLot(occupant, 5, 5, 1, 2);
        CHECK(twelve->GetNetworkLot(5, 5) == lot);
        // An incompatible piece (zone 2 is not allowed) removes it; the re-insert finds no compatible
        // configuration, so no lot is created.
        local.propRemovals.clear();
        local.floraRemovals.clear();
        twelve->UpdateLot(occupant, 5, 5, 2, 2);
        CHECK(twelve->GetNetworkLot(5, 5) == nullptr);
        CHECK(local.propRemovals.size() == 1 && local.propRemovals[0] == std::make_pair(5, 5));
        CHECK(local.floraRemovals.size() == 1);
        // A lot built from the forced configuration is never re-evaluated.
        occupant->pieceId = 0;
        auto* forcedLot = new cSC4NetworkLot();
        forcedLot->AddRef();
        forcedLot->SetNetworkOccupant(occupant);
        // Build the lot through the public path with the forced configuration.
        twelve->SetLotConfigurationForNextPiece(configuration);
        twelve->DoNetworkOccupantInserted(occupant, 1, 2);
        cSC4NetworkLot* forced = twelve->GetNetworkLot(5, 5);
        CHECK(forced != nullptr);
        if (forced) {
            CHECK(forced->UsesNextPieceConfiguration());
            forced->SetUsesNextPieceConfiguration(true);
            twelve->UpdateLot(occupant, 5, 5, 2, 2);   // incompatible zone, but the lot is fixed
            CHECK(twelve->GetNetworkLot(5, 5) == forced);
        }
        forcedLot->Release();
        configuration->Release();
        A::MarkInited(*twelve);
        twelve->Shutdown();
        twelve->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 11. GetZoneTypeForNetwork and GetWealthForNetwork
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* thirteen = new cSC4NetworkLotManager();
        thirteen->AddRef();
        local.cellsX = 8;
        local.cellsZ = 6;
        // A zoned cell returns its own zone.
        local.zones[{2, 2}] = 6;
        CHECK(thirteen->GetZoneTypeForNetwork(2, 2) == 6);
        // An unzoned cell takes the highest-priority neighbour (the lowest rank in the table).
        local.zones[{2, 2}] = 0;
        local.zones[{1, 2}] = 3;
        CHECK(thirteen->GetZoneTypeForNetwork(2, 2) == 3);
        // Diagonals are consulted only when the best rank so far is 0, 7 or 14.
        local.zones[{1, 2}] = 0;
        local.zones[{1, 1}] = 9;                    // diagonal, rank 1
        CHECK(thirteen->GetZoneTypeForNetwork(2, 2) == 9);
        local.zones[{1, 2}] = 5;                    // orthogonal, rank 4 (not 0/7/14)
        CHECK(thirteen->GetZoneTypeForNetwork(2, 2) == 5);
        local.zones[{1, 2}] = 7;                    // rank 8, but 7 is in the 0x4081 set: diagonals
        CHECK(thirteen->GetZoneTypeForNetwork(2, 2) == 9);
        local.zones.clear();
        // Out-of-bounds neighbours are skipped (x = 0 skips the west column, z = 0 the north row).
        local.zones[{0, 0}] = 0;
        local.zones[{1, 0}] = 4;
        CHECK(thirteen->GetZoneTypeForNetwork(0, 0) == 4);

        // GetWealthForNetwork: the mode of the neighbouring lots' wealth.
        local.lots.clear();
        FakeLot* north = local.AddLot(2, 1);
        north->livable = false;
        north->wealth = 2;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 2);
        FakeLot* west = local.AddLot(1, 2);
        west->livable = false;
        west->wealth = 1;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 2);   // tie -> the higher wealth (Mac i386 0x0030c880)
        // A wealth must count at least as many lots as the lots without one: two wealth-0 lots outvote one wealth-1 lot.
        local.lots.clear();
        for (int x : {1, 3}) {
            FakeLot* poor = local.AddLot(x, 2);
            poor->livable = false;
            poor->wealth = 0;
        }
        FakeLot* one = local.AddLot(2, 1);
        one->livable = false;
        one->wealth = 1;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 0);
        FakeLot* another = local.AddLot(2, 3);
        another->livable = false;
        another->wealth = 1;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 1);   // 2 against 2: the wealth wins the tie
        // A wealth above 3 is counted as 0.
        local.lots.clear();
        FakeLot* big = local.AddLot(2, 1);
        big->livable = false;
        big->wealth = 9;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 0);
        // A livable empty lot takes its building's wealth profile.
        local.lots.clear();
        FakeLot* livable = local.AddLot(2, 1);
        livable->wealth = 0;
        livable->livable = true;
        livable->buildingType = 0x99;
        local.buildingWealth[0x99] = 3;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 3);
        CHECK(local.buildingWealthQueries.size() == 1 && local.buildingWealthQueries[0] == 0x99);
        // No orthogonal lot at all: the diagonals are scanned.
        local.lots.clear();
        FakeLot* diagonal = local.AddLot(1, 1);
        diagonal->livable = false;
        diagonal->wealth = 3;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 3);
        // One orthogonal lot (even wealth 0) stops the diagonal scan.
        FakeLot* orthogonal = local.AddLot(2, 1);
        orthogonal->livable = false;
        orthogonal->wealth = 0;
        CHECK(thirteen->GetWealthForNetwork(2, 2) == 0);
        thirteen->Init();   // Shutdown releases the lots only once initialized
        thirteen->Shutdown();
        thirteen->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 12. UpdateAvenueStyle: the connected run, its tallies and its style
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        auto* fourteen = new cSC4NetworkLotManager();
        fourteen->AddRef();
        local.cellsX = 8;
        local.cellsZ = 6;
        FakeOccupantObject west, middle, east;
        auto* pieceWest = local.AddOccupant(1, 2, &west);
        auto* pieceMiddle = local.AddOccupant(2, 2, &middle);
        auto* pieceEast = local.AddOccupant(3, 2, &east);
        pieceWest->anyFlags = pieceMiddle->anyFlags = pieceEast->anyFlags = 0x1040;
        pieceWest->networkFlags = pieceMiddle->networkFlags = pieceEast->networkFlags = 0x1040;
        pieceWest->edgeTypes = 0x010000;    // east edge only
        pieceMiddle->edgeTypes = 0x010001;  // west + east
        pieceEast->edgeTypes = 0x000001;    // west edge only
        local.zones[{1, 2}] = 3;
        local.zones[{2, 2}] = 3;
        local.zones[{3, 2}] = 3;
        A::AddUpdateCell(*fourteen, NetworkLotCellKey(2, 2));
        fourteen->UpdateAvenueStyle(2, 2);
        const std::uint32_t underTexture = kNetworkLotUnderTextureId[3 * 4 + 0];
        const std::uint8_t variation = kNetworkLotVariation[3 * 3 + 0];
        CHECK(pieceWest->underTexture == underTexture && pieceWest->variation == variation);
        CHECK(pieceMiddle->underTexture == underTexture && pieceMiddle->variation == variation);
        CHECK(pieceEast->underTexture == underTexture && pieceEast->variation == variation);
        CHECK(!A::HasUpdateCell(*fourteen, NetworkLotCellKey(2, 2)));
        // A piece with a different intersection flag blocks the walk and is left unstyled.
        pieceWest->underTexture = 0xffffffffu;
        pieceMiddle->underTexture = 0xffffffffu;
        pieceEast->underTexture = 0xffffffffu;
        pieceWest->intersection = true;
        fourteen->UpdateAvenueStyle(2, 2);
        CHECK(pieceWest->underTexture == 0xffffffffu);
        CHECK(pieceMiddle->underTexture == underTexture);
        CHECK(pieceEast->underTexture == underTexture);
        pieceWest->intersection = false;
        fourteen->Init();   // Shutdown releases the lots only once initialized
        fourteen->Shutdown();
        fourteen->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 13. DoMessage: all six branches
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        local.cellsX = 8;
        local.cellsZ = 6;
        local.areaSlope = 4.0f;
        auto* fifteen = new cSC4NetworkLotManager();
        fifteen->AddRef();
        auto* configuration = NewConfiguration(0xC001, 0x63);
        fifteen->AddLotConfiguration(configuration);
        FakeOccupantObject object;
        object.position[0] = 2.0f * 16.0f;
        object.position[2] = 2.0f * 16.0f;
        auto* occupant = local.AddOccupant(2, 2, &object, 0x63);
        occupant->networkFlags = 0x1000;
        occupant->anyFlags = 0x1000;
        local.zones[{2, 2}] = 1;

        // 0x89d3e9f3 with data1 != 0 refreshes the occupant's cell and creates its network lot.
        auto* usable = NewMessage(0x89d3e9f3);
        usable->SetIGZUnknown(occupant);
        usable->SetData1(1);
        local.findCalls.clear();
        CHECK(fifteen->DoMessage(usable));
        CHECK(local.findCalls.size() >= 1);
        cSC4NetworkLot* lot = fifteen->GetNetworkLot(2, 2);
        CHECK(lot != nullptr);
        if (lot) CHECK(lot->GetConfiguration() == configuration);
        CHECK(occupant->underTexture == kNetworkLotUnderTextureId[1 * 4 + 0]);
        CHECK(occupant->variation == kNetworkLotVariation[1 * 3 + 0]);
        CHECK((occupant->networkFlags & kNetworkLotUnderTextureFlag) != 0);
        usable->Release();

        // The same message with data1 == 0 removes the lot again.
        auto* removedByUsable = NewMessage(0x89d3e9f3);
        removedByUsable->SetIGZUnknown(occupant);
        removedByUsable->SetData1(0);
        local.propRemovals.clear();
        local.floraRemovals.clear();
        CHECK(fifteen->DoMessage(removedByUsable));
        CHECK(fifteen->GetNetworkLot(2, 2) == nullptr);
        CHECK(local.propRemovals.size() == 1 && local.propRemovals[0] == std::make_pair(2, 2));
        CHECK(local.floraRemovals.size() == 1);
        removedByUsable->Release();

        // 0x99ef1143: the occupant removed from the manager. A rail piece is ignored.
        auto* reinsert = NewMessage(0x89d3e9f3);
        reinsert->SetIGZUnknown(occupant);
        reinsert->SetData1(1);
        CHECK(fifteen->DoMessage(reinsert));
        CHECK(fifteen->GetNetworkLot(2, 2) != nullptr);
        reinsert->Release();
        occupant->networkFlags |= kNetworkLotRailFlag;
        auto* railRemoved = NewMessage(0x99ef1143);
        railRemoved->SetVoid1(occupant);
        CHECK(fifteen->DoMessage(railRemoved));
        CHECK(fifteen->GetNetworkLot(2, 2) != nullptr);        // ignored because of 0x200000
        railRemoved->Release();
        occupant->networkFlags = 0x1000;
        auto* notNetwork = NewMessage(0x99ef1143);
        occupant->anyFlags = 0x1000000;                        // not a network piece
        notNetwork->SetVoid1(occupant);
        CHECK(fifteen->DoMessage(notNetwork));
        CHECK(fifteen->GetNetworkLot(2, 2) != nullptr);
        occupant->anyFlags = 0x1000;
        notNetwork->Release();
        auto* removed = NewMessage(0x99ef1143);
        removed->SetVoid1(occupant);
        CHECK(fifteen->DoMessage(removed));
        CHECK(fifteen->GetNetworkLot(2, 2) == nullptr);
        removed->Release();

        // 0xa9b6c2d6 re-creates a lot's objects from its configuration.
        auto* objectConfiguration = NewConfiguration(0xC002, 0x64);
        cSC4LotConfigurationObject propObject;
        propObject.type = 1;
        propObject.value = 0x77;
        propObject.id = 0x42;
        objectConfiguration->GetObjectArray() = {propObject};
        local.objectIds[{1, 0x77}] = {0x33};
        fifteen->AddLotConfiguration(objectConfiguration);
        FakeOccupantObject object2;
        object2.position[0] = 3.0f * 16.0f;
        object2.position[2] = 3.0f * 16.0f;
        auto* occupant2 = local.AddOccupant(3, 3, &object2, 0x64);
        occupant2->networkFlags = 0x1000;
        occupant2->anyFlags = 0x1000;
        fifteen->DoNetworkOccupantInserted(occupant2, 1, 0);
        cSC4NetworkLot* lot2 = fifteen->GetNetworkLot(3, 3);
        CHECK(lot2 != nullptr);
        local.propCreates.clear();
        auto* recreate = NewMessage(0xa9b6c2d6);
        recreate->SetVoid1(lot2);
        CHECK(fifteen->DoMessage(recreate));
        CHECK(local.propCreates.size() == 1 && local.propCreates[0].first == 0x33);
        recreate->Release();

        // 0x07699455 marks the expanded rectangle (x0-1..x1+1, z0-1..z1+1) for a road refresh.
        std::int32_t rect[4] = {1, 1, 2, 2};
        local.findCalls.clear();
        auto* rectangle = NewMessage(0x07699455);
        rectangle->SetData2(reinterpret_cast<std::uint64_t>(rect));
        CHECK(fifteen->DoMessage(rectangle));
        {
            std::set<std::pair<int, int>> expected;
            for (int z = 0; z <= 3; ++z)
                for (int x = 0; x <= 3; ++x) expected.insert({x, z});
            CHECK(RefreshCells(local, kNetworkLotOccupantFlagMask) == expected);
        }
        rectangle->Release();

        // 0xe8bda2e7 marks the one-cell border around the lot. Quirk: cell (0,0) is always marked and
        // the first row starts at x = 1 (the ring's middle rows only mark the two side cells).
        FakeLot* changedLot = local.AddLot(2, 2);
        changedLot->rect[0] = 2;
        changedLot->rect[1] = 2;
        changedLot->rect[2] = 3;
        changedLot->rect[3] = 3;
        local.findCalls.clear();
        auto* lotChanged = NewMessage(0xe8bda2e7);
        lotChanged->SetIGZUnknown(changedLot);
        CHECK(fifteen->DoMessage(lotChanged));
        {
            std::set<std::pair<int, int>> expected;
            expected.insert({0, 0});
            for (int x = 1; x <= 4; ++x) expected.insert({x, 0});
            for (int z = 1; z <= 3; ++z) { expected.insert({1, z}); expected.insert({4, z}); }
            for (int x = 1; x <= 4; ++x) expected.insert({x, 4});
            CHECK(RefreshCells(local, kNetworkLotOccupantFlagMask) == expected);
        }
        lotChanged->Release();

        // 0x69247dc8 creates and removes a pothole.
        local.createVisualEffectCalls = 0;
        auto* damage = NewMessage(0x69247dc8);
        damage->SetData1(6);
        damage->SetData2(1);
        damage->SetData3(1);
        CHECK(fifteen->DoMessage(damage));
        CHECK(local.createVisualEffectCalls == 1);
        CHECK(A::Potholes(*fifteen) == 1);
        damage->SetData3(0);
        CHECK(fifteen->DoMessage(damage));
        CHECK(A::Potholes(*fifteen) == 0);
        damage->Release();

        A::MarkInited(*fifteen);
        fifteen->Shutdown();
        fifteen->Release();
        objectConfiguration->Release();
        configuration->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 14. DoRoadDamageChange: the pothole visual effect
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        local.cellsX = 8;
        local.cellsZ = 6;
        local.terrainAltitude = 1.5f;
        auto* sixteen = new cSC4NetworkLotManager();
        sixteen->AddRef();
        // An empty cell: the effect is named "pothole" and sits at the cell centre.
        local.createVisualEffectCalls = 0;
        local.filterFlags.clear();
        local.inserted.clear();
        sixteen->DoRoadDamageChange(6, 1, true);
        CHECK(local.createVisualEffectCalls == 1);
        CHECK(A::Potholes(*sixteen) == 1);
        INetworkLotVisualEffect* effect = A::Pothole(*sixteen, NetworkLotCellKey(6, 1));
        CHECK(effect != nullptr);
        if (effect) {
            auto* fake = static_cast<FakeEffect*>(effect);
            CHECK(fake->effectName == "pothole");
            CHECK(fake->starts == 1 && fake->stops == 0);
            CHECK(fake->object->position[0] == 6.0f * 16.0f + 8.0f);
            CHECK(fake->object->position[2] == 1.0f * 16.0f + 8.0f);
            CHECK(fake->object->position[1] == 1.5f);
        }
        CHECK(local.filterFlags.size() == 1 && local.filterFlags[0] == kNetworkLotOccupantFlagMask);
        CHECK(local.filters.size() == 1 && local.filters[0]->destroyed);   // AddRef'd then released
        CHECK(local.inserted.size() == 1);
        // A second damage message for the same cell does nothing.
        sixteen->DoRoadDamageChange(6, 1, true);
        CHECK(local.createVisualEffectCalls == 1);
        // A cell with a rail piece: "pothole_rail", the occupant altitude and the active-edge nudge.
        FakeOccupantObject object;
        object.position[0] = 6.0f * 16.0f;
        object.position[2] = 3.0f * 16.0f;
        auto* occupant = local.AddOccupant(6, 3, &object, 0);
        occupant->refs = 2;                          // one for the host, one for GetFirstOccupant
        occupant->networkFlags = 0x2002;             // (flags & 0x1fff) == 2
        occupant->altitude = 3.0f;
        occupant->activeEdges = 4;                   // east: +4 units in x
        local.firstOccupants[{6, 3}] = occupant;
        sixteen->DoRoadDamageChange(6, 3, true);
        INetworkLotVisualEffect* railEffect = A::Pothole(*sixteen, NetworkLotCellKey(6, 3));
        CHECK(railEffect != nullptr);
        if (railEffect) {
            auto* fake = static_cast<FakeEffect*>(railEffect);
            CHECK(fake->effectName == "pothole_rail");
            CHECK(fake->object->position[0] == 6.0f * 16.0f + 8.0f + 4.0f);
            CHECK(fake->object->position[2] == 3.0f * 16.0f + 8.0f);
            CHECK(fake->object->position[1] == 3.0f);
        }
        local.firstOccupants.erase({6, 3});
        // Removing a pothole stops the effect, removes the occupant and releases the effect.
        local.removed.clear();
        FakeEffect* plain = static_cast<FakeEffect*>(effect);
        sixteen->DoRoadDamageChange(6, 1, false);
        CHECK(A::Potholes(*sixteen) == 1);                    // the rail one is still there
        CHECK(plain->stops == 1);
        CHECK(plain->destroyed);
        CHECK(local.removed.size() == 1 && local.removed[0].second == 0);
        sixteen->DoRoadDamageChange(6, 3, false);
        CHECK(A::Potholes(*sixteen) == 0);
        sixteen->DoRoadDamageChange(6, 3, false);             // no pothole: no-op
        CHECK(local.removed.size() == 2);
        sixteen->Init();   // Shutdown releases the lots only once initialized
        sixteen->Shutdown();
        sixteen->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 15. Shutdown: releases the lots, configurations and potholes (the lot map is left populated)
    // ================================================================================================
    {
        Host local;
        SetNetworkLotManagerHost(&local);
        local.cellsX = 8;
        local.cellsZ = 6;
        local.areaSlope = 4.0f;
        auto* seventeen = new cSC4NetworkLotManager();
        seventeen->AddRef();
        CHECK(seventeen->Init());
        auto* configuration = NewConfiguration(0xD001, 0x65);
        seventeen->AddLotConfiguration(configuration);
        FakeOccupantObject object;
        object.position[0] = 1.0f * 16.0f;
        object.position[2] = 1.0f * 16.0f;
        auto* occupant = local.AddOccupant(1, 1, &object, 0x65);
        occupant->networkFlags = 0x1000;
        occupant->anyFlags = 0x1000;
        seventeen->DoNetworkOccupantInserted(occupant, 1, 0);
        cSC4NetworkLot* lot = seventeen->GetNetworkLot(1, 1);
        CHECK(lot != nullptr);
        seventeen->DoRoadDamageChange(5, 4, true);
        CHECK(A::Potholes(*seventeen) == 1);
        CHECK(local.messages.removed.empty());
        CHECK(seventeen->Shutdown());
        CHECK(local.messages.removed.size() == 6);
        for (std::size_t i = 0; i < local.messages.removed.size() && i < 6; ++i) {
            CHECK(local.messages.removed[i].first == static_cast<cIGZMessageTarget2*>(seventeen));
            CHECK(local.messages.removed[i].second == kNetworkLotManagerMessages[i]);
        }
        CHECK(!A::Inited(*seventeen));
        CHECK(A::ConfigBuckets(*seventeen) == 0);
        CHECK(A::Potholes(*seventeen) == 0);
        // Quirk: Shutdown releases every lot but does not clear the registry map, so the map keeps
        // dangling pointers and a second Init would read the freed lots (never done by the game, which
        // destroys the object after Shutdown).
        CHECK(A::LotCount(*seventeen) == 1);
        CHECK(A::Lot(*seventeen, NetworkLotCellKey(1, 1)) == lot);
        // A manager with an empty registry can be re-initialised after Shutdown.
        auto* restarted = new cSC4NetworkLotManager();
        restarted->AddRef();
        local.messages.added.clear();
        local.messages.removed.clear();
        CHECK(restarted->Init());
        CHECK(local.messages.added.size() == 6);
        CHECK(restarted->Shutdown());
        CHECK(local.messages.removed.size() == 6);
        restarted->Release();
        configuration->Release();
        seventeen->Release();
        SetNetworkLotManagerHost(&host);
    }

    // ================================================================================================
    // 16. the real cSCNetworkLot / cSC4NetworkLotConfiguration records the manager stores
    // ================================================================================================
    {
        auto* configuration = new cSC4NetworkLotConfiguration();
        configuration->AddRef();
        CHECK(configuration->GetID() == 0 && configuration->GetNetworkPieceId() == 0);
        CHECK(configuration->TileNeighborhood() == 0x0f0f0f0f);
        CHECK(configuration->Field58() == 4);
        CHECK(configuration->MinimumSlope() == 0.0f && configuration->MaximumSlope() == 64.0f);
        CHECK(configuration->RotationMask() == 0x0f && configuration->FlipMode() == 0);
        CHECK(configuration->ZoneTypeBits().size() == 16 && configuration->WealthTypeBits().size() == 4);
        FakeOccupantObject object;
        object.refs = 1;   // the lot takes and releases its own reference
        auto* lot = new cSC4NetworkLot();
        lot->AddRef();
        CHECK(lot->GetNetworkOccupant() == nullptr);
        CHECK(lot->GetConfiguration() == nullptr);
        CHECK(lot->GetConfigurationId() == 0 && !lot->UsesNextPieceConfiguration());
        CHECK(lot->GetLotConfigurationObjectArray().empty());
        lot->SetConfiguration(configuration);
        CHECK(lot->GetConfiguration() == configuration);
        lot->SetConfigurationId(0x42);
        lot->SetUsesNextPieceConfiguration(true);
        CHECK(lot->GetConfigurationId() == 0x42 && lot->UsesNextPieceConfiguration());
        lot->SetNetworkOccupant(&object);
        CHECK(lot->GetNetworkOccupant() == &object);
        lot->SetNetworkOccupant(nullptr);
        CHECK(lot->GetNetworkOccupant() == nullptr);
        lot->Release();
        configuration->Release();
    }

    manager->Shutdown();
    manager->Release();

    std::printf("sc4_networklotmanager: %s\n", g_fail == 0 ? "ok" : "FAILED");
    return g_fail == 0 ? 0 : 1;
}
