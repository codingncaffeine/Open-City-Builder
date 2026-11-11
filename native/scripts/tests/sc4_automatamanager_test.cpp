// cSC4AutomataManager — unit test with a fake host. Every public entry point the manager exposes is exercised: the lifecycle,
// the automata registry and the group tracking, the dispatch-location translation, the animation tick, the message handlers
// (both message flavours), the exemplar scan/tuning readers and the camera follow. The fakes stand in for the neighbour
// classes (the seam declarations in AutomataManager.h); the assertions mark the quirks kept from the binary with "quirk".
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataManager.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

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
struct FakeAutomaton final : IAutomataAutomaton {
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
    FakeAutomaton* created = nullptr;
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
    int zoom = 3, rotation = 1;
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

// ---- host ------------------------------------------------------------------------------------------------------------------
struct FakeHost final : IAutomataManagerHost {
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
};

// ---- property helpers ------------------------------------------------------------------------------------------------------
void AddUint(cSCPropertyHolder& holder, std::uint32_t id, std::uint32_t value) {
    cRZVariant variant(value);
    holder.AddProperty(id, &variant, false);
}
void AddFloat(cSCPropertyHolder& holder, std::uint32_t id, float value) {
    cRZVariant variant;
    variant.SetValFloat32(value);
    holder.AddProperty(id, &variant, false);
}
void AddBool(cSCPropertyHolder& holder, std::uint32_t id, bool value) {
    cRZVariant variant;
    variant.SetValBool(value);
    holder.AddProperty(id, &variant, false);
}
void AddByte(cSCPropertyHolder& holder, std::uint32_t id, std::uint8_t value) {
    cRZVariant variant;
    variant.SetValUint8(value);
    holder.AddProperty(id, &variant, false);
}
void AddString(cSCPropertyHolder& holder, std::uint32_t id, const char* value) {
    cRZString text(value);
    holder.AddProperty(id, text, false);
}
void AddFloatArray(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<float> values) {
    cRZVariant variant;
    variant.RefFloat32(values.begin(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, false);
}
void AddUintArray(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<std::uint32_t> values) {
    cRZVariant variant;
    variant.RefUint32(values.begin(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, false);
}
void AddByteArray(cSCPropertyHolder& holder, std::uint32_t id, std::initializer_list<std::uint8_t> values) {
    cRZVariant variant;
    variant.RefUint8(values.begin(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, false);
}

}  // namespace

int main() {
    FakeHost host;
    host.tuningHolder->AddRef();   // the test's own reference (the manager releases the one it is handed)
    SetAutomataManagerHost(&host);

    // ---------------------------------------------------------------- lifecycle
    {
        auto* manager = new cSC4AutomataManager();
        manager->AddRef();   // the creator's reference (refcount 1 means deleted)
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 0);
        CHECK(manager->StaticInit());
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 1);
        CHECK(host.cheatsRegistered.size() == 5);
        CHECK(host.cheatsRegistered[0].second == "DrawPaths");
        CHECK(host.cheatsRegistered[1].second == "HidePaths");
        CHECK(host.cheatsRegistered[2].second == "udriveit");
        CHECK(host.cheatsRegistered[3].second == "NoCSI");
        CHECK(host.cheatsRegistered[4].second == "Casper");

        CHECK(!manager->Initialized());
        CHECK(manager->Init());
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 2);   // Init runs StaticInit too
        CHECK(manager->Initialized());
        CHECK(!manager->PostCityInitPending());
        CHECK(host.ticksAdded.size() == 1 && host.ticksAdded[0] == 0xff);
        CHECK(host.cheatListenerAdds == 1);
        CHECK(host.messages.added.size() == 14);
        CHECK(manager->Init());   // idempotent
        CHECK(host.messages.added.size() == 14);

        CHECK(manager->GetMinZoom() == 0);
        manager->SetMinZoom(2);
        CHECK(manager->GetMinZoom() == 2);
        CHECK(manager->IsActive());
        CHECK(!manager->IsPaused());
        manager->Pause();
        CHECK(manager->IsPaused());
        manager->Resume();
        CHECK(!manager->IsPaused());
        CHECK(manager->GetAutomataCount() == 0);

        // QI: four views + the unknown id
        void* view = nullptr;
        CHECK(manager->QueryInterface(kGZIID_cISC4AutomataManager, &view) && view == static_cast<cISC4AutomataManager*>(manager));
        manager->Release();
        CHECK(manager->QueryInterface(kGZIID_cIGZSerializableView, &view) && view != nullptr);
        manager->Release();
        CHECK(manager->QueryInterface(1, &view) && view != nullptr);
        manager->Release();
        CHECK(manager->QueryInterface(kGZIID_cIGZMessageTarget2View, &view) && view != nullptr);
        manager->Release();
        CHECK(!manager->QueryInterface(0xdeadbeef, &view));
        CHECK(manager->Release() == 0);   // Shutdown + delete

        CHECK(host.ticksRemoved.size() == 1 && host.ticksRemoved[0] == 0xff);
        CHECK(host.cheatListenerRemoves == 1);
        CHECK(host.messages.removed.size() == 14);
        CHECK(host.network.refs == 1);   // acquired and released once
        CHECK(host.script.refs == 1);
        CHECK(host.clock.refs == 1);
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 1);   // Release -> Shutdown -> StaticShutdown
        auto* closer = new cSC4AutomataManager();
        CHECK(closer->StaticShutdown());
        CHECK(cSC4AutomataManager::StaticInstanceCount() == 0);
        CHECK(host.cheatsUnregistered.size() == 5);
        delete closer;
    }

    // ---------------------------------------------------------------- exemplars and group tracking
    FakeGroupTemplate groupA, groupB, parent;
    groupA.id = 0x1402;
    groupA.classId = 0xa9b40f05;
    groupA.parent = &parent;
    groupB.id = 0x1406;
    groupB.parent = &parent;
    parent.id = 0x1000;
    parent.source = 0x2000;
    host.script.byId[0x1402] = &groupA;
    host.script.byId[0x1406] = &groupB;
    host.script.byId[0x1000] = &parent;
    host.script.trackBuildings = {0x2000, 0x3000};

    FakeExemplar exemplarA, exemplarB;
    exemplarA.key = cGZPersistResourceKey{0x6534284a, 1, 2};
    AddUint(exemplarA.holder, 0x10, 0x4002);   // the exemplar group the manager keeps
    AddString(exemplarA.holder, 0x20, "car_a");
    AddUintArray(exemplarA.holder, 0xaa1dd396, {0x1402});
    AddUintArray(exemplarA.holder, 0x9c80416, {0x6534284a, 1, 2});
    AddByteArray(exemplarA.holder, 0x696e88db, {1, 2});
    AddByteArray(exemplarA.holder, 0x296e88e9, {3});
    AddByteArray(exemplarA.holder, 0x496e88f1, {2});
    AddByteArray(exemplarA.holder, 0xc973c9aa, {1});
    AddBool(exemplarA.holder, 0xaa5a9633, true);
    AddUint(exemplarA.holder, 0x2a62d9f3, 7);
    exemplarB.key = cGZPersistResourceKey{0x6534284a, 3, 4};
    AddUint(exemplarB.holder, 0x10, 0x4002);
    AddString(exemplarB.holder, 0x20, "car_b");
    AddUintArray(exemplarB.holder, 0xaa1dd396, {0x1406});   // a group that is not in the script system
    host.exemplars = {&exemplarA};
    host.exemplarKeys = {exemplarA.key};

    auto* manager = new cSC4AutomataManager();
    manager->AddRef();
    manager->SetExemplarGroup(0x4002);
    manager->SetDefaultGroup(0x1000);
    manager->SetFallbackGroup(0x4002);
    CHECK(manager->Init());

    CHECK(manager->GetExemplarData().size() == 1);
    const tExemplarData& data = manager->GetExemplarData()[0];
    CHECK(std::strcmp(data.name.Data(), "car_a") == 0);
    CHECK(data.groupCount == 2);   // 0x1402 + its parent 0x1000
    CHECK(data.groupIds[0] == 0x1402);
    CHECK(data.groupIds[1] == 0x1000);
    CHECK(data.defaultGroup == 0x1402);   // the most specific listed group (the leaf of the chain)
    CHECK(data.networkMask0 == ((1u << 0) | (1u << 1)));
    CHECK(data.networkMask1 == (1u << 2));
    CHECK(data.networkMask2 == (1u << 1));
    CHECK(data.layerMask == (1u << 1));
    CHECK(data.flag50);
    CHECK(data.crimeType == 7);
    CHECK(data.modelKey.type == 0x6534284a && data.modelKey.group == 1 && data.modelKey.instance == 2);
    CHECK(manager->ExemplarDataMap().count(0x1402) == 1);
    CHECK(manager->ExemplarDataMap().count(0x1000) == 1);
    CHECK(manager->TrackedBuildings().count(0x2000) == 1);   // parent's source building
    CHECK(manager->TrackedBuildings().count(0x3000) == 1);   // seeded from the script's tracking list

    CHECK(manager->CanCreateGroup(0x1402));
    CHECK(!manager->CanCreateGroup(0x9999));

    // FindExemplarFromModelKey + the choosers
    CHECK(manager->FindExemplarFromModelKey(cGZPersistResourceKey{0x6534284a, 1, 2}) == &data);
    CHECK(manager->FindExemplarFromModelKey(cGZPersistResourceKey{0x6534284a, 9, 9}) == nullptr);
    CHECK(manager->ChooseRandomExemplar(0x1402, PopulateLayer::kLayerNone, -1, -1) == &data);
    CHECK(manager->ChooseRandomExemplar(0x1402, PopulateLayer::kLayer2, -1, -1) == nullptr);   // layer 2 not in the mask
    CHECK(manager->ChooseRandomExemplar(0x1402, PopulateLayer::kLayer1, -1, -1) == &data);
    host.network.masks[0] = 1;
    host.network.masks[1] = 4;
    host.network.masks[2] = 2;
    CHECK(manager->ChooseRandomExemplar(0x1402, PopulateLayer::kLayerNone, 1, 1) == &data);
    host.network.masks[2] = 0x80;   // the exemplar does not match
    CHECK(manager->ChooseRandomExemplar(0x1402, PopulateLayer::kLayerNone, 1, 1) == nullptr);
    host.network.cellFound = false;
    CHECK(manager->ChooseRandomExemplar(0x1402, PopulateLayer::kLayerNone, 1, 1) == &data);
    host.network.cellFound = true;
    CHECK(manager->ChooseRandomExemplarByFilter(0x1402, 1u << 1, 1, 4, 2) == &data);
    CHECK(manager->ChooseRandomExemplarByFilter(0x1402, 1u << 2, 1, 4, 2) == nullptr);
    CHECK(manager->ChooseExemplarByCrimeType(0x1402, 7, PopulateLayer::kLayer1) == &data);
    CHECK(manager->ChooseExemplarByCrimeType(0x1402, 8, PopulateLayer::kLayer1) == nullptr);
    CHECK(manager->ChooseExemplarByCrimeType(0x1402, 0, PopulateLayer::kLayer1) == &data);

    // ---------------------------------------------------------------- registry and group instances
    FakeAutomaton automaton;
    automaton.group = 0x1402;
    automaton.occupant.groups = {0x1402};
    automaton.position[0] = 100.0f;
    automaton.position[2] = 50.0f;

    CHECK(manager->TrackAutomataGroupInstances(0x1402, true));
    CHECK(!manager->TrackAutomataGroupInstances(0x1402, true));   // already tracked
    CHECK(manager->TrackedGroupInstances().count(0x1402) == 1);
    CHECK(manager->AddAutomaton(&automaton));
    CHECK(manager->GetAutomataCount() == 1);
    CHECK(automaton.inits == 1);
    CHECK(automaton.refs == 3);   // caller + list + the tracked group set
    CHECK(automaton.placedOccupant);
    CHECK(automaton.placedRenderer);
    CHECK(manager->TrackedGroupInstances().at(0x1402).count(&automaton) == 1);

    std::unordered_set<IAutomataAutomaton*> instances;
    CHECK(manager->GetInstancesOfGroup(0x1402, instances));
    CHECK(instances.count(&automaton) == 1);
    manager->ReleaseAutomatonSet(instances);
    CHECK(instances.empty());
    CHECK(automaton.refs == 3);   // the set's reference is gone again, the tracked set keeps one
    instances.clear();
    CHECK(manager->GetInstancesOfGroup(0x9999, instances) == false);

    // occupant group changed on a tracked group
    manager->HandleOccupantGroupChanged(&automaton);
    CHECK(manager->TrackedGroupInstances().at(0x1402).count(&automaton) == 1);
    CHECK(automaton.refs == 3);

    // RemoveAutomaton(force = false) retires but keeps the automaton (quirk)
    CHECK(manager->RemoveAutomaton(&automaton, false));
    CHECK(automaton.sentEvents.size() == 1 && automaton.sentEvents[0] == 2);
    CHECK(automaton.marked);
    CHECK(automaton.effectsCleared);
    CHECK(manager->GetAutomataCount() == 1);
    CHECK(automaton.refs == 3);

    // RemoveAutomaton(force = true) unlinks and releases
    CHECK(manager->RemoveAutomaton(&automaton, true));
    CHECK(automaton.sentEvents.size() == 2 && automaton.sentEvents[1] == 3);
    CHECK(!automaton.placedOccupant && !automaton.placedRenderer);
    CHECK(automaton.shutdowns == 1);
    CHECK(manager->GetAutomataCount() == 0);
    CHECK(automaton.refs == 1);
    CHECK(manager->TrackedGroupInstances().at(0x1402).empty());

    // ---------------------------------------------------------------- SetMaxCount / AdjustCountForAutomaton
    FakeAutomaton low, high, protectedOne;
    low.priority = 1;
    high.priority = 10;
    protectedOne.priority = 0;
    protectedOne.flags = 0x200000;
    manager->AddAutomaton(&low);
    manager->AddAutomaton(&high);
    manager->AddAutomaton(&protectedOne);
    CHECK(manager->GetAutomataCount() == 3);
    manager->SetMaxCount(2);
    CHECK(manager->MaxCount() == 2);
    CHECK(manager->MaxCountLimit() == 2);   // 2 * 1.25 = 2 (truncated)
    CHECK(!low.placedOccupant);             // unplaced (RemoveAutomatonInternal)
    CHECK(protectedOne.placedOccupant);      // flag 0x200000 keeps it
    manager->SetMaxCount(-5);
    CHECK(manager->MaxCount() == 0);
    manager->SetMaxCount(9000);
    CHECK(manager->MaxCount() == 5000);
    CHECK(manager->GetAutomataCount() == 3);   // the list is never shrunk by SetMaxCount (quirk)

    CHECK(!manager->AdjustCountForAutomaton(0));
    low.priority = 1;
    high.priority = 10;
    CHECK(manager->AdjustCountForAutomaton(5));
    CHECK(protectedOne.removedSoft);   // the first automaton in the list is removed regardless of its flag
    manager->DestroyAutomata();
    CHECK(manager->GetAutomataCount() == 0);
    CHECK(low.shutdowns == 1 && high.shutdowns == 1 && protectedOne.shutdowns == 1);

    // ---------------------------------------------------------------- dispatch locations
    tAutomataLocation location;
    tCellRect rect;
    location.kind = kLocationCell;
    location.x = 5.0f;
    location.y = 6.0f;
    CHECK(manager->TranslateDispatchLocation(location, rect));
    CHECK(rect.x0 == 5 && rect.x1 == 5 && rect.z0 == 6 && rect.z1 == 6);
    location.kind = kLocationCellRect;
    location.z = 9.0f;
    location.w = 11.0f;
    CHECK(manager->TranslateDispatchLocation(location, rect));
    CHECK(rect.x0 == 5 && rect.z0 == 6 && rect.x1 == 9 && rect.z1 == 11);
    location.kind = kLocationWorldPosition;
    location.x = 100.0f;
    location.y = 0.0f;
    location.z = 64.0f;   // 64/16 = 4 -> cell 4
    CHECK(manager->TranslateDispatchLocation(location, rect));
    CHECK(rect.x0 == 6 && rect.z0 == 4);
    location.kind = kLocationOccupant;
    location.occupant = &automaton.occupant;
    automaton.occupant.position[0] = 12.0f;
    automaton.occupant.position[2] = 34.0f;
    automaton.occupant.type = kBuildingOccupantType;
    CHECK(manager->TranslateDispatchLocation(location, rect));
    CHECK(rect.x0 == 11 && rect.x1 == 13);
    automaton.occupant.type = 0x12345678;
    CHECK(manager->TranslateDispatchLocation(location, rect));
    CHECK(rect.x0 == 0 && rect.z0 == 2 && rect.x1 == 0);   // z1 untouched (quirk)
    location.kind = 9;
    CHECK(!manager->TranslateDispatchLocation(location, rect));
    location.occupant = nullptr;
    location.kind = kLocationOccupant;
    CHECK(!manager->TranslateDispatchLocation(location, rect));

    cS3DVector3 world{0, 0, 0};
    location.kind = kLocationCell;
    location.x = 2.0f;
    location.y = 3.0f;
    CHECK(manager->TranslateDispatchLocation(location, world));
    CHECK(world.x == 32.0f && world.z == 48.0f && world.y == host.network.height);
    location.kind = kLocationCellRect;
    location.z = 4.0f;
    location.w = 5.0f;
    CHECK(manager->TranslateDispatchLocation(location, world));
    CHECK(world.x == (64.0f - 32.0f) * 0.5f + 32.0f);
    location.kind = kLocationWorldPosition;
    location.x = 1.0f;
    location.y = 2.0f;
    location.z = 3.0f;
    CHECK(manager->TranslateDispatchLocation(location, world));
    CHECK(world.x == 1.0f && world.z == 3.0f);
    location.kind = kLocationOccupant;
    location.occupant = &automaton.occupant;
    automaton.occupant.position[0] = 12.0f;
    automaton.occupant.position[2] = 34.0f;
    CHECK(manager->TranslateDispatchLocation(location, world));
    CHECK(world.x == 12.0f && world.z == 34.0f);
    location.occupant = nullptr;
    CHECK(!manager->TranslateDispatchLocation(location, world));

    // the two dispatch entry points are stubs (quirk)
    CHECK(manager->Dispatch(0x1402, location, location, 0, 0, cGZPersistResourceKey{}, 0) == nullptr);
    CHECK(!manager->Dispatch(&automaton, location, location, 0, 0));

    // ---------------------------------------------------------------- creation
    manager->SetExemplarGroup(0x4002);
    manager->ScanExemplars();
    CHECK(manager->GetExemplarData().size() == 1);
    FakeAutomaton created;
    host.kinds[0].handled = 0x1402;
    host.kinds[0].created = &created;
    CHECK(cSC4AutomataManager::StaticCreateAutomaton(0x1402, cS3DVector3{0, 0, 0}, 0, cGZPersistResourceKey{}, nullptr, 0) ==
          &created);
    CHECK(created.group == 0x1402);   // the per-kind manager stamps the group
    // an unknown group reports the error text and returns the last probed manager (null here)
    host.kinds[3].handled = 0x7777;
    CHECK(host.reported == 0);
    manager->SetExemplarGroup(0x4002);
    // CreateAutomaton with a listener and a velocity
    FakeAutomaton autoCreated;
    host.classCreated = &autoCreated;
    IAutomataAutomaton* result =
        manager->CreateAutomaton(0x1402, cS3DVector3{1, 2, 3}, 0, cGZPersistResourceKey{}, nullptr, 0, nullptr);
    CHECK(result == &autoCreated);
    CHECK(autoCreated.callbacks == 1);
    CHECK(autoCreated.position[0] == 1.0f && autoCreated.position[2] == 3.0f);
    CHECK(manager->GetAutomataCount() == 1);
    manager->DestroyAutomata();

    // GetRandomCreationPoint: needs a creatable group and the terrain height
    cS3DVector3 direction{0, 0, 0};
    CHECK(manager->GetRandomCreationPoint(0x1402, cS3DVector3{0, 0, 0}, 2.0f, 10.0f, cS3DVector3{5, 0, 5}, direction,
                                          0));
    CHECK(direction.y == host.terrainHeight);   // the height is written over the direction's y (quirk)
    CHECK(!manager->GetRandomCreationPoint(0x9999, cS3DVector3{0, 0, 0}, 0, 0, cS3DVector3{0, 0, 0}, direction, 0));

    // GetAutomataManagerForGroup
    host.kinds[2].handled = 0x1406;
    CHECK(cSC4AutomataManager::GetAutomataManagerForGroup(0x1406) == &host.kinds[2]);
    host.kinds[2].handled = 0;
    host.reported = 0;
    CHECK(cSC4AutomataManager::GetAutomataManagerForGroup(0x1234) == &host.kinds[3]);
    CHECK(host.reported == 1);
    CHECK(host.reports.back().find("Couldn't find automata manager") == 0);

    // ---------------------------------------------------------------- collision / onscreen
    FakeAutomaton other;
    other.radius = 4.0f;
    other.position[0] = 100.0f;
    other.position[2] = 50.0f;
    manager->AddAutomaton(&automaton);
    automaton.position[0] = 0.0f;
    automaton.position[2] = 0.0f;
    automaton.radius = 4.0f;
    other.position[0] = 100.0f;
    other.position[2] = 50.0f;
    manager->AddAutomaton(&other);
    CHECK(manager->GetCollidingAutomaton(&automaton, 0.0f) == nullptr);   // far apart, distance > 0
    other.position[0] = 0.0f;
    other.position[2] = 0.0f;   // same place -> distance < 0, wins
    CHECK(manager->GetCollidingAutomaton(&automaton, 0.0f) == &other);
    host.view.onscreen = false;
    CHECK(!manager->IsAutomatonOnscreen(&automaton));
    host.view.onscreen = true;
    CHECK(manager->IsAutomatonOnscreen(&automaton));
    manager->DestroyAutomata();

    // ---------------------------------------------------------------- animation tick
    host.simulator.speed = 1;
    FakeAutomaton ticking;
    ticking.occupant.flags = 4;
    ticking.flags = 0x210040;
    manager->AddAutomaton(&ticking);
    manager->DoAnimationTick(1, 16.0f);
    CHECK(ticking.ticks == 1);
    CHECK((ticking.flags & 0x4000000) != 0);
    // flagged automata skip the "marked for removal" early path; at full opacity they stay
    ticking.marked = true;
    manager->DoAnimationTick(1, 16.0f);
    CHECK(ticking.sentEvents.empty());
    manager->DestroyAutomata();
    // opacity below 0.1 while marked -> force removal
    FakeAutomaton fading;
    fading.occupant.flags = 4;
    fading.flags = 0x210040;
    fading.opacity = 0.05f;
    fading.marked = true;
    manager->AddAutomaton(&fading);
    manager->DoAnimationTick(1, 16.0f);
    CHECK(fading.sentEvents.size() == 1 && fading.sentEvents[0] == 3);
    CHECK(fading.shutdowns == 1);
    manager->DestroyAutomata();

    // ---------------------------------------------------------------- DoMessage(cIGZMessage2*)
    {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgPostCityInit);
        CHECK(manager->DoMessage(message));
        // A scroll: the camera's target messages are ignored while it lasts, and its end reads the view rectangle again
        // (i386 0x000c8be0), so the automata are kept and culled against where the view stopped.
        message->SetType(kMsgCameraMoved);
        CHECK(manager->DoMessage(message));
        const float rectBeforeScroll = manager->ViewRect(0);
        message->SetType(kMsgScrollStart);
        CHECK(manager->DoMessage(message));
        CHECK(manager->ScrollInProgress());
        for (float& edge : host.view.rect) edge += 400.0f;   // the view moves during the scroll
        message->SetType(kMsgCameraTargetDone);
        CHECK(manager->DoMessage(message));
        CHECK(manager->ViewRect(0) == rectBeforeScroll);
        message->SetType(kMsgScrollEnd);
        CHECK(manager->DoMessage(message));
        CHECK(!manager->ScrollInProgress());
        CHECK(manager->ViewRect(0) == host.view.rect[0] - 64.0f);
        CHECK(manager->ViewRect(2) == host.view.rect[2] + 64.0f);
        message->SetType(kMsgAutomataEnabled);
        message->SetData1(1);
        CHECK(manager->DoMessage(message));
        CHECK(manager->LoadedFlag() == 1);
        message->SetData1(0);
        CHECK(manager->DoMessage(message));
        CHECK(manager->LoadedFlag() == 0);
        message->SetType(kMsgCameraMoved);
        CHECK(manager->DoMessage(message));
        CHECK(manager->ViewRect(0) == host.view.rect[0] - 64.0f);
        message->SetType(kMsgCameraTargetDone);
        CHECK(manager->DoMessage(message));   // no scroll, movement above 0 -> view changed
        message->SetType(kMsgRenderPropertiesChanged);
        CHECK(manager->DoMessage(message));
        message->SetType(kMsgViewFilterChanged);
        FakeFilter filter;
        filter.limited = true;
        filter.onlyGroup = 0x1402;
        message->SetVoid1(&filter);
        message->SetVoid2(nullptr);
        FakeAutomaton filtered;
        filtered.group = 0x9999;
        manager->AddAutomaton(&filtered);
        CHECK(manager->DoMessage(message));
        CHECK(!filtered.visible);
        manager->DestroyAutomata();
        message->SetType(kMsgOccupantGroupAdded);
        message->SetVoid1(&automaton);
        CHECK(manager->DoMessage(message));
        message->SetVoid1(nullptr);
        CHECK(manager->DoMessage(message));
        message->SetType(kMsgControllerRemoved);
        CHECK(manager->DoMessage(message));
        message->SetType(kMsgBuildingCompleted);
        FakeBuilding building;
        automaton.occupant.groups = {0x2000};
        building.occupant = &automaton.occupant;
        message->SetVoid1(&building);
        manager->DestroyAutomata();
        CHECK(manager->DoMessage(message));
        CHECK(manager->TrackedBuildings().at(0x2000) == 1);   // the building's source group count went up
        message->SetType(kMsgOccupantInserted);
        message->SetVoid1(&automaton.occupant);
        CHECK(manager->DoMessage(message));
        message->SetType(kMsgOccupantRemoved);
        CHECK(manager->DoMessage(message));
        CHECK(manager->TrackedBuildings().at(0x2000) == 0);   // removed again
        message->SetType(kMsgTick);
        message->SetData2(1);
        message->SetData3(0);
        CHECK(manager->DoMessage(message));
        message->SetType(0x12345678);
        CHECK(manager->DoMessage(message));
        message->Release();
        // a message that is not a standard one is ignored
        struct PlainMessage final : cIGZMessage2 {
            std::uint32_t type = 1;
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return 2; }
            std::uint32_t Release() override { return 1; }
            bool Create(std::uint32_t, void**) const override { return false; }
            std::uint32_t GetType() const override { return type; }
            void SetType(std::uint32_t t) override { type = t; }
            bool IsEqual(const cIGZMessage2& other) const override { return type == other.GetType(); }
            bool IsLess(const cIGZMessage2& other) const override { return type < other.GetType(); }
        } plain;
        CHECK(manager->DoMessage(&plain));
    }

    // ---------------------------------------------------------------- DoMessage(cGZMessage&): the cheats
    {
        cRZString reset("DrawPaths reset");
        cGZMessage message;
        message.type = kMessageCheatCode;
        message.param1 = kCheatPlayerDrive;
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&reset));
        CHECK(manager->DoMessage(message));
        CHECK(host.budget.releases == 1);
        cRZString plainText("udriveit");
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&plainText));
        CHECK(manager->DoMessage(message));
        CHECK(host.playerDriveCreates == 1);
        CHECK(host.playerDrive.choosing);
        CHECK(host.viewInputAdds == 1);
        CHECK(manager->IsDrivingMode() == false);
        host.inputControlIsDrive = true;
        CHECK(manager->IsDrivingMode());
        manager->ExitPlayerDriveView();
        CHECK(host.playerDriveStores == 2);   // Enter + Exit

        cRZString casper("Casper");
        message.param1 = kCheatGhostModels;
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&casper));
        CHECK(manager->DoMessage(message));
        CHECK(host.ghostSets == 1 && host.ghostEnabled);
        cRZString casperOff("Casper off");
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&casperOff));
        CHECK(manager->DoMessage(message));
        CHECK(host.ghostSets == 2 && !host.ghostEnabled);

        cRZString paths("DrawPaths 7");
        message.param1 = kCheatDrawAutomataPaths;
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&paths));
        CHECK(manager->DoMessage(message));
        CHECK(manager->PathView() == &host.pathView);
        CHECK(host.pathView.initialised && host.pathView.ignoreDepth);
        CHECK(host.pathView.filter == 7);
        CHECK(host.rendererAdds == 1);
        message.param1 = kCheatHideAutomataPaths;
        CHECK(manager->DoMessage(message));
        CHECK(manager->PathView() == nullptr);
        CHECK(host.pathView.shutdowns == 1);
        CHECK(host.rendererRemoves == 1);

        message.param1 = kCheatNoCsi;
        CHECK(manager->DoMessage(message));
        CHECK(host.dispatchView.shows == 1 && host.dispatchView.lastType == 4 && !host.dispatchView.lastShow);
        message.param1 = 0x11111111;
        CHECK(!manager->DoMessage(message));
        message.type = 0x22222222;
        CHECK(!manager->DoMessage(message));
    }

    // ---------------------------------------------------------------- player drive with an automaton
    {
        host.playerDriveCreates = 0;
        host.playerDrive.refs = 1;
        manager->EnterPlayerDriveView(&automaton, nullptr);
        CHECK(host.playerDrive.driving);
        CHECK(automaton.priority == 0xff);
        CHECK(host.vehicleSpeedSets >= 1);
        manager->ExitPlayerDriveView();
    }

    // ---------------------------------------------------------------- chasing
    {
        FakeAutomaton chaser, target;
        chaser.group = 0x1402;
        chaser.occupant.groups = {0x1402};
        manager->AddAutomaton(&chaser);
        CHECK(manager->ChaseAutomaton(&chaser, &chaser) == false);
        CHECK(manager->ChaseAutomaton(&chaser, nullptr) == false);
        CHECK(manager->ChaseAutomaton(nullptr, &target) == false);
        chaser.hasState = false;
        CHECK(!manager->ChaseAutomaton(&chaser, &target));
        chaser.hasState = true;
        CHECK(manager->ChaseAutomaton(&chaser, &target));
        CHECK(chaser.pushedState == 0x34);
        CHECK(chaser.stateTarget == &target);
        CHECK(target.refs == 2);   // held by the chaser's state

        // by group: the chaser is in 0x1402 and within the radius
        chaser.foundState = 0;
        chaser.position[0] = 10.0f;
        chaser.position[2] = 0.0f;
        target.position[0] = 0.0f;
        target.position[2] = 0.0f;
        CHECK(manager->ChaseAutomaton(0x1402, 30.0f, &target, true));
        CHECK(manager->ChaseAutomaton(0x1402, 5.0f, &target, true) == false);   // outside the radius
        CHECK(manager->ChaseAutomaton(0x9999, 30.0f, &target, false) == false);
        manager->DestroyAutomata();
    }

    // ---------------------------------------------------------------- camera follow
    {
        cSC4AutomataManager::ResetCameraFollowState();
        CHECK(cSC4AutomataManager::CameraFollowTarget() == nullptr);
        FakeAutomaton followed;
        followed.position[0] = 10.0f;
        followed.position[2] = 20.0f;
        manager->SetCameraFollow(&followed);
        CHECK(cSC4AutomataManager::CameraFollowTarget() == &followed);
        CHECK(followed.refs == 2);
        CHECK(!manager->IsCameraFollowUpdating());
        CHECK(host.camera.targetSets >= 1);
        manager->UpdateCameraFollow(16.0f);
        CHECK(host.camera.velocitySets >= 1);
        CHECK(host.view.projections > 0);
        manager->SetCameraFollow(&followed);   // same target: nothing happens
        manager->SetCameraFollow(nullptr);
        CHECK(cSC4AutomataManager::CameraFollowTarget() == nullptr);
        CHECK(followed.refs == 1);
        cSC4AutomataManager::ResetCameraFollowState();
    }

    // ---------------------------------------------------------------- clock multiplier / sim speed
    {
        AutomataTuningGlobals& tuning = AutomataTuning();
        tuning.f21aa8 = true;
        tuning.clockCurves.clear();
        cSC4ResponseCurve curve;
        const std::vector<SC4Point> points = {{0.0f, 1.0f}, {12.0f, 1.0f}, {24.0f, 2.0f}};
        curve.SetResponseCurveData(points);
        tuning.clockCurves[static_cast<std::uint32_t>(PopulateLayer::kLayer4)] = curve;
        host.clock.hour = 12;
        host.clock.minute = 0;
        CHECK(manager->GetClockMultiplier(PopulateLayer::kLayer6) == 1.0f);   // layer 6 maps onto 4
        host.clock.hour = 23;
        CHECK(manager->GetClockMultiplier(PopulateLayer::kLayer4) > 1.5f);
        tuning.f21aa8 = false;
        CHECK(manager->GetClockMultiplier(PopulateLayer::kLayer4) == 1.0f);

        tuning.f21a54 = 1.0f;
        tuning.f21a58 = 2.0f;
        tuning.f21a5c = 4.0f;
        tuning.f21a60 = 8.0f;
        host.simulator.speed = 2;
        CHECK(!manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 4.0f);
        host.simulator.speed = 1;
        CHECK(!manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 2.0f);
        host.simulator.speed = 3;   // i386 0x000bd8a8: any speed but 0 and 2 takes the +0x58 value
        CHECK(!manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 2.0f);
        host.simulator.speed = 0;
        CHECK(!manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 1.0f);
        host.simulator.panel = false;
        host.simulator.speed = 2;
        CHECK(!manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 1.0f);   // variable speed automata off
        host.simulator.panel = true;
        host.simulator.speed = 0;
        host.simulator.paused = true;
        CHECK(manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedPaused());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 0.0f);
        host.simulator.paused = false;
        host.simulator.emergency = true;
        CHECK(!manager->CalculateSimSpeedModifier());
        CHECK(cSC4AutomataManager::SimSpeedModifier() == 1.0f);
        host.simulator.emergency = false;
    }

    // ---------------------------------------------------------------- tuning reader
    {
        host.tuningPresent = true;
        AddFloat(*host.tuningHolder, 0x4998c916, 0.5f);
        AddFloat(*host.tuningHolder, 0x6b42922c, 1.0f);
        std::initializer_list<float> speeds = {2.0f, 3.0f, 4.0f, 5.0f};
        cRZVariant variant;
        variant.RefFloat32(speeds.begin(), 4);
        host.tuningHolder->AddProperty(0x6b42922c, &variant, false);
        AddFloatArray(*host.tuningHolder, 0x6b42922c, {2.0f, 3.0f, 4.0f, 5.0f});
        AddFloatArray(*host.tuningHolder, 0x4bae8fe0, {0.25f, 0.5f});
        AddFloat(*host.tuningHolder, 0x8c3e8185, 0.0f);
        AddUint(*host.tuningHolder, 0xca5545ea, 3600);
        AddBool(*host.tuningHolder, 0x4998c91c, true);
        manager->ReadTuningParameters();
        CHECK(AutomataTuning().f219b4 == 0.5f);
        CHECK(AutomataTuning().f21a54 == 2.0f && AutomataTuning().f21a60 == 5.0f);
        CHECK(AutomataTuning().f219d8.size() == 2);
        CHECK(AutomataTuning().f219bc > 0.99f && AutomataTuning().f219bc < 1.01f);
        CHECK(AutomataTuning().f21aa8);
        // the same readers with a holder: per-layer clock curves and the spawn/recall parameters
        cSCPropertyHolder local;
        AddFloatArray(local, 0x4998c90c, {0.0f, 1.0f, 24.0f, 3.0f});
        manager->ReadTuningClock(&local, 0x4998c90c, PopulateLayer::kLayer1);
        CHECK(AutomataTuning().clockCurves.count(1) == 1);
        CHECK(AutomataTuning().clockCurves[1].GetOutputValue(24.0f) == 3.0f);
        AddFloatArray(local, 0xcba5895b, {0.1f, 0.2f, 0.3f, 0.4f});
        manager->ReadSpawnRecallParameters(&local, 0xcba5895b, 3);
        CHECK(AutomataTuning().spawnRecall.count(3) == 1);
        CHECK(AutomataTuning().spawnRecall[3].f0 == 0.1f);
        CHECK(AutomataTuning().spawnRecall[3].fc == 0.4f);
    }

    // ---------------------------------------------------------------- post city init / occupant counting
    {
        manager->Shutdown();
        manager->Init();
        FakeAutomaton stale, keep;
        stale.flags = 0;
        keep.flags = 0x8000;
        // Read marks the post-city-init flag; use the message to set it through ReadInternal below, so call the handler
        // after a Read that sets it.
        // a version 5 stream: it sets the "recount the tracked buildings" flag (version 6 does not)
        PortMemoryOStream os;
        os.SetUint16(5);
        os.SetSint32(1);
        os << true;
        os << false;
        os.SetUint32(0);   // automata
        os.SetUint32(1);   // tracked buildings: one entry (version 5 reads it but requests the recount)
        os.SetUint32(0x2000);
        os.SetUint32(0);
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        CHECK(manager->Read(is));
        CHECK(manager->PostCityInitPending());
        manager->AddAutomaton(&stale);
        manager->AddAutomaton(&keep);
        host.occupants.clear();
        host.occupants.push_back(&automaton.occupant);
        automaton.occupant.groups = {0x2000};
        host.enumerateResult = true;
        manager->HandlePostCityInit();
        CHECK(stale.inits == 2);            // AddAutomaton + HandlePostCityInit
        CHECK(keep.shutdowns == 1);         // flag 0x8000 -> removed
        CHECK(manager->GetAutomataCount() == 1);
        CHECK(host.occupantListReleases == 1);
        CHECK(manager->TrackedBuildings().at(0x2000) == 1);
        manager->DestroyAutomata();
    }

    // ---------------------------------------------------------------- source building counts
    {
        CHECK(manager->GetSourceBuildingCount(0x2000) == 1);   // the count of the tracked source building
        CHECK(manager->GetSourceBuildingCount(0x3000) == 0);
        CHECK(manager->GetSourceBuildingCount(0x1406) == 42);   // the landfill developer answers for 0x1402/0x1406
        CHECK(manager->GetSourceBuildingCount(0x1402 | 4) == 42);
        CHECK(manager->GetSourceBuildingCountForAutomataGroup(0x1402) == 1);   // walks up to 0x1000 -> source 0x2000
        CHECK(manager->GetSourceBuildingCountForAutomataGroup(0x9999) == 0);
    }

    // ---------------------------------------------------------------- persistence
    {
        manager->SetMaxCount(12);
        manager->DestroyAutomata();
        CHECK(manager->Write(*new PortMemoryOStream()));   // never leaks in the test; keeps the API honest
        PortMemoryOStream os;
        CHECK(manager->Write(os));
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        CHECK(manager->Read(is));
        CHECK(manager->MaxCount() == 12);
        CHECK(manager->PostCityInitPending());
        // a newer stream version is refused
        unsigned char bad[8] = {7, 0, 0, 0, 0, 0, 0, 0};
        PortMemoryIStream newer(bad, sizeof bad);
        CHECK(!manager->Read(newer));
    }

    // ---------------------------------------------------------------- view helpers
    {
        manager->SetMinZoom(2);
        host.view.zoom = 2;   // equal to the min zoom -> still active
        manager->UpdateZoomAndRotation();
        CHECK(manager->IsActive());
        host.view.zoom = 0;
        manager->UpdateZoomAndRotation();
        CHECK(!manager->IsActive());
        host.view.zoom = 3;
        manager->SetMinZoom(1);
        CHECK(manager->HandleViewZoomChanged(1, 2));
        CHECK(manager->IsActive());
        manager->Activate(false);
        CHECK(!manager->IsActive());
        manager->Activate(true);
        CHECK(manager->IsActive());
        manager->HandleViewChanged();
        CHECK(manager->ViewRect(0) == host.view.rect[0] - 64.0f);
        FakeFilter filter;
        manager->HandleViewFilterChanged(nullptr, &filter);
        CHECK(manager->IsActive());
        IAutomataPathView* pathView = cSC4AutomataManager::PathView();
        CHECK(pathView == nullptr);
    }

    // ---------------------------------------------------------------- notifications
    {
        int before = host.posts;
        manager->PostNotificationMessage(0x1234, &automaton, 5, 6);
        CHECK(host.posts == before + 1);
    }

    // ---------------------------------------------------------------- shutdown and teardown
    manager->Shutdown();
    CHECK(!manager->Initialized());
    manager->Release();
    CHECK(host.network.refs == 1 && host.script.refs == 1 && host.clock.refs == 1);
    SetAutomataManagerHost(nullptr);
    host.tuningHolder->Release();

    if (failures == 0) std::printf("PASS sc4_automatamanager\n");
    else std::printf("FAIL sc4_automatamanager: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
