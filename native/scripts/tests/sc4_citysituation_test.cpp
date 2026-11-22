// cSC4CitySituationManager: lifetime and QueryInterface, the situation value store the query windows read (mission
// successes, visibility, maximum CSI, target counters), the serialization round trip with its version gate, the
// tutorial reset over the preferences, the situation track list (trigger/untrigger), automaton tracking through the
// dispatch view, the occupant routing and the Init/Shutdown subscriptions.
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/CitySituationManager.h"
#include "sc4/GameDataRegistry.h"
#include "rz/cRZCOMDllDirector.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

int g_liveOccupants = 0;

// --- message server ---------------------------------------------------------------------------------------------
class FakeMessageServer final : public cIGZMessageServer2 {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool MessageSend(cIGZMessage2* message) override {
        sends.push_back(message->GetType());
        sendData1.push_back(static_cast<cIGZMessage2Standard*>(message)->GetData1());
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        posts.push_back(message->GetType());
        postVoid1.push_back(static_cast<cIGZMessage2Standard*>(message)->GetVoid1());
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
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return false; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return refs; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }

    std::vector<std::uint32_t> added, removed, sends, posts;
    std::vector<void*> postVoid1;
    std::vector<std::uint64_t> sendData1;
    std::uint32_t refs = 1;
};

// --- application / preferences ----------------------------------------------------------------------------------
class FakeApp final : public ISituationApp {
public:
    bool DebugFunctionalityEnabled() override { return debug; }
    std::uint32_t* CompletedMissionIds() override { return preferences.empty() ? nullptr : preferences.data() + 1; }
    void SavePreferences() override { ++saves; }

    bool debug = false;
    std::vector<std::uint32_t> preferences = std::vector<std::uint32_t>(0x4d0, 0);   // 64 ids at +1
    int saves = 0;
};

class FakeCheats final : public ISituationCheatCodes {
public:
    void RegisterCheatCode(std::uint32_t id, const char* text) override {
        registered.emplace_back(id, text ? text : "");
    }
    void UnregisterCheatCode(std::uint32_t id) override { unregistered.push_back(id); }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t) override { ++notifications; }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { --notifications; }

    std::vector<std::pair<std::uint32_t, std::string>> registered, unused;
    std::vector<std::uint32_t> unregistered;
    int notifications = 0;
};

// --- dispatch view (the on-screen indicators) -------------------------------------------------------------------
class FakeDispatchView final : public ISituationDispatchView {
public:
    void ShowAllIndicators(int type, bool show) override { indicatorsShown.emplace_back(type, show); }
    void SetIndicatorMax(int type, int max) override { indicatorMax.emplace_back(type, max); }
    std::uint32_t TrackOccupant(ISituationOccupant* occupant, int type, int value, int flags) override {
        tracked.push_back({occupant, type, value, flags});
        return nextTrackId++;
    }
    void UntrackOccupant(std::uint32_t trackId, bool force) override { untracked.emplace_back(trackId, force); }
    void FlashIndicator(std::uint32_t, bool, float) override {}

    struct Track { ISituationOccupant* occupant; int type; int value; int flags; };
    std::vector<Track> tracked;
    std::vector<std::pair<std::uint32_t, bool>> untracked;
    std::vector<std::pair<int, bool>> indicatorsShown;
    std::vector<std::pair<int, int>> indicatorMax;
    std::uint32_t nextTrackId = 0x77;
};

// --- automata managers ------------------------------------------------------------------------------------------
class FakeAutomataManager final : public ISituationAutomataManager {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Pause() override { ++paused; }
    void Resume() override { ++resumed; }
    bool HandlesGroup(std::uint32_t group) override { return group == handledGroup; }
    void AlertGroup(float, std::uint32_t, ISituationAutomaton*, bool) override {}
    void AppendInstancesOfGroup(std::uint32_t, std::unordered_set<ISituationAutomaton*>& out) override {
        for (ISituationAutomaton* automaton : instances) out.insert(automaton);
    }
    void ReleaseInstances(std::unordered_set<ISituationAutomaton*>& list) override {
        ++instancesReleased;
        list.clear();
    }
    bool FindSpawnPosition(float, float, std::uint32_t, const float from[3], float spawn[3], std::uint32_t) override {
        if (!spawnAvailable) return false;
        spawn[0] = from[0] + 10.0f;
        spawn[1] = from[1];
        spawn[2] = from[2];
        return true;
    }
    ISituationAutomaton* CreateAutomaton(std::uint32_t group, const float[3], std::uint32_t) override {
        createdGroups.push_back(group);
        if (!toCreate) return nullptr;
        toCreate->AddRef();
        return toCreate;
    }
    bool spawnAvailable = false;
    ISituationAutomaton* toCreate = nullptr;
    std::vector<std::uint32_t> createdGroups;
    int paused = 0, resumed = 0;

    std::uint32_t handledGroup = 1;
    std::vector<ISituationAutomaton*> instances;
    int instancesReleased = 0;
    std::uint32_t refs = 1;
};

class FakeAura final : public ISituationAuraSim {
public:
    void AddAura(float x, float z, float radius, float magnitude) override { auras.push_back({x, z, radius, magnitude}); }
    std::vector<std::vector<float>> auras;
};

class FakeBudget final : public ISituationBudgetSim {
public:
    void AddMoney(long long amount) override { deposits.push_back(amount); }
    std::vector<long long> deposits;
};

class FakeNotifier final : public ISituationAutomatonNotifier {
public:
    void AddNotification(cIGZMessageTarget2*, std::uint32_t mask) override { added.push_back(mask); }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t mask) override { removed.push_back(mask); }
    std::vector<std::uint32_t> added, removed;
};

// --- automaton / occupant ---------------------------------------------------------------------------------------
class FakeAutomaton final : public ISituationAutomaton {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }   // test double: never destroys itself
    ISituationOccupant* Occupant() override { return occupant; }
    void Position(float xyz[3]) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    void Remove(bool) override { ++removed; }
    void GetOrientation(float xyz[3]) override { xyz[0] = 1; xyz[1] = xyz[2] = 0; }
    void SetFlag(std::uint32_t flag, bool on) override {
        if (on) flags |= flag; else flags &= ~flag;
    }
    bool CheckFlag(std::uint32_t flag) override { return (flags & flag) != 0; }
    void AddListener(cIGZMessageTarget2* listener) override { listeners.push_back(listener); }
    void RemoveListener(cIGZMessageTarget2* listener) override {
        listeners.erase(std::remove(listeners.begin(), listeners.end(), listener), listeners.end());
    }
    ISituationAutomaton* StateTarget() override { return pursuitTarget; }
    std::uint32_t GetBehavior() override { return behavior; }
    void StartSFX(std::uint32_t sfx) override { commands.push_back(sfx); }
    std::uint32_t SituationIndicatorImageId() override { return firstIndicator; }
    void RegisterInGameDataRegistry() override { ++attached; }

    float position[3] = {1, 2, 3};
    std::uint32_t flags = 2;   // driveable
    std::uint32_t behavior = 0x20;
    std::uint32_t firstIndicator = 0;
    int removed = 0;
    ISituationOccupant* occupant = nullptr;
    ISituationAutomaton* pursuitTarget = nullptr;
    std::vector<cIGZMessageTarget2*> listeners;
    std::vector<std::uint32_t> commands;
    int attached = 0;
    std::uint32_t refs = 1;
};

class FakePropertyHolder final : public ISituationPropertyHolder {
public:
    bool HasProperty(std::uint32_t propertyId) override { return propertyId == property; }
    std::uint32_t property = 0;
};

class FakeOccupant final : public ISituationOccupant {
public:
    FakeOccupant() { ++g_liveOccupants; }
    ~FakeOccupant() { --g_liveOccupants; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs == 1) { --refs; delete this; return 0; }
        return --refs;
    }
    ISituationPropertyHolder* PropertyHolder() override { return &holder; }
    std::uint32_t OccupantType() override { return 0x480e6151; }
    void Position(float xyz[3]) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    void SetPosition(const float xyz[3]) override { position[0] = xyz[0]; position[1] = xyz[1]; position[2] = xyz[2]; }
    float position[3] = {0, 0, 0};
    bool IsInGroup(std::uint32_t group) override { return group == this->group; }
    ISituationAutomaton* AsAutomaton() override { return automaton; }
    bool IsNetworkOccupant() override { return false; }
    bool NetworkFlagsMatch(std::uint32_t) override { return false; }
    void NetworkCell(int& x, int& z) override { x = z = 0; }
    void SetGroup(std::uint32_t value) override { group = value; }

    FakePropertyHolder holder;
    ISituationAutomaton* automaton = nullptr;
    std::uint32_t group = 0;
    std::uint32_t refs = 1;
};

// --- the situation advice (a minimal one: the manager reads groups, flags and the reward fields) -----------------
class FakeAdvice final : public cISC4CitySituationAdvice {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4CitySituationAdvice) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs == 1) { --refs; delete this; return 0; }
        return --refs;
    }
    void* AsAdvice() override { return this; }
    std::uint32_t GetSuccessCondition() const override { return successCondition; }
    bool GetSuccessConditionText(cIGZString&) const override { return false; }
    float GetTimeLimit() const override { return 60.0f; }
    float GetSuccessDistance() const override { return successDistance; }
    float GetMinTargetDistance() const override { return 0; }
    float GetMaxTargetDistance() const override { return 0; }
    float GetSuccessTimeout() const override { return 0; }
    float GetDelayAfterDisabled() const override { return delayAfterDisabled; }
    const std::vector<std::uint32_t>& GetAutomataGroups() const override { return automataGroups; }
    const std::vector<std::uint32_t>& GetTargetGroups() const override { return targetGroups; }
    std::uint32_t GetSituationFlags() const override { return flags; }
    bool CheckSituationFlag(std::uint32_t flag) const override { return (flags & flag) != 0; }
    const std::vector<std::uint32_t>& GetTargetSequence() const override { return targetSequence; }
    bool HasTargetSequence() const override { return (flags & kSituationFlagTargetSequence) != 0; }
    const std::vector<std::uint32_t>& GetEvadeList() const override { return evadeList; }
    bool HasEvadeList() const override { return !evadeList.empty(); }
    float GetEvadeDistance() const override { return evadeDistance; }
    float GetEvadeTimeout() const override { return evadeTimeout; }
    float evadeDistance = 0.0f, evadeTimeout = 0.0f;
    const cRZString& GetSuccessText() const override { return text; }
    const cRZString& GetFailureText() const override { return text; }
    const cRZString& GetProgressText(std::uint16_t) const override { return text; }
    std::uint32_t GetSituationImageId() const override { return 0; }
    std::uint32_t GetSuccessImageId() const override { return 0; }
    std::uint32_t GetSuccessMood() const override { return 0; }
    std::uint32_t GetFailureImageId() const override { return 0; }
    std::uint32_t GetFailureMood() const override { return 0; }
    int GetScriptTableRef() const override { return -2; }
    std::uint32_t GetEvilTwinGuid() const override { return 0; }
    std::uint32_t GetRewardGuid() const override { return 0; }
    const cRZString& GetRewardProgressText() const override { return text; }
    const cRZString& GetRewardUnlockedText() const override { return text; }
    std::uint32_t GetRewardUnlockedImageId() const override { return 0; }
    std::uint32_t GetCoverageType() const override { return coverageType; }
    std::uint32_t GetCoverageZone() const override { return coverageZone; }
    std::uint32_t GetCoverageOccupantGroup() const override { return 0; }
    std::uint32_t GetCoverageNetworkType() const override { return coverageNetwork; }
    std::uint32_t coverageType = 0, coverageZone = 0, coverageNetwork = 0, coverageMin = 0, coverageMax = 0, coverageSuccess = 0;
    const std::uint16_t* GetCoverageRect() const override { return coverageRect; }
    const std::list<SituationCell>& GetCellCoverageList() const override { return cells; }
    std::uint32_t GetCoverageCellsMin() const override { return coverageMin; }
    std::uint32_t GetCoverageCellsMax() const override { return coverageMax; }
    std::uint32_t GetCoverageSuccessCount() const override { return coverageSuccess; }
    float GetCoverageSuccessPct() const override { return 0; }
    bool GetSuccessAura(float& radius, float& magnitude) const override {
        radius = successAuraRadius;
        magnitude = successAuraMagnitude;
        return true;
    }
    bool GetSuccessEffectName(cRZString&) const override { return false; }
    bool GetSuccessMoney(long long& money) const override {
        money = successMoney;
        return true;
    }
    bool GetFailureAura(float& radius, float& magnitude) const override {
        radius = failureAuraRadius;
        magnitude = failureAuraMagnitude;
        return true;
    }
    bool GetFailureEffectName(cRZString&) const override { return false; }
    bool GetFailureMoney(long long& money) const override {
        money = failureMoney;
        return true;
    }
    float GetActiveRadius() const override { return activeRadius; }
    bool IsAutomatonRelevant(ISituationAutomaton*) const override { return relevant; }
    bool IsGroupRelevant(const std::vector<std::uint32_t>&) const override { return relevant; }
    void UpdateSituationFields() override {}
    int GetAdviceId() const override { return adviceId; }
    int Priority() const override { return priority; }

    std::uint32_t refs = 1;
    std::uint32_t successCondition = 1;
    std::uint32_t flags = 0;
    float successDistance = 16.0f;
    float delayAfterDisabled = 0.0f;
    float activeRadius = 100.0f;
    float successAuraRadius = 2.0f, successAuraMagnitude = 3.0f;
    float failureAuraRadius = 4.0f, failureAuraMagnitude = 5.0f;
    std::int64_t successMoney = 1000, failureMoney = -500;
    int adviceId = 7;
    int priority = 10;
    bool relevant = true;
    std::vector<std::uint32_t> automataGroups{1};
    std::vector<std::uint32_t> targetGroups;
    std::vector<std::uint32_t> targetSequence;
    std::vector<std::uint32_t> evadeList;
    std::list<SituationCell> cells;
    std::uint16_t coverageRect[4] = {0, 0, 0, 0};
    cRZString text;
};

// --- Lua (only the four calls the script bridge makes) ----------------------------------------------------------
class FakeLua final : public ISituationLua {
public:
    int GetTop() override { return static_cast<int>(stack.size()); }
    void SetTop(int top) override { stack.resize(static_cast<std::size_t>(top)); }
    void PushValue(int) override { stack.push_back(Value::Number(0)); }
    void Pop(int count) override { stack.resize(stack.size() - static_cast<std::size_t>(count)); }
    bool IsNumber(int) override { return false; }
    double ToNumber(int) override { return 0; }
    void PushNumber(double) override { stack.push_back(Value::Number(0)); }
    void GetGlobal(const char*) override {}
    void RawGetTable(int) override {}
    void RawSetTable(int) override {}
    void GetRef(int ref) override { stack.push_back(ref == functionRef ? Value::Function() : Value::Number(0)); }
    void NewTable() override {}
    int Ref(bool) override {
        if (!stack.empty()) stack.pop_back();   // Ref pops the value into the registry
        return newRef;
    }
    void CallProtected(int argCount, int resultCount) override {
        calls.push_back({argCount, resultCount});
        Pop(argCount + resultCount);
    }
    void Unref(int) override {}
    bool IsFunction(int index) override { return !stack.empty() && At(index).function; }
    bool IsTable(int) override { return false; }
    bool GetGlobalTable(const char*) override { return false; }
    bool GetField(const char* name, int) override {
        fields.push_back(name);
        if (name != foundName) return false;      // nothing is pushed when the field is missing
        stack.push_back(Value::Function());
        return true;
    }
    void ReadFieldU32(const char*, std::uint32_t&, int) override {}
    void ReadFieldInt(const char*, std::int32_t&, int) override {}
    void ReadFieldFloat(const char*, float&, int) override {}
    void ReadFieldBool(const char*, bool&, int) override {}
    void ReadFieldString(const char*, cRZString&, int) override {}
    void ReadTableAsArrayU32(std::vector<std::uint32_t>&, int) override {}
    void ReadTableAsArrayString(std::vector<cRZString>&, int) override {}
    void ForEachEntry(void (*)(void*), void*) override {}

    struct Value {
        bool function = false;
        static Value Function() { Value v; v.function = true; return v; }
        static Value Number(double) { return Value(); }
    };
    Value& At(int index) {
        const int resolved = index < 0 ? static_cast<int>(stack.size()) + index : index - 1;
        return stack[static_cast<std::size_t>(resolved)];
    }

    std::vector<Value> stack;
    std::vector<std::string> fields;
    std::vector<std::pair<int, int>> calls;
    const char* foundName = "on_success";
    int functionRef = -1;   // no registry reference is a function until a test says so
    int newRef = 42;
};

// --- the host ---------------------------------------------------------------------------------------------------
class FakeHost final : public ICitySituationManagerHost {
public:
    ISituationLua* Lua() override { return lua; }
    cIGZMessageServer2* MessageServer() override { return &messages; }
    ISituationApp* App() override { return app; }
    ISituationCheatCodes* CheatCodes() override { return cheats; }
    ISituationView3D* View3D() override { return nullptr; }
    ISituationDispatchView* DispatchView() override { return dispatch; }
    ISituationCity* City() override { return city; }
    ISituationCity* city = nullptr;
    std::vector<std::pair<int, int>> connectionCells;
    bool neighbors = false;
    bool NeighborConnectionCells(std::vector<std::pair<int, int>>& out) override {
        out = connectionCells;
        return neighbors;
    }
    ISituationLotManager* LotManager() override { return nullptr; }
    ISituationEffectsManager* EffectsManager() override { return effects; }
    ISituationEffectsManager* effects = nullptr;
    ISituationTerrain* terrain = nullptr;
    ISituationTrafficNetwork* network = nullptr;
    ISituationZoneManager* zones = nullptr;
    ISituationOccupant* coverageTarget = nullptr;
    int coverageTargetsMade = 0;
    ISituationOccupant* NewCoverageTarget(const float position[3]) override {
        ++coverageTargetsMade;
        if (!coverageTarget) return nullptr;
        coverageTarget->SetPosition(position);
        coverageTarget->AddRef();
        return coverageTarget;
    }
    int AutomataManagerCount() override { return static_cast<int>(managers.size()); }
    ISituationAutomataManager* AutomataManager(int index) override {
        return index >= 0 && index < static_cast<int>(managers.size()) ? managers[static_cast<std::size_t>(index)] : nullptr;
    }
    ISituationAutomataManager* VehicleManager() override { return managers.empty() ? nullptr : managers.front(); }
    ISituationDriveView* PlayerDriveView() override { return nullptr; }
    ISituationMissionActor* MySimActor() override { return nullptr; }
    ISituationOccupantManager* OccupantManager() override { return nullptr; }
    ISituationZoneManager* ZoneManager() override { return zones; }
    ISituationTerrain* Terrain() override { return terrain; }
    ISituationTrafficNetwork* TrafficNetwork() override { return network; }
    ISituationAuraSim* AuraSim() override { return aura; }
    ISituationBudgetSim* BudgetSim() override { return budget; }
    ISituationLandfillDeveloper* LandfillDeveloper() override { return nullptr; }
    void EnterPlayerDriveView(ISituationAutomaton* automaton, cIGZMessageTarget2* target) override { drives.emplace_back(automaton, target); }
    cISC4CitySituationAdvice* AdvisorSituationByGuid(std::uint32_t guid) override {
        if (!situationByGuid || guid != situationGuid) return nullptr;
        situationByGuid->AddRef();
        return situationByGuid;
    }
    std::vector<std::pair<ISituationAutomaton*, cIGZMessageTarget2*>> drives;
    cISC4CitySituationAdvice* situationByGuid = nullptr;
    std::uint32_t situationGuid = 0;
    ISituationAuraSim* aura = nullptr;
    ISituationBudgetSim* budget = nullptr;
    cIGZWinMgr* WindowManager() override { return nullptr; }
    bool DoURL(const void*) override { return false; }
    std::vector<cISC4CitySituationAdvice*> AdvisorSituations() override { return advisorSituations; }
    void RetireAdvisorSituation(cISC4CitySituationAdvice* situation) override { retired.push_back(situation); }
    sc4::cISC4GameDataRegistry* GameDataRegistry() override { return registry; }
    ISituationAutomatonNotifier* AutomataNotifier() override { return &notifier; }
    ISituationOccupant* OccupantOfMessage(void* o) override { return static_cast<ISituationOccupant*>(o); }
    ISituationAutomaton* AutomatonOfMessage(void* a) override { return static_cast<ISituationAutomaton*>(a); }
    sc4::cISC4GameDataRegistry* registry = nullptr;

    FakeLua* lua = nullptr;
    FakeMessageServer messages;
    FakeApp* app = nullptr;
    FakeCheats* cheats = nullptr;
    FakeDispatchView* dispatch = nullptr;
    FakeNotifier notifier;
    std::vector<ISituationAutomataManager*> managers;
    std::vector<cISC4CitySituationAdvice*> advisorSituations;
    std::vector<cISC4CitySituationAdvice*> retired;
};

// A 64 x 64 city of 16 m cells.
class FakeCity final : public ISituationCity {
public:
    int CellsX() override { return 64; }
    int CellsZ() override { return 64; }
    float CellSizeXMeters() override { return 16.0f; }
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    void CellToWorld(int cellX, int cellZ, float xyz[3]) override {
        xyz[0] = cellX * 16.0f + 8.0f;
        xyz[1] = 0.0f;
        xyz[2] = cellZ * 16.0f + 8.0f;
    }
};

class FakeEffect final : public ISituationEffect {
public:
    explicit FakeEffect(std::string n) : name(std::move(n)) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) { delete this; return 0; }
        return refs;
    }
    void Start(bool) override { ++starts; }
    void Stop(bool) override { ++stops; }
    void SetTransform(const SituationEffectTransform&) override {}
    void SetCellRect(const float r[4]) override { for (int i = 0; i < 4; ++i) rect[i] = r[i]; }
    std::string name;
    float rect[4] = {};
    int starts = 0, stops = 0;
    std::uint32_t refs = 1;
};
class FakeEffects final : public ISituationEffectsManager {
public:
    bool CreateEffect(const char* name, ISituationEffect*& out) override {
        names.push_back(name);
        out = new FakeEffect(name);
        return true;
    }
    std::vector<std::string> names;
};
class FakeTerrain final : public ISituationTerrain {
public:
    bool IsWaterAt(int x, int z) override { return water.count({x, z}) != 0; }
    float HeightAt(float, float) override { return 100.0f; }
    float SurfaceWaterDepthAt(int x, int z) override { return water.count({x, z}) ? 5.0f : 0.0f; }
    bool IsCellWaterZoneCandidate(int x, int z) override { return water.count({x, z}) != 0; }
    std::set<std::pair<int, int>> water;
};
class FakeNetwork final : public ISituationTrafficNetwork {
public:
    bool HasNetworkAt(int x, int z) override { return roads.count({x, z}) != 0; }
    bool CellHasNetworkWithFlags(int x, int z, std::uint32_t) override { return roads.count({x, z}) != 0; }
    std::set<std::pair<int, int>> roads;
};
class FakeZones final : public ISituationZoneManager {
public:
    int ZoneTypeAt(int x, int z) override { auto it = zone.find({x, z}); return it == zone.end() ? 0 : it->second; }
    std::map<std::pair<int, int>, int> zone;
};

cRZMessage2Standard* MakeMessage(std::uint32_t type) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    return message;
}

}  // namespace

// The registry registers itself with the director framework only in Init, which the test does not call.
IDirectorFramework* DirectorFramework() { return nullptr; }

int main() {
    SetCitySituationManagerHost(nullptr);

    // --- the default state of a fresh manager ---------------------------------------------------------------
    {
        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->GetCurrentStatus() == kSituationNone);
        CHECK(manager->GetVisible());
        CHECK(manager->GetActiveSituation() == nullptr);
        CHECK(manager->GetActiveAuto() == nullptr);
        CHECK(manager->GetCurrentTarget() == nullptr);
        CHECK(manager->GetNumTargets() == 0 && manager->GetCurrentTargetIndex() == 0);
        CHECK(manager->GetNumTargetsRemaining() == 0);
        CHECK(manager->GetTrackedAutomataCount() == 0);
        CHECK(!manager->CountdownIsInProgress());
        CHECK(manager->GetCurrentTimeLimitSeconds() == 0.0f && manager->GetCurrentSituationTime() == 0.0f);
        CHECK(manager->GetCurrentTimeRemainingSeconds() == 0.0f);
        CHECK(manager->GetMaxCSI() == 0);
        CHECK(manager->GetGZCLSID() == kGZCLSID_cSC4CitySituationManager);
        CHECK(manager->GetMissionSuccessCount(1234) == 0);

        // the getters without state
        cRZString name;
        manager->GetCurrentTargetName(name);
        CHECK(name.Strlen() == 0);
        float radius = 0, magnitude = 0;
        std::int64_t money = 0;
        CHECK(!manager->GetSituationAura(radius, magnitude));
        CHECK(!manager->GetSituationMoney(money));

        // QueryInterface: the manager id, the message target id, 1 / 0xe98b2f57 / 0xe4fda3d4 answer this object;
        // anything else is refused. Each success takes one reference.
        void* out = nullptr;
        CHECK(manager->QueryInterface(kGZIID_cISC4CitySituationManager, &out) && out == manager);
        CHECK(manager->QueryInterface(0x452294aa, &out) && out == manager);
        CHECK(manager->QueryInterface(kGZIID_cISC4CitySituationManagerSerializable, &out) && out == manager);
        CHECK(manager->QueryInterface(0xe98b2f57, &out) && out == manager);
        CHECK(manager->QueryInterface(1, &out) && out == manager);
        CHECK(!manager->QueryInterface(0x12345678, &out));
        CHECK(!manager->QueryInterface(0x80000000, &out));
        manager->Release();   // 5 references left (1 + five QueryInterface calls)
        manager->Release();
        manager->Release();
        manager->Release();
        manager->Release();
        manager->Release();   // the creating reference: Shutdown + delete
    }

    // --- Init / Shutdown: subscriptions, automata managers, cheat code, completed missions ------------------
    {
        FakeHost host;
        FakeApp app;
        FakeCheats cheats;
        FakeDispatchView dispatch;
        FakeAutomataManager vehicles;
        FakeAutomataManager pedestrians;
        app.debug = true;
        app.preferences[1] = 0x1111;   // completed mission ids: 0x1111 then end
        app.preferences[2] = 0;
        host.app = &app;
        host.cheats = &cheats;
        host.dispatch = &dispatch;
        sc4::cSC4GameDataRegistry registry;
        host.registry = &registry;
        host.managers = {&vehicles, &pedestrians};
        SetCitySituationManagerHost(&host);

        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());
        CHECK(manager->Init());   // idempotent
        // the "sitmgr" node hangs under the registry root and its two raw values under the node
        {
            const auto node = registry.GetEntryID(static_cast<cIGZUnknown*>(static_cast<cIGZMessageTarget2*>(manager)));
            CHECK(node != 0 && registry.GetParentNodeEntry(node) == registry.GetRootNodeEntry());
        }
        CHECK(host.messages.added.size() == 10);
        CHECK(cheats.notifications == 1);
        CHECK(cheats.registered.size() == 1 && cheats.registered[0].first == 0x8c4136db &&
              cheats.registered[0].second == "cISC4CitySituationManager");
        CHECK(host.notifier.added.size() == 1 && host.notifier.added[0] == 0xff);
        CHECK(manager->GetMissionSuccessCount(0x1111) == 1);
        CHECK(manager->GetMissionSuccessCount(0) == 0);
        CHECK(dispatch.indicatorsShown.size() == 1 && dispatch.indicatorsShown[0].first == 4 &&
              dispatch.indicatorsShown[0].second);
        // Init re-applied the shared maximum once; SetMaxCSI adds another call, HandleTuningParametersChanged one more
        CHECK(dispatch.indicatorMax.size() == 1);
        manager->SetMaxCSI(7);
        CHECK(manager->GetMaxCSI() == 7);
        CHECK(dispatch.indicatorMax.size() == 2 && dispatch.indicatorMax[1].first == 4 && dispatch.indicatorMax[1].second == 7);
        CHECK(manager->HandleTuningParametersChanged());
        CHECK(dispatch.indicatorMax.size() == 3 && dispatch.indicatorMax[2].second == 7);

        // ResetTutorials clears the mission successes, wipes the 64 completed-mission ids and saves the preferences
        manager->ResetTutorials();
        CHECK(manager->GetMissionSuccessCount(0x1111) == 0);
        bool allZero = true;
        for (std::size_t i = 0; i < 0x100 / sizeof(std::uint32_t); ++i) allZero = allZero && app.preferences[1 + i] == 0;
        CHECK(allZero);
        CHECK(app.saves == 1);

        // Shutdown: unsubscribes everywhere, unregisters the cheat code, forgets the tracking
        CHECK(manager->Shutdown());
        CHECK(host.messages.removed.size() == 10);
        CHECK(cheats.notifications == 0 && cheats.unregistered.size() == 1 && cheats.unregistered[0] == 0x8c4136db);
        CHECK(host.notifier.removed.size() == 1 && host.notifier.removed[0] == 0xff);
        CHECK(manager->Shutdown());   // idempotent
        manager->Release();

        // without debug functionality the cheat code is not registered
        FakeHost quietHost;
        FakeApp quietApp;
        FakeCheats quietCheats;
        quietHost.app = &quietApp;
        quietHost.cheats = &quietCheats;
        SetCitySituationManagerHost(&quietHost);
        auto* quiet = new cSC4CitySituationManager();
        quiet->AddRef();
        CHECK(quiet->Init());
        CHECK(quietCheats.registered.empty() && quietCheats.notifications == 0);
        // no preferences: ResetTutorials only clears the map
        quietApp.preferences.clear();
        quiet->ResetTutorials();
        CHECK(quietApp.saves == 0);
        CHECK(quiet->Shutdown());
        quiet->Release();
    }

    // --- serialization: version gate and round trip --------------------------------------------------------
    {
        FakeHost host;
        FakeApp app;
        FakeDispatchView dispatch;
        app.preferences[1] = 0x2001;
        app.preferences[2] = 0x2002;
        app.preferences[3] = 0;
        host.app = &app;
        host.dispatch = &dispatch;
        SetCitySituationManagerHost(&host);

        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());
        CHECK(manager->GetMissionSuccessCount(0x2001) == 1);
        manager->SetVisible(false);

        PortMemoryOStream out;
        CHECK(manager->Write(out) == 1);
        CHECK(out.bytes().size() == 2 + 4 + 2 * 6 + 1);   // version, count, two (u32,u16) entries, visible byte

        cSC4CitySituationManager* reader = new cSC4CitySituationManager();
        reader->AddRef();
        SetCitySituationManagerHost(&host);
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(reader->Read(in) == 1);
        CHECK(reader->GetMissionSuccessCount(0x2001) == 1);
        CHECK(reader->GetMissionSuccessCount(0x2002) == 1);
        CHECK(!reader->GetVisible());
        reader->Release();

        // version 3 is refused and changes nothing
        const unsigned char v3[] = {3, 0, 0, 0, 0, 0, 0, 0};
        PortMemoryIStream in3(v3, sizeof(v3));
        CHECK(manager->Write(out) == 1);
        auto* refused = new cSC4CitySituationManager();
        refused->AddRef();
        PortMemoryIStream bad(v3, sizeof(v3));
        CHECK(refused->Read(bad) == 0);
        CHECK(refused->GetMissionSuccessCount(0x2001) == 0);
        CHECK(refused->GetVisible());
        refused->Release();

        // a version-0 stream only carries the map (the visibility byte is absent)
        const unsigned char v0[] = {0, 0, 1, 0, 0, 0, 0x22, 0x22, 0, 0, 9, 0};
        auto* oldSave = new cSC4CitySituationManager();
        oldSave->AddRef();
        PortMemoryIStream in0(v0, sizeof(v0));
        CHECK(oldSave->Read(in0) == 1);
        CHECK(oldSave->GetMissionSuccessCount(0x2222) == 9);
        CHECK(oldSave->GetVisible());
        oldSave->Release();

        manager->Release();
    }

    // --- the situation track list: trigger / untrigger through a fake advice and manager --------------------
    {
        FakeHost host;
        FakeApp app;
        FakeCheats cheats;
        FakeDispatchView dispatch;
        FakeAutomataManager vehicles;
        FakeAutomaton automatonA;
        auto* occupantA = new FakeOccupant();
        automatonA.occupant = occupantA;
        automatonA.firstIndicator = 3;   // a non-zero indicator value makes it trackable
        vehicles.instances = {&automatonA};
        app.debug = false;
        host.app = &app;
        host.cheats = &cheats;
        host.dispatch = &dispatch;
        host.managers = {&vehicles};
        SetCitySituationManagerHost(&host);

        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());
        manager->SetMaxCSI(4);

        auto* advice = new FakeAdvice();
        advice->AddRef();

        // FindAutomatonForSituation answers the group's driveable automaton
        ISituationAutomaton* found = nullptr;
        CHECK(manager->FindAutomatonForSituation(advice, &found) && found == &automatonA);
        if (found) found->Release();
        CHECK(vehicles.instancesReleased == 1);   // the temporary instance set is released through the vehicle manager

        // CanPlayerDrive: needs the driveable flag, no excluded flags and a type that is not a pursuit car
        CHECK(manager->CanPlayerDrive(&automatonA));
        automatonA.flags = 2;   // "driveable"
        CHECK(manager->CanPlayerDrive(&automatonA));
        automatonA.flags = 1;
        CHECK(!manager->CanPlayerDrive(&automatonA));
        automatonA.flags = 0x8000000;
        CHECK(!manager->CanPlayerDrive(&automatonA));
        automatonA.flags = 0x10000000;
        CHECK(!manager->CanPlayerDrive(&automatonA));
        automatonA.flags = 2;
        automatonA.behavior = 0x34;   // (0x34 | 0x20) == 0x34: behaviors 0x14 / 0x34 cannot be driven
        CHECK(!manager->CanPlayerDrive(&automatonA));
        automatonA.behavior = 0x20;
        CHECK(manager->CanPlayerDrive(&automatonA));

        // HandleSituationTriggered: the advice becomes a track and gets the automaton, which is tracked
        manager->HandleSituationTriggered(advice);
        CHECK(manager->GetTrackedAutomataCount() == 1);
        CHECK(dispatch.tracked.size() == 1 && dispatch.tracked[0].type == 4 && dispatch.tracked[0].value == 3);
        CHECK(automatonA.listeners.size() == 1);
        CHECK(automatonA.refs == 3);   // the tracked set, the track and the finder's reference

        // a second trigger of the same situation does nothing (the track already has its automaton)
        manager->HandleSituationTriggered(advice);
        CHECK(manager->GetTrackedAutomataCount() == 1);

        // TreatAutomatonAsGroup puts the occupant into the group and tracks it (already tracked: no change)
        manager->TreatAutomatonAsGroup(&automatonA, 0x5005);
        CHECK(occupantA->group == 0x5005);

        // HandleRemoveAutomaton: the track loses its automaton, the indicator is returned and a replacement is looked for
        manager->HandleRemoveAutomaton(&automatonA);
        CHECK(manager->GetTrackedAutomataCount() == 1);   // re-triggered for the same track → tracked again
        CHECK(!dispatch.untracked.empty());

        // Finally the trigger is removed: the track goes away and its automaton is released
        manager->HandleSituationUntriggered(advice);
        CHECK(manager->GetTrackedAutomataCount() == 0);
        CHECK(automatonA.listeners.empty());

        // HandleInsertOccupant: the "no situation automaton" property keeps the occupant out
        occupantA->holder.property = kPropertyNoSituationAutomaton;
        manager->HandleInsertOccupant(occupantA);
        occupantA->holder.property = 0;
        occupantA->automaton = &automatonA;
        manager->HandleInsertOccupant(occupantA);
        CHECK(manager->GetTrackedAutomataCount() == 0);   // no track to put it in anymore

        // HandleInsertAutomaton without a matching track does nothing either
        manager->HandleInsertAutomaton(&automatonA);
        CHECK(manager->GetTrackedAutomataCount() == 0);

        // RetireSituation takes the advice off the advisor list and untriggers it
        CHECK(manager->RetireSituation(advice));
        CHECK(host.retired.size() == 1 && host.retired[0] == advice);

        // a fresh track/automaton pair (kept until Shutdown clears the tracked set)
        manager->HandleSituationTriggered(advice);
        CHECK(manager->GetTrackedAutomataCount() == 1);
        automatonA.SetFlag(0x10000000, true);   // (HandleAutomatonDisabled is not recovered)
        CHECK(automatonA.CheckFlag(0x10000000));

        CHECK(manager->Shutdown());
        CHECK(manager->GetTrackedAutomataCount() == 0);
        manager->Release();
        advice->Release();
        advice->Release();   // the construction reference (the object starts at one)
        occupantA->Release();
    }

    // --- visibility and status -----------------------------------------------------------------------------
    {
        FakeHost host;
        FakeDispatchView dispatch;
        host.dispatch = &dispatch;
        SetCitySituationManagerHost(&host);
        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());   // binds the dispatch view (and shows the indicators once)
        CHECK(manager->GetVisible());
        manager->SetVisible(false);
        CHECK(!manager->GetVisible());
        CHECK(dispatch.indicatorsShown.size() == 2 && dispatch.indicatorsShown[1].first == 4 &&
              !dispatch.indicatorsShown[1].second);

        // the status only moves when it changes; without an active situation no speed factor is computed
        manager->HandleStatusChanged(kSituationRunning);
        CHECK(manager->GetCurrentStatus() == kSituationRunning);
        manager->HandleStatusChanged(kSituationRunning);
        manager->HandleStatusChanged(kSituationFailure);
        CHECK(manager->GetCurrentStatus() == kSituationFailure);
        manager->Release();
    }

    // --- the message routing ---------------------------------------------------------------------------------
    {
        FakeHost host;
        FakeApp app;
        FakeCheats cheats;
        FakeDispatchView dispatch;
        FakeAutomataManager vehicles;
        FakeAutomaton automaton;
        auto* occupant = new FakeOccupant();
        automaton.occupant = occupant;
        automaton.firstIndicator = 9;
        vehicles.instances = {&automaton};
        host.app = &app;
        host.cheats = &cheats;
        host.dispatch = &dispatch;
        host.managers = {&vehicles};
        SetCitySituationManagerHost(&host);

        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());
        manager->SetMaxCSI(2);

        // an unknown message is not handled
        cRZMessage2Standard* unknown = MakeMessage(0x1234);
        CHECK(!manager->DoMessage(unknown));
        unknown->Release();

        // an occupant insertion is routed (the occupant is not an automaton here)
        occupant->automaton = nullptr;
        cRZMessage2Standard* insert = MakeMessage(kSituationMsgOccupantInserted);
        insert->SetVoid1(occupant);
        CHECK(manager->DoMessage(insert));
        insert->Release();

        // the advice trigger route adds the track for the advice and tracks the automaton
        auto* advice = new FakeAdvice();
        advice->AddRef();
        cRZMessage2Standard* triggered = MakeMessage(kSituationMsgAdviceTriggered);
        triggered->SetIGZUnknown(advice);
        CHECK(manager->DoMessage(triggered));
        CHECK(manager->GetTrackedAutomataCount() == 1);
        triggered->Release();

        // ... and the untrigger route removes it again
        cRZMessage2Standard* untriggered = MakeMessage(kSituationMsgAdviceUntriggered);
        untriggered->SetIGZUnknown(advice);
        CHECK(manager->DoMessage(untriggered));
        CHECK(manager->GetTrackedAutomataCount() == 0);
        untriggered->Release();

        // the mission actor is only kept for a service mission with an object
        cRZMessage2Standard* actor = MakeMessage(kSituationMsgMissionActorSet);
        actor->SetIGZUnknown(advice);
        CHECK(manager->DoMessage(actor));
        actor->Release();

        // the "situation shown" notification is answered but has no effect
        cRZMessage2Standard* shown = MakeMessage(kSituationMsgAdviceShown);
        CHECK(manager->DoMessage(shown));
        shown->Release();

        // the cheat message without the right id is answered (true) but posts nothing
        cRZMessage2Standard* cheat = MakeMessage(kSituationMsgCheatTyped);
        cheat->SetData1(0x77);
        CHECK(manager->DoMessage(cheat));
        CHECK(host.messages.posts.empty());
        cheat->Release();

        // PostMessageToSelf and SendSituationMessage go through the message server
        manager->PostMessageToSelf(kSituationMsgTimeUp, advice, &automaton);
        CHECK(host.messages.posts.size() == 1 && host.messages.posts[0] == kSituationMsgTimeUp);
        manager->SendSituationMessage(0xabbbca64, advice, 1);
        CHECK(host.messages.sends.size() == 1 && host.messages.sends[0] == 0xabbbca64);

        // the cGZMessage view never handles anything
        cGZMessage raw{};
        CHECK(!manager->DoMessage2(raw));

        CHECK(manager->Shutdown());
        manager->Release();
        advice->Release();
        advice->Release();   // the construction reference (the object starts at one)
        occupant->Release();
    }

    // --- the script bridge -----------------------------------------------------------------------------------
    {
        FakeHost host;
        FakeLua lua;
        host.lua = &lua;
        SetCitySituationManagerHost(&host);
        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());

        int ref = -2;
        manager->InitScriptFuncRef("on_success", 1, ref);
        CHECK(ref == 42);
        int missing = -2;
        manager->InitScriptFuncRef("on_failure", 1, missing);
        CHECK(missing == -2);

        // CallScriptFunction(selfRef, functionRef): the function reference is pushed and tested, then the self reference
        // becomes its one argument
        lua.functionRef = 5;
        manager->CallScriptFunction(11, 5);
        CHECK(lua.calls.size() == 1 && lua.calls[0].first == 1 && lua.calls[0].second == 0);
        CHECK(lua.GetTop() == 0);   // the stack top is restored

        // a -2 argument reference means "no call"
        manager->CallScriptFunction(11, -2);
        CHECK(lua.calls.size() == 1);

        CHECK(manager->Shutdown());
        manager->Release();
    }

    // --- the mission flow: initiate (0xac1045ef), accept through the window procedure, countdown, checks, result -------
    // `reach` places the driven automaton next to the target (a success) or far from it (the time limit fails it).
    for (const bool reach : {true, false}) {
        FakeHost host;
        FakeApp app;
        FakeAura aura;
        FakeBudget budget;
        FakeAutomataManager vehicles;
        FakeAutomaton automaton, targetAutomaton;
        auto* occupant = new FakeOccupant();
        auto* targetOccupant = new FakeOccupant();
        automaton.occupant = occupant;
        occupant->automaton = &automaton;
        targetAutomaton.occupant = targetOccupant;
        targetOccupant->automaton = &targetAutomaton;
        if (!reach) {
            automaton.position[0] = 1000.0f;
            automaton.position[2] = 1000.0f;
        }
        vehicles.instances = {&automaton};
        vehicles.spawnAvailable = true;
        vehicles.toCreate = &targetAutomaton;
        host.app = &app;
        host.aura = &aura;
        host.budget = &budget;
        host.managers = {&vehicles};
        auto* advice = new FakeAdvice();
        advice->successCondition = 1;
        advice->successDistance = 32.0f;
        advice->activeRadius = 0.0f;
        advice->targetGroups = {0x55};
        advice->flags = kSituationFlagCreateTarget | kSituationFlagTraining;
        advice->adviceId = static_cast<int>(0x8c151efdu);
        advice->successMoney = 500;
        advice->failureMoney = -100;
        host.situationByGuid = advice;
        host.situationGuid = 0x8c151efd;
        SetCitySituationManagerHost(&host);
        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());

        // the kit verb / the mission cheat post 0xac1045ef (guid, 1 = for the automaton's occupant, 0 = the advice's groups)
        cRZMessage2Standard* initiate = MakeMessage(kSituationMsgActivateAdvice);
        initiate->SetData1(0x8c151efd);
        initiate->SetData2(1);
        initiate->SetData3(0);
        CHECK(manager->DoMessage(initiate));
        initiate->Release();
        CHECK(manager->GetActiveSituation() == advice);
        CHECK(manager->GetActiveAuto() == &automaton);
        CHECK(manager->GetCurrentStatus() == kSituationNone);
        CHECK(manager->GetCurrentTarget() == targetOccupant);                 // CreateTarget made the target automaton
        CHECK(vehicles.createdGroups.size() == 1 && vehicles.createdGroups[0] == 0x55);
        CHECK(automaton.CheckFlag(0x4000) && automaton.attached == 1);         // SetActiveAutomaton
        CHECK(automaton.listeners.size() == 1 && automaton.listeners[0] == static_cast<cIGZMessageTarget2*>(manager));
        CHECK(manager->GetNumTargets() == 1 && manager->GetCurrentTimeLimitSeconds() == 60.0f);
        // the briefing popup is posted with the automaton
        CHECK(!host.messages.posts.empty() && host.messages.posts.back() == kSituationMsgShowPopup);
        CHECK(!host.messages.postVoid1.empty() && host.messages.postVoid1.back() == static_cast<void*>(&automaton));

        // the dialog's accept button (command 3 from sender 0x287259f6, button 6) activates it
        cGZMessage accept;
        accept.type = 3;
        accept.param1 = kSituationButtonSender;
        accept.param2 = kSituationButtonAccept;
        CHECK(manager->DoWinProcMessage(nullptr, accept));
        CHECK(manager->GetCurrentStatus() == kSituationRunning);
        CHECK(host.drives.size() == 1 && host.drives[0].first == &automaton && host.drives[0].second == manager);
        CHECK(!host.messages.sends.empty() && host.messages.sends.back() == kSituationMsgSituationActive &&
              host.messages.sendData1.back() == 1);

        // the first tick starts the 4 s countdown (the automata pause); its end resumes them
        manager->DoAnimationTick(16.0f);
        CHECK(manager->CountdownIsInProgress() && vehicles.paused == 1 && vehicles.resumed == 0);
        manager->DoAnimationTick(4000.0f);
        CHECK(!manager->CountdownIsInProgress() && vehicles.resumed == 1);
        if (reach) {
            // next to the target with a success timeout of 0: success, then the 1 s result delay, then the reward
            CHECK(manager->GetCurrentStatus() == kSituationSuccess);
            manager->DoAnimationTick(500.0f);
            CHECK(budget.deposits.empty());
            manager->DoAnimationTick(600.0f);
            CHECK(budget.deposits.size() == 1 && budget.deposits[0] == 500);
            CHECK(aura.auras.size() == 1);
            CHECK(manager->GetMissionSuccessCount(0x8c151efd) == 1);
            CHECK(app.CompletedMissionIds()[0] == 0x8c151efd);                          // a training mission is remembered
            CHECK(host.retired.size() == 1 && host.retired[0] == advice);
        } else {
            // far away: still running; past the 60 s limit the time limit fails it and the failure money is paid
            CHECK(manager->GetCurrentStatus() == kSituationRunning);
            for (int i = 0; i < 55; ++i) manager->DoAnimationTick(1000.0f);   // 4 s (the countdown tick) + 55 s
            CHECK(manager->GetCurrentStatus() == kSituationRunning);
            manager->DoAnimationTick(1500.0f);
            CHECK(manager->GetCurrentStatus() == kSituationFailure);
            manager->DoAnimationTick(1100.0f);
            CHECK(budget.deposits.size() == 1 && budget.deposits[0] == -100);
            CHECK(manager->GetMissionSuccessCount(0x8c151efd) == 0 && host.retired.empty());
        }
        // the result popup is posted without an automaton
        CHECK(host.messages.posts.back() == kSituationMsgShowPopup && host.messages.postVoid1.back() == nullptr);

        // completing the situation (the result dialog's close) ends it
        cRZMessage2Standard* complete = MakeMessage(kSituationMsgSituationComplete);
        complete->SetIGZUnknown(advice);
        CHECK(manager->DoMessage(complete));
        complete->Release();
        CHECK(manager->GetActiveSituation() == nullptr && manager->GetActiveAuto() == nullptr && manager->GetCurrentTarget() == nullptr);
        CHECK(automaton.listeners.empty());

        CHECK(manager->Shutdown());
        manager->Release();
        advice->Release();
        occupant->Release();
        targetOccupant->Release();
    }

    // --- escape (condition 2) and evade (flag 2) ------------------------------------------------------------------------------
    // 0 heads out at the east edge next to a connection: success. 1 the same with no connection near: running. 2 inside the
    // city: running. 3 the escape run with a chaser of the evade list within range: the timer fails it after the timeout; a
    // chaser out of range resets the timer.
    for (int mode = 0; mode < 4; ++mode) {
        FakeHost host;
        FakeApp app;
        FakeCity city;
        FakeAutomataManager vehicles;
        FakeAutomaton automaton, chaser;
        auto* occupant = new FakeOccupant();
        automaton.occupant = occupant;
        occupant->automaton = &automaton;
        automaton.position[0] = mode == 2 ? 500.0f : 63.0f * 16.0f + 8.0f;   // cell 63 of 64: within two cells of the edge
        automaton.position[2] = 8.0f;                                       // cell 0
        vehicles.instances = {&automaton};
        host.app = &app;
        host.city = &city;
        host.neighbors = true;
        host.connectionCells = {mode == 1 ? std::make_pair(64, 40) : std::make_pair(64, 1)};
        host.managers = {&vehicles};
        auto* advice = new FakeAdvice();
        advice->successCondition = 2;
        advice->flags = kSituationFlagTraining;
        if (mode == 3) {
            advice->flags |= kSituationFlagEvade;
            advice->evadeList = {1};
            advice->evadeDistance = 50.0f;
            advice->evadeTimeout = 10.0f;
            chaser.behavior = 0x34;
            chaser.pursuitTarget = &automaton;
            chaser.position[0] = automaton.position[0] - 30.0f;
            chaser.position[1] = 400.0f;   // measured level with the active automaton
            chaser.position[2] = automaton.position[2];
            host.connectionCells = {std::make_pair(64, 40)};   // the escape itself does not succeed
        }
        advice->adviceId = 0x1234;
        host.situationByGuid = advice;
        host.situationGuid = 0x1234;
        SetCitySituationManagerHost(&host);
        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());
        cRZMessage2Standard* initiate = MakeMessage(kSituationMsgActivateAdvice);
        initiate->SetData1(0x1234);
        initiate->SetData2(1);
        initiate->SetData3(0);
        CHECK(manager->DoMessage(initiate));
        initiate->Release();
        CHECK(manager->GetActiveAuto() == &automaton);
        cGZMessage accept;
        accept.type = 3;
        accept.param1 = kSituationButtonSender;
        accept.param2 = kSituationButtonAccept;
        CHECK(manager->DoWinProcMessage(nullptr, accept));
        manager->DoAnimationTick(16.0f);
        if (mode == 3) vehicles.instances = {&automaton, &chaser};
        manager->DoAnimationTick(4000.0f);   // the countdown ends; the first check runs
        if (mode == 0) {
            CHECK(manager->GetCurrentStatus() == kSituationSuccess);
        } else if (mode != 3) {
            CHECK(manager->GetCurrentStatus() == kSituationRunning);
        } else {
            CHECK(manager->GetCurrentStatus() == kSituationRunning);   // 4 s of chase, below the 10 s timeout
            chaser.position[0] = automaton.position[0] - 80.0f;          // out of range: the timer resets
            manager->DoAnimationTick(500.0f);
            CHECK(manager->GetCurrentStatus() == kSituationRunning);
            chaser.position[0] = automaton.position[0] - 30.0f;
            manager->DoAnimationTick(9500.0f);
            CHECK(manager->GetCurrentStatus() == kSituationRunning);   // 9.5 s since the reset
            manager->DoAnimationTick(600.0f);
            CHECK(manager->GetCurrentStatus() == kSituationFailure);   // 10.1 s
            CHECK(vehicles.instancesReleased > 0);
        }
        manager->Shutdown();
        manager->Release();
        advice->Release();
        occupant->Release();
    }

    // --- coverage (condition 5) ------------------------------------------------------------------------------------------
    {
        FakeHost host;
        FakeApp app;
        FakeCity city;
        FakeEffects effects;
        FakeTerrain terrain;
        FakeNetwork network;
        FakeZones zones;
        FakeAutomataManager vehicles;
        FakeAutomaton automaton;
        auto* occupant = new FakeOccupant();
        auto* target = new FakeOccupant();
        automaton.occupant = occupant;
        occupant->automaton = &automaton;
        automaton.position[0] = 8.0f;
        automaton.position[2] = 8.0f;
        vehicles.instances = {&automaton};
        host.app = &app;
        host.city = &city;
        host.effects = &effects;
        host.terrain = &terrain;
        host.network = &network;
        host.zones = &zones;
        host.coverageTarget = target;
        host.managers = {&vehicles};
        auto* advice = new FakeAdvice();
        advice->successCondition = 5;
        advice->coverageType = kCoverageRect;
        advice->coverageRect[0] = 10; advice->coverageRect[1] = 20; advice->coverageRect[2] = 12; advice->coverageRect[3] = 20;
        advice->activeRadius = 0.0f;
        advice->flags = kSituationFlagTraining;
        advice->adviceId = 0x777;
        host.situationByGuid = advice;
        host.situationGuid = 0x777;
        SetCitySituationManagerHost(&host);
        auto* manager = new cSC4CitySituationManager();
        manager->AddRef();
        CHECK(manager->Init());
        cRZMessage2Standard* initiate = MakeMessage(kSituationMsgActivateAdvice);
        initiate->SetData1(0x777);
        initiate->SetData2(1);
        initiate->SetData3(0);
        CHECK(manager->DoMessage(initiate));
        initiate->Release();
        // three cells (10..12, 20), a yellow highlight each, the centre (11, 20) and the target made there
        CHECK(manager->CoverageCellCount() == 3 && manager->CoverageTotal() == 3);
        CHECK(std::count(effects.names.begin(), effects.names.end(), std::string("mission_selection_yellow")) == 3);
        CHECK(manager->CoverageCenter().x == 11 && manager->CoverageCenter().z == 20);
        CHECK(host.coverageTargetsMade == 1 && manager->GetCurrentTarget() == target);
        float at[3] = {};
        target->Position(at);
        CHECK(at[0] == 11 * 16 + 8 && at[2] == 20 * 16 + 8 && at[1] == 100.0f);
        cGZMessage accept;
        accept.type = 3;
        accept.param1 = kSituationButtonSender;
        accept.param2 = kSituationButtonAccept;
        CHECK(manager->DoWinProcMessage(nullptr, accept));
        manager->DoAnimationTick(16.0f);
        manager->DoAnimationTick(4000.0f);
        CHECK(manager->GetCurrentStatus() == kSituationRunning && manager->CoverageCovered() == 0);
        // over cell (10, 20): it is covered, its highlight shrinks
        automaton.position[0] = 10 * 16 + 4.0f;
        automaton.position[2] = 20 * 16 + 4.0f;
        manager->DoAnimationTick(16.0f);
        CHECK(manager->CoverageCovered() == 1 && !manager->CoverageHas(10, 20) && manager->CoverageCellCount() == 2);
        CHECK(std::count(effects.names.begin(), effects.names.end(), std::string("mission_selection_yellow_shrink")) == 1);
        CHECK(manager->GetCurrentStatus() == kSituationRunning);
        automaton.position[0] = 11 * 16 + 4.0f;
        manager->DoAnimationTick(16.0f);
        automaton.position[0] = 12 * 16 + 4.0f;
        manager->DoAnimationTick(16.0f);
        CHECK(manager->CoverageCovered() == 3 && manager->GetCurrentStatus() == kSituationSuccess);

        // the cell list (type 7) is offset to the city centre (32, 32)
        advice->coverageType = kCoverageCells;
        advice->cells = {SituationCell{1, 2}, SituationCell{3, 4}};
        CHECK(manager->InitCoverageCells());
        CHECK(manager->CoverageHas(33, 34) && manager->CoverageHas(35, 36) && manager->CoverageTotal() == 2);

        // a network walk: from (5, 5) along a road to the east, branching nowhere else
        for (int x = 5; x <= 9; ++x) network.roads.insert({x, 5});
        manager->InitCoverageCells();   // resets the set (cells list again)
        std::uint32_t left = 3;
        manager->WalkCoverageNetwork(SituationCellPoint{5, 5}, 2, 1, left);
        CHECK(left == 0 && manager->CoverageHas(6, 5) && manager->CoverageHas(8, 5) && !manager->CoverageHas(9, 5));
        // growing a lot's 3 x 1 cells (wider than tall) to 7: whole rows past z1 first
        advice->coverageType = kCoverageCells;
        advice->cells.clear();
        manager->InitCoverageCells();
        manager->AddCellsToCoverage(20, 20, 22, 20, 3, 7);
        CHECK(manager->CoverageHas(20, 21) && manager->CoverageHas(22, 21) && manager->CoverageHas(20, 22) && !manager->CoverageHas(20, 19));

        // zone coverage: a block of zone 3 two cells east of the automaton, outside the inner square
        advice->coverageType = kCoverageZone;
        advice->coverageZone = 3;
        advice->coverageMin = 4;
        advice->coverageMax = 6;
        automaton.position[0] = 8.0f;
        automaton.position[2] = 8.0f;
        for (int x = 3; x <= 5; ++x)
            for (int z = 0; z <= 1; ++z) zones.zone[{x, z}] = 3;
        CHECK(manager->InitCoverageCells());
        CHECK(manager->CoverageTotal() >= 4 && manager->CoverageTotal() <= 6 && manager->CoverageHas(3, 0));
        // the water kind of the zone: the flood fill keeps to navigable cells
        advice->coverageType = kCoverageWater;
        advice->coverageZone = 0;
        zones.zone.clear();
        for (int x = 3; x <= 4; ++x)
            for (int z = 0; z <= 1; ++z) {
                zones.zone[{x, z}] = 0;
                terrain.water.insert({x, z});
            }
        CHECK(manager->InitCoverageCells());
        CHECK(manager->CoverageHas(3, 0) && manager->CoverageHas(4, 1) && !manager->CoverageHas(2, 0));
        CHECK(std::count(effects.names.begin(), effects.names.end(), std::string("mission_selection_water_yellow")) >= 3);

        manager->Shutdown();
        manager->Release();
        advice->Release();
        occupant->Release();
        target->Release();
    }

    SetCitySituationManagerHost(nullptr);
    CHECK(g_liveOccupants == 0);
    std::printf(failures ? "citysituation: %d FAILURES\n" : "citysituation: ok\n", failures);
    return failures ? 1 : 0;
}
