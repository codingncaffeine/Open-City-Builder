// cSC4WatercraftManager — unit test with fake seams. Exercises the identity/lifecycle, the four registries, the docking
// permission state machine, the position/collision queries, the ambient spawner, the random creation point and the
// version-6 stream round trip. The fakes stand in for the parent manager (IWatercraftManagerBase) and the engine
// globals (IWatercraftManagerHost); the assertions mark the quirks kept from the binary with "quirk".
#include <array>
#include <bit>
#include <list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/WatercraftManager.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-3f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), \
                static_cast<double>(b)); ++failures; } } while (0)

namespace {

constexpr float kCellSize = 16.0f;
constexpr int kCitySize = 8;

// ---- a property holder with a set of ids whose GetProperty(u32) answers one configured value ------------------------
struct FakePropertyHolder final : cISCPropertyHolder {
    int refs = 1;
    std::set<std::uint32_t> ids;
    std::uint32_t valueId = 0;
    std::uint32_t value = 0;
    cSCProperty* property = nullptr;   // when set, GetProperty(id) returns it (cSCPropertyHelp reads it)

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool HasProperty(std::uint32_t id) const override { return ids.count(id) != 0; }
    bool GetPropertyList(cIGZUnknownList**) const override { return false; }

    cISCProperty* GetProperty(std::uint32_t id) const override {
        return property && id == valueId ? property : nullptr;
    }
    bool GetProperty(std::uint32_t id, std::uint32_t& out) const override {
        if (id != valueId) return false;
        out = value;
        return true;
    }
    bool GetProperty(std::uint32_t, cIGZString&) const override { return false; }
    bool GetProperty(std::uint32_t, std::uint32_t, void**) const override { return false; }
    bool GetProperty(std::uint32_t, void*, std::uint32_t&) const override { return false; }
    void AddProperty(cISCProperty*, bool) override {}
    void AddProperty(std::uint32_t, const cIGZVariant*, bool) override {}
    void AddProperty(std::uint32_t, std::uint32_t, bool) override {}
    void AddProperty(std::uint32_t, const cIGZString&, bool) override {}
    void AddProperty(std::uint32_t, cIGZUnknown*, bool) override {}
    void AddProperty(std::uint32_t, const void*, std::uint32_t, bool) override {}
    void CopyAddProperty(cISCProperty*, bool) override {}
    bool RemoveProperty(std::uint32_t, bool) override { return false; }
    void RemoveAllProperties(bool) override {}
    void EnumProperties(void (*)(cISCProperty*, void*), void*) const override {}
    void EnumProperties(bool (*)(cISCProperty*, void*), void (*)(cISCProperty*, void*), void*) const override {}
    void CompactProperties() override {}
};

// ---- occupant --------------------------------------------------------------------------------------------------------
struct FakeOccupant final : cISC4Occupant {
    int refs = 1;
    int releases = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float minXyz[3] = {-1.0f, 0.0f, -2.0f};
    float maxXyz[3] = {1.0f, 1.0f, 2.0f};
    std::set<std::uint32_t> groups;
    std::uint32_t flags = 4;
    FakePropertyHolder* holder = nullptr;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { ++releases; return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return 0x12345678; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof position); }
    void GetBoundingBox(float* minOut, float* maxOut) override {
        std::memcpy(minOut, minXyz, sizeof minXyz);
        std::memcpy(maxOut, maxXyz, sizeof maxXyz);
    }
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = SC4CellRect{0, 0, 1, 1}; }
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
    bool IsOccupantGroup(std::uint32_t group) const override { return groups.count(group) != 0; }
    void AddOccupantGroup(std::uint32_t group) override { groups.insert(group); }
    bool GetOccupantGroups(std::set<std::uint32_t>& out) const override { out = groups; return !groups.empty(); }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t& tag) const override { tag = 0; return true; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t value) override { flags = value; }
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return flags; }
};

// ---- the surface-water body object -----------------------------------------------------------------------------------
struct FakeBodyOfWater final : IWatercraftBodyOfWater {
    std::map<std::pair<int, int>, std::int16_t> bodies;
    std::vector<std::pair<int, int>> queries;
    std::int16_t BodyOfWaterAt(int cellX, int cellZ) override {
        queries.emplace_back(cellX, cellZ);
        auto it = bodies.find({cellX, cellZ});
        return it == bodies.end() ? 0 : it->second;
    }
};

// ---- the parent manager seam ----------------------------------------------------------------------------------------
struct FakeBase final : IWatercraftManagerBase {
    bool initialized = false;
    int initCalls = 0, shutdownCalls = 0, destroyCalls = 0, readInternalCalls = 0;
    int maxCount = -1, minZoom = -100;
    bool postCityInitPending = false;
    bool doMessageResult = false;
    bool simulatePaused = false;
    bool drivingMode = false;
    bool canCreateGroup = true;
    int tickCalls = 0;
    cRZRandomSuper random{12345u};
    float tuningMaxCount = 0.0f;
    float retireOpacity = 0.0f;
    std::vector<std::uint32_t> messages;
    std::vector<std::uint32_t> queriedIids;

    bool Initialized() const override { return initialized; }
    bool Init() override { ++initCalls; initialized = true; return true; }
    bool Shutdown() override { ++shutdownCalls; initialized = false; return true; }
    bool DoMessage(cIGZMessage2*) override { return doMessageResult; }
    bool Write(cIGZOStream& stream) const override { stream.SetUint32(0x5a5a5a5a); return true; }
    bool Read(cIGZIStream& stream) override {
        std::uint32_t marker = 0;
        stream.GetUint32(marker);
        return marker == 0x5a5a5a5a;
    }
    bool ReadInternal(cIGZIStream&, std::uint16_t) override { ++readInternalCalls; return true; }
    void DoAnimationTick(std::uint32_t, float) override { ++tickCalls; }
    bool CalculateSimSpeedModifier() override { return true; }
    bool SimSpeedPaused() const override { return simulatePaused; }
    bool IsDrivingMode() const override { return drivingMode; }
    bool CanCreateGroup(std::uint32_t) const override { return canCreateGroup; }
    void SetMaxCount(int count) override { maxCount = count; }
    void SetMinZoom(int zoom) override { minZoom = zoom; }
    void DestroyAutomata() override { ++destroyCalls; }
    bool PostCityInitPending() const override { return postCityInitPending; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        queriedIids.push_back(iid);
        *out = nullptr;
        return false;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cRZRandomSuper& Random() override { return random; }
    float TuningMaxCountField() const override { return tuningMaxCount; }
    float RetireOpacityDuration() const override { return retireOpacity; }
};

// ---- exemplar (only the property holder query is used) ---------------------------------------------------------------
struct FakeExemplar final : cISCResExemplar {
    FakePropertyHolder* holder = nullptr;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Write(cIGZOStream&) const override { return false; }
    bool Read(cIGZIStream&) override { return false; }
    std::uint32_t GetGZCLSID() const override { return 0; }
    cISCPropertyHolder* AsISCPropertyHolder() override { return holder; }
    bool IsPropertyLocal(std::uint32_t) override { return false; }
    cISCResExemplarCohort* GetCohort() override { return nullptr; }
    void SetCohort(cISCResExemplarCohort*, bool) override {}
    bool GetKey(cGZPersistResourceKey&) override { return false; }
    bool SetKey(const cGZPersistResourceKey&) override { return false; }
    void CompactProperties() override {}
    void GenerateCachedMap() override {}
};

// ---- group template --------------------------------------------------------------------------------------------------
struct FakeGroup final : IAutomatonGroupTemplate {
    std::uint32_t id = 0;
    std::uint32_t GroupId() const override { return id; }
    IAutomatonGroupTemplate* Parent() const override { return nullptr; }
    float SpeedMin() const override { return 0.0f; }
    float SpeedMax() const override { return 1.0f; }
    void GetGroupHierarchyDepth() override {}
};

// ---- the engine globals ----------------------------------------------------------------------------------------------
struct FakeHost final : IWatercraftManagerHost {
    FakeBase* base = nullptr;
    int rect[4] = {0, 0, kCitySize, kCitySize};
    std::map<std::pair<int, int>, cSC4Lot*> lotsByCell;
    std::map<std::pair<int, int>, bool> waterCells;
    std::map<cISC4Occupant*, int> facings;
    FakeBodyOfWater body;
    FakeExemplar* exemplar = nullptr;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> notifications;
    int createCalls = 0;
    std::uint32_t lastCreateGroup = 0;
    cS3DVector3 lastCreatePosition;
    cS3DVector3 createdBoatPosition;
    cSC4WatercraftManager* manager = nullptr;

    // streams
    std::vector<cSC4Lot*> lotTable;
    std::vector<cISC4Occupant*> occupantTable;

    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / kCellSize));
        cellZ = static_cast<int>(std::floor(z / kCellSize));
    }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override {
        x = (static_cast<float>(cellX) + 0.5f) * kCellSize;
        z = (static_cast<float>(cellZ) + 0.5f) * kCellSize;
    }
    bool CellInCity(int cellX, int cellZ) override {
        return cellX >= 0 && cellZ >= 0 && cellX < kCitySize && cellZ < kCitySize;
    }
    bool PointInCity(float x, float z) override {
        lastPointInCity = {x, z};
        const float size = pointInCityWorld ? static_cast<float>(kCitySize) * kCellSize : static_cast<float>(kCitySize);
        return x >= 0.0f && z >= 0.0f && x < size && z < size;
    }
    bool pointInCityWorld = false;   // the GetRandomCreationPoint checks pass cell indices; a world-placed boat needs world units
    void CityCellCount(int& sizeX, int& sizeZ) override { sizeX = kCitySize; sizeZ = kCitySize; }
    float CellSize() override { return kCellSize; }
    cSC4Lot* LotAtPosition(const float* xyz) override {
        int cellX = 0, cellZ = 0;
        WorldToCell(xyz[0], xyz[2], cellX, cellZ);
        auto it = lotsByCell.find({cellX, cellZ});
        if (it == lotsByCell.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    cSC4Lot* LotAtCell(int cellX, int cellZ) override {
        auto it = lotsByCell.find({cellX, cellZ});
        if (it == lotsByCell.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    float TerrainHeight(float, float) override { return 7.0f; }
    IWatercraftBodyOfWater* BodyOfWater() override { return &body; }
    int lastWaterRadius = -1;
    bool IsWaterCell(int cellX, int cellZ, int radius) override {
        lastWaterRadius = radius;
        auto it = waterCells.find({cellX, cellZ});
        return it != waterCells.end() && it->second;
    }
    bool PropOccupantFacing(cISC4Occupant* occupant, int& facing) override {
        auto it = facings.find(occupant);
        if (it == facings.end()) return false;
        facing = it->second;
        return true;
    }
    void VisibleCellRect(int out[4]) override { std::memcpy(out, rect, sizeof rect); }
    cISCResExemplar* GetAutomataTuningExemplar() override {
        if (exemplar) exemplar->AddRef();
        return exemplar;
    }
    bool AddMessageNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        notifications.emplace_back(1, type);
        return true;
    }
    bool RemoveMessageNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        notifications.emplace_back(0, type);
        return true;
    }
    bool WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) override {
        if (!object) { stream.SetUint32(0xffffffffu); return true; }
        auto* occupant = static_cast<cISC4Occupant*>(object);
        for (std::size_t i = 0; i < occupantTable.size(); ++i) {
            if (occupantTable[i] != occupant) continue;
            stream.SetUint32(static_cast<std::uint32_t>(i));
            return true;
        }
        stream.SetUint32(0xffffffffu);
        return true;
    }
    cIGZUnknown* ReadSerializable(cIGZIStream& stream, std::uint32_t iid) override {
        if (iid != kGZIID_cISC4Occupant) return nullptr;
        std::uint32_t index = 0xffffffffu;
        stream.GetUint32(index);
        if (index >= occupantTable.size()) return nullptr;
        occupantTable[index]->AddRef();
        return occupantTable[index];
    }
    bool WriteLot(cIGZOStream& stream, cSC4Lot* lot) override {
        if (!lot) { stream.SetUint32(0xffffffffu); return true; }
        for (std::size_t i = 0; i < lotTable.size(); ++i) {
            if (lotTable[i] != lot) continue;
            stream.SetUint32(static_cast<std::uint32_t>(i));
            return true;
        }
        stream.SetUint32(0xffffffffu);
        return true;
    }
    cSC4Lot* ReadLot(cIGZIStream& stream) override {
        std::uint32_t index = 0xffffffffu;
        stream.GetUint32(index);
        if (index >= lotTable.size()) return nullptr;
        lotTable[index]->AddRef();
        return lotTable[index];
    }
    cSC4Watercraft* CreateWatercraft(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                     const cGZPersistResourceKey&, cIGZMessageTarget2*, std::uint8_t,
                                     const cS3DVector3*) override {
        (void)flags;
        ++createCalls;
        lastCreateGroup = group;
        lastCreatePosition = position;
        auto* boat = new cSC4Watercraft();
        boat->AddRef();
        boat->SetPosition(&position.x);
        boat->SetRadius(1.0f);
        if (manager) manager->AddAutomaton(boat);
        boat->Release();
        return boat;
    }
    cS3DVector3 lastPointInCity;
    // the traffic simulator, the terrain's sea level and the neighbor connections
    std::list<FerryRouteInfo> inUse;
    std::vector<std::pair<int, int>> connections;
    std::vector<std::uint8_t> waterRoute;
    std::vector<std::array<int, 4>> waterRouteAsks;
    void FerryRoutesInUse(std::list<FerryRouteInfo>& out) override { out = inUse; }
    bool GetWaterRoute(int x0, int z0, int x1, int z1, std::vector<std::uint8_t>& route) override {
        waterRouteAsks.push_back({x0, z0, x1, z1});
        if (waterRoute.empty() || x1 != connections.back().first) return false;
        route = waterRoute;
        return true;
    }
    float SeaLevel() override { return 250.0f; }
    void NeighborConnectionCells(std::uint32_t type, std::vector<std::pair<int, int>>& out) override {
        if (type == 9) out = connections;
    }
};

// cSC4Lot helpers -----------------------------------------------------------------------------------------------------
// The test keeps one reference of its own on every lot it creates (the manager registries hold another one).
cSC4Lot* MakeLot(int x0, int z0, int sizeX, int sizeZ) {
    auto* lot = new cSC4Lot(x0, z0, sizeX, sizeZ, 0);
    lot->AddRef();
    return lot;
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------------------------------------------------
    // identity / lifecycle
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.GetGZCLSID() == 0xa98f9d28);
        CHECK(manager.TerminalCount() == 0);
        CHECK(manager.MarinaOccupantCount() == 0);
        CHECK(manager.MarinaLotCount() == 0);
        CHECK(manager.AmbientBoatCount() == 0);
        CHECK(manager.BoatCount() == 0);
        CHECK(!manager.RoutesChanged());
        CHECK(manager.RefCount() == 0);

        void* out = nullptr;
        CHECK(manager.QueryInterface(0xa98f9d28, &out));
        CHECK(out == &manager);
        CHECK(manager.RefCount() == 1);
        CHECK(!manager.QueryInterface(0x12345678, &out));
        CHECK(base.queriedIids.size() == 1 && base.queriedIids[0] == 0x12345678);

        // Init registers the ferry-route notification, seeds the max count, then the tuning reader overrides it.
        base.tuningMaxCount = std::bit_cast<float>(4);   // the tuning slot holds the count as raw 32 bits
        CHECK(manager.Init());
        CHECK(base.initCalls == 1);
        CHECK(host.notifications.size() == 1 && host.notifications[0].first == 1 &&
              host.notifications[0].second == kMessageWatercraftRoutesChanged);
        CHECK(base.maxCount == 4);
        CHECK(base.minZoom == -100);   // no tuning exemplar -> the min zoom is left alone
        CHECK(manager.Init());         // already initialised: nothing happens again
        CHECK(base.initCalls == 1);

        // DoMessage: the ferry-route notification only sets the flag; the parent result is OR-ed in.
        auto* message = new cRZMessage2Standard();
        message->SetType(kMessageWatercraftRoutesChanged);
        CHECK(manager.DoMessage(message));
        CHECK(manager.RoutesChanged());
        delete message;   // the routes listener does not take a reference
        base.doMessageResult = true;
        auto* other = new cRZMessage2Standard();
        other->SetType(0x1234);
        CHECK(manager.DoMessage(other));
        base.doMessageResult = false;
        delete other;

        // Shutdown removes the notification and clears everything.
        CHECK(manager.Shutdown());
        CHECK(base.shutdownCalls == 1);
        CHECK(host.notifications.size() == 2 && host.notifications[1].first == 0);
    }

    // Init also destroys the automata when the base says a post-city-init is pending.
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        base.postCityInitPending = true;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.Init());
        CHECK(base.destroyCalls == 1);
    }

    // HandleTuningParametersChanged with an exemplar: the min zoom is the property minus one.
    {
        FakeBase base;
        FakeHost host;
        FakePropertyHolder holder;
        FakeExemplar exemplar;
        host.base = &base;
        holder.ids = {0x4998c91b};
        holder.valueId = 0x4998c91b;
        cSCProperty minZoomProperty(0x4998c91b);
        minZoomProperty.SetPropertyValue(cRZVariant(static_cast<std::uint8_t>(3)));
        holder.property = &minZoomProperty;
        exemplar.holder = &holder;
        host.exemplar = &exemplar;
        base.tuningMaxCount = std::bit_cast<float>(1);
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.HandleTuningParametersChanged());
        CHECK(base.maxCount == 1);
        CHECK(base.minZoom == 2);
        CHECK(exemplar.refs == 1);   // the manager dropped its reference
    }

    // The deleting release: the last reference shuts the manager down.
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        auto* manager = new cSC4WatercraftManager();
        manager->SetBase(&base);
        manager->SetHost(&host);
        CHECK(manager->Init());
        manager->AddRef();                  // 0 -> 1
        manager->AddRef();                  // 1 -> 2
        CHECK(manager->Release() == 1);     // 2 -> 1: not deleted yet
        CHECK(base.shutdownCalls == 0);
        CHECK(manager->Release() == 0);     // 1 -> 0: Shutdown + delete
        CHECK(base.shutdownCalls == 1);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // registries + docking state machine
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        FakePropertyHolder terminalHolder, marinaBuildingHolder, marinaLotHolder, noAutomataHolder;
        terminalHolder.ids = {kPropertyWatercraftFerryTerminal};
        marinaBuildingHolder.ids = {kMarinaBuildingProperties[3]};
        marinaLotHolder.ids = {kPropertyWatercraftMarinaLot};
        noAutomataHolder.ids = {kPropertyNoAutomata};

        FakeOccupant terminal, marinaBuilding, marinaLot, filteredLot;
        terminal.holder = &terminalHolder;
        terminal.position[0] = 10.0f;
        terminal.position[2] = 10.0f;
        marinaBuilding.holder = &marinaBuildingHolder;
        marinaLot.holder = &marinaLotHolder;
        marinaLot.position[0] = 30.0f;
        marinaLot.position[2] = 30.0f;
        filteredLot.holder = &noAutomataHolder;

        cSC4Lot* terminalLot = MakeLot(0, 0, 1, 1);
        cSC4Lot* marinaLotObject = MakeLot(1, 1, 1, 1);
        host.lotsByCell[{0, 0}] = terminalLot;
        host.lotsByCell[{1, 1}] = marinaLotObject;
        host.facings[&terminal] = 0;
        host.facings[&marinaBuilding] = 1;

        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.Init());

        manager.HandleOccupantInserted(&terminal);
        CHECK(manager.TerminalCount() == 1);
        CHECK(manager.Terminals().begin()->second.occupant.get() == &terminal);
        manager.HandleOccupantInserted(&terminal);   // a known terminal lot is left alone
        CHECK(manager.TerminalCount() == 1);

        // Dock / wait permissions: one boat per slot, a foreign Done* call leaves the slot alone.
        cSC4Watercraft* boatA = new cSC4Watercraft();
        cSC4Watercraft* boatB = new cSC4Watercraft();
        boatA->AddRef();
        boatB->AddRef();
        CHECK(!manager.RequestPermissionToDock(nullptr, boatA));
        CHECK(manager.RequestPermissionToDock(terminalLot, nullptr));   // quirk: a null boat still succeeds
        CHECK(manager.RequestPermissionToDock(terminalLot, boatA));
        CHECK(!manager.RequestPermissionToDock(terminalLot, boatB));
        manager.DoneDocking(terminalLot, boatB);                        // wrong boat: nothing happens
        CHECK(manager.Terminals().begin()->second.dockBoat.get() == boatA);
        manager.DoneDocking(terminalLot, boatA);
        CHECK(manager.Terminals().begin()->second.dockBoat.get() == nullptr);
        CHECK(manager.RequestPermissionToWait(terminalLot, boatB));
        CHECK(!manager.RequestPermissionToWait(terminalLot, boatA));
        manager.DoneQueueing(terminalLot, boatA);
        CHECK(manager.Terminals().begin()->second.waitBoat.get() == boatB);
        manager.DoneQueueing(terminalLot, boatB);
        CHECK(manager.Terminals().begin()->second.waitBoat.get() == nullptr);

        // The fleet counts the boats read.
        std::int32_t docks = 0, queues = 0;
        manager.FleetCounts(docks, queues);
        CHECK(docks == 1);
        CHECK(queues == 0);

        // A ferry route whose boat is faded out when the terminal occupant goes away.
        FerryRouteInformation route;
        route.boat = boatA;
        manager.Terminals().begin()->second.routes.push_back(route);   // (test-only direct edit)
        manager.HandleOccupantRemoved(&terminal);
        CHECK(manager.TerminalCount() == 0);
        CHECK(boatA->GetBehavior() == kWatercraftStateRemoveFade);   // PushState(0x49, ..., -1)

        // The marina/seaport building registry (occupant -> boat); removal fades the boat.
        manager.HandleOccupantInserted(&marinaBuilding);
        CHECK(manager.MarinaOccupantCount() == 1);
        manager.MarinaOccupants().begin()->second = boatB;   // (test-only: pretend CreateMarinaBoat ran)
        manager.HandleOccupantRemoved(&marinaBuilding);
        CHECK(manager.MarinaOccupantCount() == 0);
        CHECK(boatB->GetBehavior() == kWatercraftStateRemoveFade);

        // The marina terminal lot registry ignores the prop position query result (no facing configured).
        manager.HandleOccupantInserted(&marinaLot);
        CHECK(manager.MarinaLotCount() == 1);
        manager.HandleOccupantRemoved(&marinaLot);
        CHECK(manager.MarinaLotCount() == 0);

        manager.Shutdown();
        boatA->Release();
        boatB->Release();
        terminalLot->Release();
        marinaLotObject->Release();
    }

    // DoMessage routes the two occupant notifications into the watercraft handlers.
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        FakePropertyHolder holder;
        holder.ids = {kPropertyWatercraftFerryTerminal};
        FakeOccupant terminal;
        terminal.holder = &holder;
        cSC4Lot* lot = MakeLot(0, 0, 1, 1);
        host.lotsByCell[{0, 0}] = lot;
        host.facings[&terminal] = 0;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.Init());
        auto* inserted = new cRZMessage2Standard();
        inserted->SetType(kMessageOccupantInserted);
        inserted->SetVoid1(&terminal);
        CHECK(manager.DoMessage(inserted));   // the QueryInterface/Release pair deletes the message
        CHECK(manager.TerminalCount() == 1);
        auto* removed = new cRZMessage2Standard();
        removed->SetType(kMessageOccupantRemoved);
        removed->SetVoid1(&terminal);
        CHECK(manager.DoMessage(removed));
        CHECK(manager.TerminalCount() == 0);
        manager.Shutdown();
        lot->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // position queries
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.Init());

        cSC4Watercraft* boatA = new cSC4Watercraft();
        cSC4Watercraft* boatB = new cSC4Watercraft();
        boatA->AddRef();
        boatB->AddRef();
        boatA->SetRadius(2.0f);
        boatB->SetRadius(2.0f);
        const float positionA[3] = {100.0f, 5.0f, 100.0f};
        const float positionB[3] = {100.0f, 5.0f, 140.0f};
        boatA->SetPosition(positionA);
        boatB->SetPosition(positionB);
        manager.AddAutomaton(boatA);
        manager.AddAutomaton(boatB);
        boatA->Release();
        boatB->Release();
        CHECK(manager.BoatCount() == 2);

        // quirk: the search radius is twice the boat radius.
        cS3DVector3 query{100.0f, 5.0f, 103.8f};
        CHECK(manager.LookForBoatsAtPosition(query, nullptr) == boatA);
        query.z = 104.2f;
        CHECK(manager.LookForBoatsAtPosition(query, nullptr) == nullptr);
        query.z = 100.0f;
        CHECK(manager.LookForBoatsAtPosition(query, boatA) == nullptr);

        // CollisionDetection: no model instances in the port, so the OBBs are invalid and no hit comes back.
        CHECK(manager.CollisionDetection(nullptr, query, 1.0f) == nullptr);
        CHECK(manager.CollisionDetection(boatA, query, 1.0f) == nullptr);

        // ClearToLaunchBoat: water + no boat at the cell centre.
        host.waterCells[{2, 3}] = true;
        host.waterCells[{3, 3}] = true;
        CHECK(!manager.ClearToLaunchBoat(0, 0, 0));   // not water
        CHECK(manager.ClearToLaunchBoat(2, 3, 0));    // water, boats far away
        const float near[3] = {2.5f * kCellSize, 7.0f, 3.5f * kCellSize};
        boatA->SetPosition(near);
        CHECK(!manager.ClearToLaunchBoat(2, 3, 0));
        boatA->SetPosition(positionA);

        // GetPropPositionAndOrientation: position height from the terrain, direction from the facing table.
        FakeOccupant prop;
        prop.position[0] = 50.0f;
        prop.position[2] = 60.0f;
        prop.minXyz[0] = -1.0f; prop.minXyz[1] = 0.0f; prop.minXyz[2] = -2.0f;
        prop.maxXyz[0] = 1.0f;  prop.maxXyz[1] = 1.0f; prop.maxXyz[2] = 2.0f;
        host.facings[&prop] = 0;
        cS3DVector3 position{};
        cS3DVector3 direction{};
        CHECK(manager.GetPropPositionAndOrientation(&prop, position, direction, false));
        CHECK_NEAR(position.x, 50.0f);
        CHECK_NEAR(position.y, 7.0f);
        CHECK_NEAR(position.z, 60.0f - 360.0f);   // delta = min.z - max.z = -4, scale 90, direction +z
        CHECK_NEAR(direction.x, 0.0f);
        CHECK_NEAR(direction.z, 1.0f);
        host.facings[&prop] = 1;   // facing (-1, 0, 0): the x extent is the dominant one
        CHECK(manager.GetPropPositionAndOrientation(&prop, position, direction, false));
        CHECK_NEAR(position.x, 50.0f + 180.0f);   // min.x - max.x = -2, scale 90, direction -x
        CHECK_NEAR(direction.x, -1.0f);
        host.facings[&prop] = 0;
        CHECK(manager.GetPropPositionAndOrientation(&prop, position, direction, true));   // offset variant
        CHECK_NEAR(position.z, 60.0f);   // quirk: the offset variant only moves x
        CHECK_NEAR(position.x, 50.0f - 4.0f / 3.0f);
        FakeOccupant unknownProp;
        CHECK(!manager.GetPropPositionAndOrientation(&unknownProp, position, direction, false));
        CHECK(!manager.GetPropPositionAndOrientation(nullptr, position, direction, false));

        // GetLotBodyOfWater: the four corners are queried as (z, x) and the first wet one wins.
        cSC4Lot* lot = MakeLot(1, 2, 2, 2);   // rect (1,2)-(2,3)
        host.body.bodies[{2, 1}] = 0;
        host.body.bodies[{2, 2}] = 7;
        CHECK(manager.GetLotBodyOfWater(lot) == 7);
        CHECK(host.body.queries.size() == 2);
        CHECK(host.body.queries[0] == std::make_pair(2, 1));   // quirk: (z0, x0)
        CHECK(host.body.queries[1] == std::make_pair(2, 2));
        CHECK(manager.GetLotBodyOfWater(nullptr) == 0);

        // BoatPriority: ferry group 0, large 1, medium 2, the rest 3.
        FakeGroup ferryGroup, plainGroup;
        ferryGroup.id = kWatercraftManagerGroupFerryA;
        plainGroup.id = 0x4401;
        auto* priorityBoat = new cSC4Watercraft();
        priorityBoat->AddRef();
        CHECK(manager.BoatPriority(nullptr) == 3);
        priorityBoat->SetAutomataGroup(&ferryGroup);
        CHECK(manager.BoatPriority(priorityBoat) == 0);
        priorityBoat->SetAutomataGroup(&plainGroup);
        CHECK(manager.BoatPriority(priorityBoat) == 3);
        priorityBoat->AddOccupantGroup(kWatercraftGroupMediumBoat);
        CHECK(manager.BoatPriority(priorityBoat) == 2);
        priorityBoat->AddOccupantGroup(kWatercraftGroupLargeBoat);   // the large check comes first
        CHECK(manager.BoatPriority(priorityBoat) == 1);
        priorityBoat->Release();

        // AddAutomaton(null) creates a cSC4Watercraft; RemoveAutomaton(force) drops it again.
        const int before = manager.BoatCount();
        CHECK(manager.AddAutomaton(nullptr));
        CHECK(manager.BoatCount() == before + 1);
        cSC4Watercraft* last = manager.Boats().front().get();
        CHECK(manager.RemoveAutomaton(last, false));   // retire: the boat stays registered
        CHECK(manager.BoatCount() == before + 1);
        CHECK(last->GetMarkedForRemove());
        CHECK(manager.RemoveAutomaton(last, true));
        CHECK(manager.BoatCount() == before);
        CHECK(manager.BoatCount() >= 2);
        manager.Shutdown();
        lot->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // ambient spawner
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        host.manager = &manager;
        CHECK(manager.Init());

        // A marina lot must exist for the spawner to run at all.
        FakePropertyHolder marinaLotHolder;
        marinaLotHolder.ids = {kPropertyWatercraftMarinaLot};
        FakeOccupant marinaLot;
        marinaLot.holder = &marinaLotHolder;
        cSC4Lot* marinaLotObject = MakeLot(0, 0, 1, 1);
        host.lotsByCell[{0, 0}] = marinaLotObject;
        manager.HandleOccupantInserted(&marinaLot);
        CHECK(manager.MarinaLotCount() == 1);

        // Every visible edge cell is water.
        for (int x = 0; x <= kCitySize; ++x)
            for (int z = 0; z <= kCitySize; ++z) host.waterCells[{x, z}] = true;

        // Find a seed whose first RandomDoubleUniform is below the 10 % gate.
        std::uint32_t seed = 0;
        for (std::uint32_t candidate = 1; candidate < 100000; ++candidate) {
            cRZRandomSuper probe(candidate);
            if (probe.RandomDoubleUniform() < 0.1) {
                seed = candidate;
                break;
            }
        }
        CHECK(seed != 0);
        base.random.Seed(seed);   // re-seed in place (next_ points into the state)
        host.rect[0] = 0;
        host.rect[1] = 0;
        host.rect[2] = 4;
        host.rect[3] = 4;
        manager.PopulateVisibleArea(0.0f);
        CHECK(manager.AmbientBoatCount() == 1);
        CHECK(host.createCalls == 1);
        CHECK(host.lastCreateGroup == kWatercraftManagerGroupAmbientBoat);
        CHECK(host.lastCreatePosition.y == 7.0f);   // the spawner stamps the terrain height on the position
        cSC4Watercraft* spawned = manager.AmbientBoats().front().get();
        CHECK_NEAR(spawned->TowWaitThreshold(), 45000.0f);
        CHECK(!spawned->AnimationHold());

        // The 10 s timer blocks an immediate second spawn.
        manager.PopulateVisibleArea(1.0f);
        CHECK(manager.AmbientBoatCount() == 1);

        // Driving mode uses the 5 % gate and the towable group 0x4417 when the second roll is below 0.5.
        base.drivingMode = true;
        std::uint32_t driveSeed = 0;
        for (std::uint32_t candidate = 1; candidate < 200000; ++candidate) {
            cRZRandomSuper probe(candidate);
            if (probe.RandomDoubleUniform() < 0.05 && probe.RandomDoubleUniform() >= 0.5) {
                driveSeed = candidate;
                break;
            }
        }
        CHECK(driveSeed != 0);
        base.random.Seed(driveSeed);
        manager.PopulateVisibleArea(10000.0f);   // accumulates the timer to 10001 and returns
        CHECK(manager.AmbientBoatCount() == 1);
        manager.PopulateVisibleArea(0.0f);       // the timer is due again
        CHECK(manager.AmbientBoatCount() == 2);
        CHECK(host.lastCreateGroup == kWatercraftGroupTowable);
        CHECK(manager.AmbientBoats().back()->AnimationHold());

        // GetEdgeCells walks the visible rectangle's four edges and advances the index modulo 4.
        std::vector<std::pair<int, int>> cells;
        int index = 0;
        manager.GetEdgeCells(index, cells);   // top: (0..3, 0)
        CHECK(cells.size() == 4);
        CHECK(cells[0] == std::make_pair(0, 0));
        CHECK(cells[3] == std::make_pair(3, 0));
        cells.clear();
        ++index;
        manager.GetEdgeCells(index, cells);   // right: (4, 0..3)
        CHECK(cells.size() == 4);
        CHECK(cells[0] == std::make_pair(4, 0));
        cells.clear();
        index = -1;
        manager.GetEdgeCells(index, cells);   // quirk: a negative index falls through the switch
        CHECK(cells.empty());
        index = 7;
        manager.GetEdgeCells(index, cells);   // 7 % 4 = 3: left edge (0, 0..3)
        CHECK(cells.size() == 4);
        CHECK(cells[0] == std::make_pair(0, 0));

        manager.Shutdown();
        marinaLotObject->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // GetRandomCreationPoint
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        CHECK(manager.Init());

        cS3DVector3 center{10.0f, 0.0f, 10.0f};
        cS3DVector3 outPosition{};
        CHECK(!manager.GetRandomCreationPoint(0x4413, center, 0.0f, 0.0f, outPosition, 0, nullptr));

        base.canCreateGroup = true;
        // A terminal lot at the centre: the dock is direction * 2 * cellSize away from the occupant.
        FakePropertyHolder terminalHolder;
        terminalHolder.ids = {kPropertyWatercraftFerryTerminal};
        FakeOccupant terminal;
        terminal.holder = &terminalHolder;
        terminal.position[0] = 8.0f;
        terminal.position[2] = 8.0f;
        // A small prop box: the dock offset is (min.z - max.z) * 90 = -18, then +2*cellSize along +z.
        terminal.minXyz[0] = -0.1f; terminal.minXyz[1] = 0.0f; terminal.minXyz[2] = -0.1f;
        terminal.maxXyz[0] = 0.1f;  terminal.maxXyz[1] = 0.1f; terminal.maxXyz[2] = 0.1f;
        cSC4Lot* lot = MakeLot(0, 0, 1, 1);
        host.lotsByCell[{0, 0}] = lot;
        host.facings[&terminal] = 0;
        host.body.bodies[{0, 0}] = 3;
        manager.HandleOccupantInserted(&terminal);
        CHECK(manager.TerminalCount() == 1);
        // The terminal's occupant sits at (8, 8); the dock direction (0,0,1) moves 2 * 16 along z. Cell (0, 1) is water.
        host.waterCells[{0, 1}] = true;
        cS3DVector3 direction{};
        CHECK(manager.GetRandomCreationPoint(0x4413, center, 0.0f, 0.0f, outPosition, 0, &direction));
        CHECK(host.waterCells.count({0, 1}) == 1);
        CHECK_NEAR(outPosition.y, 7.0f);
        CHECK_NEAR(direction.z, 2.0f);   // the dock direction, doubled
        CHECK_NEAR(direction.x, 0.0f);

        // Without a terminal lot: a spiral search over the city, the min-distance filter applies.
        FakeOccupant noTerminal;
        cSC4Lot* otherLot = MakeLot(5, 5, 1, 1);
        host.lotsByCell[{5, 5}] = otherLot;
        cS3DVector3 farCenter{2.5f * kCellSize, 0.0f, 2.5f * kCellSize};   // cell (2,2): no lot there
        cS3DVector3 farPosition{};
        CHECK(!manager.GetRandomCreationPoint(0x4413, farCenter, 1.0e6f, 0.0f, farPosition, 0, nullptr));
        // quirk: the final PointInCity test gets the cell indices, not the world position.
        host.lastPointInCity = {-1.0f, -1.0f};
        bool found = false;
        for (int x = 0; x < kCitySize && !found; ++x)
            for (int z = 0; z < kCitySize && !found; ++z) {
                if (host.waterCells.count({x, z})) continue;
                host.waterCells[{x, z}] = true;
                found = true;
                const int recordedX = x, recordedZ = z;
                if (manager.GetRandomCreationPoint(0x4413, farCenter, 0.0f, 0.0f, farPosition, 0, nullptr))
                    CHECK(host.lastPointInCity.x >= 0.0f && host.lastPointInCity.x < static_cast<float>(kCitySize) &&
                          std::fabs(host.lastPointInCity.x - static_cast<float>(recordedX)) < 0.001f);
            }
        manager.Shutdown();
        lot->Release();
        otherLot->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // CreateAutomaton + Write/Read round trip
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        host.manager = &manager;
        manager.Init();

        // Outside the city: no creation, no host call.
        cS3DVector3 outside{-100.0f, 0.0f, -100.0f};
        CHECK(manager.CreateAutomaton(0x4418, outside, kWatercraftCreateFlags, {}, nullptr, 0, nullptr) == nullptr);
        CHECK(host.createCalls == 0);
        // A boat already there blocks the creation.
        cSC4Watercraft* existing = new cSC4Watercraft();
        existing->AddRef();
        existing->SetRadius(2.0f);
        const float existingPosition[3] = {10.0f, 0.0f, 10.0f};
        existing->SetPosition(existingPosition);
        manager.AddAutomaton(existing);
        existing->Release();
        cS3DVector3 inside{11.0f, 0.0f, 11.0f};
        CHECK(manager.CreateAutomaton(0x4418, inside, kWatercraftCreateFlags, {}, nullptr, 0, nullptr) == nullptr);
        CHECK(host.createCalls == 0);
        // Away from the existing boat the host creates (and registers) one.
        cS3DVector3 freePosition{50.0f, 0.0f, 50.0f};
        cSC4Watercraft* created =
            manager.CreateAutomaton(0x4418, freePosition, kWatercraftCreateFlags, {}, nullptr, 0, nullptr);
        CHECK(created != nullptr);
        CHECK(host.createCalls == 1);
        CHECK(host.lastCreateGroup == 0x4418);
        CHECK(manager.BoatCount() == 2);

        // ---- Write/Read round trip ---------------------------------------------------------------------------
        FakePropertyHolder terminalHolder, marinaLotHolder, buildingHolder;
        terminalHolder.ids = {kPropertyWatercraftFerryTerminal};
        marinaLotHolder.ids = {kPropertyWatercraftMarinaLot};
        buildingHolder.ids = {kMarinaBuildingProperties[0]};
        FakeOccupant terminal, marinaOccupant, marinaLotOccupant;
        terminal.holder = &terminalHolder;
        marinaOccupant.holder = &buildingHolder;
        marinaLotOccupant.holder = &marinaLotHolder;
        cSC4Lot* writeLot1 = MakeLot(0, 0, 1, 1);
        cSC4Lot* writeLot2 = MakeLot(1, 1, 1, 1);
        host.lotsByCell[{0, 0}] = writeLot1;
        host.lotsByCell[{1, 1}] = writeLot2;
        host.facings[&terminal] = 0;
        host.lotTable = {writeLot1, writeLot2};
        host.occupantTable = {&terminal, &marinaOccupant, &marinaLotOccupant};
        manager.HandleOccupantInserted(&terminal);
        manager.HandleOccupantInserted(&marinaOccupant);
        manager.HandleOccupantInserted(&marinaLotOccupant);
        CHECK(manager.TerminalCount() == 1);
        CHECK(manager.MarinaOccupantCount() == 1);
        CHECK(manager.MarinaLotCount() == 1);

        // Add a route with the fields the stream writes (the boat and the "at station" flag stay runtime-only).
        FerryRouteInformation& storedRoute = manager.Terminals().begin()->second.routes.emplace_back();
        storedRoute.route.fromLot = writeLot1;
        storedRoute.route.toLot = writeLot2;
        storedRoute.route.state = 2;   // quirk: only 0 and 1 survive, everything else is written as -1
        storedRoute.route.f14 = 0x1111;
        storedRoute.route.f16 = 0x2222;
        storedRoute.route.f18 = 0x3333;
        storedRoute.route.f1a = 0x4444;
        storedRoute.route.f1c = 0x55;
        storedRoute.route.f1e = -6;
        storedRoute.route.f20 = 7;
        storedRoute.route.route = {1, 2, 3};
        storedRoute.route.route2 = {9, 8};

        PortMemoryOStream out;
        CHECK(manager.Write(out));
        const std::vector<unsigned char> bytes = out.bytes();
        CHECK(!bytes.empty());

        FakeHost readHost;
        FakeBase readBase;
        readHost.base = &readBase;
        readHost.lotTable = host.lotTable;
        readHost.occupantTable = host.occupantTable;
        cSC4WatercraftManager reader;
        reader.SetBase(&readBase);
        reader.SetHost(&readHost);
        CHECK(reader.Init());
        PortMemoryIStream in(bytes.data(), bytes.size());
        CHECK(reader.Read(in));
        CHECK(reader.TerminalCount() == 1);
        CHECK(reader.MarinaOccupantCount() == 1);
        CHECK(reader.MarinaLotCount() == 1);
        const FerryTerminalInformation& readInfo = reader.Terminals().begin()->second;
        CHECK(readInfo.occupant.get() == &terminal);
        CHECK(readInfo.routes.size() == 1);
        CHECK(readInfo.routes[0].route.fromLot.get() == writeLot1);
        CHECK(readInfo.routes[0].route.toLot.get() == writeLot2);
        CHECK(readInfo.routes[0].route.state == -1);   // the write clamp
        CHECK(readInfo.routes[0].route.f14 == 0x1111);
        CHECK(readInfo.routes[0].route.f16 == 0x2222);
        CHECK(readInfo.routes[0].route.f18 == 0x3333);
        CHECK(readInfo.routes[0].route.f1a == 0x4444);
        CHECK(readInfo.routes[0].route.f1c == 0x55);
        CHECK(readInfo.routes[0].route.f1e == -6);
        CHECK(readInfo.routes[0].route.f20 == 7);
        CHECK(readInfo.routes[0].route.route.size() == 3);
        CHECK(readInfo.routes[0].route.route2.size() == 2);
        CHECK(readInfo.routes[0].atStation);   // the reader stores true
        CHECK(readInfo.routes[0].boat.get() == nullptr);

        // A version below 6 goes to the parent reader; a version above 6 fails.
        PortMemoryOStream oldVersion;
        oldVersion.SetUint16(5);
        PortMemoryIStream oldIn(oldVersion.bytes().data(), oldVersion.bytes().size());
        CHECK(reader.Read(oldIn));
        CHECK(readBase.readInternalCalls == 1);
        PortMemoryOStream futureVersion;
        futureVersion.SetUint16(7);
        PortMemoryIStream futureIn(futureVersion.bytes().data(), futureVersion.bytes().size());
        CHECK(!reader.Read(futureIn));

        reader.Shutdown();
        manager.Shutdown();
        writeLot1->Release();
        writeLot2->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // ferries: CreateFerryStruct, CreateFerry, and DoAnimationTick's route rebuild, launch sweep and docking cleanup
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        host.manager = &manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        manager.Init();
        FakePropertyHolder holder;
        holder.ids = {kPropertyWatercraftFerryTerminal};
        FakeOccupant terminalA, terminalB;
        terminalA.holder = &holder;
        terminalB.holder = &holder;
        terminalA.position[0] = terminalA.position[2] = 3.5f * kCellSize;   // cell (3,3): the first cell clear of the border
        terminalB.position[0] = terminalB.position[2] = 4.5f * kCellSize;
        cSC4Lot* lotA = MakeLot(3, 3, 1, 1);
        cSC4Lot* lotB = MakeLot(4, 4, 1, 1);
        host.lotsByCell[{3, 3}] = lotA;
        host.lotsByCell[{4, 4}] = lotB;
        host.facings[&terminalA] = 0;
        host.facings[&terminalB] = 0;
        manager.HandleOccupantInserted(&terminalA);
        manager.HandleOccupantInserted(&terminalB);
        CHECK(manager.TerminalCount() == 2);
        FerryTerminalInformation* infoA = &manager.Terminals().find(cRZAutoRefCount<cSC4Lot>(lotA))->second;
        FerryTerminalInformation* infoB = &manager.Terminals().find(cRZAutoRefCount<cSC4Lot>(lotB))->second;
        cS3DVector3 dockA{}, dirA{}, dockB{}, dirB{};
        CHECK(manager.GetPropPositionAndOrientation(&terminalA, dockA, dirA, false));
        CHECK(manager.GetPropPositionAndOrientation(&terminalB, dockB, dirB, false));

        // A line between the two terminals, no boat yet: both docks pushed out by kfMaxStoplightDistance (1.0).
        FerryRouteInformation route;
        route.route.fromLot = lotA;
        route.route.toLot = lotB;
        route.route.route = {0x00, 3, 3, 0x40, 2, 0x0e};
        route.route.route2 = {0x00, 4, 4, 0x40, 2, 0x05};
        FerryStationInformation stationA, stationB;
        stationB.flags = 0x00ab;
        manager.CreateFerryStruct(infoA, &route, stationA, stationB);
        CHECK(stationA.lot.get() == lotA);
        CHECK(stationA.route == route.route.route);
        CHECK(stationA.flags == 0x0100);
        CHECK(stationA.routeIndex == 0);
        CHECK(std::fabs(stationA.position.z - (dockA.z + dirA.z)) < 1e-4f && std::fabs(stationA.position.x - dockA.x) < 1e-4f);
        CHECK(stationA.dockDirection.z == dirA.z);
        CHECK(stationB.lot.get() == lotB);
        CHECK(stationB.route == route.route.route2);
        CHECK(stationB.flags == 0x0100);
        CHECK(std::fabs(stationB.position.z - (dockB.z + dirB.z)) < 1e-4f);
        // A far terminal the manager does not know: station B is a copy of station A.
        cSC4Lot* stray = MakeLot(6, 6, 1, 1);
        FerryRouteInformation strayRoute = route;
        strayRoute.route.toLot = stray;
        FerryStationInformation strayA, strayB;
        manager.CreateFerryStruct(infoA, &strayRoute, strayA, strayB);
        CHECK(strayB.lot.get() == lotA);
        CHECK(strayB.route == route.route.route);
        // A neighbor line: station B = the first cell of the return route a boat can launch from, at sea level, facing the next
        // cell; route index = that cell's index + 2. Return route: (2,4) then three steps +x; (2,4) is land, (3,4) is water.
        FerryRouteInformation neighbor = route;
        neighbor.route.f1c = 1;
        neighbor.route.route2 = {0x00, 2, 4, 0x40, 3, 0x2a};
        host.waterCells[{3, 4}] = true;
        FerryStationInformation nA, nB;
        manager.CreateFerryStruct(infoA, &neighbor, nA, nB);
        CHECK(nB.lot.get() == nullptr);
        CHECK(nB.routeIndex == 3);
        CHECK(nB.flags == 0x0100);
        CHECK(nB.position.x == 3.5f * kCellSize && nB.position.z == 4.5f * kCellSize && nB.position.y == 250.0f);
        CHECK(nB.dockDirection.x == kCellSize && nB.dockDirection.y == 0.0f && nB.dockDirection.z == 0.0f);
        CHECK(host.lastWaterRadius == 2);   // a ferry needs water two cells wide
        // No launchable cell on the return route: station B keeps everything but its flags' high byte.
        host.waterCells.clear();
        FerryStationInformation dryA, dryB;
        dryB.routeIndex = 77;
        dryB.flags = 0x01cd;
        manager.CreateFerryStruct(infoA, &neighbor, dryA, dryB);
        CHECK(dryB.routeIndex == 77 && dryB.flags == 0x00cd);

        // CreateFerry: passenger line (type 0) -> group 0x4402, starts docked at station B (flag clear), B's slot holds it.
        host.createCalls = 0;
        CHECK(manager.CreateFerry(infoA, infoB, &route));
        CHECK(host.createCalls == 1 && host.lastCreateGroup == kWatercraftManagerGroupFerryB);
        CHECK(host.lastCreatePosition.x == 1.0f && host.lastCreatePosition.y == 1.0f && host.lastCreatePosition.z == 1.0f);
        CHECK(route.boat);
        CHECK(route.atStation);
        CHECK(infoB->dockBoat.get() == route.boat.get());
        CHECK(!infoA->dockBoat);
        float at[3] = {0.0f, 0.0f, 0.0f};
        route.boat->GetPosition(at);
        CHECK(std::fabs(at[2] - (dockB.z + dirB.z * route.boat->GetRadius())) < 1e-3f);
        CHECK(route.boat->StationA().lot.get() == lotB);   // the boat's first station is the end it starts at
        CHECK(route.boat->DockingActive());
        // Car ferry (type 1) -> 0x4400; the flag now set: it starts at station A, A's slot holds it.
        FerryRouteInformation carRoute = route;
        carRoute.boat = nullptr;
        carRoute.route.state = 1;
        CHECK(manager.CreateFerry(infoA, infoB, &carRoute));
        CHECK(host.lastCreateGroup == kWatercraftManagerGroupFerryA);
        CHECK(!carRoute.atStation);
        CHECK(infoA->dockBoat.get() == carRoute.boat.get());
        CHECK(carRoute.boat->StationA().lot.get() == lotA);
        // A terminal within three cells of the border launches nothing.
        FakeOccupant edgeTerminal;
        edgeTerminal.holder = &holder;
        edgeTerminal.position[0] = edgeTerminal.position[2] = 2.5f * kCellSize;
        FerryTerminalInformation edgeInfo;
        edgeInfo.occupant = &edgeTerminal;
        FerryRouteInformation edgeRoute = route;
        edgeRoute.boat = nullptr;
        const int before = host.createCalls;
        CHECK(!manager.CreateFerry(&edgeInfo, infoB, &edgeRoute));
        CHECK(host.createCalls == before && !edgeRoute.boat);
        edgeInfo.occupant = nullptr;
        infoA->dockBoat = nullptr;
        infoB->dockBoat = nullptr;

        // DoAnimationTick: message 0x2c06ba82 makes the traffic simulator's lines the routes.
        auto* changed = new cRZMessage2Standard();
        changed->AddRef();
        changed->SetType(kMessageWatercraftRoutesChanged);
        FerryRouteInfo line;
        line.fromLot = lotA;
        line.toLot = lotB;
        line.route = route.route.route;
        line.route2 = route.route.route2;
        host.inUse = {line};
        manager.DoMessage(changed);
        CHECK(manager.RoutesChanged());
        manager.DoAnimationTick(0, 16.0f);
        CHECK(!manager.RoutesChanged());
        CHECK(infoA->routes.size() == 1 && !infoA->routes[0].boat && !infoA->routes[0].atStation);
        CHECK(infoB->routes.empty());
        // The launch sweep runs once 35 s have gone by (the timer adds while below, then fires on the next tick).
        host.createCalls = 0;
        manager.DoAnimationTick(0, 34000.0f);   // 34,016 ms: below 35 s
        manager.DoAnimationTick(0, 16.0f);
        CHECK(host.createCalls == 0);
        manager.DoAnimationTick(0, 1000.0f);    // 35,032 ms
        CHECK(host.createCalls == 0);
        manager.DoAnimationTick(0, 16.0f);
        CHECK(host.createCalls == 1);
        CHECK(infoA->routes[0].boat);
        cSC4Watercraft* ferry = infoA->routes[0].boat.get();
        CHECK(infoB->dockBoat.get() == ferry);
        // A terminal whose lot is not in good condition launches nothing.
        lotA->SetCondition(2);
        infoA->routes[0].boat = nullptr;
        infoB->dockBoat = nullptr;
        manager.DoAnimationTick(0, 35000.0f);
        manager.DoAnimationTick(0, 16.0f);
        CHECK(host.createCalls == 1);
        lotA->SetCondition(0);
        infoA->routes[0].boat = ferry;
        infoB->dockBoat = ferry;
        // The same line again keeps the route (and its boat) and hands the boat fresh stations without re-initialising the route.
        manager.DoMessage(changed);
        manager.DoAnimationTick(0, 16.0f);
        CHECK(infoA->routes.size() == 1 && infoA->routes[0].boat.get() == ferry);
        CHECK(ferry->StationA().lot.get() == lotA && ferry->StationB().lot.get() == lotB);
        // The line gone: the route is erased and its ferry fades out (state 0x49).
        host.inUse.clear();
        manager.DoMessage(changed);
        manager.DoAnimationTick(0, 16.0f);
        CHECK(infoA->routes.empty());
        CHECK(ferry->GetBehavior() == kWatercraftStateRemoveFade);
        // Docking cleanup: B's slot still names the ferry that runs no route any more; freed on the first tick after 60 s.
        CHECK(infoB->dockBoat.get() == ferry);
        manager.DoAnimationTick(0, 60000.0f);
        manager.DoAnimationTick(0, 16.0f);
        CHECK(!infoB->dockBoat);
        // Paused: only the parent ticks.
        const int ticks = base.tickCalls;
        base.simulatePaused = true;
        host.inUse = {line};
        manager.DoMessage(changed);
        manager.DoAnimationTick(0, 16.0f);
        CHECK(base.tickCalls == ticks + 1);
        CHECK(manager.RoutesChanged());
        base.simulatePaused = false;
        changed->Release();
        manager.Shutdown();
        lotA->Release();
        lotB->Release();
        stray->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // lookahead conflicts: the lower-priority boat gives way
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        host.manager = &manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        manager.Init();
        auto* first = new cSC4Watercraft();
        auto* second = new cSC4Watercraft();
        first->AddRef();
        second->AddRef();
        manager.AddAutomaton(first);
        manager.AddAutomaton(second);
        auto point = [](int x, int z) { WatercraftTravelPoint p; p.x = x; p.z = z; return p; };
        first->Lookahead() = {point(1, 1), point(2, 1), point(3, 1), point(4, 1)};
        second->Lookahead() = {point(5, 5), point(2, 1), point(2, 2), point(2, 3)};
        // Both priority 3 (no ferry, no size group): not both 0, and the other is not lower, so the boat met second in the list
        // walk gives way at its shared cell (index 1; its lookahead is cut after that cell).
        const std::vector<cSC4Watercraft*> order = manager.CurrentBoats();
        CHECK(order.size() == 2);
        cSC4Watercraft* outer = order[0];
        cSC4Watercraft* inner = order[1];
        manager.DoAnimationTick(0, 16.0f);
        CHECK(inner->Lookahead().size() == 2);
        CHECK(outer->Lookahead().size() == 4);
        // In driving mode nothing is settled.
        first->Lookahead() = {point(1, 1), point(2, 1), point(3, 1), point(4, 1)};
        second->Lookahead() = {point(5, 5), point(2, 1), point(2, 2), point(2, 3)};
        base.drivingMode = true;
        manager.DoAnimationTick(0, 16.0f);
        CHECK(first->Lookahead().size() == 4 && second->Lookahead().size() == 4);
        base.drivingMode = false;
        manager.Shutdown();
        first->Release();
        second->Release();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // marina boats: CreateMarinaBoat
    // ---------------------------------------------------------------------------------------------------------------
    {
        FakeBase base;
        FakeHost host;
        host.base = &base;
        cSC4WatercraftManager manager;
        host.manager = &manager;
        manager.SetBase(&base);
        manager.SetHost(&host);
        manager.Init();
        FakePropertyHolder holder;
        holder.ids = {kMarinaBuildingProperties[0]};
        FakeOccupant marina;
        marina.holder = &holder;
        marina.position[0] = marina.position[2] = 4.5f * kCellSize;
        for (int i = 0; i < 3; ++i) marina.minXyz[i] = marina.maxXyz[i] = 0.0f;   // the dock point stays on the building
        host.facings[&marina] = 0;
        host.pointInCityWorld = true;
        manager.HandleOccupantInserted(&marina);
        CHECK(manager.MarinaOccupantCount() == 1);
        cS3DVector3 dock{}, dir{};
        CHECK(manager.GetPropPositionAndOrientation(&marina, dock, dir, false));
        // The moored kind (0x4416): created half a step out, moved 1.5 radii further, waiting with a 60 s threshold.
        CHECK(manager.CreateMarinaBoat(&marina, kWatercraftManagerGroupMarinaBoat));
        CHECK(host.lastCreateGroup == kWatercraftManagerGroupMarinaBoat);
        CHECK(std::fabs(host.lastCreatePosition.z - (dock.z + 0.5f * dir.z)) < 1e-4f && host.lastCreatePosition.x == dock.x);
        cSC4Watercraft* moored = manager.MarinaOccupants().begin()->second.get();
        CHECK(moored != nullptr);
        if (!moored) return 1;
        float at[3] = {0.0f, 0.0f, 0.0f};
        moored->GetPosition(at);
        CHECK(std::fabs(at[2] - (dock.z + 0.5f * dir.z + moored->GetRadius() * dir.z * 1.5f)) < 1e-4f);
        CHECK(moored->GetBehavior() == kWatercraftStateWaiting);
        CHECK(moored->TowWaitThreshold() == kWatercraftMarinaWaitThreshold);
        // Another kind: created 32 to the side (-cross_y of the dock direction = (dir.z, 0, -dir.x) = +x for facing 0) and
        // moored one radius to that side.
        CHECK(manager.CreateMarinaBoat(&marina, 0x4419));
        CHECK(std::fabs(host.lastCreatePosition.x - (dock.x + 32.0f)) < 1e-4f);
        cSC4Watercraft* side = manager.MarinaOccupants().begin()->second.get();
        side->GetPosition(at);
        CHECK(std::fabs(at[0] - (dock.x + side->GetRadius())) < 1e-4f);
        // Too close to the border: nothing is created.
        FakeOccupant edge;
        edge.holder = &holder;
        edge.position[0] = edge.position[2] = 1.5f * kCellSize;
        for (int i = 0; i < 3; ++i) edge.minXyz[i] = edge.maxXyz[i] = 0.0f;
        host.facings[&edge] = 0;
        const int before = host.createCalls;
        CHECK(!manager.CreateMarinaBoat(&edge, kWatercraftManagerGroupMarinaBoat));
        CHECK(host.createCalls == before);
        // The tick gives a marina building without its boat a new one (the old boats removed first: a boat within two radii
        // of the launch point refuses the creation).
        manager.RemoveAutomaton(moored, true);
        manager.RemoveAutomaton(side, true);
        manager.MarinaOccupants().begin()->second = nullptr;
        manager.DoAnimationTick(0, 16.0f);
        CHECK(manager.MarinaOccupants().begin()->second);
        CHECK(host.lastCreateGroup == kWatercraftManagerGroupMarinaBoat);
        manager.Shutdown();
    }

    if (failures == 0) std::printf("sc4_watercraftmanager_test: all checks passed\n");
    else std::printf("sc4_watercraftmanager_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
