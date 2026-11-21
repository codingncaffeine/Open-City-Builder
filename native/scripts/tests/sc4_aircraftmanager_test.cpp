// cSC4AircraftManager — unit test with fake hosts: the airport map bookkeeping of HandleOccupantInserted / HandleOccupantRemoved
// (the helipad and airport kinds, the created automaton's group, model key, position, facing and first state), the runway
// markers and GetRunwayList (the pairs per runway number, the kInvalidVector padding, other lots left out), the "no automata"
// message filter, both Dispatch overloads (police and fire states, the station's efficiency and the 200 altitude offset),
// AddAutomaton(nullptr) and the Init / Shutdown / tuning values. The base manager's fakes are the ones of
// sc4_automatamanager_test.cpp (copied: that test keeps them in its own translation unit).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AircraftManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/Occupant.h"
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
    float completion = 1.0f;   // kSC4PercentageOneHundred (a fraction)
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
    std::vector<std::pair<std::uint32_t, float>> pushes;   // every PushState (state, timer)
    bool PushState(std::uint32_t state, void*, float timer, bool, std::uint32_t, std::uint32_t, const void*) override {
        pushedState = state;
        pushes.emplace_back(state, timer);
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
    const void* filterInformation = nullptr;
    std::uint32_t filterOnlyGroup = 0;
    bool ViewFilterIncludes(const void* information, IAutomataAutomaton* automaton) override {
        return information != filterInformation || automaton->GetAutomataGroup() == filterOnlyGroup;
    }
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
    // The base manager's view of a city occupant a message names: a neutral occupant (the aircraft manager reads the city
    // occupant itself).
    FakeOccupant messageOccupant;
    IAutomataOccupant* OccupantView(void*) override { return &messageOccupant; }
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


// ---- a city occupant (cISC4Occupant) -----------------------------------------------------------------------------------------
struct FakeCityOccupant final : cISC4Occupant {
    int refs = 1;
    cSCPropertyHolder holder;
    float position[3] = {0, 0, 0};
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &holder; }
    std::uint32_t GetType() const override { return 0x278128a0; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof position); }
    void GetBoundingBox(float* lo, float* hi) override { std::memcpy(lo, boxMin, sizeof boxMin); std::memcpy(hi, boxMax, sizeof boxMax); }
    void GetBoundingCityCells(SC4CellRect&) override {}
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
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
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

// ---- the aircraft manager's host ---------------------------------------------------------------------------------------------
struct FakeAircraftHost final : IAircraftManagerHost {
    std::map<const cISC4Occupant*, std::int32_t> orientation;
    float lotSplitX = 1000.0f;   // LotAt: lot A below x, lot B from x on (null for negative x)
    int lotA = 0, lotB = 0;
    std::vector<cISC4Occupant*> stationLot;
    bool stationHasLot = true;
    bool police = false, fire = false;
    float policeEfficiency = 0.25f, fireEfficiency = 0.75f;
    bool inBounds = true;
    float height = 30.0f, defaultAltitude = 5.0f;
    std::map<IAutomataAutomaton*, cS3DVector3> destinations;
    std::map<IAutomataAutomaton*, float> efficiencies;
    IAutomataAutomaton* helicopter = nullptr;
    IAutomataAutomaton* blimp = nullptr;
    int helicopters = 0, blimps = 0, filtersMade = 0, filtersReleased = 0;

    std::int32_t PropOrientation(cISC4Occupant* occupant) override { return orientation.count(occupant) ? orientation[occupant] : 0; }
    const void* LotAt(const cS3DVector3& p) override {
        if (p.x < 0.0f) return nullptr;
        return p.x < lotSplitX ? static_cast<const void*>(&lotA) : static_cast<const void*>(&lotB);
    }
    bool StationLotOccupants(IAutomataOccupant*, std::vector<cISC4Occupant*>& out) override {
        out = stationLot;
        return stationHasLot;
    }
    bool IsPoliceStation(IAutomataOccupant*) override { return police; }
    float PoliceStationEfficiency(IAutomataOccupant*) override { return policeEfficiency; }
    bool IsFireStation(IAutomataOccupant*) override { return fire; }
    float FireStationEfficiency(IAutomataOccupant*) override { return fireEfficiency; }
    bool TerrainPointInBounds(float, float) override { return inBounds; }
    float TerrainHeight(float, float) override { return height; }
    float DefaultAltitude() override { return defaultAltitude; }
    void SetDestination(IAutomataAutomaton* a, const cS3DVector3& d) override { destinations[a] = d; }
    void SetStationEfficiency(IAutomataAutomaton* a, float e) override { efficiencies[a] = e; }
    IAutomataAutomaton* NewHelicopter() override { ++helicopters; if (helicopter) helicopter->AddRef(); return helicopter; }
    IAutomataAutomaton* NewBlimp() override { ++blimps; if (blimp) blimp->AddRef(); return blimp; }
    cSC4BuildingPropFilter* NewBuildingPropFilter() override { ++filtersMade; return reinterpret_cast<cSC4BuildingPropFilter*>(&filtersMade); }
    void ReleaseBuildingPropFilter(cSC4BuildingPropFilter*) override { ++filtersReleased; }
};

bool Same(const cS3DVector3& v, float x, float y, float z) { return v.x == x && v.y == y && v.z == z; }

// One exemplar per group, the group's model key (1, group, 7) (the base manager's map group -> exemplar indices).
void AddExemplar(cSC4AircraftManager& manager, std::uint32_t group) {
    tExemplarData data;
    data.modelKey = cGZPersistResourceKey{1, group, 7};
    data.groupIds[0] = group;
    data.groupCount = 1;
    data.defaultGroup = group;
    manager.GetExemplarData().push_back(data);
    manager.GetExemplarDataMap()[group].push_back(static_cast<std::uint32_t>(manager.GetExemplarData().size() - 1));
}

// The occupant inserted / removed message as the occupant manager posts it (Void1 = the occupant).
bool Post(cSC4AircraftManager& manager, std::uint32_t type, cISC4Occupant* occupant) {
    cRZMessage2Standard message;
    message.AddRef();
    message.SetType(type);
    message.SetVoid1(occupant);
    return manager.DoMessage(static_cast<cIGZMessage2*>(&message));
}

}  // namespace

int main() {
    FakeHost host;
    host.tuningHolder->AddRef();
    host.network.cellFound = false;   // no cell masks: the exemplar choice takes the whole group
    SetAutomataManagerHost(&host);
    FakeAircraftHost aircraft;
    SetAircraftManagerHost(&aircraft);

    // ---------------------------------------------------------------- Init / tuning / Shutdown
    {
        AutomataTuning().params.f000 = 7;   // property 0x4998c89f
        host.tuningPresent = true;
        AddByte(*host.tuningHolder, kAircraftPropertyMinZoom, 3);
        cSC4AircraftManager manager;
        manager.AddRef();
        CHECK(cSC4AircraftManager::AircraftRandom() == nullptr);
        CHECK(manager.Init());
        CHECK(aircraft.filtersMade == 1 && manager.BuildingPropFilter() != nullptr);
        CHECK(cSC4AircraftManager::AircraftRandom() == &manager.Random());
        CHECK(manager.MaxCount() == 7);
        CHECK(manager.GetMinZoom() == 2);   // stored minus one
        CHECK(manager.Init() && aircraft.filtersMade == 1);   // idempotent
        CHECK(manager.Shutdown());
        CHECK(aircraft.filtersReleased == 1 && manager.BuildingPropFilter() == nullptr);
        CHECK(cSC4AircraftManager::AircraftRandom() == nullptr);
        host.tuningPresent = false;
    }

    // ---------------------------------------------------------------- helipad / airport insertion and removal
    {
        cSC4AircraftManager manager;
        manager.AddRef();
        CHECK(manager.Init());
        AddExemplar(manager, 0x4301);
        AddExemplar(manager, 0x4311);

        FakeAutomaton heli;
        host.classCreated = &heli;
        FakeCityOccupant station;
        AddByte(station.holder, kAircraftPropertyHelipad, 1);
        station.position[0] = 100; station.position[1] = 50; station.position[2] = 200;
        station.boxMin[1] = 40.0f;
        aircraft.orientation[&station] = 1;
        CHECK(Post(manager, kMessageOccupantInserted, &station));
        CHECK(manager.AirportCount() == 1);
        CHECK(manager.Airports().count(&station) == 1 && manager.Airports().at(&station) == &heli);
        CHECK(station.refs == 2);   // the map's reference
        CHECK(heli.position[0] == 100 && heli.position[1] == 40 && heli.position[2] == 200);   // on the bottom of the box
        CHECK(heli.orientation[0] == -1 && heli.orientation[1] == 0 && heli.orientation[2] == 0);   // kFacing[1]
        CHECK(heli.modelKey.group == 0x4301 && heli.modelKey.instance == 7);   // the specific group's exemplar
        CHECK((heli.flags & kAircraftCreateFlags) == kAircraftCreateFlags);
        CHECK(heli.pushes.size() == 1 && heli.pushes[0].first == 0x36 && heli.pushes[0].second == -1.0f);
        CHECK(manager.GetAutomataCount() == 1);

        // property 0xbfa15a5 = 3: the fire plane group 0x4311, no first state
        FakeAutomaton plane;
        host.classCreated = &plane;
        FakeCityOccupant airport;
        AddByte(airport.holder, kAircraftPropertyAirport, 3);
        CHECK(Post(manager, kMessageOccupantInserted, &airport));
        CHECK(manager.AirportCount() == 2 && manager.Airports().at(&airport) == &plane);
        CHECK(plane.modelKey.group == 0x4311 && plane.pushes.empty());
        CHECK(plane.orientation[2] == 1.0f);   // kFacing[0]

        // a kind without exemplars and an occupant marked "no automata" make nothing
        FakeAutomaton none;
        host.classCreated = &none;
        FakeCityOccupant noExemplar;
        AddByte(noExemplar.holder, kAircraftPropertyHelipad, 2);   // group 0x4305 has no exemplar
        Post(manager, kMessageOccupantInserted, &noExemplar);
        FakeCityOccupant marked;
        AddByte(marked.holder, kAircraftPropertyHelipad, 1);
        AddBool(marked.holder, kPropertyNoAutomata, true);
        Post(manager, kMessageOccupantInserted, &marked);
        CHECK(manager.AirportCount() == 2 && none.inits == 0);

        // removal: the automaton fades out (Remove(false)), both references go
        const int heliRefs = heli.refs;
        Post(manager, kMessageOccupantRemovedFromManager, &station);
        CHECK(manager.AirportCount() == 1 && manager.Airports().count(&station) == 0);
        CHECK(heli.removedSoft && heli.refs == heliRefs - 1 && station.refs == 1);
        Post(manager, kMessageOccupantRemovedFromManager, &station);   // a second removal changes nothing
        CHECK(manager.AirportCount() == 1 && station.refs == 1);
        manager.Shutdown();
        CHECK(manager.AirportCount() == 0 && airport.refs == 1);
        host.classCreated = nullptr;
    }

    // ---------------------------------------------------------------- runway markers and GetRunwayList
    {
        cSC4AircraftManager manager;
        manager.AddRef();
        CHECK(manager.Init());
        FakeCityOccupant r1a, r1b, r2, other, sameSpotOtherRunway;
        AddByte(r1a.holder, kAircraftPropertyRunway, 1);
        AddByte(r1b.holder, kAircraftPropertyRunway, 1);
        AddByte(r2.holder, kAircraftPropertyRunway, 2);
        AddByte(other.holder, kAircraftPropertyRunway, 1);
        AddByte(sameSpotOtherRunway.holder, kAircraftPropertyRunway, 2);
        r1a.position[0] = 10; r1a.position[2] = 20;
        r1b.position[0] = 300; r1b.position[2] = 20;
        r2.position[0] = 50; r2.position[2] = 400;
        other.position[0] = 1500; other.position[2] = 20;   // on lot B
        sameSpotOtherRunway.position[0] = 10; sameSpotOtherRunway.position[2] = 20;
        for (FakeCityOccupant* o : {&r1a, &r1b, &r2, &other}) Post(manager, kMessageOccupantInserted, o);
        CHECK(manager.RunwayMarkers().size() == 4 && manager.AirportCount() == 0);
        CHECK(r1a.refs == 1);   // markers hold no reference

        std::vector<cS3DVector3> ends{cS3DVector3{9, 9, 9}};
        manager.GetRunwayList(cS3DVector3{100, 0, 100}, ends);
        CHECK(ends.size() == 4);
        CHECK(ends.size() == 4 && Same(ends[0], 10, 0, 20) && Same(ends[1], 300, 0, 20));
        CHECK(ends.size() == 4 && Same(ends[2], 50, 0, 400) && Same(ends[3], -1, -2, -3));   // padded with kInvalidVector
        manager.GetRunwayList(cS3DVector3{1200, 0, 100}, ends);
        CHECK(ends.size() == 2 && Same(ends[0], 1500, 0, 20) && Same(ends[1], -1, -2, -3));
        manager.GetRunwayList(cS3DVector3{-5, 0, 100}, ends);   // no lot there
        CHECK(ends.empty());

        // removal matches position and runway number: the same spot with another number leaves the marker
        Post(manager, kMessageOccupantRemovedFromManager, &sameSpotOtherRunway);
        CHECK(manager.RunwayMarkers().size() == 4);
        Post(manager, kMessageOccupantRemovedFromManager, &r1a);
        CHECK(manager.RunwayMarkers().size() == 3);
        manager.GetRunwayList(cS3DVector3{100, 0, 100}, ends);
        CHECK(ends.size() == 4 && Same(ends[0], 300, 0, 20) && Same(ends[1], -1, -2, -3));
        manager.Shutdown();
        CHECK(manager.RunwayMarkers().empty());
    }

    // ---------------------------------------------------------------- Dispatch (both overloads)
    {
        cSC4AircraftManager manager;
        manager.AddRef();
        CHECK(manager.Init());
        AddExemplar(manager, 0x4301);
        FakeAutomaton heli;
        heli.occupant.groups.insert(kAircraftGroupPoliceHelicopter);
        host.classCreated = &heli;
        FakeCityOccupant pad;
        AddByte(pad.holder, kAircraftPropertyHelipad, 1);
        Post(manager, kMessageOccupantInserted, &pad);
        heli.pushes.clear();

        FakeOccupant stationView;   // the dispatching station as the base manager sees it
        tAutomataLocation from;
        from.kind = kLocationOccupant;
        from.occupant = &stationView;
        tAutomataLocation to;
        to.kind = kLocationWorldPosition;
        to.x = 500; to.y = 0; to.z = 600;

        // 0x88: no aircraft on the station's lot -> null
        aircraft.police = true;
        aircraft.stationLot = {};
        CHECK(manager.Dispatch(0x4301, from, to, 0, 0, cGZPersistResourceKey{}, 0xff) == nullptr);
        // the helipad stands on the station's lot: its helicopter goes, with the police states
        FakeCityOccupant building;
        aircraft.stationLot = {&building, &pad};
        IAutomataAutomaton* sent = manager.Dispatch(0x4301, from, to, 0, 0, cGZPersistResourceKey{}, 0xff);
        CHECK(sent == &heli);
        CHECK(Same(aircraft.destinations[&heli], 500, 30.0f + 200.0f, 600));
        CHECK(aircraft.efficiencies[&heli] == 0.25f);
        CHECK(heli.pushes.size() == 2 && heli.pushes[0].first == 0x35 && heli.pushes[0].second == 10000.0f &&
              heli.pushes[1].first == 0x33 && heli.pushes[1].second == -1.0f);
        // a fire station sends with state 0x39; outside the terrain the altitude starts from the default height
        heli.pushes.clear();
        aircraft.police = false;
        aircraft.fire = true;
        aircraft.inBounds = false;
        CHECK(manager.Dispatch(0x4301, from, to, 0, 0, cGZPersistResourceKey{}, 0xff) == &heli);
        CHECK(aircraft.efficiencies[&heli] == 0.75f && aircraft.destinations[&heli].y == 205.0f);
        CHECK(heli.pushes.size() == 1 && heli.pushes[0].first == 0x39);
        // a location that is not a station sends nothing
        tAutomataLocation nowhere;
        nowhere.kind = kLocationWorldPosition;
        CHECK(manager.Dispatch(0x4301, nowhere, to, 0, 0, cGZPersistResourceKey{}, 0xff) == nullptr);

        // 0x90: the group of the aircraft decides; a police helicopter from a fire station keeps 100% efficiency
        heli.pushes.clear();
        aircraft.inBounds = true;
        CHECK(manager.Dispatch(&heli, from, to, 0, 2));
        CHECK(aircraft.efficiencies[&heli] == 1.0f);
        CHECK(heli.pushes.size() == 2 && heli.pushes[0].first == 0x35 && heli.pushes[1].first == 0x33);
        aircraft.police = true;
        CHECK(manager.Dispatch(&heli, from, to, 0, 2) && aircraft.efficiencies[&heli] == 0.25f);
        FakeAutomaton plane;   // not a police helicopter: state 0x39 and the fire station's efficiency
        aircraft.police = false;
        CHECK(manager.Dispatch(&plane, from, to, 0, 2));
        CHECK(plane.pushes.size() == 1 && plane.pushes[0].first == 0x39 && aircraft.efficiencies[&plane] == 0.75f);
        CHECK(!manager.Dispatch(static_cast<IAutomataAutomaton*>(nullptr), from, to, 0, 2));
        manager.Shutdown();
        host.classCreated = nullptr;
    }

    // ---------------------------------------------------------------- AddAutomaton(nullptr)
    {
        cSC4AircraftManager manager;
        manager.AddRef();
        CHECK(manager.Init());
        FakeAutomaton heli, blimp;
        aircraft.helicopter = &heli;
        aircraft.blimp = &blimp;
        for (int i = 0; i < 40; ++i) manager.AddAutomaton(nullptr);
        CHECK(aircraft.helicopters + aircraft.blimps == 40);
        CHECK(aircraft.helicopters > 0 && aircraft.blimps > 0);   // an even draw over 40 tries
        CHECK(manager.GetAutomataCount() == 40);
        FakeAutomaton given;
        CHECK(manager.AddAutomaton(&given) && given.inits == 1 && manager.GetAutomataCount() == 41);
        aircraft.helicopter = aircraft.blimp = nullptr;
        manager.Shutdown();
    }

    SetAircraftManagerHost(nullptr);
    SetAutomataManagerHost(nullptr);
    host.tuningHolder->Release();

    if (failures == 0) std::printf("PASS sc4_aircraftmanager\n");
    else std::printf("FAIL sc4_aircraftmanager: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
