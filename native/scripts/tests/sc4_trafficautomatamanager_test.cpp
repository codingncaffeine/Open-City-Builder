// cSC4TrafficAutomataManager — unit test with a fake host. Exercises the lifecycle, the vehicle-manager defaults, the
// layer / path-type / automata-group tables, the 4x4 edge-density table (positive and mirrored), the response list, the
// automaton-facing traffic slots, the effect manager, the persistence record and the per-tick populate path. The fake
// neighbours implement the seams declared in TrafficAutomataManager.h; the assertions mark the original behaviour kept
// from the binary (addresses in the comments).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataManager.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/TrafficAutomataManager.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("\nFAIL line %d: %s", __LINE__, #c); ++failures; } } while (0)

namespace {
// ---- generic counted base --------------------------------------------------------------------------------------------------
struct Counted : cIGZUnknown {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
};

// ---- occupant --------------------------------------------------------------------------------------------------------------
struct FakeOccupant final : IAutomataOccupant {
    int refs = 1;
    std::uint32_t type = 0x12345678;
    std::uint32_t flags = 4;
    float position[3] = {0, 0, 0};
    std::set<std::uint32_t> groups;
    cISCPropertyHolder* holder = nullptr;
    int releases = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4NetworkOccupant) { *out = static_cast<IAutomataNetworkOccupant*>(nullptr); return false; }
        (void)out;
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { ++releases; return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void GetBoundingBox(float* minXyz, float* maxXyz) override { for (int i = 0; i < 3; ++i) { minXyz[i] = position[i] - 1; maxXyz[i] = position[i] + 1; } }
    void GetBoundingCityCells(tCellRect& rect) override { rect = tCellRect{1, 2, 3, 4}; }
    bool IsOccupantGroup(std::uint32_t group) override { return groups.count(group) != 0; }
    bool GetOccupantGroups(std::set<std::uint32_t>& out) override { out = groups; return !groups.empty(); }
    std::uint32_t GetFlags() override { return flags; }
};

struct FakeBuilding final : IAutomataBuildingOccupant {
    int refs = 1;
    FakeOccupant* occupant = nullptr;
    float completion = 100.0f;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    IAutomataOccupant* AsOccupant() const override { return occupant; }
    const float& GetCompletionPercent() override { return completion; }
};

// ---- automaton -------------------------------------------------------------------------------------------------------------
struct FakeTrafficAutomaton final : ITrafficAutomataAutomaton {
    int refs = 1;
    FakeOccupant occupant;
    float position[3] = {0, 0, 0};
    float orientation[3] = {0, 0, 0};
    float radius = 4.0f;
    float opacity = 1.0f;
    std::uint8_t priority = 0;
    bool visible = true;
    bool marked = false;
    bool removedForce = false;
    bool removedSoft = false;
    bool effectsCleared = false;
    bool placedOccupant = false, placedRenderer = false;
    std::uint32_t flags = 0;
    std::uint32_t group = 0;
    std::uint32_t callbacks = 0;
    int shutdowns = 0, inits = 0;
    std::vector<std::uint32_t> sentEvents;
    IAutomataAutomaton* stateTarget = nullptr;
    bool hasState = true;
    std::uint32_t foundState = 0, pushedState = 0;
    bool pushOk = true;
    bool tickResult = true;
    int ticks = 0;
    int placedOccupantCalls = 0;
    IAutomataGroupTemplate* groupTemplate = nullptr;
    cGZPersistResourceKey modelKey;
    int modelKeyAdds = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZMessageTarget2View) { *out = this; return true; }
        (void)out;
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { ++inits; return true; }
    bool Shutdown() override { ++shutdowns; return true; }
    IAutomataOccupant* Occupant() override { return &occupant; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof position); }
    void SetVisible(bool v) override { visible = v; }
    bool Remove(bool force) override { removedSoft = !force; removedForce = force; return true; }
    void SetOrientation(const float* dir) override { std::memcpy(orientation, dir, sizeof orientation); }
    void GetOrientation(float* dir) override { std::memcpy(dir, orientation, sizeof orientation); }
    float GetRadius() const override { return radius; }
    void SetAutomataFlag(std::uint32_t flag, bool on) override { if (on) flags |= flag; else flags &= ~flag; }
    bool CheckAutomataFlag(std::uint32_t flag) override { return (flags & flag) != 0; }
    float GetOpacity() const override { return opacity; }
    void SetOpacity(float value, float) override { opacity = value; }
    void AddCallback(cIGZMessageTarget2*) override { ++callbacks; }
    void* GetState() override { return hasState ? this : nullptr; }
    void* FindState(std::uint32_t state) override { return foundState == state ? this : nullptr; }
    bool PushState(std::uint32_t state, void*, float, bool, std::uint32_t, std::uint32_t, const void*) override {
        pushedState = state;
        return pushOk;
    }
    std::uint8_t GetPriority() override { return priority; }
    void SetPriority(std::uint8_t value) override { priority = value; }
    std::uint32_t GetAutomataGroup() override { return group; }
    bool DoAnimationTick(std::uint32_t, float) override { ++ticks; return tickResult; }
    void SetZoomAndRotation(int, int) override {}
    void HandleRemoveController(void*) override {}
    bool GetMarkedForRemove() override { return marked; }
    void SetMarkedForRemove(bool value) override { marked = value; }
    bool PostInit() override { return true; }
    void SendCallbackMessage(std::uint32_t event, std::uint32_t) override { sentEvents.push_back(event); }
    bool ShouldCollide(IAutomataAutomaton* other) override { return other != nullptr; }
    bool PlaceInOccupantManager(bool place) override { ++placedOccupantCalls; placedOccupant = place; return true; }
    void PlaceInRenderer(bool place) override { placedRenderer = place; }
    bool IsPlacedInOccupantManager() override { return placedOccupant; }
    bool IsPlacedInRenderer() override { return placedRenderer; }
    void StopEffectsAndClearModel() override { effectsCleared = true; }
    IAutomataAutomaton* StateTarget() override { return stateTarget; }
    void SetStateTarget(IAutomataAutomaton* target) override {
        if (target == stateTarget) return;
        if (target) target->AddRef();
        IAutomataAutomaton* old = stateTarget;
        stateTarget = target;
        if (old) old->Release();
    }
    void SetPropertyVariant(std::uint32_t, cRZVariant&, bool) override {}
    void SetExemplar(cISCResExemplar*) override {}
    void AddModelResourceKey(const cGZPersistResourceKey& key) override { modelKey = key; ++modelKeyAdds; }
    void SetAutomataGroup(IAutomataGroupTemplate* tmpl) override { groupTemplate = tmpl; }

    // ---- ITrafficAutomataAutomaton (the traffic slots on top of the base view) ----
    std::uint32_t populateLayer = 7;
    std::uint8_t cellLifespan = 0;
    ITrafficNetworkMapOccupant* networkOccupant = nullptr;
    std::uint32_t pathKey = 0xffff;
    bool train = false;
    bool detectCollisions = true;
    IAutomataAutomaton* leader = nullptr;
    IAutomataAutomaton* findLeaderResult = nullptr;
    int startTrips = 0, occupantRemovals = 0, pathSets = 0;
    bool startTripResult = true;
    std::uint32_t tripKey = 0;
    ITrafficNetworkMapOccupant* tripOccupant = nullptr;
    float targetDirection[3] = {0, 0, 0};

    std::uint32_t PopulateLayer() const override { return populateLayer; }
    void SetPopulateLayer(std::uint32_t value) override { populateLayer = value; }
    void SetCellLifespan(std::uint8_t value) override { cellLifespan = value; }
    bool StartTrip(ITrafficNetworkMapOccupant* occ, std::uint32_t key, bool) override {
        ++startTrips;
        tripOccupant = occ;
        tripKey = key;
        return startTripResult;
    }
    void SetPathToOccupant(ITrafficNetworkMapOccupant* occ, std::uint32_t key, const void*, bool) override {
        ++pathSets;
        networkOccupant = occ;
        pathKey = key;
    }
    void HandleNetworkOccupantRemoved() override { ++occupantRemovals; }
    std::uint32_t GetPathKey() const override { return pathKey; }
    ITrafficNetworkMapOccupant* GetNetworkOccupant() const override { return networkOccupant; }
    bool IsTrain() const override { return train; }
    bool GetDetectCollisions() const override { return detectCollisions; }
    IAutomataAutomaton* FindLeader() override { return findLeaderResult; }
    IAutomataAutomaton* Leader() const override { return leader; }
    void SetLeader(IAutomataAutomaton* value) override {
        if (value == leader) return;
        if (value) value->AddRef();
        IAutomataAutomaton* old = leader;
        leader = value;
        if (old) old->Release();
    }
    void HandleDispatched(const cS3DVector3& position, cISC4Occupant*) override {
        targetDirection[0] = position.x;
        targetDirection[1] = position.y;
        targetDirection[2] = position.z;
    }
    std::deque<RouteData>* RouteDeque() override { return nullptr; }
    bool BeginRoute(bool) override { return false; }
    void ReturnToDefaultSpeed() override {}
};

// ---- group / script --------------------------------------------------------------------------------------------------------
struct FakeGroupTemplate final : IAutomataGroupTemplate {
    std::uint32_t id = 0, classId = 0, source = 0;
    FakeGroupTemplate* parent = nullptr;
    cRZVariant variant;
    std::uint32_t GroupId() const override { return id; }
    std::uint32_t ClassId() const override { return classId; }
    cRZVariant& GroupIdVariant() override { return variant; }
    IAutomataGroupTemplate* Parent() const override { return parent; }
    std::uint32_t SourceBuilding() const override { return source; }
    bool IsOccupantGroup(std::uint32_t group) const override {
        for (const IAutomataGroupTemplate* t = this; t; t = t->Parent())
            if (t->GroupId() == group) return true;
        return false;
    }
};

struct FakeScriptGroups final : IAutomataScriptGroups {
    int refs = 1;
    std::map<std::uint32_t, FakeGroupTemplate*> byId;
    std::list<std::uint32_t> trackBuildings;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    IAutomataGroupTemplate* GetAutomataGroup(std::uint32_t id) override { return byId.count(id) ? byId[id] : nullptr; }
    IAutomataGroupTemplate* GetOccupantGroup(std::uint32_t id) override { return byId.count(id) ? byId[id] : nullptr; }
    const std::list<std::uint32_t>& GetTrackBuildingGroups() override { return trackBuildings; }
};

// ---- city services ---------------------------------------------------------------------------------------------------------
struct FakeNetwork final : IAutomataNetworkManager {
    int refs = 1;
    float height = 7.0f;
    std::uint32_t masks[3] = {0, 0, 0};
    bool cellFound = true;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool CellMasks(int, int, std::uint32_t out[3]) override {
        std::memcpy(out, masks, sizeof masks);
        return cellFound;
    }
    float Height(float, float) override { return height; }
};

struct FakeClock final : IAutomataClock {
    int refs = 1;
    int hour = 6, minute = 30;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    int Hour() override { return hour; }
    int Minute() override { return minute; }
};

struct FakeKindManager final : IAutomataKindManager {
    std::uint32_t handled = 0;
    FakeTrafficAutomaton* created = nullptr;
    bool HandlesGroup(std::uint32_t group) override { return group == handled; }
    IAutomataAutomaton* CreateAutomaton(std::uint32_t group, const cS3DVector3&, std::uint32_t, const cGZPersistResourceKey&,
                                        cIGZMessageTarget2*, std::uint8_t, const cS3DVector3*) override {
        created->group = group;
        created->AddRef();
        return created;
    }
    void BeginPlayerDrive() override {}
    void EndPlayerDrive() override {}
};

struct FakeGroupBudget final : IAutomataGroupBudget {
    std::uint32_t handled = 0, current = 0;
    std::uint8_t maximum = 0;
    int releases = 0;
    bool HandlesGroup(std::uint32_t group) override { return group == handled; }
    std::uint32_t CurrentCount() override { return current; }
    std::uint8_t MaxCount() override { return maximum; }
    void ClearTrackedAutomata() override { ++releases; }
};

// ---- views / camera / misc -------------------------------------------------------------------------------------------------
struct FakeView final : IAutomataViewUtilities {
    int zoom = 3, rotation = 1, level = 1;
    float position[3] = {10, 5, 20};
    float rect[4] = {1, 2, 3, 4};
    bool onscreen = true;
    int projections = 0;
    void GetZoomAndRotation(int& z, int& r) override { z = zoom; r = rotation; }
    void ViewPosition(float out[3]) override { std::memcpy(out, position, sizeof position); }
    void ViewRect(float out[4]) override { std::memcpy(out, rect, sizeof rect); }
    bool IsPointInViewFrustum(const float*) override { return onscreen; }
    void ProjectPointToViewCoordinates(float*) override { ++projections; }
    void ProjectPointFromViewCoordinatesToViewportCoordinates(float*) override { ++projections; }
    void UnProjectPointFromViewportCoordinatesToWorldCoordinates(float*) override { ++projections; }
    int ViewLevel() override { return level; }
};

struct FakeCamera final : IAutomataCameraControl {
    int virtualZoom = 2;
    int width = 1024, height = 768;
    int targetSets = 0, velocitySets = 0, zooms = 0;
    float target[3] = {0, 0, 0};
    cS3DVector3 lastVelocity{0, 0, 0};
    int VirtualZoom() override { return virtualZoom; }
    bool SetZoom(float) override { ++zooms; return true; }
    void GetViewportSize(int& w, int& h) override { w = width; h = height; }
    bool SetViewTargetPosition(const cS3DVector3& position) override {
        ++targetSets;
        target[0] = position.x;
        target[1] = position.y;
        target[2] = position.z;
        return true;
    }
    bool SetViewTargetVelocity(const cS3DVector3& velocity) override {
        ++velocitySets;
        lastVelocity = velocity;
        return true;
    }
    void TargetVelocity(float out[3]) override { std::memcpy(out, target, sizeof target); }
};

struct FakePlayerDrive final : IAutomataPlayerDriveControl {
    int refs = 1;
    bool choosing = false, driving = false, hasDriven = false;
    std::uint32_t drivenGroup = 0, handled = 0;
    cIGZMessageTarget2* callback = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    void EnterChoosingMode() override { choosing = true; }
    void EnterDrivingMode(IAutomataAutomaton*, bool) override { driving = true; }
    void AddCallbackTarget(cIGZMessageTarget2* target) override { callback = target; }
    bool HandlesGroup(std::uint32_t group) override { return group == handled; }
    bool HasDrivenAutomaton() override { return hasDriven; }
    std::uint32_t DrivenGroup() override { return drivenGroup; }
};

struct FakePathView final : IAutomataPathView {
    int refs = 1;
    bool initialised = false, ignoreDepth = false;
    int filter = -99, shutdowns = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { initialised = true; return true; }
    void SetShouldIgnoreDepthBuffer(bool value) override { ignoreDepth = value; }
    void SetPathFilter(int value) override { filter = value; }
    void Shutdown() override { ++shutdowns; }
};

struct FakeDispatchView final : IAutomataDispatchView {
    int shows = 0, lastType = -1;
    bool lastShow = true;
    void ShowAllIndicators(int type, bool show) override { ++shows; lastType = type; lastShow = show; }
};

struct FakeSimulator final : IAutomataSimulator {
    bool emergency = false, paused = false, panel = true;
    int speed = 0;
    bool IsEmergencyPaused() override { return emergency; }
    bool IsPaused() override { return paused; }
    int SimSpeed() override { return speed; }
    bool VariableSpeedAutomata() override { return panel; }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    int refs = 1, posts = 0;
    std::vector<std::uint32_t> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { ++posts; return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override { added.push_back(type); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override { removed.push_back(type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return false; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeFilter final : IAutomataViewFilterInformation {
    std::uint32_t onlyGroup = 0;
    bool limited = false;
    bool Includes(std::uint32_t group) const override { return !limited || group == onlyGroup; }
};

// ---- exemplar + property holder --------------------------------------------------------------------------------------------
struct FakeExemplar final : cISCResExemplar {
    int refs = 1;
    cSCPropertyHolder holder;
    cGZPersistResourceKey key;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Write(cIGZOStream&) const override { return true; }
    bool Read(cIGZIStream&) override { return true; }
    std::uint32_t GetGZCLSID() const override { return 0x6534284a; }
    cISCPropertyHolder* AsISCPropertyHolder() override { return &holder; }
    bool IsPropertyLocal(std::uint32_t) override { return true; }
    cISCResExemplarCohort* GetCohort() override { return nullptr; }
    void SetCohort(cISCResExemplarCohort*, bool) override {}
    bool GetKey(cGZPersistResourceKey& out) override { out = key; return true; }
    bool SetKey(const cGZPersistResourceKey& value) override { key = value; return true; }
    void CompactProperties() override {}
    void GenerateCachedMap() override {}
};


// ---- traffic-specific fakes ---------------------------------------------------------------------------------------------
// cISC4Occupant view of the city occupant (GetBoundingCityCells / GetType are all the manager reads).
struct FakeCityOccupant final : cISC4Occupant {
    int refs = 1;
    std::uint32_t type = 0x12345678;
    SC4CellRect rect{0, 0, 0, 0};
    std::set<std::uint32_t> groups;
    std::vector<std::uint32_t> occupantMessages;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { xyz[0] = xyz[1] = xyz[2] = 0.0f; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override { for (int i = 0; i < 3; ++i) { minXyz[i] = 0; maxXyz[i] = 0; } }
    void GetBoundingCityCells(SC4CellRect& out) override { out = rect; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t message, std::uint32_t) override { occupantMessages.push_back(message); }
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t group) const override { return groups.count(group) != 0; }
    void AddOccupantGroup(std::uint32_t group) override { groups.insert(group); }
    bool GetOccupantGroups(std::set<std::uint32_t>& out) const override { out = groups; return !out.empty(); }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 4; }
};

struct FakeTrafficOccupant final : ITrafficNetworkOccupant {
    int refs = 1;
    FakeCityOccupant city;
    std::uint32_t networkFlags = kTrafficOccupantType;
    bool pathConnection = false;
    bool connections[8] = {};
    int releases = 0;
    std::uint32_t GetType() { return networkFlags; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { ++releases; return static_cast<std::uint32_t>(--refs); }
    cISC4Occupant* AsOccupant() override { return &city; }
    bool HasNetworkFlag(std::uint32_t flags) const override { return (networkFlags & flags) != 0; }
    bool HasAnyNetworkFlag(std::uint32_t flags) const override { return (networkFlags & flags) != 0; }
    void SetNetworkFlag(std::uint32_t flags) override { networkFlags |= flags; }
    void ClearNetworkFlag(std::uint32_t flags) override { networkFlags &= ~flags; }
    bool ConnectsToNeighbor(int edge, std::uint32_t networkType) const override {
        if (edge < 0 || edge >= 8) return false;
        return connections[edge] && (networkType & networkFlags) != 0;
    }
    ITrafficNetworkMapPathInfo* GetPathInfo() override { return nullptr; }
    void ReleasePathInfo() override {}
    bool PathConnectionExists(std::uint32_t, std::uint8_t, std::uint8_t) const override { return pathConnection; }
    bool IsIntersection() const override { return false; }
    float GetAltitude(float, float) const override { return 0.0f; }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { x = 0; z = 0; }
    bool IsOfType(std::uint32_t) const override { return false; }
    bool IsUsable() const override { return true; }
};

struct FakeTileAutomaton final : ITrafficTileAutomaton {
    int refs = 1;
    std::uint32_t pathKey = 0;
    std::uint32_t flags = 0;
    float opacity = 1.0f;
    bool removed = false;
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool IsRemoved() override { return removed; }
    std::uint32_t GetLane() override { return pathKey; }
    std::uint32_t PathTypeWord() const override { return 0; }
    std::uint32_t GetPathKey() const override { return pathKey; }
    bool CheckAutomataFlag(std::uint32_t flag) override { return (flags & flag) != 0; }
    float GetOpacity() const override { return opacity; }
    void Remove(bool fade) override { removed = true; (void)fade; }
};

struct FakeTrafficSimulator final : ITrafficSimulatorApi {
    int refs = 1;
    std::uint16_t table[16] = {};
    bool mirrored = false;
    unsigned lastCellX = 0, lastCellZ = 0, lastNetworkType = 0;
    int calls = 0;
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    const std::uint16_t* GetEdgeCountTable(ITrafficNetworkMapOccupant*, unsigned cellX, unsigned cellZ,
                                           unsigned networkType, bool mirror) override {
        ++calls;
        lastCellX = cellX;
        lastCellZ = cellZ;
        lastNetworkType = networkType;
        mirrored = mirror;
        return table;
    }
};

struct FakeTrafficNetwork final : ITrafficNetworkManagerApi {
    std::map<std::pair<int, int>, TrafficTileInfo*> tiles;
    int rect[4] = {0, 0, 7, 7};
    std::vector<ITrafficNetworkMapOccupant*> responseOccupants;
    bool responseFound = true;
    int responseCalls = 0;
    std::map<std::pair<std::uint32_t, std::uint32_t>, float> pathDensity;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint16_t> edgeCounts;
    TrafficTileInfo* GetTileInfoAtCell(int x, int z) override {
        auto it = tiles.find({x, z});
        return it == tiles.end() ? nullptr : it->second;
    }
    std::vector<std::pair<int, int>> swept;   // the cells the population pass asked for
    TrafficTileInfo* GetFirstTileInfoAtCell(int x, int z) override { swept.push_back({x, z}); return GetTileInfoAtCell(x, z); }
    void GetVisibleCellRect(int out[4]) override { std::memcpy(out, rect, sizeof rect); }
    bool GetResponseOccupants(cISC4Occupant*, int, int, std::uint32_t,
                              std::vector<ITrafficNetworkMapOccupant*>& out) override {
        ++responseCalls;
        out = responseOccupants;
        for (ITrafficNetworkMapOccupant* occupant : out) occupant->AddRef();
        return responseFound;
    }
    bool ResponseAllowed(ITrafficNetworkMapOccupant*) override { return true; }
    float& OccupantPathDensity(ITrafficNetworkMapOccupant*, std::uint32_t layer, std::uint32_t pathType) override {
        return pathDensity[{layer, pathType}];
    }
    std::uint16_t& OccupantPathEdgeCount(ITrafficNetworkMapOccupant*, std::uint32_t layer, std::uint32_t pathType,
                                         std::uint8_t entry, std::uint8_t exit) override {
        return edgeCounts[{(layer << 8) | pathType, static_cast<std::uint32_t>(entry), static_cast<std::uint32_t>(exit)}];
    }
};

// ---- host ------------------------------------------------------------------------------------------------------------------
struct FakeTrafficHost final : ITrafficAutomataManagerHost {
    FakeMessageServer messages;
    cIGZMessageServer2* server = &messages;
    std::vector<std::uint32_t> registeredMessages, removedMessages, ticksAdded, ticksRemoved;
    std::vector<std::pair<std::uint32_t, std::string>> cheatsRegistered, cheatsUnregistered;
    int cheatListenerAdds = 0, cheatListenerRemoves = 0;
    bool cheats = true, debug = true;
    FakeNetwork network;
    FakeScriptGroups script;
    FakeClock clock;
    FakeKindManager kinds[4];
    FakeGroupBudget budget;
    FakeView view;
    FakeCamera camera;
    FakeSimulator simulator;
    FakePathView pathView;
    FakeDispatchView dispatchView;
    FakePlayerDrive playerDrive;
    FakeGroupBudget* budgetPtr = &budget;
    IAutomataAutomaton* classCreated = nullptr;
    std::vector<FakeExemplar*> exemplars;
    std::vector<cGZPersistResourceKey> exemplarKeys;
    bool tuningPresent = false;
    cSCPropertyHolder* tuningHolder = new cSCPropertyHolder();   // the test keeps one reference
    std::list<IAutomataOccupant*> occupants;
    std::vector<std::uint32_t> filterGroups;
    bool enumerateResult = false;
    int occupantListReleases = 0, reported = 0, posts = 0, ghostSets = 0, debugRemovals = 0;
    bool ghostEnabled = true;
    IAutomataPlayerDriveControl* activeControl = nullptr;
    int playerDriveCreates = 0, playerDriveStores = 0, viewInputAdds = 0;
    float vehicleSpeed = 0.0f;
    int vehicleSpeedSets = 0;
    bool inputControlIsDrive = false;
    bool cameraFast = false;
    bool terrainShadow = false;
    float terrainHeight = 3.0f;
    int rendererAdds = 0, rendererRemoves = 0;
    std::vector<std::string> reports;

    cIGZMessageServer2* MessageServer() override { return server; }
    void AddTickNotification(cIGZMessageTarget2*, int channel) override { ticksAdded.push_back(static_cast<std::uint32_t>(channel)); }
    void RemoveTickNotification(cIGZMessageTarget2*, int channel) override { ticksRemoved.push_back(static_cast<std::uint32_t>(channel)); }
    bool CheatManagerAvailable() override { return cheats; }
    bool DebugFunctionalityEnabled() override { return debug; }
    void RegisterCheat(std::uint32_t id, const char* name) override { cheatsRegistered.emplace_back(id, name); }
    void UnregisterCheat(std::uint32_t id) override { cheatsUnregistered.emplace_back(id, ""); }
    void AddCheatListener(cIGZMessageTarget*) override { ++cheatListenerAdds; }
    void RemoveCheatListener(cIGZMessageTarget*) override { ++cheatListenerRemoves; }
    void PostMessage(cIGZMessage2*, bool) override { ++posts; }
    IAutomataNetworkManager* NetworkManager() override { return &network; }
    IAutomataScriptGroups* ScriptGroups() override { return &script; }
    IAutomataClock* CityClock() override { return &clock; }
    IAutomataKindManager* KindManager(int index) override { return &kinds[index]; }
    IAutomataGroupBudget* GroupBudget() override { return budgetPtr; }
    bool CellToWorld(int cellX, int cellZ, float& x, float& z) override {
        x = static_cast<float>(cellX) * 16.0f;
        z = static_cast<float>(cellZ) * 16.0f;
        return true;
    }
    bool WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
        return true;
    }
    float CellSize() override { return 16.0f; }
    int CityCellCount() override { return 8; }
    bool OccupantCells(IAutomataOccupant*, tCellRect& rect) override {
        rect = tCellRect{11, 12, 13, 14};
        return true;
    }
    bool LotCellMasks(int, int, std::uint32_t masks[3]) override {
        std::memcpy(masks, network.masks, sizeof network.masks);
        return network.cellFound;
    }
    cIGZUnknown* CreateOccupantGroupFilter(const std::vector<std::uint32_t>& groups) override {
        filterGroups = groups;
        return nullptr;
    }
    bool EnumerateOccupants(cIGZUnknown*, std::list<IAutomataOccupant*>& out) override {
        out = occupants;
        return enumerateResult;
    }
    void ReleaseOccupantList(std::list<IAutomataOccupant*>& list) override {
        ++occupantListReleases;
        list.clear();
    }
    IAutomataViewUtilities* ViewUtilities() override { return &view; }
    IAutomataPlayerDriveControl* ActiveInputControl() override { return activeControl; }
    IAutomataPlayerDriveControl* PlayerDriveControl() override { return playerDrive.refs ? &playerDrive : nullptr; }
    void SetPlayerDriveControl(IAutomataPlayerDriveControl* control) override {
        activeControl = control;
        ++playerDriveStores;
    }
    IAutomataPlayerDriveControl* CreatePlayerDriveControl() override {
        ++playerDriveCreates;
        playerDrive.AddRef();
        return &playerDrive;
    }
    void AddViewInputControl(IAutomataPlayerDriveControl*, int) override { ++viewInputAdds; }
    void SetVehicleManagerSpeed(float speed) override {
        vehicleSpeed = speed;
        ++vehicleSpeedSets;
    }
    void SetGhostModels(bool enabled) override {
        ++ghostSets;
        ghostEnabled = enabled;
    }
    bool CurrentInputControlIsPlayerDrive() override { return inputControlIsDrive; }
    IAutomataCameraControl* CameraControl() override { return &camera; }
    bool CameraFollowFast() override { return cameraFast; }
    float TerrainHeight(float, float) override { return terrainHeight; }
    bool TerrainShadowEnabled() override { return terrainShadow; }
    float TerrainShadowHeight(float, float) override { return terrainHeight + 1.0f; }
    IAutomataPathView* CreatePathView() override { return &pathView; }
    void AddRendererLayer(IAutomataPathView*) override { ++rendererAdds; }
    void RemoveRendererLayer(IAutomataPathView*) override { ++rendererRemoves; }
    IAutomataDispatchView* DispatchView() override { return &dispatchView; }
    IAutomataAutomaton* CreateAutomatonByClassId(std::uint32_t) override { return classCreated; }
    void ReleaseObject(cIGZUnknown* object) override {
        if (object) object->Release();
    }
    IAutomataSimulator* Simulator() override { return &simulator; }
    bool VehicleManagerPaused() override { return false; }
    bool LandfillDeveloperPresent() override { return true; }
    int LandfillSourceBuildingCount() override { return 42; }
    bool GetTuningProperties(cISCPropertyHolder** out) override {
        if (!tuningPresent) return false;
        tuningHolder->AddRef();
        *out = tuningHolder;
        return true;
    }
    bool GetExemplar(const cGZPersistResourceKey&, cISCResExemplar** out) override {
        for (FakeExemplar* exemplar : exemplars) {
            *out = exemplar;
            exemplar->AddRef();
            return true;
        }
        return false;
    }
    bool EnumerateResourceKeys(std::uint32_t, std::vector<cGZPersistResourceKey>& out) override {
        out = exemplarKeys;
        return !out.empty();
    }
    void DebugAutomatonRemoved(IAutomataAutomaton*) override { ++debugRemovals; }
    void DebugViewSetEnabled(bool) override {}
    void DebugViewShutdown() override {}
    void Report(const std::string& message) override {
        ++reported;
        reports.push_back(message);
    }

    // ---- the traffic manager's extended host ----
    FakeTrafficSimulator trafficSimulator;
    FakeTrafficNetwork trafficNetwork;
    int hour = 12;
    float timeFraction = 0.5f;
    float clockPercent = 80.0f;
    std::map<IAutomataAutomaton*, ITrafficAutomataAutomaton*> trafficAutomata;
    std::map<IAutomataOccupant*, cISC4Occupant*> cityOccupants;
    std::map<IAutomataNetworkOccupant*, ITrafficNetworkOccupant*> networkOccupantViews;
    ITrafficSimulatorApi* TrafficSimulator() override { return &trafficSimulator; }
    ITrafficNetworkManagerApi* TrafficNetworkManager() override { return &trafficNetwork; }
    int CityHour() override { return hour; }
    float CityTimeFraction() override { return timeFraction; }
    float CityClockPercent() override { return clockPercent; }
    ITrafficAutomataAutomaton* AsTrafficAutomaton(IAutomataAutomaton* automaton) override {
        auto it = trafficAutomata.find(automaton);
        return it == trafficAutomata.end() ? nullptr : it->second;
    }
    cISC4Occupant* AsCityOccupant(IAutomataOccupant* occupant) override {
        auto it = cityOccupants.find(occupant);
        return it == cityOccupants.end() ? nullptr : it->second;
    }
    IAutomataOccupant* AsAutomataOccupant(cISC4Occupant* occupant) override {
        for (const auto& entry : cityOccupants)
            if (entry.second == occupant) return entry.first;
        return nullptr;
    }
    ITrafficNetworkOccupant* AsTrafficNetworkOccupant(IAutomataNetworkOccupant* occupant) override {
        auto it = networkOccupantViews.find(occupant);
        return it == networkOccupantViews.end() ? nullptr : it->second;
    }
};
// ---- property helpers ------------------------------------------------------------------------------------------------------
void AddUint(cSCPropertyHolder& holder, std::uint32_t id, std::uint32_t value) {
    cRZVariant variant(value);
    holder.AddProperty(id, &variant, false);
}
void AddUintArray(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<std::uint32_t> values) {
    cRZVariant variant;
    variant.RefUint32(values.begin(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, false);
}
}  // namespace

int main() {
    FakeTrafficHost host;
    host.tuningHolder->AddRef();   // the test's own reference (the manager releases the one it is handed)
    SetAutomataManagerHost(&host);
    AutomataTuningGlobals& tuning = AutomataTuning();
    std::printf("sc4_trafficautomatamanager: ");

    // ---------------------------------------------------------------- lifecycle and the vehicle-manager defaults
    {
        auto* manager = new cSC4TrafficAutomataManager(&host);
        manager->AddRef();   // the creator's reference (Release at the end deletes)
        CHECK(manager->DefaultPathType() == 0);
        CHECK(manager->DefaultNetworkType() == 0);
        CHECK(manager->Layers().empty());
        CHECK(manager->Responses().empty());
        CHECK(manager->TrafficSimulator() == nullptr);
        CHECK(manager->EffectManager().Effects().empty());
        CHECK(!manager->Initialized());
        CHECK(manager->Init());
        CHECK(manager->Initialized());
        CHECK(manager->TrafficSimulator() == &host.trafficSimulator);
        CHECK(host.trafficSimulator.refs == 2);   // acquired in Init
        CHECK(manager->Init());                    // idempotent
        CHECK(host.trafficSimulator.refs == 2);
        CHECK(host.view.zoom == 3);

        // cSC4VehicleManager's constructor (0x1000d9c40) writes exactly these values.
        manager->SetDefaultTypes(1, 1);
        manager->SetCrimeLayerMultiplier(0.2f);
        for (int i = 0; i < cSC4TrafficAutomataManager::kVehicleLayerCount; ++i)
            manager->AddLayer(cSC4TrafficAutomataManager::VehicleLayerOrder()[i]);
        CHECK(manager->DefaultPathType() == 1);
        CHECK(manager->DefaultNetworkType() == 1);
        CHECK(manager->CrimeLayerMultiplier() == 0.2f);
        CHECK(manager->Layers().size() == 10);
        std::vector<std::uint32_t> order;
        for (const tTrafficLayerUpdate& update : manager->Layers()) order.push_back(update.layer);
        CHECK((order == std::vector<std::uint32_t>{1, 2, 3, 5, 4, 7, 6, 8, 9, 10}));
        CHECK(cSC4TrafficAutomataManager::PathTypeTable()[0] == 3);
        CHECK(cSC4TrafficAutomataManager::AutomataGroupTable()[0] == 0x4200);

        // ---- the layer -> path type / automata group tables (0x100a18b20 / 0x100a18b40)
        manager->SetDefaultTypes(1, 1);
        manager->SetDefaultGroup(0x4002);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(1)) == 1);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(2)) == 1);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(3)) == 1);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(4)) == 3);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(5)) == 3);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(6)) == 4);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(7)) == 1);   // mask 0x67 skips layer 7
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(8)) == 1);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(9)) == 6);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(10)) == 7);
        CHECK(manager->GetPathTypeForLayer(static_cast<PopulateLayer>(11)) == 1);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(1)) == 0x4002);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(2)) == 0x4200);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(3)) == 0x4204);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(4)) == 0x4215);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(5)) == 0x4216);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(6)) == 0x4217);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(7)) == 0x4002);   // mask 0x1df skips 7
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(8)) == 0x4102);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(9)) == 0x422c);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(10)) == 0x422e);
        CHECK(manager->GetAutomataGroupForLayer(static_cast<PopulateLayer>(11)) == 0x4002);

        // ---- the max-autos table (0x100a752d0) and the curve table (0x100a75280)
        tuning.f218f8 = 10.0f;
        tuning.f21970 = 20.0f;
        tuning.f21948 = 30.0f;
        tuning.f21920 = 40.0f;
        tuning.f218d0 = 50.0f;
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(2)) == 10.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(3)) == 20.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(4)) == 30.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(5)) == 40.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(6)) == 30.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(7)) == 50.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(8)) == 50.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(9)) == 30.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(10)) == 30.0f);
        CHECK(manager->GetMaxAutosPerSecond(static_cast<PopulateLayer>(11)) == 50.0f);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(2)) == &tuning.f218d8);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(3)) == &tuning.f21950);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(4)) == &tuning.f21928);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(5)) == &tuning.f21900);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(6)) == &tuning.f21928);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(7)) == &tuning.f218b0);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(8)) == &tuning.f218b0);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(9)) == &tuning.f21928);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(10)) == &tuning.f21928);
        CHECK(&manager->GetEdgeDensityCurve(static_cast<PopulateLayer>(11)) == &tuning.f218b0);

        // ---- IsTrainGroup (0x100112812): the 0x4f80000700085 mask over 0x4201..0x4233 plus 0x4005
        const std::uint32_t trainGroups[] = {0x4201, 0x4203, 0x4208, 0x4215, 0x4216, 0x4217,
                                             0x422c, 0x422d, 0x422e, 0x422f, 0x4230, 0x4233, 0x4005};
        for (std::uint32_t group : trainGroups) CHECK(manager->IsTrainGroup(group));
        const std::uint32_t nonTrainGroups[] = {0x4200, 0x4202, 0x4204, 0x4205, 0x4234, 0x4004, 0};
        for (std::uint32_t group : nonTrainGroups) CHECK(!manager->IsTrainGroup(group));

        // ---- IsLayerVisible (i386 0x000ca21a): view level 1 for the 0x7be layers, level 2 (underground) for layer 6
        host.view.level = 1;
        for (std::uint32_t layer : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 10u})
            CHECK(manager->IsLayerVisible(layer));
        CHECK(!manager->IsLayerVisible(6));
        CHECK(!manager->IsLayerVisible(0));
        CHECK(!manager->IsLayerVisible(11));
        host.view.level = 2;
        CHECK(manager->IsLayerVisible(6));
        CHECK(!manager->IsLayerVisible(1));
        host.view.level = 3;
        for (std::uint32_t layer = 0; layer <= 11; ++layer) CHECK(!manager->IsLayerVisible(layer));
        host.view.level = 1;

        // ---- the effect manager (+0xb60): CreateAccident and DoAnimationTick's update
        FakeCityOccupant* accidentOccupant = new FakeCityOccupant();
        CHECK(manager->EffectManager().CreateAccident(accidentOccupant, 5, 1000) != nullptr);
        CHECK(manager->EffectManager().Effects().size() == 1);
        const TrafficEffect& effect = manager->EffectManager().Effects().front();
        CHECK(effect.radius == 5 && effect.timeLeft == 1000);
        CHECK(effect.nearRect.x0 == -4 && effect.farRect.x1 == 5);
        manager->CreateAccident(accidentOccupant, 2, 500);
        CHECK(manager->EffectManager().Effects().size() == 2);
        // Update runs through the manager's ITrafficEffectHost (the timer at +0xb78).
        manager->EffectManager().Update();
        host.clockPercent = 80.0f;
        manager->DoAnimationTick(0, 16.0f);
        CHECK(manager->EffectManager().Effects().size() == 2);

        // ---- GetCommotionMultiplier (0x10011596c): only with the base's loaded flag, clamped to [0, 0.5]
        CHECK(manager->GetCommotionMultiplier(nullptr, PopulateLayer::kLayer1) == 1.0f);
        auto* loaded = new cRZMessage2Standard();
        loaded->AddRef();
        loaded->SetType(0x4a7fb807);
        loaded->SetData1(1);
        CHECK(manager->cSC4AutomataManager::DoMessage(loaded));
        CHECK(manager->LoadedFlag() != 0);
        loaded->Release();
        host.clockPercent = 80.0f;
        CHECK(std::fabs(manager->GetCommotionMultiplier(nullptr, PopulateLayer::kLayer1) - 0.2f) < 1e-6f);
        host.clockPercent = 200.0f;
        CHECK(manager->GetCommotionMultiplier(nullptr, PopulateLayer::kLayer1) == 0.0f);
        host.clockPercent = 0.0f;
        CHECK(manager->GetCommotionMultiplier(nullptr, PopulateLayer::kLayer1) == 0.5f);
        host.clockPercent = 80.0f;

        // ---- GetEdgeDensityMultiplier (0x1001153c6) = (fraction / 3600) * curve(hour)
        tuning.f218b0.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {12.0f, 3.0f}, {24.0f, 5.0f}});
        host.hour = 12;
        host.timeFraction = 0.5f;
        const double expectedMultiplier = static_cast<double>(0.5f * 0.00027777778f) * 3.0;
        CHECK(manager->GetEdgeDensityMultiplier(static_cast<PopulateLayer>(7)) == expectedMultiplier);

        // ---- IsAutomatonOnscreen (0x1001159fc): the base view rect expanded by 48, x/z only
        host.view.rect[0] = 10.0f; host.view.rect[1] = 20.0f; host.view.rect[2] = 30.0f; host.view.rect[3] = 40.0f;
        manager->HandleViewChanged();
        CHECK(manager->ViewRect(0) == 10.0f - 64.0f);   // HandleViewChanged expands the rect by 64
        FakeTrafficAutomaton onscreen;
        onscreen.position[0] = 0.0f; onscreen.position[2] = 0.0f;
        CHECK(manager->IsAutomatonOnscreen(&onscreen));
        onscreen.position[0] = -54.0f - 64.0f;
        CHECK(!manager->IsAutomatonOnscreen(&onscreen));

        // ---- release: Shutdown releases the traffic simulator, the responses and the effects
        CHECK(manager->Release() == 0);
        CHECK(host.trafficSimulator.refs == 1);
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 0);
        delete accidentOccupant;
    }

    // ---------------------------------------------------------------- automaton-facing traffic slots
    {
        auto* manager = new cSC4TrafficAutomataManager(&host);
        manager->AddRef();
        CHECK(manager->Init());
        manager->SetDefaultTypes(1, 1);
        manager->SetDefaultGroup(0x4002);
        auto* automaton = new FakeTrafficAutomaton();
        host.trafficAutomata[automaton] = automaton;

        // ---- IsTrafficAutomaton (0x100112d36)
        automaton->occupant.type = kTrafficOccupantType;
        CHECK(manager->IsTrafficAutomaton(automaton));
        automaton->occupant.type = kTrainOccupantType;
        CHECK(manager->IsTrafficAutomaton(automaton));
        automaton->occupant.type = 0xdeadbeef;
        CHECK(!manager->IsTrafficAutomaton(automaton));
        CHECK(!manager->IsTrafficAutomaton(nullptr));

        // ---- GetDefaultPathType (0x100115fa0)
        automaton->train = false;
        CHECK(manager->GetDefaultPathType(automaton) == 1);
        automaton->train = true;
        automaton->occupant.groups = {0x422f};
        CHECK(manager->GetDefaultPathType(automaton) == 7);
        automaton->occupant.groups = {0x422d};
        CHECK(manager->GetDefaultPathType(automaton) == 6);
        automaton->occupant.groups = {0x4208};
        CHECK(manager->GetDefaultPathType(automaton) == 4);
        automaton->occupant.groups = {0x4217};
        CHECK(manager->GetDefaultPathType(automaton) == 5);
        automaton->occupant.groups = {0x4201};
        CHECK(manager->GetDefaultPathType(automaton) == 3);
        automaton->train = false;
        automaton->occupant.groups.clear();

        // ---- leader bookkeeping: RecalculateLeaders (0x100115ac0), RemoveAutomaton (0x1001159c4)
        auto* follower = new FakeTrafficAutomaton();
        auto* newLeader = new FakeTrafficAutomaton();
        host.trafficAutomata[follower] = follower;
        host.trafficAutomata[newLeader] = newLeader;
        manager->AddAutomaton(automaton);
        manager->AddAutomaton(follower);
        follower->AddRef();   // the test's own reference
        automaton->AddRef();
        automaton->findLeaderResult = newLeader;
        follower->leader = automaton;
        follower->leader->AddRef();
        manager->RecalculateLeaders(automaton);
        CHECK(follower->leader == newLeader);
        automaton->findLeaderResult = nullptr;
        follower->SetLeader(automaton);
        manager->RemoveAutomaton(automaton, true);
        CHECK(follower->leader == nullptr);   // RecalculateLeaders ran through the traffic view
        newLeader->Release();

        // ---- HandleRemoveOccupant (0x100112074) drops the response entries that target the occupant
        auto* cityOccupant = new FakeCityOccupant();
        FakeOccupant* requester = new FakeOccupant();
        host.cityOccupants[requester] = cityOccupant;
        CHECK(manager->DispatchResponseAutomata(0x1402, cityOccupant, 3, false, 0, 0));
        CHECK(manager->DispatchResponseAutomata(0x1402, nullptr, 1, false, 0, 0) == false);
        CHECK(manager->DispatchResponseCount() == 1);
        CHECK(manager->Responses().front().flags == 0x02000000u);
        CHECK(manager->Responses().front().count == 3);
        manager->HandleRemoveOccupant(requester);
        CHECK(manager->DispatchResponseCount() == 0);

        // ---- HandleRemoveNetworkOccupant (0x100111ee2)
        FakeTrafficOccupant networkOccupant;
        networkOccupant.city.rect = SC4CellRect{10, 10, 12, 12};
        host.networkOccupantViews[&networkOccupant] = &networkOccupant;
        auto* matching = new FakeTrafficAutomaton();
        auto* inRect = new FakeTrafficAutomaton();
        auto* special = new FakeTrafficAutomaton();
        auto* outside = new FakeTrafficAutomaton();
        for (FakeTrafficAutomaton* candidate : {matching, inRect, special, outside})
            host.trafficAutomata[candidate] = candidate;
        matching->networkOccupant = &networkOccupant;
        inRect->flags = 0x2000;
        inRect->position[0] = 11.0f * 16.0f;
        inRect->position[2] = 11.0f * 16.0f;
        special->flags = 0x2000 | 0x20000;
        special->position[0] = 11.0f * 16.0f;
        special->position[2] = 11.0f * 16.0f;
        outside->flags = 0x2000;
        outside->position[0] = 40.0f * 16.0f;
        outside->position[2] = 40.0f * 16.0f;
        for (FakeTrafficAutomaton* candidate : {matching, inRect, special, outside}) {
            candidate->AddRef();
            manager->AddAutomaton(candidate);
        }
        CHECK(manager->HandleRemoveNetworkOccupant(&networkOccupant));
        CHECK(matching->pathSets == 1 && matching->networkOccupant == nullptr && matching->occupantRemovals == 1);
        CHECK(inRect->occupantRemovals == 1);
        CHECK(special->occupantRemovals == 0);
        CHECK(outside->occupantRemovals == 0);

        // ---- ChooseNetworkTypeForPathType (0x100114de4)
        networkOccupant.networkFlags = 0xffffffffu;
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 1) == 2);
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 2) == 2);
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 3) == 1);
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 4) == 7);
        networkOccupant.networkFlags = 0x8;   // no flag 2 / 6 / 0 -> no network
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 1) == -1);
        networkOccupant.networkFlags = 0x0;
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 1) == -1);
        CHECK(manager->ChooseNetworkTypeForPathType(&networkOccupant, 9) == -1);

        // ---- RecallAutomatonFromNetwork (0x100114c6e) removes one matching automaton of the tile list
        TrafficTileInfo tile;
        FakeTileAutomaton good, wrongType, flagged, faded;
        good.pathKey = 3u << 24;
        good.flags = 2;
        good.opacity = 1.0f;
        wrongType.pathKey = 4u << 24;
        wrongType.flags = 2;
        flagged.pathKey = 3u << 24;
        flagged.flags = 2 | 0x80;
        faded.pathKey = 3u << 24;
        faded.flags = 2;
        faded.opacity = 0.5f;
        for (FakeTileAutomaton* candidate : {&wrongType, &flagged, &faded, &good})
            tile.automata.push_back(candidate);
        manager->RecallAutomatonFromNetwork(&tile, 3);
        CHECK(good.removed);
        CHECK(!wrongType.removed && !flagged.removed && !faded.removed);

        // ---- IsOffscreenEntryEdge (0x100114162)
        host.trafficNetwork.rect[0] = 0;
        host.trafficNetwork.rect[1] = 0;
        host.trafficNetwork.rect[2] = 5;
        host.trafficNetwork.rect[3] = 5;
        FakeTrafficOccupant roadOccupant;
        roadOccupant.networkFlags = 0x200000;
        TrafficTileInfo roadTile;
        roadTile.occupant = &roadOccupant;
        host.trafficNetwork.tiles[{2, 1}] = &roadTile;
        CHECK(!manager->IsOffscreenEntryEdge(&roadTile, 1, 1, 0));   // cell (2,1) in rect with flag 0x200000
        CHECK(manager->IsOffscreenEntryEdge(&roadTile, 9, 9, 0));    // outside the rect
        roadOccupant.networkFlags = 0;
        CHECK(manager->IsOffscreenEntryEdge(&roadTile, 1, 1, 0));

        // ---- GetOccupantTripEdgeDensity (0x100115310): positive and transposed/negated negative multiplier
        TrafficTileInfo tripTile;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                tripTile.distribution[row][column] = static_cast<std::uint16_t>(row * 4 + column + 1);
        float weights[4][4] = {};
        // i386 0x000ca46a scales by GetCommotionMultiplier (1 while the commotion flag is clear), never by the max autos
        // per second: a max-autos value of 2 must not show in the weights.
        tuning.f218d0 = 2.0f;
        manager->GetOccupantTripEdgeDensity(nullptr, &tripTile, 0.5f, weights);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                CHECK(weights[row][column] == static_cast<float>(tripTile.distribution[row][column]) * 0.5f);
        manager->GetOccupantTripEdgeDensity(nullptr, &tripTile, -0.5f, weights);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                CHECK(weights[row][column] ==
                      -(static_cast<float>(tripTile.distribution[column][row]) * -0.5f));

        // ---- GetEdgeDensity (0x100115196 / i386 0x000cb5da): the 4x4 table from the traffic simulator, times the multiplier
        // and GetCommotionMultiplier (1 here); the max autos per second (3) must not show in the weights
        for (int i = 0; i < 16; ++i) host.trafficSimulator.table[i] = static_cast<std::uint16_t>(i * 10 + 1);
        networkOccupant.city.rect = SC4CellRect{4, 5, 6, 7};
        tuning.f218d0 = 3.0f;
        manager->GetEdgeDensity(&networkOccupant, &tile, PopulateLayer::kLayer1, 2.0f, weights);
        CHECK(host.trafficSimulator.lastNetworkType == 1);
        CHECK(host.trafficSimulator.lastCellX == 4 && host.trafficSimulator.lastCellZ == 5);
        CHECK(!host.trafficSimulator.mirrored);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                CHECK(weights[row][column] ==
                      static_cast<float>(host.trafficSimulator.table[row * 4 + column]) * 2.0f);
        manager->GetEdgeDensity(&networkOccupant, &tile, PopulateLayer::kLayer1, -2.0f, weights);
        CHECK(host.trafficSimulator.mirrored);
        manager->GetEdgeDensity(&networkOccupant, &tile, static_cast<PopulateLayer>(11), 1.0f, weights);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column) CHECK(weights[row][column] == 0.0f);
        // layer 3 maps to network type 4, layer 4 to 3, layer 9 to 7, layer 10 to 8.
        manager->GetEdgeDensity(&networkOccupant, &tile, PopulateLayer::kLayer3, 1.0f, weights);
        CHECK(host.trafficSimulator.lastNetworkType == 4);
        manager->GetEdgeDensity(&networkOccupant, &tile, PopulateLayer::kLayer4, 1.0f, weights);
        CHECK(host.trafficSimulator.lastNetworkType == 3);
        manager->GetEdgeDensity(&networkOccupant, &tile, PopulateLayer::kLayer9, 1.0f, weights);
        CHECK(host.trafficSimulator.lastNetworkType == 7);
        manager->GetEdgeDensity(&networkOccupant, &tile, PopulateLayer::kLayer10, 1.0f, weights);
        CHECK(host.trafficSimulator.lastNetworkType == 8);
        manager->GetEdgeDensity(&networkOccupant, &tripTile, PopulateLayer::kLayer7, 0.5f, weights);
        CHECK(weights[0][0] == static_cast<float>(tripTile.distribution[0][0]) * 0.5f);   // commotion 1, not max autos

        // ---- ChooseRandomModel (0x10011471a): no exemplar -> the key stays untouched
        cGZPersistResourceKey key;
        key.type = 7;
        manager->ChooseRandomModel(0x4200, &networkOccupant, PopulateLayer::kLayer1, key);
        CHECK(key.type == 7);

        // ---- Dispatch (0x100112842): a zero-key dispatch picks an exemplar, creates, fails to start and cleans up
        auto* exemplar = new FakeExemplar();
        AddUint(exemplar->holder, 0x10, 0x1234);
        AddUintArray(exemplar->holder, kPropertyOccupantGroups, {0x4200, 0x1402});
        host.exemplars.push_back(exemplar);
        host.exemplarKeys.push_back(cGZPersistResourceKey{kAutomataTuningResourceType, kAutomataTuningResourceGroup, 1});
        manager->SetExemplarGroup(0x1234);
        manager->ScanExemplars();
        CHECK(manager->GetExemplarData().size() == 1);
        FakeTrafficAutomaton* created = new FakeTrafficAutomaton();
        host.classCreated = created;
        host.trafficAutomata[created] = created;
        tAutomataLocation from;
        from.kind = kLocationCell;
        from.x = 5.0f;
        from.y = 7.0f;
        tAutomataLocation to;
        to.kind = kLocationCell;
        to.x = 6.0f;
        to.y = 8.0f;
        CHECK(manager->Dispatch(0x4200, from, to, 0, 3, cGZPersistResourceKey{}, 0) == nullptr);
        CHECK(created->shutdowns >= 1);   // no path finder: the failed route removes the new automaton
        delete created;

        // ---- UpdateResponseList (0x10011220c): retire after six failed lookups, refresh on success
        host.trafficNetwork.responseFound = false;
        cityOccupant->AddRef();
        CHECK(manager->DispatchResponseAutomata(0x4200, cityOccupant, 4, false, 6, 0));
        for (int i = 0; i < 5; ++i) manager->UpdateResponseList(1001.0f);
        CHECK(manager->DispatchResponseCount() == 1);   // five failures keep the entry
        manager->UpdateResponseList(1001.0f);
        CHECK(manager->DispatchResponseCount() == 0);   // the sixth retires it
        host.trafficNetwork.responseFound = true;
        host.trafficNetwork.responseOccupants.clear();
        CHECK(manager->DispatchResponseAutomata(0x4200, cityOccupant, 4, false, 6, 0));
        manager->UpdateResponseList(1.0f);
        CHECK(manager->DispatchResponseCount() == 1);
        CHECK(manager->Responses().front().timeLeft == 1000.0f);
        CHECK(manager->Responses().front().count == 4);
        CHECK(manager->DispatchResponseAutomata(0x4200, cityOccupant, 0, false, 6, 0));
        manager->UpdateResponseList(1.0f);
        CHECK(manager->DispatchResponseCount() == 1);   // the zero-count entry retired

        // ---- persistence (0x100116078 / 0x1001161b4): version 3, the base record, the (dropped) path type and layers
        PortMemoryOStream output;
        CHECK(manager->Write(output));
        auto* reader = new cSC4TrafficAutomataManager(&host);
        reader->AddRef();
        CHECK(reader->Init());
        for (int i = 0; i < cSC4TrafficAutomataManager::kVehicleLayerCount; ++i)
            reader->AddLayer(cSC4TrafficAutomataManager::VehicleLayerOrder()[i]);
        PortMemoryIStream input(output.bytes().data(), output.bytes().size());
        CHECK(reader->Read(input));
        CHECK(reader->DefaultPathType() == 0);   // the original drops the field (0x1001161f7)
        CHECK(reader->DefaultNetworkType() == 0);
        CHECK(reader->Responses().empty());
        std::uint16_t badVersion = 1;
        PortMemoryOStream badOutput;
        badOutput.SetUint16(badVersion);
        PortMemoryIStream badInput(badOutput.bytes().data(), badOutput.bytes().size());
        CHECK(!reader->Read(badInput));   // only versions 2 and 3 are accepted
        CHECK(reader->Release() == 0);

        // ---- the tick: no crash with layers and the populate paths, and the layers rotate
        for (int i = 0; i < cSC4TrafficAutomataManager::kVehicleLayerCount; ++i)
            manager->AddLayer(cSC4TrafficAutomataManager::VehicleLayerOrder()[i]);
        host.trafficNetwork.tiles.clear();
        host.trafficNetwork.rect[0] = 0;
        host.trafficNetwork.rect[1] = 0;
        host.trafficNetwork.rect[2] = 1;
        host.trafficNetwork.rect[3] = 1;
        TrafficTileInfo tickTile;
        FakeTrafficOccupant tickOccupant;
        tickOccupant.city.rect = SC4CellRect{0, 0, 1, 1};
        tickTile.occupant = &tickOccupant;
        host.trafficNetwork.tiles[{0, 0}] = &tickTile;
        host.trafficNetwork.edgeCounts[{static_cast<std::uint32_t>((5u << 8) | 3u), 0, 0}] = 1;
        tuning.f21aac = 0xffffffffu;
        manager->DoAnimationTick(0, 16.0f);
        manager->DoAnimationTick(0, 16.0f);
        manager->DoAnimationTick(0, 16.0f);
        manager->DoAnimationTick(0, 16.0f);
        CHECK(manager->TrafficSimulator() != nullptr);
        CHECK(manager->Layers().size() == 10);

        // ---- PopulateVisibleArea sweeps only cells inside the rectangle the manager keeps its automata by: the view's cells
        // can be ahead of it (a scroll in progress), and a car spawned outside it is removed as off screen on its first tick.
        {
            auto& network = host.trafficNetwork;
            network.rect[0] = 0; network.rect[1] = 0; network.rect[2] = 7; network.rect[3] = 7;
            const auto sweep = [&] {
                network.swept.clear();
                for (int i = 0; i < 24; ++i) manager->PopulateVisibleArea(16.0f);
            };
            host.view.rect[0] = 0.0f; host.view.rect[1] = 0.0f; host.view.rect[2] = 128.0f; host.view.rect[3] = 128.0f;
            manager->HandleViewChanged();   // kept: -64..192 m, every one of the 8 x 8 cells
            sweep();
            bool outside = false;
            for (const auto& cell : network.swept) outside = outside || cell.first < 6 || cell.second < 6;
            CHECK(!network.swept.empty());
            CHECK(outside);                 // the control: with the whole view kept, the pass reaches the low cells
            host.view.rect[0] = 160.0f; host.view.rect[1] = 160.0f; host.view.rect[2] = 200.0f; host.view.rect[3] = 200.0f;
            manager->HandleViewChanged();   // kept: 96..264 m = cells 6 and up
            sweep();
            outside = false;
            for (const auto& cell : network.swept) outside = outside || cell.first < 6 || cell.second < 6;
            CHECK(!network.swept.empty());
            CHECK(!outside);
        }

        for (FakeTrafficAutomaton* candidate : {matching, inRect, special, outside, follower}) {
            manager->RemoveAutomaton(candidate, true);
            candidate->Release();
        }
        manager->Release();
        delete automaton;
        delete exemplar;
        delete requester;
        delete cityOccupant;
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 0);
    }

    std::printf("%s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
