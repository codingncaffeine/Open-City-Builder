// Tests cSC4TutorialTask / cTutorialString (src/sc4/TutorialTask.cpp) against a fake cISCLua stack, a fake host seam
// and fake effects / disaster objects, plus real cGZWin windows for the target-window walk.
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "rz/RZStringUtils.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Disaster.h"
#include "sc4/DisasterManagers.h"
#include "sc4/TutorialTask.h"
#include "sc4/ViewInputControlEffect.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { if (!((a) == (b))) { std::printf("FAIL line %d: %s == %s\n", __LINE__, #a, #b); ++failures; } } while (0)

using namespace sc4;

// ---------------------------------------------------------------------------------------------------------------
// A small Lua value model: tables with string fields and a 1-based array part, as the tutorial scripts use them.
struct Table;
struct Value {
    enum Kind { Nil, Number, Text, TableK } kind = Nil;
    double number = 0;
    std::string text;
    std::shared_ptr<Table> table;
    static Value Num(double v) { Value x; x.kind = Number; x.number = v; return x; }
    static Value Str(const char* s) { Value x; x.kind = Text; x.text = s; return x; }
    static Value Tbl(std::shared_ptr<Table> t) { Value x; x.kind = TableK; x.table = std::move(t); return x; }
};
struct Table {
    std::map<std::string, Value> fields;
    std::vector<Value> array;
    Table& Set(const char* key, Value v) { fields[key] = std::move(v); return *this; }
    Table& Push(Value v) { array.push_back(std::move(v)); return *this; }
    Value Get(const Value& key) const {
        if (key.kind == Value::Number) {
            const auto index = static_cast<std::size_t>(key.number);
            return index >= 1 && index <= array.size() ? array[index - 1] : Value{};
        }
        if (key.kind == Value::Text) {
            const auto it = fields.find(key.text);
            return it == fields.end() ? Value{} : it->second;
        }
        return {};
    }
    int Count() const { return static_cast<int>(array.size()); }
};

struct FakeLua final : cISCLua {
    std::vector<Value> stack;
    std::map<std::string, Value> globals;
    int topCalls = 0, setTopCalls = 0;

    int Abs(int index) const { return index < 0 ? static_cast<int>(stack.size()) + index : index - 1; }
    Value& At(int index) { return stack[static_cast<std::size_t>(Abs(index))]; }
    void Push(Value v) { stack.push_back(std::move(v)); }

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Open(int) override { return false; }
    bool Close() override { return false; }
    void* AsIGZLua5() override { return nullptr; }
    int GetTop() override { ++topCalls; return static_cast<int>(stack.size()); }
    void SetTop(int index) override { ++setTopCalls; stack.resize(static_cast<std::size_t>(index < 0 ? 0 : index)); }
    void PushValue(int) override {}
    void Remove(int) override {}
    void Insert(int) override {}
    void Pop(int count) override { stack.resize(stack.size() - static_cast<std::size_t>(count)); }
    int Type(int) override { return 0; }
    const char* TypeName(int) override { return ""; }
    bool IsNumber(int index) override { return At(index).kind == Value::Number; }
    bool IsString(int index) override { return At(index).kind == Value::Text; }
    bool IsCFunction(int) override { return false; }
    int Tag(int) override { return 0; }
    bool IsBoolean(int) override { return false; }
    bool Equal(int, int) override { return false; }
    bool LessThan(int, int) override { return false; }
    std::uint32_t ToUint32(int index) override { return static_cast<std::uint32_t>(At(index).number); }
    std::int32_t ToSint32(int index) override { return static_cast<std::int32_t>(At(index).number); }
    double ToNumber(int index) override { return At(index).number; }
    const char* ToString(int index) override { return At(index).kind == Value::Text ? At(index).text.c_str() : ""; }
    int Strlen(int index) override { return static_cast<int>(At(index).text.size()); }
    CFunction ToCFunction(int) override { return nullptr; }
    void* ToUserData(int) override { return nullptr; }
    void* ToUserTag(int) override { return nullptr; }
    bool ToBoolean(int) override { return false; }
    void PushNil() override { Push(Value{}); }
    void PushNumber(double value) override { Push(Value::Num(value)); }
    void PushLString(const char* s, int length) override {
        Value value = Value::Str("");
        value.text.assign(s, static_cast<std::size_t>(length));
        Push(std::move(value));
    }
    void PushString(const char* s) override { Push(Value::Str(s)); }
    void PushCClosure(CFunction, int) override {}
    void PushUserTag(void*, int) override {}
    void PushBoolean(bool) override {}
    void GetGlobal(const char* name) override {
        const auto it = globals.find(name);
        Push(it == globals.end() ? Value{} : it->second);
    }
    void GetTable(int index) override {
        const Value table = At(index);          // resolved before the key is popped, as the Lua C API does
        const Value key = stack.back();
        stack.pop_back();
        Push(table.kind == Value::TableK ? table.table->Get(key) : Value{});
    }
    void RawGet(int) override {}
    void RawGetI(int index, int n) override {
        const Value table = At(index);
        if (table.kind == Value::TableK && n >= 1 && static_cast<std::size_t>(n) <= table.table->array.size())
            Push(table.table->array[static_cast<std::size_t>(n) - 1]);
        else
            Push(Value{});
    }
    void GetGlobals() override {}
    void GetTagMethod(int, const char*) override {}
    bool GetRef(int) override { return false; }
    void NewTable() override { Push(Value::Tbl(std::make_shared<Table>())); }
    void SetGlobal(const char*) override {}
    void SetTable(int) override {}
    void RawSet(int) override {}
    void RawSetI(int, int) override {}
    void SetTagMethod(int, const char*) override {}
    int Ref(int) override { return 0; }
    int CallProtected(int, int) override { return 0; }
    void RawCall(int, int) override {}
    int DoFile(const char*) override { return 0; }
    int DoString(const char*) override { return 0; }
    int DoBuffer(const char*, int, const char*) override { return 0; }
    int GetGCCount() override { return 0; }
    int GetGCThreshold() override { return 0; }
    void SetGCThreshold(int) override {}
    int NewTag() override { return 0; }
    void CopyTagMethods(int, int) override {}
    void SetTag(int) override {}
    void Unref(int) override {}
    int Next(int) override { return 0; }
    int GetN(int index) override {
        const Value& value = At(index);
        return value.kind == Value::TableK ? value.table->Count() : 0;
    }
    void Error(const char*) override {}
    void Concat(int) override {}
    void* NewUserData(int) override { return nullptr; }
    void Register(CFunction, const char*) override {}
    void PushUserData(void*, int) override {}
    int CloneTag(int) override { return 0; }
    bool IsFunction(int) override { return false; }
    bool IsTable(int index) override { return At(index).kind == Value::TableK; }
    bool IsUserData(int) override { return false; }
    bool IsNil(int index) override { return At(index).kind == Value::Nil; }
    std::uintptr_t ToDataRegistryEntryID(cISC4GameDataRegistry*, int) override { return 0; }
    cIGZUnknown* ToDataRegistryNode(cISC4GameDataRegistry*, int) override { return nullptr; }
    void* ToDataRegistryValue(cISC4GameDataRegistry*, int) override { return nullptr; }
};

// ---------------------------------------------------------------------------------------------------------------
// Fake seams / objects.
struct EffectLog {
    int hasEffect = 0, createEffect = 0, start = 0, stop = 0, setFloatParams = 0, decalReleases = 0;
    std::string lastName;
    int lastStop = -1, lastStart = -1, lastParamType = -1, lastParamCount = 0;
    float params[4] = {0, 0, 0, 0};
};

struct FakeDecal final : IToolVisualEffect {
    EffectLog* log;
    int refs = 1;
    explicit FakeDecal(EffectLog* l) : log(l) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        ++log->decalReleases;
        const int remaining = --refs;
        if (remaining > 0) return static_cast<std::uint32_t>(remaining);
        delete this;
        return 0;
    }
    void Start(int transition) override { ++log->start; log->lastStart = transition; }
    void Stop(int transition) override { ++log->stop; log->lastStop = transition; }
    bool IsStarted() override { return false; }
    void SetSourceTransform(const cS3DTransform&) override {}
    bool SetVectorParams(std::uint32_t, const cS3DVector3*, int) override { return false; }
    bool SetFloatParams(std::uint32_t type, const float* values, int count) override {
        ++log->setFloatParams;
        log->lastParamType = static_cast<int>(type);
        log->lastParamCount = count;
        for (int i = 0; i < 4 && i < count; ++i) log->params[i] = values[i];
        return true;
    }
};

struct FakeEffects final : IToolEffectsManager {
    EffectLog* log;
    bool has = true;
    int refs = 1;
    explicit FakeEffects(EffectLog* l) : log(l) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        const int remaining = --refs;
        if (remaining > 0) return static_cast<std::uint32_t>(remaining);
        delete this;
        return 0;
    }
    bool HasEffect(const char* name) override { ++log->hasEffect; log->lastName = name; return has; }
    bool CreateEffect(const char* name, IToolVisualEffect** out) override {
        ++log->createEffect;
        log->lastName = name;
        if (!has) { *out = nullptr; return false; }
        *out = new FakeDecal(log);   // CreateEffect answers a reference
        return true;
    }
    bool CreateEffect(std::uint32_t, int, IToolVisualEffect** out) override { *out = nullptr; return false; }
};

struct DisasterLog {
    int layerAddRefs = 0, managerLookups = 0, managerAddRefs = 0, fireQueries = 0, ignites = 0, causes = 0;
    std::uint32_t lastCause = 0;
    bool fireQuerySucceeds = true;
    cISC4Occupant* ignitedOccupant = reinterpret_cast<cISC4Occupant*>(0x1234);
};

struct FakeFire final : cISC4DisasterManagerFire {
    DisasterLog* log;
    int refs = 1;
    explicit FakeFire(DisasterLog* l) : log(l) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        const int remaining = --refs;
        if (remaining > 0) return static_cast<std::uint32_t>(remaining);
        delete this;
        return 0;
    }
    cISC4DisasterInstanceFire* GetFireInstance() override { return nullptr; }
    std::uint32_t SetCause(std::uint32_t cause) override { ++log->causes; log->lastCause = cause; return 0; }
    std::uint32_t GetCause() override { return log->lastCause; }
    bool IgniteOccupant(cISC4Occupant* occupant) override { ++log->ignites; log->ignitedOccupant = occupant; return true; }
    bool IgniteCell(std::uint32_t, std::uint32_t, bool, bool, std::uint32_t) override { return false; }
    std::uint32_t ExtinguishCell(std::uint32_t, std::uint32_t, int) override { return 0; }
    bool QuickBurnCell(std::uint32_t, std::uint32_t) override { return false; }
    bool IgniteCellRadius(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return false; }
};

struct FakeManager final : cISC4DisasterManager, cISC4DisasterManagerFire {
    DisasterLog* log;
    FakeFire* fire;
    int refs = 1;
    explicit FakeManager(DisasterLog* l) : log(l), fire(new FakeFire(l)) {}
    ~FakeManager() { fire->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4DisasterManagerFire) return false;
        ++log->fireQueries;
        if (!log->fireQuerySucceeds) return false;
        *out = static_cast<cISC4DisasterManagerFire*>(fire);
        fire->AddRef();
        return true;
    }
    std::uint32_t AddRef() override { ++log->managerAddRefs; return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        const int remaining = --refs;
        if (remaining > 0) return static_cast<std::uint32_t>(remaining);
        delete this;
        return 0;
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool AddInstanceToList(cISC4DisasterInstance*) override { return false; }
    bool RemoveInstanceFromList(cISC4DisasterInstance*) override { return false; }
    int GetActiveInstanceCount() const override { return 0; }
    const std::list<cISC4DisasterInstance*>& GetInstances() const override { return instances_; }
    std::uint32_t GetID() const override { return kDisasterManagerIdFire; }
    bool AttemptDisasterStart() override { return false; }
    bool AttemptDisasterStart(int, int) override { return false; }
    // cISC4DisasterManagerFire
    cISC4DisasterInstanceFire* GetFireInstance() override { return fire->GetFireInstance(); }
    std::uint32_t SetCause(std::uint32_t cause) override { return fire->SetCause(cause); }
    std::uint32_t GetCause() override { return fire->GetCause(); }
    bool IgniteOccupant(cISC4Occupant* occupant) override { return fire->IgniteOccupant(occupant); }
    bool IgniteCell(std::uint32_t x, std::uint32_t z, bool a, bool b, std::uint32_t c) override { return fire->IgniteCell(x, z, a, b, c); }
    std::uint32_t ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) override { return fire->ExtinguishCell(x, z, amount); }
    bool QuickBurnCell(std::uint32_t x, std::uint32_t z) override { return fire->QuickBurnCell(x, z); }
    bool IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t r, std::uint32_t c) override { return fire->IgniteCellRadius(x, z, r, c); }

private:
    std::list<cISC4DisasterInstance*> instances_;
};

struct FakeLayer final : cISC4DisasterLayer {
    DisasterLog* log;
    FakeManager* manager;
    int refs = 1;
    explicit FakeLayer(DisasterLog* l) : log(l), manager(new FakeManager(l)) {}
    ~FakeLayer() { manager->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { ++log->layerAddRefs; return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        const int remaining = --refs;
        if (remaining > 0) return static_cast<std::uint32_t>(remaining);
        delete this;
        return 0;
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void EnableDisasters(bool) override {}
    bool DisastersEnabled() const override { return true; }
    cISC4DisasterManager* GetDisasterManager(std::uint32_t id) const override {
        ++log->managerLookups;
        // borrowed, like the real layer's lookup: the game takes its own reference
        if (id != kDisasterManagerIdFire) return nullptr;
        return manager;
    }
    void DisasterHasBegun(cISC4DisasterInstance*) override {}
    void DisasterHasEnded() override {}
    bool IsADisasterOngoing() const override { return false; }
    bool GoToNextDisaster() override { return false; }
    bool GetMostRecentDisasterLocation(cISC4DisasterInstance*, int*, int*) override { return false; }
    bool RegisterDisasterLocation(cISC4DisasterInstance*, int, int, cISC4Occupant*, int, bool) override { return false; }
    bool UnregisterDisasterLocations(cISC4DisasterInstance*) override { return false; }
    bool UnregisterDisasterLocationByOccupant(cISC4DisasterInstance*, cISC4Occupant*) override { return false; }
    int GetDisasterLocationCount() const override { return 0; }
    bool GetDisasterLocationByIndex(int, cISC4DisasterInstance**, int*, int*) const override { return false; }
    DisasterCost* GetDisasterCost() override { return nullptr; }
};

struct FakeBuilding final : cISC4BuildingOccupant {
    int refs = 1, addRefs = 0, releases = 0, occupantRequests = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { ++addRefs; return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        ++releases;
        const int remaining = --refs;
        if (remaining > 0) return static_cast<std::uint32_t>(remaining);
        delete this;
        return 0;
    }
    cISC4Occupant* AsOccupant() override { ++occupantRequests; return nullptr; }
    std::uint32_t GetBuildingType() const override { return 0; }
    void SetBuildingType(std::uint32_t) override {}
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { return completion_; }
    void SetCompletionPercent(float) override {}
    const BuildingProfile& GetBuildingProfile() const override { return profile_; }
    const cRZString& GetBuildingName() const override { return name_; }
    const cRZString& GetExemplarName() const override { return name_; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return true; }
    bool IsLit() const override { return false; }
    void SetLit(bool) override {}
    void SetName(const cRZString&) override {}
    bool GetName(cRZString& out) override { out = name_; return true; }

private:
    float completion_ = 0.0f;
    BuildingProfile profile_;
    cRZString name_;
};

struct FakeHost final : ITutorialTaskHost, ITutorialCSIVisibility {
    FakeLua* lua = nullptr;
    bool established = true;
    cIGZWin* root = nullptr;
    bool inBounds = true;
    bool cameraAvailable = true;
    float cellWidth = 16.0f;
    float altitudeOffset = 100.0f;
    FakeEffects* effects = nullptr;
    FakeLayer* layer = nullptr;
    bool hasCsi = true;
    bool csiVisible = false;
    int csiGets = 0, csiSets = 0, lastCsiSet = -1;
    int pauses = 0, resumes = 0;
    int cameraCalls = 0;
    float camX = 0, camY = 0, camZ = 0;
    int boundsCalls = 0;
    std::vector<std::pair<int, int>> cells;
    FakeBuilding* building = nullptr;
    std::pair<int, int> buildingCell{-1000, -1000};

    bool CityEstablished() override { return established; }
    cIGZWin* View3DMainWindow() override { return root; }
    bool CityCellIsInBounds(int, int) override { ++boundsCalls; return inBounds; }
    bool CameraControlAvailable() override { return cameraAvailable; }
    float TerrainCellWidth() override { return cellWidth; }
    float TerrainAltitude(float x, float z) override { return altitudeOffset + x + z * 2.0f; }
    void SetCameraTargetPosition(float x, float y, float z) override {
        ++cameraCalls;
        camX = x;
        camY = y;
        camZ = z;
    }
    void HiddenPauseSimulator() override { ++pauses; }
    void HiddenResumeSimulator() override { ++resumes; }
    cISC4DisasterLayer* DisasterLayer() override { return layer; }
    IToolEffectsManager* EffectsManager() override { return effects; }
    ITutorialCSIVisibility* CSIVisibility() override { return hasCsi ? this : nullptr; }
    cISC4BuildingOccupant* BuildingAtCell(int cellX, int cellZ) override {
        cells.emplace_back(cellX, cellZ);
        if (building && std::make_pair(cellX, cellZ) == buildingCell) return building;
        return nullptr;
    }
    // ITutorialCSIVisibility
    bool GetVisible() override { ++csiGets; return csiVisible; }
    void SetVisible(bool visible) override { ++csiSets; lastCsiSet = visible ? 1 : 0; }
};

// ---------------------------------------------------------------------------------------------------------------
// Script-table builders.
std::shared_ptr<Table> MakeTaskRow(int axisStartX, int axisStartZ, int axisEndX, int axisEndZ, int function) {
    auto row = std::make_shared<Table>();
    auto action = std::make_shared<Table>();
    action->Set("function_index", Value::Num(function));
    action->Set("startX", Value::Num(axisStartX));
    action->Set("startZ", Value::Num(axisStartZ));
    action->Set("endX", Value::Num(axisEndX));
    action->Set("endZ", Value::Num(axisEndZ));
    action->Set("extraData", Value::Num(42));
    // The original reads every one of these numbers with GetNumber, which invalidates the task when a field is
    // missing (and leaves the value on the Lua stack); a real script row carries all of them.
    row->Set("zoomLevel", Value::Num(0));
    row->Set("cameraX", Value::Num(0));
    row->Set("cameraZ", Value::Num(0));
    row->Set("window_offsetX", Value::Num(0));
    row->Set("window_offsetY", Value::Num(0));
    row->Set("no_edge_scroll", Value::Num(0));
    row->Set("csi", Value::Num(0));
    row->Set("instruction_msg", Value::Str("text@0x00000063"));
    row->Set("try_again_msg", Value::Str("text@0x00000064"));
    row->Set("congratulation_msg", Value::Str("text@0x00000065"));
    row->Set("task_action", Value::Tbl(action));
    return row;
}

void PutTasks(FakeLua& lua, std::vector<std::shared_ptr<Table>> rows) {
    auto tasks = std::make_shared<Table>();
    for (auto& row : rows) tasks->Push(Value::Tbl(row));
    lua.globals["tutorialtasks"] = Value::Tbl(tasks);
}

cSC4TutorialTask* NewTask() {
    auto* task = new cSC4TutorialTask();
    task->AddRef();
    return task;
}

void DestroyTask(cSC4TutorialTask* task) { task->Release(); }

std::string Text(const cIGZString& value) { return std::string(value.ToChar(), value.Strlen()); }

// A window tree that owns the windows it creates and cleans them up at the end (all windows come back with the same
// refcount the tree gave them; cSC4TutorialTask balances every reference it takes).
struct WindowTree {
    FakeMgr mgr;
    std::vector<cGZWin*> windows;
    cGZWin* Make(std::uint32_t id, bool visible) {
        auto* w = new cGZWin(&mgr);
        w->SetID(id);
        w->AddRef();   // the tree's own reference
        if (visible) w->SetFlag(kWinVisible, true);
        windows.push_back(w);
        return w;
    }
    ~WindowTree() {
        for (cGZWin* w : windows) {
            if (cIGZWin* parent = w->GetParentWin()) parent->ChildRemove(w);
        }
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) (*it)->Release();
    }
};

int main() {
    // ---- defaults -------------------------------------------------------------------------------------------
    {
        cSC4TutorialTask* task = NewTask();
        CHECK_EQ(task->GetGUID(), 0u);
        CHECK_EQ(task->GetTargetGUID(), 0u);
        CHECK_EQ(task->GetZoomLevel(), 0);
        CHECK_EQ(task->GetExtraData(), 0u);
        CHECK_EQ(task->GetWhichFunction(), 0);
        CHECK_EQ(task->GetCatalogButtonItemID(), 0u);
        CHECK_EQ(task->GetWindowOffsetX(), 0);
        CHECK_EQ(task->GetWindowOffsetY(), 0);
        CHECK(task->IsEdgeScrollingAllowed());   // the no-edge-scroll byte starts 0
        CHECK(task->GetTargetWindow() == nullptr);
        // the constructor (i386 0x000d4716) starts the region at -1: no region until the action table gives one
        CHECK(task->GetRegion().x0 == -1 && task->GetRegion().z0 == -1 && task->GetRegion().x1 == -1 && task->GetRegion().z1 == -1);
        CHECK(Text(task->GetInstructionMsg()).empty());
        CHECK(task->DoLink(cRZString("x")));
        CHECK(task->Shutdown());
        CHECK(!task->HasButtonGUID(0));
        CHECK(task->ButtonGUIDs().empty());
        CHECK(task->GridData().empty());
        task->SetGUID(0x11223344);
        CHECK_EQ(task->GetGUID(), 0x11223344u);
        DestroyTask(task);
    }

    // ---- cTutorialString: the resource key is parsed out of the script text ---------------------------------
    {
        cTutorialString text;
        cGZPersistResourceKey key;
        text.FromScriptText("text@0x0000000A");   // the one-argument virtual forwards to the two-argument one
        text.GetKey(key);
        CHECK_EQ(key.type, kResourceTypeSLString);
        CHECK_EQ(key.group, kTutorialStringGroup);
        CHECK_EQ(key.instance, 0x0Au);
        text.FromScriptText("text@a1b2", 9);
        text.GetKey(key);
        CHECK_EQ(key.instance, 0xA1B2u);
        text.FromScriptText("no key here", 11);
        text.GetKey(key);
        CHECK_EQ(key.instance, 0u);              // an unparsable text leaves the instance 0
        CHECK_EQ(key.group, kTutorialStringGroup);
        cRZString other("text@0x0000000B");
        text.AssignFromIGZString(other);
        CHECK_EQ(std::string(text.ToChar()), std::string("text@0x0000000B"));
    }

    // ---- Init: a complete task ------------------------------------------------------------------------------
    FakeLua lua;
    FakeHost host;
    EffectLog effectLog;
    host.effects = new FakeEffects(&effectLog);
    DisasterLog disasterLog;
    host.layer = new FakeLayer(&disasterLog);
    SetTutorialTaskHost(&host);

    auto row = MakeTaskRow(1, 2, 3, 4, 1);
    row->Set("zoomLevel", Value::Num(2));
    row->Set("cameraX", Value::Num(5));
    row->Set("cameraZ", Value::Num(7));
    row->Set("window_offsetX", Value::Num(3));
    row->Set("window_offsetY", Value::Num(-4));
    row->Set("no_edge_scroll", Value::Num(1));
    row->Set("csi", Value::Num(1));
    row->Set("decal_color", Value::Str("selection_red"));
    row->Set("target_buttons", Value::Tbl(std::make_shared<Table>()));
    row->fields["target_buttons"].table->Push(Value::Str("0x00000011"));
    row->fields["target_buttons"].table->Push(Value::Str("a78827a"));
    row->fields["task_action"].table->Set("target_guid", Value::Str("0x00000042"));
    PutTasks(lua, {row});
    {
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->IsValid());
        CHECK_EQ(task->TaskNumber(), 1);
        CHECK_EQ(task->GetWhichFunction(), 1);
        CHECK_EQ(task->GetRegion().x0, 1);
        CHECK_EQ(task->GetRegion().z0, 2);
        CHECK_EQ(task->GetRegion().x1, 3);
        CHECK_EQ(task->GetRegion().z1, 4);
        CHECK_EQ(task->GetZoomLevel(), 2);
        CHECK_EQ(task->GetWindowOffsetX(), 3);
        CHECK_EQ(task->GetWindowOffsetY(), -4);
        CHECK_EQ(task->GetExtraData(), 42u);
        CHECK_EQ(task->GetTargetGUID(), 0x42u);
        CHECK(!task->IsEdgeScrollingAllowed());
        CHECK_EQ(Text(task->GetInstructionMsg()), "***_NOT_IN_TEXT_DATABASE_*** text@0x00000063");
        CHECK_EQ(Text(task->GetTryAgainMsg()), "***_NOT_IN_TEXT_DATABASE_*** text@0x00000064");
        CHECK_EQ(Text(task->GetCongratulationMsg()), "***_NOT_IN_TEXT_DATABASE_*** text@0x00000065");
        CHECK_EQ(task->ButtonGUIDs().size(), 2u);
        CHECK_EQ(task->GetCatalogButtonItemID(), 0x11u);
        CHECK(task->HasButtonGUID(0x11));
        CHECK(task->HasButtonGUID(0xa78827a));
        CHECK(!task->HasButtonGUID(0x12));
        CHECK(task->GridData().empty());   // function 1 does not build a grid
        CHECK(task->checkGridDataJobFinished(0, 0));   // and its grid check always answers true
        CHECK(lua.stack.empty());
        DestroyTask(task);
    }

    // ---- Init: function 0x11 builds a grid of cells and the job-completion check needs every cell ------------
    {
        PutTasks(lua, {MakeTaskRow(1, 2, 3, 5, 0x11)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK_EQ(task->GetWhichFunction(), 0x11);
        // width 2, height 3 -> 3 columns x 4 rows; taller than wide, so nothing is marked for function 0x11.
        CHECK_EQ(task->GridData().size(), 4u);
        CHECK_EQ(task->GridData()[0].size(), 3u);
        CHECK_EQ(task->GridData()[0][0].cellX, 1);
        CHECK_EQ(task->GridData()[0][0].cellZ, 2);
        CHECK_EQ(task->GridData()[3][2].cellX, 3);
        CHECK_EQ(task->GridData()[3][2].cellZ, 5);
        CHECK(!task->GridData()[0][0].border);
        CHECK(!task->checkGridDataJobFinished(2, 3));
        for (int z = 2; z <= 5; ++z)
            for (int x = 1; x <= 3; ++x) {
                const bool done = task->checkGridDataJobFinished(x, z);
                CHECK_EQ(done, z == 5 && x == 3);   // only the last cell completes the job
            }
        CHECK(task->checkGridDataJobFinished(2, 3));   // and the grid stays complete
        CHECK(!task->checkGridDataJobFinished(0, 3));  // outside the region is never marked
        CHECK(!task->checkGridDataJobFinished(2, 6));
        DestroyTask(task);
    }

    // ---- Init: function 2 marks the left/right column of a region that is not taller than wide --------------
    {
        PutTasks(lua, {MakeTaskRow(0, 0, 2, 1, 2)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK_EQ(task->GridData().size(), 2u);   // height 1 -> rows 0..1
        CHECK_EQ(task->GridData()[0].size(), 3u);
        CHECK(task->GridData()[0][0].border);
        CHECK(!task->GridData()[0][1].border);
        CHECK(task->GridData()[0][2].border);
        CHECK(task->GridData()[1][0].border);
        CHECK(!task->GridData()[1][1].border);
        CHECK(task->GridData()[1][2].border);
        CHECK(!task->checkGridDataJobFinished(1, 0));   // the centre column is still missing
        CHECK(task->checkGridDataJobFinished(1, 1));
        CHECK(task->checkGridDataJobFinished(1, 0));
        DestroyTask(task);
    }

    // ---- Init: a region that is taller than wide marks the top and bottom rows of function 2 ----------------
    {
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 2, 2)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK_EQ(task->GridData().size(), 3u);
        CHECK_EQ(task->GridData()[0].size(), 2u);
        CHECK(task->GridData()[0][0].border && task->GridData()[0][1].border);
        CHECK(!task->GridData()[1][0].border && !task->GridData()[1][1].border);
        CHECK(task->GridData()[2][0].border && task->GridData()[2][1].border);
        DestroyTask(task);
    }

    // ---- Init: missing function_index fails internally but Init still reports the task valid -----------------
    {
        auto broken = MakeTaskRow(0, 0, 1, 1, 1);
        broken->fields["task_action"].table->fields.erase("function_index");
        PutTasks(lua, {broken});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));       // Init always returns true
        CHECK(task->IsValid());           // quirk: the valid flag is re-armed at the end
        CHECK_EQ(task->GetWhichFunction(), 0);
        CHECK_EQ(task->GetRegion().x1, -1);  // the numbers after the failed one were not read: the constructor's -1 stays
        CHECK(!task->ExecuteTask());          // function 0 does nothing
        CHECK(lua.stack.empty());             // SetTop restored the stack
        DestroyTask(task);
    }

    // ---- Init: a task number above the table ----------------------------------------------------------------
    {
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 5));
        CHECK_EQ(task->GetWhichFunction(), 0);
        CHECK(Text(task->GetInstructionMsg()).empty());
        CHECK(task->ButtonGUIDs().empty());
        CHECK(lua.stack.empty());
        DestroyTask(task);
    }

    // ---- Init: task number 0 (no row loaded) ----------------------------------------------------------------
    {
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 0));
        CHECK_EQ(task->GetWhichFunction(), 0);
        CHECK(task->ButtonGUIDs().empty());
        DestroyTask(task);
    }

    // ---- GetEnableButtonList: the target GUID of function 4 is inserted at the front -------------------------
    {
        auto fn4 = MakeTaskRow(0, 0, 1, 1, 4);
        fn4->Set("target_buttons", Value::Tbl(std::make_shared<Table>()));
        fn4->fields["target_buttons"].table->Push(Value::Str("0x10"));
        fn4->fields["target_buttons"].table->Push(Value::Str("0x20"));
        fn4->fields["task_action"].table->Set("target_guid", Value::Str("0x99"));
        PutTasks(lua, {fn4});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK_EQ(task->ButtonGUIDs().size(), 3u);
        CHECK_EQ(task->ButtonGUIDs()[0], 0x99u);   // quirk: insert at the front, not append
        CHECK_EQ(task->ButtonGUIDs()[1], 0x10u);
        CHECK_EQ(task->GetCatalogButtonItemID(), 0x99u);
        CHECK(task->handleButtons(0x99));
        CHECK(!task->handleButtons(0x10));
        CHECK(!task->handleButtons(0));   // a zero target GUID never matches
        DestroyTask(task);
    }

    // ---- ExecuteTask: the camera jump uses the script camera cells when both are non-zero -------------------
    {
        effectLog = EffectLog{};
        host.cameraCalls = 0;
        host.boundsCalls = 0;
        PutTasks(lua, {MakeTaskRow(1, 2, 3, 4, 1)});
        lua.globals["tutorialtasks"].table->array[0].table->Set("cameraX", Value::Num(5));
        lua.globals["tutorialtasks"].table->array[0].table->Set("cameraZ", Value::Num(7));
        lua.globals["tutorialtasks"].table->array[0].table->Set("csi", Value::Num(1));
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        host.csiVisible = false;
        CHECK(task->ExecuteTask());
        CHECK_EQ(host.cameraCalls, 1);
        CHECK_EQ(host.boundsCalls, 1);
        CHECK_EQ(host.camX, 80.0f);                      // 16 * 5
        CHECK_EQ(host.camZ, 112.0f);                     // 16 * 7
        CHECK_EQ(host.camY, 100.0f + 80.0f + 112.0f * 2); // 404
        CHECK_EQ(host.pauses, 0);
        CHECK_EQ(effectLog.hasEffect, 1);
        CHECK_EQ(effectLog.createEffect, 1);
        CHECK_EQ(effectLog.setFloatParams, 1);
        CHECK_EQ(effectLog.lastParamType, 0xc);
        CHECK_EQ(effectLog.lastParamCount, 4);
        CHECK_EQ(effectLog.params[0], 1.0f);
        CHECK_EQ(effectLog.params[1], 2.0f);
        CHECK_EQ(effectLog.params[2], 4.0f);   // endX + 1
        CHECK_EQ(effectLog.params[3], 5.0f);   // endZ + 1
        CHECK_EQ(effectLog.lastStart, 0);
        CHECK_EQ(host.csiGets, 1);
        CHECK_EQ(host.csiSets, 1);
        CHECK_EQ(host.lastCsiSet, 1);   // the csi flag written by ExecuteTask
        CHECK(task->ExecuteTask());     // already running: does nothing
        CHECK_EQ(host.cameraCalls, 1);
        CHECK_EQ(host.csiSets, 1);
        // EndTask stops the decal with transition 1 and restores the CSI flag.
        CHECK(task->EndTask());
        CHECK_EQ(effectLog.lastStop, 1);
        CHECK(effectLog.decalReleases >= 1);
        CHECK_EQ(host.csiSets, 2);
        CHECK_EQ(host.lastCsiSet, 0);   // EndTask restores the saved visibility
        CHECK(task->EndTask());   // no longer running: true, no calls
        CHECK_EQ(host.csiSets, 2);
        DestroyTask(task);
    }

    // ---- ExecuteTask: both camera cells 0 -> jump to the region centre when endX+startX+1 >= 3 -------------
    {
        host.cameraCalls = 0;
        PutTasks(lua, {MakeTaskRow(1, 1, 3, 3, 8)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->ExecuteTask());
        CHECK_EQ(host.cameraCalls, 1);
        CHECK_EQ(host.camX, 32.0f);   // centre cell 2
        CHECK_EQ(host.camZ, 32.0f);
        DestroyTask(task);
        // A one-cell region at 0,0 has sum 1 < 3 and no jump.
        host.cameraCalls = 0;
        PutTasks(lua, {MakeTaskRow(0, 0, 0, 0, 8)});
        cSC4TutorialTask* again = NewTask();
        CHECK(again->Init(&lua, 1));
        CHECK(again->ExecuteTask());
        CHECK_EQ(host.cameraCalls, 0);
        DestroyTask(again);
        // Camera X set but camera Z 0: no jump at all.
        host.cameraCalls = 0;
        PutTasks(lua, {MakeTaskRow(1, 1, 3, 3, 8)});
        lua.globals["tutorialtasks"].table->array[0].table->Set("cameraX", Value::Num(4));
        cSC4TutorialTask* noZ = NewTask();
        CHECK(noZ->Init(&lua, 1));
        CHECK(noZ->ExecuteTask());
        CHECK_EQ(host.cameraCalls, 0);
        DestroyTask(noZ);
    }

    // ---- ExecuteTask: function 5 pauses the simulator and 0x13 does neither it nor the decal ----------------
    {
        effectLog = EffectLog{};
        host.pauses = host.resumes = 0;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 5)});
        cSC4TutorialTask* five = NewTask();
        CHECK(five->Init(&lua, 1));
        CHECK(five->ExecuteTask());
        CHECK_EQ(host.pauses, 1);
        CHECK_EQ(effectLog.createEffect, 1);
        CHECK(five->EndTask());
        CHECK_EQ(host.resumes, 1);
        DestroyTask(five);

        effectLog = EffectLog{};
        host.pauses = 0;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 4)});
        cSC4TutorialTask* four = NewTask();
        CHECK(four->Init(&lua, 1));
        CHECK(four->ExecuteTask());
        CHECK_EQ(host.pauses, 0);
        CHECK_EQ(effectLog.hasEffect, 0);   // function 4 is not in the 0x2010e mask and is not 5 / 9
        DestroyTask(four);

        effectLog = EffectLog{};
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 0x13)});
        cSC4TutorialTask* big = NewTask();
        CHECK(big->Init(&lua, 1));
        CHECK(big->ExecuteTask());
        CHECK_EQ(effectLog.hasEffect, 0);
        DestroyTask(big);
    }

    // ---- ShowHintsForUsers draws for functions 1 / 2 / 0x11 only --------------------------------------------
    {
        effectLog = EffectLog{};
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1), MakeTaskRow(0, 0, 1, 1, 4)});
        cSC4TutorialTask* one = NewTask();
        CHECK(one->Init(&lua, 1));
        one->ShowHintsForUsers();
        CHECK_EQ(effectLog.createEffect, 1);
        DestroyTask(one);
        effectLog = EffectLog{};
        cSC4TutorialTask* four = NewTask();
        CHECK(four->Init(&lua, 2));
        four->ShowHintsForUsers();
        CHECK_EQ(effectLog.createEffect, 0);
        DestroyTask(four);
    }

    // ---- TriggerFire: no building -> false and the disaster layer is not touched ----------------------------
    {
        disasterLog = DisasterLog{};
        host.building = nullptr;
        host.cells.clear();
        PutTasks(lua, {MakeTaskRow(1, 2, 3, 4, 9)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        host.buildingCell = {2, 3};
        CHECK(task->ExecuteTask());   // ExecuteTask itself returns true; the fire call was what failed
        CHECK_EQ(disasterLog.layerAddRefs, 0);
        CHECK_EQ(disasterLog.managerLookups, 0);
        // The spiral starts at the centre of the rectangle.
        CHECK(!host.cells.empty());
        CHECK_EQ(host.cells.front().first, 2);
        CHECK_EQ(host.cells.front().second, 3);
        for (const auto& cell : host.cells) {
            CHECK(cell.first >= 1 && cell.first <= 3);
            CHECK(cell.second >= 2 && cell.second <= 4);
        }
        DestroyTask(task);
    }

    // ---- TriggerFire: a building is lit with cause 6 --------------------------------------------------------
    {
        disasterLog = DisasterLog{};
        host.cells.clear();
        auto* building = new FakeBuilding();
        host.building = building;
        host.buildingCell = {3, 4};
        PutTasks(lua, {MakeTaskRow(1, 2, 3, 4, 9)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->ExecuteTask());
        CHECK_EQ(disasterLog.managerLookups, 1);
        CHECK_EQ(disasterLog.fireQueries, 1);
        CHECK_EQ(disasterLog.ignites, 1);
        CHECK_EQ(disasterLog.causes, 1);
        CHECK_EQ(disasterLog.lastCause, 6u);
        CHECK(disasterLog.ignitedOccupant == nullptr);   // the fake building has no occupant body
        CHECK_EQ(building->occupantRequests, 1);
        CHECK_EQ(building->addRefs, 1);
        CHECK_EQ(building->releases, 1);
        CHECK_EQ(building->refs, 1);   // only the test's own reference left
        host.building = nullptr;
        building->Release();
        // Direct TriggerFire with a building that exists but no fire interface answer.
        disasterLog = DisasterLog{};
        disasterLog.fireQuerySucceeds = false;
        auto* second = new FakeBuilding();
        host.building = second;
        host.buildingCell = {2, 3};
        cSC4TutorialTask* direct = NewTask();
        CHECK(direct->Init(&lua, 1));
        SC4TutorialRect area{1, 2, 3, 4};
        CHECK(!direct->TriggerFire(area));
        CHECK_EQ(disasterLog.ignites, 0);
        host.building = nullptr;
        second->Release();
        DestroyTask(direct);
        DestroyTask(task);
    }

    // ---- handleButtons compares against the target GUID ------------------------------------------------------
    {
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        lua.globals["tutorialtasks"].table->array[0].table->fields["task_action"].table->Set("target_guid", Value::Str("0x77"));
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->handleButtons(0x77));
        CHECK(!task->handleButtons(0x78));
        CHECK_EQ(task->GetTargetGUID(), 0x77u);
        DestroyTask(task);
    }

    // ---- GetTargetWindow: no button GUIDs -> null -----------------------------------------------------------
    {
        WindowTree tree;
        host.root = tree.Make(1, true);
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->GetTargetWindow() == nullptr);
        DestroyTask(task);
        host.root = nullptr;
    }

    // ---- GetTargetWindow: established city walks up to the root and answers the button window ---------------
    {
        WindowTree tree;
        cGZWin* root = tree.Make(1, true);
        cGZWin* middle = tree.Make(0x999, true);
        cGZWin* button = tree.Make(0x1111, true);
        CHECK(root->ChildAdd(middle));
        CHECK(middle->ChildAdd(button));
        host.root = root;
        host.established = true;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        auto buttonRow = lua.globals["tutorialtasks"].table->array[0].table;
        buttonRow->Set("target_buttons", Value::Tbl(std::make_shared<Table>()));
        buttonRow->fields["target_buttons"].table->Push(Value::Str("0x1111"));
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        cIGZWin* found = task->GetTargetWindow();
        CHECK(found == static_cast<cIGZWin*>(button));
        CHECK(button->GetFlag(kWinEnabled));   // the walk enables every window it finds
        // The returned pointer is borrowed: the window is still the tree's child and alive.
        CHECK(button->GetParentWin() == static_cast<cIGZWin*>(middle));
        DestroyTask(task);

        // The result follows the visibility of the last ancestor visited, not of the button itself.
        middle->SetFlag(kWinVisible, false);
        cSC4TutorialTask* second = NewTask();
        CHECK(second->Init(&lua, 1));
        CHECK(second->GetTargetWindow() == nullptr);
        DestroyTask(second);
        middle->SetFlag(kWinVisible, true);
        button->SetFlag(kWinVisible, false);
        cSC4TutorialTask* third = NewTask();
        CHECK(third->Init(&lua, 1));
        CHECK(third->GetTargetWindow() == nullptr);   // an invisible button never enters the walk
        DestroyTask(third);
        host.root = nullptr;
    }

    // ---- GetTargetWindow: several GUIDs; the first found result wins and all are enabled ---------------------
    {
        WindowTree tree;
        cGZWin* root = tree.Make(1, true);
        cGZWin* first = tree.Make(0x1111, true);
        cGZWin* second = tree.Make(0x2222, true);
        CHECK(root->ChildAdd(first));
        CHECK(root->ChildAdd(second));
        host.root = root;
        host.established = true;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        auto buttonRow = lua.globals["tutorialtasks"].table->array[0].table;
        buttonRow->Set("target_buttons", Value::Tbl(std::make_shared<Table>()));
        buttonRow->fields["target_buttons"].table->Push(Value::Str("0"));
        buttonRow->fields["target_buttons"].table->Push(Value::Str("0x3333"));   // no such window
        buttonRow->fields["target_buttons"].table->Push(Value::Str("0x1111"));
        buttonRow->fields["target_buttons"].table->Push(Value::Str("0x2222"));
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        cIGZWin* found = task->GetTargetWindow();
        CHECK(found == static_cast<cIGZWin*>(first));
        CHECK(first->GetFlag(kWinEnabled));
        CHECK(second->GetFlag(kWinEnabled));
        DestroyTask(task);
        host.root = nullptr;
    }

    // ---- GetTargetWindow: an unestablished city substitutes the alt window and its same-id child ------------
    {
        WindowTree tree;
        cGZWin* root = tree.Make(1, true);
        cGZWin* alt = tree.Make(0xc991eda8, true);   // added first, so the parent list keeps the parent below first
        cGZWin* parent = tree.Make(0x0a78827a, true);
        cGZWin* button = tree.Make(0xa9ed5617, true);
        cGZWin* altChild = tree.Make(0xa9ed5617, true);
        CHECK(root->ChildAdd(alt));
        CHECK(root->ChildAdd(parent));
        CHECK(parent->ChildAdd(button));
        CHECK(alt->ChildAdd(altChild));
        host.root = root;
        host.established = false;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        auto buttonRow = lua.globals["tutorialtasks"].table->array[0].table;
        buttonRow->Set("target_buttons", Value::Tbl(std::make_shared<Table>()));
        buttonRow->fields["target_buttons"].table->Push(Value::Str("0xa9ed5617"));
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        cIGZWin* found = task->GetTargetWindow();
        CHECK(found == static_cast<cIGZWin*>(altChild));   // the same-id child of the alt window
        DestroyTask(task);
        // The established walk does not substitute: it answers the button window itself.
        host.established = true;
        cSC4TutorialTask* established = NewTask();
        CHECK(established->Init(&lua, 1));
        CHECK(established->GetTargetWindow() == static_cast<cIGZWin*>(button));
        DestroyTask(established);
        host.root = nullptr;
    }

    // ---- A missing numeric row field invalidates the rest of Init (GetNumber quirk) -------------------------
    {
        auto incomplete = MakeTaskRow(0, 0, 1, 1, 9);
        incomplete->fields.erase("zoomLevel");
        PutTasks(lua, {incomplete});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->IsValid());                  // re-armed at the end of Init
        CHECK_EQ(task->GetWhichFunction(), 0);   // GetTaskInformation was skipped
        CHECK_EQ(Text(task->GetInstructionMsg()), "***_NOT_IN_TEXT_DATABASE_*** text@0x00000063");
        DestroyTask(task);
    }

    // ---- Init twice appends to the button list and to the grid (quirk) --------------------------------------
    {
        auto twice = MakeTaskRow(0, 0, 1, 1, 0x11);
        twice->Set("target_buttons", Value::Tbl(std::make_shared<Table>()));
        twice->fields["target_buttons"].table->Push(Value::Str("0x10"));
        PutTasks(lua, {twice});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        const std::size_t rows = task->GridData().size();
        CHECK(task->Init(&lua, 1));
        CHECK_EQ(task->GridData().size(), rows * 2);
        CHECK_EQ(task->ButtonGUIDs().size(), 2u);
        DestroyTask(task);
    }

    // ---- drawDecals: a missing effect keeps the previous decal cleared and returns true ---------------------
    {
        effectLog = EffectLog{};
        host.effects->has = false;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->drawDecals(0, 0, 1, 1));
        CHECK_EQ(effectLog.hasEffect, 1);
        CHECK_EQ(effectLog.createEffect, 0);
        CHECK_EQ(effectLog.start, 0);
        DestroyTask(task);
        host.effects->has = true;
    }

    // ---- TriggerFire: no disaster layer releases the building and answers false -----------------------------
    {
        disasterLog = DisasterLog{};
        auto* building = new FakeBuilding();
        host.building = building;
        host.buildingCell = {1, 1};
        FakeLayer* saved = host.layer;
        host.layer = nullptr;
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 9)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(!task->TriggerFire(SC4TutorialRect{0, 0, 1, 1}));
        CHECK_EQ(disasterLog.managerLookups, 0);
        CHECK_EQ(building->releases, 1);
        host.layer = saved;
        host.building = nullptr;
        building->Release();
        DestroyTask(task);
    }

    // ---- QueryInterface / refcounts -------------------------------------------------------------------------
    {
        cSC4TutorialTask* task = NewTask();
        CHECK_EQ(task->RefCount(), 1u);
        cISC4TutorialTask* iface = nullptr;
        CHECK(task->QueryInterface(kGZIID_cISC4TutorialTask, reinterpret_cast<void**>(&iface)));
        CHECK(iface == static_cast<cISC4TutorialTask*>(task));
        CHECK_EQ(task->RefCount(), 2u);   // QueryInterface takes a reference
        CHECK(iface->Shutdown());
        CHECK_EQ(iface->Release(), 1u);
        cIGZUnknown* unknown = nullptr;
        CHECK(task->QueryInterface(kGZIID_cIGZUnknown, reinterpret_cast<void**>(&unknown)));
        CHECK(unknown != nullptr);
        CHECK(reinterpret_cast<void*>(unknown) != reinterpret_cast<void*>(task));   // the cRZUnknown sub-object
        CHECK_EQ(task->RefCount(), 2u);
        unknown->Release();
        CHECK(!task->QueryInterface(0xdeadbeef, reinterpret_cast<void**>(&unknown)));
        CHECK_EQ(task->Release(), 0u);   // destroys the object (heap allocated)
    }

    // ---- The destructor releases a decal that was never stopped ---------------------------------------------
    {
        effectLog = EffectLog{};
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 1)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(task->drawDecals(0, 0, 2, 2));
        CHECK_EQ(effectLog.createEffect, 1);
        CHECK_EQ(effectLog.stop, 0);
        task->Release();   // the destructor stops nothing but releases the decal
        CHECK_EQ(effectLog.stop, 0);
        CHECK_EQ(effectLog.decalReleases, 1);
    }

    // ---- A host that is not there keeps every call safe ------------------------------------------------------
    {
        SetTutorialTaskHost(nullptr);
        PutTasks(lua, {MakeTaskRow(0, 0, 1, 1, 9)});
        cSC4TutorialTask* task = NewTask();
        CHECK(task->Init(&lua, 1));
        CHECK(!task->TriggerFire(SC4TutorialRect{0, 0, 1, 1}));
        CHECK(task->drawDecals(0, 0, 1, 1));
        CHECK(task->ExecuteTask());   // no camera / effects / CSI without a host
        CHECK(task->EndTask());
        CHECK(task->GetTargetWindow() == nullptr);
        DestroyTask(task);
    }

    SetTutorialTaskHost(nullptr);
    delete host.effects;
    delete host.layer;
    if (failures == 0) std::printf("sc4_tutorialtask: ok\n");
    return failures == 0 ? 0 : 1;
}
