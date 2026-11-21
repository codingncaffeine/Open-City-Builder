// cSC4MySim (src/sc4/MySim.cpp): construction/interface views, Init with the advisor script tables, the identity and
// state accessors, MoveIn/MoveOut, the residence/job flows, the work/home state machine, the balloon queues,
// DoMessage (dispatch notifications, cancellation, hide-all), the dispatch tool, the window/camera paths, the script
// commands, ScriptLua updates and the serialization guard.
//
// The host seam (IMySimHost), the dispatch (IMySimDispatch), the balloon window proc, the callout box, the effects
// object and the pylon are fakes; the Lua context is a fake cISCLua (with an IScriptLua adapter for the advisor base)
// and the advisor host is a fake too. The building occupants and the lot are local fakes as well.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AdviceSubject.h"
#include "sc4/Lot.h"
#include "sc4/MySim.h"
#include "sc4/SLString.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

// =====================================================================================================================
// A small fake cISCLua: values on a stack, globals, tables, registry refs and userdata owners.
// =====================================================================================================================
struct LuaTable;
struct LuaValue {
    enum Kind { Nil, Bool, Number, Text, TableKind, Function, UserData } kind = Nil;
    double number = 0.0;
    std::string text;
    bool boolean = false;
    void* user = nullptr;
    int tag = 0;
    std::shared_ptr<LuaTable> table;
    static LuaValue Num(double v) { LuaValue x; x.kind = Number; x.number = v; return x; }
    static LuaValue Str(const char* s) { LuaValue x; x.kind = Text; x.text = s ? s : ""; return x; }
    static LuaValue Tbl(std::shared_ptr<LuaTable> t) { LuaValue x; x.kind = TableKind; x.table = std::move(t); return x; }
};
struct LuaTable {
    std::map<std::string, LuaValue> fields;
    std::vector<LuaValue> array;
    LuaValue Get(const LuaValue& key) const {
        if (key.kind == LuaValue::Number) {
            const int index = static_cast<int>(key.number);
            if (index >= 1 && static_cast<std::size_t>(index) <= array.size()) return array[static_cast<std::size_t>(index - 1)];
            return LuaValue{};
        }
        if (key.kind == LuaValue::Text) {
            const auto it = fields.find(key.text);
            return it == fields.end() ? LuaValue{} : it->second;
        }
        return LuaValue{};
    }
    int Count() const { return static_cast<int>(array.size()); }
};

struct FakeLua final : cISCLua {
    std::vector<LuaValue> stack;
    std::map<std::string, LuaValue> globals;
    std::vector<LuaValue> refs;
    std::vector<void*> owners;
    int calls = 0;
    double protectedResult = 7.0;

    ~FakeLua() {
        for (void* p : owners) static_cast<cIGZUnknown*>(p)->Release();
    }
    int Abs(int index) const { return index < 0 ? static_cast<int>(stack.size()) + index : index - 1; }
    LuaValue& At(int index) { return stack[static_cast<std::size_t>(Abs(index))]; }
    const LuaValue& At(int index) const { return stack[static_cast<std::size_t>(Abs(index))]; }
    void Push(LuaValue v) { stack.push_back(std::move(v)); }
    bool IsNil(int index) override { return Abs(index) < 0 || static_cast<std::size_t>(Abs(index)) >= stack.size() || At(index).kind == LuaValue::Nil; }
    int Type(int index) override { return IsNil(index) ? 0 : static_cast<int>(At(index).kind); }
    bool IsNumber(int index) override { return !IsNil(index) && At(index).kind == LuaValue::Number; }
    bool IsString(int index) override { return !IsNil(index) && At(index).kind == LuaValue::Text; }
    bool IsTable(int index) override { return !IsNil(index) && At(index).kind == LuaValue::TableKind; }
    bool IsFunction(int index) override { return !IsNil(index) && At(index).kind == LuaValue::Function; }
    bool IsUserData(int index) override { return !IsNil(index) && At(index).kind == LuaValue::UserData; }
    double ToNumber(int index) override { return IsNil(index) ? 0.0 : At(index).number; }
    const char* ToString(int index) override { return IsNil(index) ? "" : At(index).text.c_str(); }
    int Strlen(int index) override { return IsNil(index) ? 0 : static_cast<int>(At(index).text.size()); }
    void* ToUserData(int index) override { return (!IsNil(index) && At(index).kind == LuaValue::UserData) ? At(index).user : nullptr; }
    void* ToUserTag(int index) override { return ToUserData(index); }
    int GetTop() override { return static_cast<int>(stack.size()); }
    void SetTop(int index) override { stack.resize(static_cast<std::size_t>(index < 0 ? 0 : index)); }
    void Pop(int count) override { stack.resize(stack.size() - static_cast<std::size_t>(count)); }
    void PushNil() override { Push(LuaValue{}); }
    void PushNumber(double v) override { Push(LuaValue::Num(v)); }
    void PushString(const char* s) override { Push(LuaValue::Str(s)); }
    void PushLString(const char* s, int n) override { Push(LuaValue::Str(std::string(s, static_cast<std::size_t>(n)).c_str())); }
    void PushBoolean(bool v) override { LuaValue x; x.kind = LuaValue::Bool; x.boolean = v; Push(x); }
    void PushUserData(void* p, int tag) override {
        LuaValue x; x.kind = LuaValue::UserData; x.user = p; x.tag = tag; Push(x);
        if (p) owners.push_back(p);          // the Lua userdata owns the reference the caller handed over
    }
    void PushUserTag(void* p, int tag) override { PushUserData(p, tag); }
    void NewTable() override { Push(LuaValue::Tbl(std::make_shared<LuaTable>())); }
    void GetGlobal(const char* name) override {
        const auto it = globals.find(name);
        Push(it == globals.end() ? LuaValue{} : it->second);
    }
    void SetGlobal(const char* name) override { globals[name] = stack.back(); stack.pop_back(); }
    void GetTable(int index) override {
        const int abs = Abs(index);
        const LuaValue key = stack.back();
        stack.pop_back();
        const LuaValue table = stack[static_cast<std::size_t>(abs)];
        Push(table.kind == LuaValue::TableKind ? table.table->Get(key) : LuaValue{});
        ++calls;
    }
    void RawGet(int index) override { GetTable(index); }
    void RawGetI(int index, int n) override {
        const LuaValue table = At(index);
        if (table.kind == LuaValue::TableKind && n >= 1 && static_cast<std::size_t>(n) <= table.table->array.size()) {
            Push(table.table->array[static_cast<std::size_t>(n) - 1]);
        } else {
            Push(LuaValue{});
        }
    }
    void SetTable(int index) override {
        const int abs = Abs(index);
        const LuaValue value = stack.back(); stack.pop_back();
        const LuaValue key = stack.back(); stack.pop_back();
        LuaValue& table = stack[static_cast<std::size_t>(abs)];
        if (table.kind != LuaValue::TableKind) return;
        if (key.kind == LuaValue::Number) {
            const int i = static_cast<int>(key.number);
            if (i >= 1 && static_cast<std::size_t>(i) <= table.table->array.size()) table.table->array[static_cast<std::size_t>(i - 1)] = value;
        } else if (key.kind == LuaValue::Text) {
            table.table->fields[key.text] = value;
        }
        ++calls;
    }
    void RawSet(int index) override { SetTable(index); }
    void RawSetI(int index, int n) override {
        LuaValue& table = At(index);
        if (table.kind != LuaValue::TableKind) return;
        if (n >= 1 && static_cast<std::size_t>(n) <= table.table->array.size()) table.table->array[static_cast<std::size_t>(n - 1)] = stack.back();
        stack.pop_back();
    }
    bool GetRef(int ref) override {
        if (ref < 1 || static_cast<std::size_t>(ref) > refs.size()) return false;
        Push(refs[static_cast<std::size_t>(ref - 1)]);
        return true;
    }
    int Ref(int) override { refs.push_back(stack.back()); stack.pop_back(); return static_cast<int>(refs.size()); }
    void Unref(int ref) override { if (ref >= 1 && static_cast<std::size_t>(ref) <= refs.size()) refs[static_cast<std::size_t>(ref - 1)] = LuaValue{}; }
    int CallProtected(int, int results) override {
        ++calls;
        for (int i = 0; i < results; ++i) PushNumber(protectedResult);
        return 0;
    }
    int GetN(int index) override { return IsNil(index) ? 0 : (At(index).kind == LuaValue::TableKind ? At(index).table->Count() : 0); }
    // unused Lua API
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Open(int) override { return false; }
    bool Close() override { return false; }
    void* AsIGZLua5() override { return nullptr; }
    void PushValue(int) override {}
    void Remove(int) override {}
    void Insert(int) override {}
    const char* TypeName(int) override { return ""; }
    bool IsCFunction(int) override { return false; }
    int Tag(int) override { return 0; }
    bool IsBoolean(int) override { return false; }
    bool Equal(int, int) override { return false; }
    bool LessThan(int, int) override { return false; }
    std::uint32_t ToUint32(int index) override { return static_cast<std::uint32_t>(ToNumber(index)); }
    std::int32_t ToSint32(int index) override { return static_cast<std::int32_t>(ToNumber(index)); }
    CFunction ToCFunction(int) override { return nullptr; }
    bool ToBoolean(int) override { return false; }
    void PushCClosure(CFunction, int) override {}
    void GetGlobals() override {}
    void GetTagMethod(int, const char*) override {}
    void SetTagMethod(int, const char*) override {}
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
    int Next(int) override { return 0; }
    void Error(const char*) override {}
    void Concat(int) override {}
    void* NewUserData(int) override { return nullptr; }
    void Register(CFunction, const char*) override {}
    int CloneTag(int) override { return 0; }
    bool ToDataRegistryEntryID(cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryNode(cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryValue(cISC4GameDataRegistry*, int) override { return false; }
};

// IScriptLua view of the same stack, for the advisor base's cTablePropertyReader.
struct ScriptLuaAdapter final : IScriptLua {
    FakeLua* lua;
    explicit ScriptLuaAdapter(FakeLua* l) : lua(l) {}
    int GetTop() override { return lua->GetTop(); }
    void SetTop(int top) override { lua->SetTop(top); }
    void Pop(int count) override { lua->Pop(count); }
    int Type(int index) override { return lua->Type(index); }
    bool IsNumber(int index) override { return lua->IsNumber(index); }
    bool IsBoolean(int index) override { return lua->IsBoolean(index); }
    double ToNumber(int index) override { return lua->ToNumber(index); }
    const char* ToString(int index) override { return lua->ToString(index); }
    bool ToBoolean(int index) override { return lua->ToBoolean(index); }
    void PushNumber(double value) override { lua->PushNumber(value); }
    void PushString(const char* text) override { lua->PushString(text); }
    void GetGlobal(const char* name) override { lua->GetGlobal(name); }
    void RawGet(int tableIndex) override { lua->RawGet(tableIndex); }
    void GetRef(int ref) override { lua->GetRef(ref); }
    int Ref(bool) override { return lua->Ref(1); }
    int CallProtected(int args, int results) override { return lua->CallProtected(args, results); }
    int DoString(const char* chunk) override { return lua->DoString(chunk); }
    int DoBuffer(const char* buffer, std::size_t size, const char* name) override {
        return lua->DoBuffer(buffer, static_cast<int>(size), name);
    }
    void Unref(int ref) override { lua->Unref(ref); }
    int ObjLen(int index) override { return lua->GetN(index); }
    bool IsFunction(int index) override { return lua->IsFunction(index); }
    bool IsTable(int index) override { return lua->IsTable(index); }
    bool GetField(const char* name, int tableIndex) override {
        lua->PushString(name);
        lua->RawGet(tableIndex);
        const bool present = !lua->IsNil(-1);
        if (!present) lua->Pop(1);
        return present;
    }
    void ReadTableAsList(std::list<std::uint32_t>& out, int tableIndex) override {
        const int count = lua->GetN(tableIndex);
        for (int i = 1; i <= count; ++i) out.push_back(static_cast<std::uint32_t>(lua->ToNumber(tableIndex)));
    }
};

// =====================================================================================================================
// Fake objects for the seams.
// =====================================================================================================================
struct FakeOccupant final : cISC4Occupant {
    int refs = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    SC4CellRect cells{0, 0, 0, 0};
    std::uint32_t type = kOccupantDefaultType;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4Occupant || iid == kGZIID_cIGZUnknown) {
            *out = static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(this));
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
    void GetBoundingBox(float* minXyz, float* maxXyz) override { minXyz[0] = position[0]; minXyz[2] = position[2]; maxXyz[0] = position[0]; maxXyz[2] = position[2]; }
    void GetBoundingCityCells(SC4CellRect& out) override { out = cells; }
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

struct FakeBuilding final : cISC4BuildingOccupant, cISC4Occupant {
    int refs = 0;
    BuildingProfile profile;
    cRZString name{"Fake Building"};
    cRZString exemplar{"fakeexemplar"};
    float position[3] = {0.0f, 0.0f, 0.0f};
    SC4CellRect cells{1, 2, 3, 4};
    float completion = 1.0f;
    bool lit = false;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4BuildingOccupant) {
            *out = static_cast<cIGZUnknown*>(static_cast<cISC4BuildingOccupant*>(this));
            AddRef();
            return true;
        }
        if (iid == kGZIID_cISC4Occupant) {
            *out = static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(this));
            AddRef();
            return true;
        }
        if (iid == kGZIID_cIGZUnknown) {
            *out = static_cast<cIGZUnknown*>(static_cast<cISC4BuildingOccupant*>(this));
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    cISC4Occupant* AsOccupant() override { return static_cast<cISC4Occupant*>(this); }
    std::uint32_t GetBuildingType() const override { return 0x1001; }
    void SetBuildingType(std::uint32_t) override {}
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { return completion; }
    void SetCompletionPercent(float value) override { completion = value; }
    const BuildingProfile& GetBuildingProfile() const override { return profile; }
    const cRZString& GetBuildingName() const override { return name; }
    const cRZString& GetExemplarName() const override { return exemplar; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return true; }
    bool IsLit() const override { return lit; }
    void SetLit(bool value) override { lit = value; }
    void SetName(const cRZString& value) override { name = value; }
    bool GetName(cRZString& out) override { out = name; return true; }
    // cISC4Occupant
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return kOccupantTypeBuilding; }
    void GetPosition(float* xyz) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
    void GetBoundingBox(float* minXyz, float* maxXyz) override { minXyz[0] = position[0] - 1; minXyz[1] = position[1]; minXyz[2] = position[2] - 1; maxXyz[0] = position[0] + 1; maxXyz[1] = position[1] + 2; maxXyz[2] = position[2] + 1; }
    void GetBoundingCityCells(SC4CellRect& out) override { out = cells; }
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

// Global records the fakes fill.
struct DispatchLog {
    int created = 0, init = 0, shutdown = 0, teleports = 0, goTo = 0, addDestination = 0, startTracking = 0, stopTracking = 0;
    int trackHandle = 0xbeef;
    int state = 0;
    int travelType = 0;
    bool followCamera = false;
    bool goToResult = true;
    bool checkFlags = false;
    int flags = 0;
    int tuning[5] = {0, 0, 0, 0, 0};
    std::vector<cGZPersistResourceKey> modelKeys;
    std::vector<int> modelOffsets;
    MySimLocation current, destination, defaultDestination, commuteDestination;
};

struct FakeDispatch final : IMySimDispatch {
    DispatchLog* log;
    int refs = 0;
    explicit FakeDispatch(DispatchLog* l) : log(l) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<IMySimDispatch*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    bool Init() override { ++log->init; return true; }
    bool Shutdown() override { ++log->shutdown; return true; }
    void SetDefaultDestination(const MySimLocation& location) override { log->defaultDestination = location; }
    bool SetCurrentCommuteDestination(const MySimLocation& location, bool) override { log->commuteDestination = location; return true; }
    bool AddDestinationLocation(const MySimLocation& location, int) override { ++log->addDestination; log->destination = location; return true; }
    bool GoTo(const MySimLocation& location) override { ++log->goTo; log->destination = location; return log->goToResult; }
    void TeleportToCommuteLocation() override { ++log->teleports; }
    const MySimLocation& GetCurrentLocation() const override { return log->current; }
    const MySimLocation& GetCurrentDestination() const override { return log->destination; }
    const MySimLocation& GetCurrentDefaultDestination() const override { return log->defaultDestination; }
    const MySimLocation& GetCurrentCommuteDestination() const override { return log->commuteDestination; }
    bool CheckFlags(int) const override { return log->checkFlags; }
    int GetState() const override { return log->state; }
    int GetTravelType() const override { return log->travelType; }
    int GetTransitSwitchWaitTimeMunutes() override { return 0; }
    void SetModelOffset(int type, int offset, bool) override { log->modelOffsets.push_back(type * 1000 + offset); }
    void SetModelBaseResKey(int, const cGZPersistResourceKey& key, int) override { log->modelKeys.push_back(key); }
    void StartTracking(int, const cRZString&) override { ++log->startTracking; }
    void StopTracking() override { ++log->stopTracking; }
    int TrackHandle() const override { return log->trackHandle; }
    void SetFollowCamera(bool follow) override { log->followCamera = follow; }
    bool CanUDriveIt() override { return true; }
    bool EnterUDriveIt() override { return true; }
    bool ExitUDriveIt() override { return true; }
    void SetDestinationApproachDistance(int value) override { log->tuning[0] = value; }
    void SetDestinationProximityMin(int value) override { log->tuning[1] = value; }
    void SetMinTimeIdling(int value) override { log->tuning[2] = value; }
    void SetMinTimeAtLocation(int value) override { log->tuning[3] = value; }
    void SetMinTimeAtSwitch(int value) override { log->tuning[4] = value; }
};

struct FakeWinProc final : IMySimBalloonWinProc {
    int refs = 0, inits = 0, shutdowns = 0;
    int hidden = 0, shown = 0, fade = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<IMySimBalloonWinProc*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    bool Init() override { ++inits; return true; }
    bool Shutdown() override { ++shutdowns; return true; }
    void SetHiddenDuration(int value) override { hidden = value; }
    void SetShownDuration(int value) override { shown = value; }
    void SetFadeDuration(int value) override { fade = value; }
};

struct CalloutLog {
    int styles = 0, options = 0, images = 0, texts = 0, positions = 0, addToView = 0, shows = 0, activates = 0, hides = 0;
    std::uint32_t style = 0;
    float screen[2] = {0.0f, 0.0f};
    std::string text;
};
struct FakeCallout final : IMySimCallout {
    CalloutLog* log;
    int refs = 0;
    explicit FakeCallout(CalloutLog* l) : log(l) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<IMySimCallout*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    void SetStyle(std::uint32_t style) override { ++log->styles; log->style = style; }
    void SetOptions(int) override { ++log->options; }
    void SetImage(const cGZPersistResourceKey&) override { ++log->images; }
    void SetText(const cIGZString& text) override { ++log->texts; log->text = text.ToChar(); }
    void SetScreenPosition(const float* xy) override { ++log->positions; log->screen[0] = xy[0]; log->screen[1] = xy[1]; }
    void AddToView() override { ++log->addToView; }
    void Show() override { ++log->shows; }
    void Activate() override { ++log->activates; }
    void Hide() override { ++log->hides; }
};

struct PylonLog { int inits = 0, shutdowns = 0, kinds = 0, flags = 0, states = 0, bitmaps = 0, linked = 0, agents = 0, visibility = 0; int lastKind = -1; bool lastVisible = false; };
struct FakePylon final : IMySimPylon {
    PylonLog* log;
    FakeOccupant* occupant;
    int refs = 0;
    FakePylon(PylonLog* l, FakeOccupant* o) : log(l), occupant(o) { occupant->AddRef(); }
    ~FakePylon() { occupant->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<IMySimPylon*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    bool Init() override { ++log->inits; return true; }
    bool Shutdown() override { ++log->shutdowns; return true; }
    void SetState(int) override { ++log->states; }
    void SetBitmap(int, const cIGZString&) override { ++log->bitmaps; }
    void SetFlags(int) override { ++log->flags; }
    void SetKind(int kind) override { ++log->kinds; log->lastKind = kind; }
    void SetLinkedObject(cISC4Occupant*) override { ++log->linked; }
    cISC4Occupant* GetLinkedOccupant() override { return occupant; }
    void SetLinkedAgent(cIGZUnknown*) override { ++log->agents; }
    bool IsDead() const override { return false; }
    cISC4Occupant* AsOccupant() override { return occupant; }
    void SetVisibility(bool visible) override { ++log->visibility; log->lastVisible = visible; }
};

struct FakeEffect final : IMySimEffect {
    int refs = 0, active = 0, transforms = 0;
    float matrix[16] = {};
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<IMySimEffect*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    bool activeState = false;
    void SetActive(bool value) override { ++active; activeState = value; }
    void SetTransform(const float* m) override { ++transforms; std::memcpy(matrix, m, sizeof(matrix)); }
};

struct FakeHomeSound final : IMySimHomeSound {
    int refs = 0, positions = 0, playing = 0;
    float xyz[3] = {};
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<IMySimHomeSound*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    void SetPosition(const float* p) override { ++positions; std::memcpy(xyz, p, sizeof(xyz)); }
    bool IsPlaying() const override { return playing != 0; }
};

struct FakeBus final : cIGZMessageServer2 {
    struct Sent { std::uint32_t type = 0; cIGZUnknown* unknown = nullptr; bool has1 = false, has2 = false; std::uint64_t data1 = 0, data2 = 0; };
    int refs = 0;
    std::vector<Sent> sent;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessageServer2) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<cIGZMessageServer2*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        Sent entry;
        entry.type = message->GetType();
        entry.unknown = standard->GetIGZUnknown();
        entry.has1 = standard->GetHasData1();
        entry.has2 = standard->GetHasData2();
        if (entry.has1) entry.data1 = standard->GetData1();
        if (entry.has2) entry.data2 = standard->GetData2();
        sent.push_back(entry);
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override { return MessageSend(message); }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { added.emplace_back(target, type); return true; }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { removed.emplace_back(target, type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 0; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void** out) override { *out = nullptr; return false; }
};

// The whole seam host + the advisor host (two interfaces) in one object.
struct FakeHost final : IMySimHost {
    FakeLua lua;
    ScriptLuaAdapter adapter{&lua};
    FakeBus bus;
    DispatchLog dispatchLog;
    CalloutLog calloutLog;
    PylonLog pylonLog;
    std::vector<std::string> loadedStrings;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loadCalls;
    cSC4Lot* lot = nullptr;
    FakeBuilding* residenceBuilding = nullptr;
    FakeBuilding* jobBuilding = nullptr;
    std::vector<FakeBuilding*> candidates;
    std::vector<FakeBuilding*> nearby;
    FakeBuilding* neighbor = nullptr;
    std::map<std::pair<int, int>, cISC4Occupant*> occupantsAtCell;
    std::vector<cISC4Occupant*> cityOccupants;
    float terrainHeight = 12.5f;
    float seaLevel = 10.0f;
    bool paused = false;
    bool simulatorTick = true;
    int hour = 10;
    int residentialAge = 30;
    float eq = 60.0f, hq = 70.0f;
    int schoolGrade = 55, hospitalGrade = 66;
    bool cameraCalled = false;
    float cameraPosition[3] = {};
    std::string dialogTitle, dialogBody;
    int dialogCalls = 0;
    int registryAdds = 0, registryRemoves = 0;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> posts;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loadStringCalls;
    cIGZUnknown* inputControl = nullptr;
    cIGZUnknown* viewInputControl = nullptr;
    int viewPushes = 0, viewSets = 0, toolCreates = 0, moveInTools = 0;
    int homeSounds = 0, citySounds = 0, homeSoundDestroyed = 0;
    int iconRects = 0;
    int iconRect[4] = {10, 20, 30, 40};
    int occupantAtCellCalls = 0;
    int commuteCalls = 0;
    bool commuteResult = true;
    int commuteMinutes = 7;
    std::map<std::uint32_t, std::uint32_t>* lastGroups = nullptr;
    int bitmapCalls = 0;
    bool closed = false;

    std::vector<cIGZUnknown*> kept;
    void Keep(cIGZUnknown* object) { if (object) { object->AddRef(); kept.push_back(object); } }
    FakeHost() { SetMySimHost(this); }
    ~FakeHost() override {
        for (cIGZUnknown* object : kept) object->Release();
        if (lot) lot->Release();
        if (inputControl) inputControl->Release();
        if (viewInputControl) viewInputControl->Release();
        if (residenceBuilding) residenceBuilding->Release();
        if (jobBuilding) jobBuilding->Release();
        for (FakeBuilding* b : candidates) b->Release();
        for (FakeBuilding* b : nearby) b->Release();
        if (neighbor) neighbor->Release();
        for (auto& entry : occupantsAtCell) entry.second->Release();
        for (cISC4Occupant* o : cityOccupants) o->Release();
        SetMySimHost(nullptr);
    }

    // ---- IMySimHost ----
    cISCLua* Lua() override { return &lua; }
    int LuaUserDataTag(bool stringValue) override { return stringValue ? 2 : 1; }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        loadStringCalls.emplace_back(group, instance);
        char text[64];
        std::snprintf(text, sizeof(text), "str:%08x", instance);
        out.AssignFromCString(text);
        return true;
    }
    void ResidenceCandidates(std::vector<cISC4BuildingOccupant*>& out) override {
        for (FakeBuilding* b : candidates) out.push_back(b);
    }
    bool IsResidenceCandidate(cISC4BuildingOccupant* building) override { return building == residenceBuilding || building == jobBuilding; }
    void BuildingsNearResidence(int, int, int, std::vector<cISC4BuildingOccupant*>& out) override {
        for (FakeBuilding* b : nearby) out.push_back(b);
    }
    cISC4Occupant* OccupantAtCell(int x, int z) override {
        ++occupantAtCellCalls;
        const auto it = occupantsAtCell.find({x, z});
        if (it == occupantsAtCell.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    cSC4Lot* LotByBuilding(cISC4BuildingOccupant*) override { return lot; }
    cSC4Lot* LotAtCell(int, int) override { return lot; }
    cSC4MySim* MySimAtBuilding(cISC4BuildingOccupant*) override { return nullptr; }
    bool AnyTravelConnection(int, int) override { return true; }
    bool FindCommutePath(const int[4], const int[4], int& minutes) override {
        ++commuteCalls;
        minutes = commuteMinutes;
        return commuteResult;
    }
    float TerrainHeight(float, float) override { return terrainHeight; }
    float TerrainSeaLevel() override { return seaLevel; }
    void StandardCellToPosition(int x, int z, float& px, float& pz) override { px = static_cast<float>(x << 4); pz = static_cast<float>(z << 4); }
    int CityCellsX() override { return 128; }
    int CityCellsZ() override { return 128; }
    int ResidentialAgeForWealth(int) override { return 25; }
    int ResidentialAgeAtCell(int, int) override { return residentialAge; }
    float ResidentialEducationQuotient(int, int, int) override { return eq; }
    float ResidentialHealthQuotient(int, int, int) override { return hq; }
    int HourOfDay() override { return hour; }
    int TrafficCongestion(int, int) override { return 0; }
    bool SimulatorTickEnabled() override { return simulatorTick; }
    bool GamePaused() override { return paused; }
    void CityOccupants(std::vector<cISC4Occupant*>& out) override { out = cityOccupants; }
    void BuildingOccupantGroups(std::uint32_t, std::vector<std::uint32_t>& out) override { out = {0x1001, 0x1002}; }
    bool GetLocalSchoolGrade(std::uint32_t, float, float, int& grade) override { grade = schoolGrade; return true; }
    bool GetLocalHospitalGrade(std::uint32_t, float, float, int& grade) override { grade = hospitalGrade; return true; }
    bool CameraLookAt(const float pos[3], bool) override { cameraCalled = true; std::memcpy(cameraPosition, pos, sizeof(cameraPosition)); return true; }
    void NotificationDialog(const cIGZString& title, const cIGZString& body) override { ++dialogCalls; dialogTitle = title.ToChar(); dialogBody = body.ToChar(); }
    bool CityName(cIGZString& out) override { out.AssignFromCString("SimCity"); return true; }
    IMySimCallout* CreateCallout() override {
        auto* callout = new FakeCallout(&calloutLog);
        Keep(static_cast<cIGZUnknown*>(static_cast<IMySimCallout*>(callout)));
        return callout;
    }
    bool LoadMySimBitmap(std::uint32_t, const cIGZString*, cIGZBuffer** out) override { ++bitmapCalls; *out = nullptr; return false; }
    bool LoadExemplar(const cGZPersistResourceKey&, std::uint32_t, cIGZUnknown** out) override { *out = nullptr; return false; }
    bool PickRandomWalkModelKey(cGZPersistResourceKey& out) override { out = cGZPersistResourceKey{0x111, 0x222, 0x333}; return true; }
    IMySimEffect* CreateGhostEffect() override {
        effect = new FakeEffect();
        Keep(static_cast<cIGZUnknown*>(static_cast<IMySimEffect*>(effect)));
        return effect;
    }
    bool DispatchIconRect(int, int rect[4]) override { ++iconRects; std::memcpy(rect, iconRect, sizeof(iconRect)); return true; }
    IMySimDispatch* CreateDispatch(cSC4MySim*) override {
        ++dispatchLog.created;
        auto* dispatch = new FakeDispatch(&dispatchLog);
        Keep(static_cast<cIGZUnknown*>(static_cast<IMySimDispatch*>(dispatch)));
        return dispatch;             // new-object semantics: no reference for the caller
    }
    IMySimBalloonWinProc* CreateBalloonWinProc(cISC4MySim*) override {
        winProc = new FakeWinProc();
        Keep(static_cast<cIGZUnknown*>(static_cast<IMySimBalloonWinProc*>(winProc)));
        return winProc;
    }
    IMySimPylon* CreatePylon() override {
        pylonOccupant = new FakeOccupant();
        auto* pylon = new FakePylon(&pylonLog, pylonOccupant);
        Keep(static_cast<cIGZUnknown*>(static_cast<IMySimPylon*>(pylon)));
        return pylon;
    }
    cIGZUnknown* CurrentViewInputControl() override { return inputControl; }
    void SetCurrentViewInputControl(cIGZUnknown* control) override {
        if (control) control->AddRef();
        if (inputControl) inputControl->Release();
        inputControl = control;
    }
    cIGZUnknown* ViewCurrentInputControl() override { return viewInputControl; }
    void ViewSetInputControl(cIGZUnknown* control) override { if (control) control->AddRef(); if (viewInputControl) viewInputControl->Release(); viewInputControl = control; }
    void ViewPushInputControl(cIGZUnknown* control) override { ++viewPushes; if (control) control->AddRef(); if (viewInputControl) viewInputControl->Release(); viewInputControl = control; }
    cIGZUnknown* MakeTool() { tool = new FakeTool(); Keep(tool); return tool; }
    cIGZUnknown* CreateMoveInTool(cSC4MySim*) override { ++moveInTools; return MakeTool(); }
    cIGZUnknown* CreateDispatchToolControl(cSC4MySim*) override { ++toolCreates; return MakeTool(); }
    cIGZUnknown* CreateChangeHouseTool(cSC4MySim*) override { ++toolCreates; return MakeTool(); }
    cIGZUnknown* CreateChangeJobTool(cSC4MySim*) override { ++toolCreates; return MakeTool(); }
    void RegisterInputControlTarget(cIGZMessageTarget2*) override { ++atmRegisters; }
    void UnregisterInputControlTarget(cIGZMessageTarget2*) override { ++atmUnregisters; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { return bus.AddNotification(target, type); }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { return bus.RemoveNotification(target, type); }
    void AddToGameDataRegistry(cSC4MySim&) override { ++registryAdds; }
    void RemoveFromGameDataRegistry() override { ++registryRemoves; }
    void WriteOccupant(cIGZOStream& stream, cISC4BuildingOccupant* occupant) override { stream.SetUint32(occupant ? 1u : 0u); }
    bool ReadOccupant(cIGZIStream& stream, cISC4BuildingOccupant** out) override {
        std::uint32_t value = 0;
        stream.GetUint32(value);
        *out = value ? new FakeBuilding() : nullptr;
        if (*out) (*out)->AddRef();
        return value != 0;
    }
    bool ReadPylon(cIGZIStream&, IMySimPylon** out) override { *out = nullptr; return false; }
    void WritePylon(cIGZOStream& stream, IMySimPylon*) override { stream.SetUint32(1); }
    void WriteDispatch(cIGZOStream& stream, IMySimDispatch* dispatch) override { stream.SetUint32(dispatch ? 1u : 0u); }
    bool ReadDispatch(cIGZIStream& stream, IMySimDispatch** out) override {
        std::uint32_t value = 0;
        stream.GetUint32(value);
        *out = nullptr;
        if (value) {
            auto* dispatch = new FakeDispatch(&dispatchLog);
            dispatch->AddRef();
            *out = dispatch;
        }
        return value != 0;
    }
    cISC4AdviceSubject* CreateLuaSubject(cISC4Occupant* occupant, bool) override {
        auto* subject = new cSC4AdviceSubject();
        subject->AddRef();
        Keep(static_cast<cIGZUnknown*>(static_cast<cISC4AdviceSubject*>(subject)));
        if (occupant) subject->SetObject(static_cast<cIGZUnknown*>(occupant));
        return subject;
    }
    cIGZUnknown* Messages2() { return nullptr; }
    void PostMessage2(std::uint32_t type, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::uint32_t d4, cIGZUnknown*) override {
        posts.emplace_back(type, d1, d2, d3, d4);
    }
    cIGZMessageServer2* Messages() override { return &bus; }
    IMySimHomeSound* CreateHomeSound(std::uint32_t) override {
        ++homeSounds;
        homeSound = new FakeHomeSound();
        Keep(static_cast<cIGZUnknown*>(static_cast<IMySimHomeSound*>(homeSound)));
        return homeSound;
    }
    void DestroyHomeSound(IMySimHomeSound* sound) override { ++homeSoundDestroyed; if (sound) sound->Release(); }
    void PlayCitySound(IMySimHomeSound* sound, const float* pos) override { ++citySounds; if (sound) sound->SetPosition(pos); }
    bool CalloutImageAvailable() override { return true; }
    void ProjectPointToScreen(float xyz[3]) override { xyz[0] += 1.0f; xyz[1] += 2.0f; }
    void ReportSimEvent(const cIGZString& text, const float[3]) override { reported = text.ToChar(); }

    int day = 0;
    int atmRegisters = 0, atmUnregisters = 0;
    int adviceRegisters = 0, adviceUnregisters = 0;
    std::string reported;
    FakeEffect* effect = nullptr;
    FakeWinProc* winProc = nullptr;
    FakeOccupant* pylonOccupant = nullptr;
    FakeHomeSound* homeSound = nullptr;
    struct FakeTool final : cIGZUnknown {
        int refs = 0;
        bool QueryInterface(std::uint32_t iid, void** out) override { if (iid != kGZIID_cIGZUnknown) return false; *out = this; AddRef(); return true; }
        std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
        std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    }* tool = nullptr;
};

// The advisor base's host methods are not virtual-compatible with IMySimHost (Lua returns different types), so the
// advisor side gets its own small adapter object instead.
struct AdvisorHostAdapter final : IAdvisorHost {
    FakeHost* host;
    explicit AdvisorHostAdapter(FakeHost* h) : host(h) {}
    IScriptLua* Lua() override { return &host->adapter; }
    void RegisterAdvice(int, cISC4Advisor*) override { ++host->adviceRegisters; }
    void UnregisterAdvice(int, cISC4Advisor*) override { ++host->adviceUnregisters; }
    cIGZMessageServer2* Messages() override { return &host->bus; }
    bool CreateInstance(std::uint32_t, std::uint32_t, void** out) override { *out = nullptr; return false; }
    int SimDayNumber() override { return host->day; }
    void PostMessage2(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, cIGZUnknown*) override {}
};

// cSC4AdviceSubject needs the city size for SetLocation's clamp.
struct SubjectHost final : IAdviceSubjectHost {
    std::int32_t CityCellsX() override { return 128; }
    std::int32_t CityCellsZ() override { return 128; }
};


// A fake cISC4Advice (the interface the balloon advice exposes through AsISC4Advice) and the balloon advice itself.
struct FakeAdvice final : cISC4Advice {
    int refs = 0;
    std::uint32_t flags = 0;
    int frequency = 30, timeout = 30, guid = 1, advisorId = 0, sessionId = 0, eventId = 0;
    cRZString trigger{"1"}, title{"T"}, message{"M"};
    cISC4AdviceSubject* subject = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4Advice) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<cISC4Advice*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    bool Clone(cISC4Advice** out) override { *out = nullptr; return false; }
    int GetMood() const override { return 3; }
    int GetPriority() const override { return 50; }
    bool GetTrigger() const override { return (flags & 0x400) != 0; }
    cIGZString& GetTriggerString() override { return trigger; }
    int GetGUID() const override { return guid; }
    int GetAdvisorID() const override { return advisorId; }
    void SetAdvisorID(int id) override { advisorId = id; }
    int GetSessionID() const override { return sessionId; }
    bool SetFlags(int value, bool on) override { if (on) flags |= static_cast<std::uint32_t>(value); else flags &= ~static_cast<std::uint32_t>(value); return true; }
    bool CheckFlags(int value) const override { return (flags & static_cast<std::uint32_t>(value)) != 0; }
    int GetFrequencyInDays() const override { return frequency; }
    int GetTimeOutInDays() const override { return timeout; }
    const cIGZString& GetTextTitle() const override { return title; }
    std::uint32_t GetTextTitleGUID() const override { return 0; }
    const cIGZString& GetTextMessage() const override { return message; }
    std::uint32_t GetTextMessageGUID() const override { return 0; }
    bool DoLink(const cIGZString&) override { return true; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(cISC4AdviceSubject* value) override { subject = value; return true; }
    cISC4AdviceSubject* GetEventSubject() override { return subject; }
    int GetEventID() override { return eventId; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return false; }
};

struct FakeBalloonAdvice final : cISC4MySimBalloonAdvice {
    int refs = 0;
    FakeAdvice advice;
    std::uint32_t flags = 0;
    bool trigger = true;
    cRZString text{"balloon"};
    cRZString sound{"sound"};
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4MySimBalloonAdvice) return false;
        *out = static_cast<cIGZUnknown*>(static_cast<cISC4MySimBalloonAdvice*>(this));
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { const int r = --refs; if (r <= 0) delete this; return static_cast<std::uint32_t>(r < 0 ? 0 : r); }
    cISC4Advice* AsISC4Advice() override { return &advice; }
    const cISC4Advice* AsISC4AdviceConst() const override { return const_cast<FakeAdvice*>(&advice); }
    const cIGZString& GetTextBalloon() const override { return text; }
    int GetTextBalloonID() const override { return 0; }
    int GetIconBalloonCount() const override { return 0; }
    cIGZBuffer* GetIconBalloon(int) const override { return nullptr; }
    std::uint32_t GetIconBalloonID(int) const override { return 0; }
    int GetSoundID(bool) const override { return 0; }
    int GetSoundChance() const override { return 100; }
    bool GetRawTrigger() override { return trigger; }
    void SetStatusFlags(int value, bool on) override { if (on) flags |= static_cast<std::uint32_t>(value); else flags &= ~static_cast<std::uint32_t>(value); }
    bool CheckStatusFlags(int value) const override { return (flags & static_cast<std::uint32_t>(value)) != 0; }
};

static void BuildAdvisorTables(FakeLua& lua) {
    auto advisors = std::make_shared<LuaTable>();
    LuaValue row = LuaValue::Tbl(std::make_shared<LuaTable>());
    row.table->fields["id"] = LuaValue::Num(42);
    row.table->fields["caption"] = LuaValue::Str("Mayor|The Mayor");
    row.table->fields["name"] = LuaValue::Str("Bob");
    row.table->fields["advice_type"] = LuaValue::Num(3);
    advisors->array.push_back(row);
    lua.globals["advisors"] = LuaValue::Tbl(advisors);
    lua.globals["advices"] = LuaValue::Tbl(std::make_shared<LuaTable>());
}

int main() {
    SubjectHost subjectHost;
    SetAdviceSubjectHost(&subjectHost);
    FakeHost host;
    AdvisorHostAdapter advisorHost(&host);
    SetAdvisorHost(&advisorHost);
    BuildAdvisorTables(host.lua);

    // ---- construction and interface views ----
    auto* sim = new cSC4MySim();
    sim->AddRef();
    CHECK(sim->GetGZCLSID() == kGZCLSID_cSC4MySim);
    CHECK(sim->State() == 0);
    CHECK(sim->GetSlot() == 0);
    CHECK(sim->GetAge() == 0);
    CHECK(sim->GetSign() == 0);
    CHECK(sim->IsMale());
    CHECK(!sim->IsActive());
    CHECK(sim->GetWealth() == 0);
    CHECK(host.dispatchLog.created == 1);
    CHECK(sim->Dispatch() != nullptr);
    {
        void* out = nullptr;
        CHECK(sim->QueryInterface(kGZIID_cISC4MySim, &out));
        // the primary interface is the view the dispatch keeps as its owner (a dispatch read from a save gets it this way)
        CHECK(out == static_cast<IMySimDispatchOwner*>(sim));
        if (out == static_cast<IMySimDispatchOwner*>(sim)) {
            auto* owner = static_cast<IMySimDispatchOwner*>(out);
            CHECK(owner->IsMale() == sim->IsMale());
            CHECK(owner->GetWealth() == sim->GetWealth());
            owner->Release();
        }
        out = nullptr;
        CHECK(sim->QueryInterface(kGZIID_cISC4MySimLink, &out));
        CHECK(out == static_cast<cISC4MySim*>(sim));
        out = nullptr;
        CHECK(sim->QueryInterface(kGZIID_cISC4Advisor, &out));
        CHECK(out == static_cast<cISC4Advisor*>(sim));
        out = reinterpret_cast<void*>(0x1234);
        CHECK(!sim->QueryInterface(0xdeadbeef, &out));
        CHECK(out == nullptr);
        CHECK(static_cast<cISC4MySim*>(sim) != nullptr);
        CHECK(sim->AsAdvisor() == static_cast<cISC4Advisor*>(sim));
        const std::ptrdiff_t linkOffset = reinterpret_cast<char*>(static_cast<cISC4MySim*>(sim)) - reinterpret_cast<char*>(sim);
        CHECK(linkOffset == static_cast<std::ptrdiff_t>(sizeof(cSC4Advisor)));
    }

    // ---- Init ----
    CHECK(sim->Init(1));
    CHECK(sim->TableRow() == 1);
    CHECK(sim->GetID() == 42);
    // No SL string resource loads here (the advisor host has no resource manager), so LoadScriptString keeps the raw
    // script text: the "name" column of the advisors row ("Bob") is the generation-1 personal name.
    CHECK(std::strcmp(sim->GetPersonalName().ToChar(), "Bob") == 0);
    CHECK(host.winProc != nullptr);
    // The inactive sim created near the end of the test gets its own balloon window proc; keep this sim's.
    FakeWinProc* const simWinProc = host.winProc;
    CHECK(host.winProc->inits == 1);
    CHECK(host.winProc->hidden == 300);
    CHECK(host.winProc->shown == 0x5dc);
    CHECK(host.winProc->fade == 300);
    CHECK(host.dispatchLog.tuning[0] == 10);
    CHECK(host.dispatchLog.tuning[1] == 10);
    CHECK(host.dispatchLog.tuning[2] == 1);
    CHECK(host.dispatchLog.tuning[3] == 1);
    CHECK(host.dispatchLog.tuning[4] == 1);
    CHECK(host.registryAdds == 1);
    CHECK(host.atmRegisters == 1);
    CHECK(!host.bus.added.empty());
    // The base's personal name and the "no job" strings.
    CHECK(std::strcmp(sim->GetJobName().ToChar(), "str:aa665d3e") == 0);
    CHECK(std::strcmp(sim->GetJobTitle().ToChar(), "str:aa665d3e") == 0);
    CHECK(std::strcmp(sim->GetHomeName().ToChar(), "") == 0);
    CHECK(sim->GetVehicleKey() == cGZPersistResourceKey{});

    // ---- MoveIn / accessors ----
    {
        sim->SetStateForTest(2);
        cRZString name("Alice"), bio("bio text"), file("mysim.png");
        sim->MoveIn(3, name, false, 5, bio, 0, file);
    }
    CHECK(sim->GetSlot() == 3);
    CHECK(std::strcmp(sim->GetPersonalName().ToChar(), "Alice") == 0);
    CHECK(!sim->IsMale());
    CHECK(sim->GetSign() == 5);
    CHECK(std::strcmp(sim->GetBio().ToChar(), "bio text") == 0);
    CHECK(std::strcmp(sim->GetBitmapFile().ToChar(), "mysim.png") == 0);
    CHECK(sim->GetBitmapID() == 0);
    CHECK(host.moveInTools == 1);
    CHECK(host.viewPushes == 1);
    CHECK(host.dispatchLog.startTracking == 1);
    CHECK(sim->BalloonsEnabled());
    {
        cRZString text;
        CHECK(sim->GetEQAsText(text));
        CHECK(std::strcmp(text.ToChar(), "0") == 0);
        sim->SetStateForTest(0);
        CHECK(!sim->GetHQAsText(text));                                  // no health text while the sim is out of the city
        sim->SetStateForTest(2);
        CHECK(sim->GetHQAsText(text));
        CHECK(std::strcmp(text.ToChar(), "str:0a5fe2df") == 0);          // hq 0 -> the low text
    }
    // GetModelButtonBitmapID picks the pedestrian id when asked for it.
    CHECK(sim->GetModelButtonBitmapID(false) == 0);
    CHECK(sim->GetModelButtonBitmapID(true) == 0);

    // ---- SetResidence starts the sim ----
    host.lot = new cSC4Lot();
    host.lot->AddRef();
    auto* residence = new FakeBuilding();
    residence->AddRef();
    residence->profile.purpose = 5;
    residence->profile.wealth = 1;
    residence->position[0] = 100.0f;
    residence->position[2] = 200.0f;
    host.residenceBuilding = residence;
    sim->SetStateForTest(0);
    sim->SetResidence(residence);
    CHECK(sim->GetResidence() == residence);
    CHECK(sim->State() == 1);
    CHECK(sim->GetAge() == 30);                                          // host's residential age (> 0x12)
    CHECK(sim->GetWealth() == 0);                                        // lot wealth 0
    CHECK(host.dispatchLog.init == 1);
    CHECK(host.dispatchLog.teleports == 1);                              // DispatchCommute from state 1
    CHECK(host.dispatchLog.followCamera);
    CHECK(host.dispatchLog.modelKeys.size() >= 2);                       // SetCar(nullptr)/SetPedestrian(nullptr)
    CHECK(!host.bus.sent.empty());
    CHECK(host.bus.sent.back().type == kMessageMySimsChanged);

    // ---- work / home transitions ----
    sim->LeaveForWork();
    CHECK(sim->State() == 4);
    CHECK(sim->get_track_handle() == 0xbeef);
    sim->ReturnHome();
    CHECK(sim->State() == 5);
    sim->simulate_day();
    CHECK(host.dispatchLog.stopTracking == 0);
    // hidden pylon timer path is inert without a pylon

    // ---- simulate_month ages on the birth month and rotates the histories ----
    {
        const int month = sim->Month();
        sim->SetStateForTest(5);
        sim->simulate_month();
        CHECK(sim->GetAge() == 31);                                      // month 0 == birthMonth 0
        (void)month;
    }

    // ---- balloon queues ----
    {
        sim->Balloons().clear();
        auto* advice = new FakeBalloonAdvice();
        advice->AddRef();          // the test's own reference
        MySimBalloonInfo info;
        info.advice = advice;
        info.nextDay = host.day;
        sim->Balloons().push_back(info);
        sim->BalloonQueue().clear();
        sim->EventQueue().clear();
        sim->push_balloon_queue(2, 0);
        CHECK(sim->BalloonQueue().size() == 1);
        CHECK(sim->BalloonQueue().front() == 0);
        CHECK(advice->flags & 1);                                        // shown
        CHECK(sim->GetCurrentBalloonAdvice() == advice);
        CHECK(sim->GetNextBalloonAdvice() == advice);
        // get_next's initial pop_balloon_queue(4) only drops a balloon with status bit 4 (blocked), not a shown one.
        CHECK(sim->BalloonQueue().size() == 1);
        sim->simulate_balloons(true);
        // Still queued (its "shown" bit 0 is set), so simulate_balloons does not queue it a second time.
        CHECK(sim->BalloonQueue().size() == 1);
        sim->empty_all_balloon_queues();
        CHECK(sim->BalloonQueue().empty());
        sim->push_balloon_queue(1, 0);       // queue it the way the sim does; pop needs a status bit set
        CHECK(sim->GetCurrentBalloonEventAdvice() == advice);
        sim->empty_all_balloon_queues();
        CHECK(sim->EventQueue().empty());
        sim->Balloons().clear();
        advice->Release();
    }

    // ---- DoMessage: ignored / changed / hide-all / input / tool ----
    {
        auto make = [](std::uint32_t type, std::uint64_t data1, cIGZUnknown* unknown) {
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(type);
            if (data1 != 0) message->SetData1(data1);
            if (unknown) message->SetIGZUnknown(unknown);
            return message;
        };
        cRZMessage2Standard* message = make(kMessageMySimChanged, 0, nullptr);
        CHECK(sim->DoMessage(message));
        message->Release();
        // The changed message with this interface refreshes the quick variables and the balloons.
        message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMessageMySimChanged);
        message->SetIGZUnknown(static_cast<cIGZUnknown*>(static_cast<cISC4MySim*>(sim)));
        CHECK(sim->DoMessage(message));
        CHECK(host.lua.calls > 0);
        message->Release();
        // Hide all.
        message = make(kMessageMySimHideAll, 0, nullptr);
        CHECK(sim->DoMessage(message));
        message->Release();
        CHECK(!sim->BalloonsEnabled());
        // Cancel input for this advisor id (42).
        message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMessageMySimInput2);
        message->SetData1(42);
        CHECK(sim->DoMessage(message));
        message->Release();
        // Dispatch tool message creates the control.
        const int before = host.toolCreates;
        message = make(kMessageMySimToolMessage, 0, nullptr);
        CHECK(sim->DoMessage(message));
        message->Release();
        CHECK(host.toolCreates == before + 1);
        // Unknown messages go to the advisor base and answer true.
        message = make(0xdeadbeef, 0, nullptr);
        CHECK(sim->DoMessage(message));
        message->Release();
    }

    // ---- dispatch notifications ----
    {
        sim->SetStateForTest(2);
        auto notify = [&](std::uint32_t event, std::uint64_t data2, std::uint64_t data3) {
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(kMessageMySimDispatchNotify);
            message->SetData1(event);
            if (data2) message->SetData2(data2);
            if (data3) message->SetData3(data3);
            CHECK(sim->DoMessage(message));
            message->Release();
        };
        // idle offset -> SetModelOffset
        host.dispatchLog.modelOffsets.clear();
        notify(kDispatchNotifyIdleOffset, 0, 0);
        CHECK(host.dispatchLog.modelOffsets.size() == 1);
        // The sim is at its default destination (the residence the fake dispatch stored) -> state 2.
        sim->SetStateForTest(5);
        host.dispatchLog.current = host.dispatchLog.defaultDestination;
        notify(kDispatchNotifyLocation, 0, 0);
        CHECK(sim->State() == 2);
        // Trip ended at the commute destination -> state 4.
        host.dispatchLog.current = MySimLocation();
        host.dispatchLog.destination = MySimLocation();
        host.dispatchLog.commuteDestination = MySimLocation();
        host.dispatchLog.defaultDestination = MySimLocation();
        host.dispatchLog.destination.hasOccupant = false;
        sim->SetStateForTest(0);
        notify(kDispatchNotifyNeighbor, 0, 0);
        CHECK(sim->State() == 6);                                        // neither commute nor default
        notify(kDispatchNotifyTripEnd, 3, 4);
        notify(kDispatchNotifyTripLeg, 4, 3);
        // Arrived at a commute destination that has an occupant but no lot -> cleared.
        host.dispatchLog.commuteDestination = MySimLocation();
        notify(kDispatchNotifyArrived, 0, 0);
        // Arrived at a neighbour connection: the occupant at the cell becomes the neighbour job.
        auto* neighbour = new FakeBuilding();
        neighbour->AddRef();
        host.occupantsAtCell[{7, 8}] = neighbour;
        host.dispatchLog.commuteDestination = MySimLocation();
        host.dispatchLog.commuteDestination.hasOccupant = false;
        host.dispatchLog.commuteDestination.lotCellX = 7;
        host.dispatchLog.commuteDestination.lotCellZ = 8;
        host.dispatchLog.commuteDestination.cellX = 9;
        host.dispatchLog.commuteDestination.cellZ = 10;
        notify(kDispatchNotifyArrived, 0, 0);
    }

    // ---- AnnounceMoveIn: the moved-in message and the "moved in" event the tail call pushes ----
    {
        sim->SetLastSoundTimeForTest(-1000);                             // let RequestToPlaySound pass
        const std::size_t sent = host.bus.sent.size();
        const std::size_t events = sim->Events().size();
        sim->AnnounceMoveIn();
        CHECK(host.bus.sent.size() == sent + 1);
        CHECK(host.bus.sent.back().type == kMessageMySimMovedIn);
        CHECK(sim->Events().size() == events + 1);
        CHECK(sim->Events().back().first == static_cast<int>(kEventMySimMovedIn));
    }

    // ---- dispatch tool ----
    {
        sim->SetStateForTest(2);
        void* pylon = sim->DispatchToolBegin();
        CHECK(pylon != nullptr);
        CHECK(host.pylonLog.inits == 1);
        CHECK(host.pylonLog.kinds >= 1);
        CHECK(host.pylonLog.agents == 1);
        CHECK(host.pylonLog.bitmaps == 1);
        CHECK(sim->DispatchToolGetPylon() != nullptr);
        host.dispatchLog.goToResult = true;
        CHECK(sim->DispatchToolLockPylon(true));
        CHECK(sim->DispatchToolIsPylonLocked());
        CHECK(host.dispatchLog.goTo == 1);
        CHECK(sim->State() == 6);
        CHECK(host.pylonLog.lastKind == 3);
        CHECK(sim->DispatchToolCanDispatch(10.0f, 20.0f));
        host.seaLevel = 12.5f;                       // equal is not above
        CHECK(!sim->DispatchToolCanDispatch(10.0f, 20.0f));
        host.seaLevel = 10.0f;
        sim->SetPylonVisibility(true);
        CHECK(host.pylonLog.visibility >= 1);
        host.dispatchLog.state = 2;
        host.dispatchLog.destination = MySimLocation();
        host.dispatchLog.destination.hasOccupant = false;
        // The pylon is locked, so ending the tool keeps it (the drop only happens for an unlocked pylon).
        CHECK(sim->DispatchToolEnd());
        CHECK(host.pylonLog.shutdowns == 0);
        CHECK(sim->DispatchToolGetPylon() != nullptr);
        // Unlock, start the tool again (it reuses the pylon) and end it at the empty destination: now it drops it.
        CHECK(sim->DispatchToolBegin() != nullptr);
        CHECK(sim->DispatchToolLockPylon(false));
        CHECK(sim->DispatchToolEnd());
        CHECK(host.pylonLog.shutdowns >= 1);
        CHECK(sim->DispatchToolGetPylon() == nullptr);
        sim->DispatchToolSetFollowCamera(true);
        CHECK(host.dispatchLog.followCamera);
    }

    // ---- window paths ----
    {
        sim->SetStateForTest(2);
        host.calloutLog = CalloutLog{};
        sim->ShowResidence();
        CHECK(host.calloutLog.styles == 1);
        CHECK(host.calloutLog.images == 1);
        CHECK(host.calloutLog.addToView == 1);
        CHECK(host.calloutLog.shows == 1);
        CHECK(host.calloutLog.activates == 1);
        CHECK(host.calloutLog.positions >= 1);
        CHECK(host.calloutLog.texts == 1);
        sim->HideResidence();
        CHECK(host.calloutLog.hides == 1);
        int sx = 0, sy = 0;
        host.iconRect[0] = 10; host.iconRect[1] = 20; host.iconRect[2] = 30; host.iconRect[3] = 40;
        CHECK(sim->GetCurrentScreenLocationXY(&sx, &sy));
        CHECK(sx == 20);
        CHECK(sy == 20 + 0x19);
        host.dispatchLog.current = MySimLocation();
        host.dispatchLog.current.x = 100.0f;
        host.dispatchLog.current.z = 200.0f;
        host.dispatchLog.current.cellX = 6;
        host.dispatchLog.current.cellZ = 12;
        sim->Find();
        CHECK(host.cameraCalled);
        CHECK(host.cameraPosition[0] == 100.0f);                         // the residence position
        CHECK(host.dispatchLog.followCamera);
        float xyz[3] = {};
        CHECK(sim->GetLocation(xyz));
        CHECK(xyz[2] == 200.0f);                                         // residence position
        CHECK(xyz[1] == host.terrainHeight);

    }

    // ---- distances and grades ----
    {
        host.nearby.clear();
        auto* near = new FakeBuilding();
        near->AddRef();
        near->position[0] = 130.0f;
        near->position[2] = 240.0f;
        host.nearby.push_back(near);
        sim->UpdateLocalBuildings();
        const int distance = sim->DistanceToClosest(0x1001);
        CHECK(distance == 50);                                           // sqrt(30^2 + 40^2)
        CHECK(sim->DistanceToClosest(0x9999) == -1);
        std::vector<cISC4Occupant*> city;
        auto* special = new FakeBuilding();
        special->AddRef();
        special->position[0] = 100.0f;
        special->position[2] = 200.0f;
        host.cityOccupants.push_back(special);
        CHECK(sim->DistanceToClosestSpecial(0x1001) == 0);
        CHECK(sim->DistanceToClosestSpecial(0x2222) == 0xffffffffu);
        CHECK(sim->LocalSchoolGrade(0x4149) == host.schoolGrade);
        CHECK(sim->LocalHospitalGrade(0x4149) == host.hospitalGrade);
    }

    // ---- script commands ----
    {
        sim->SetStateForTest(2);
        sim->DoScriptCommand(static_cast<int>(kScriptMySimIncreaseWealth));
        CHECK(sim->GetWealth() == 1);
        sim->DoScriptCommand(static_cast<int>(kScriptMySimRetire));
        CHECK(sim->Retired());
        CHECK(sim->State() == 2);
        sim->DoScriptCommand(static_cast<int>(kScriptMySimSpawn));
        CHECK(!sim->Retired());
        CHECK(sim->GetAge() == 25);                                      // host's ResidentialAgeForWealth
        CHECK(host.effect != nullptr);
        CHECK(host.effect->transforms == 1);
        sim->DoScriptCommand(static_cast<int>(kScriptMySimQuitJob));
        sim->DoScriptCommand(static_cast<int>(kScriptMySimLeaveCity));
        CHECK(sim->State() == 0);
    }

    // ---- sounds ----
    {
        sim->SetStateForTest(2);
        sim->SetLastSoundTimeForTest(-1000);
        sim->PlayNightSound();
        CHECK(host.homeSounds == 1);                                     // RequestToPlaySound passed
        CHECK(host.citySounds == 1);
        sim->RemoveHomeSound();
        CHECK(host.homeSoundDestroyed == 0);                             // IsPlaying false -> no destroy call
        // RequestToPlaySound is time limited: a second call within 5 units fails.
        CHECK(!sim->RequestToPlaySound());
    }

    // ---- SetCar / SetPedestrian defaults through the seam ----
    {
        CHECK(sim->SetCar(nullptr));
        CHECK(sim->SetPedestrian(nullptr));
        CHECK(sim->GetModelButtonBitmapID(false) == 0);
        CHECK(sim->GetModelButtonBitmapID(true) == 0);
        // The pedestrian fallback key is the random walk key when the exemplar has no model key.
        bool foundWalk = false;
        for (const cGZPersistResourceKey& key : host.dispatchLog.modelKeys) {
            if (key.type == 0x111 && key.group == 0x222 && key.instance == 0x333) foundWalk = true;
        }
        CHECK(foundWalk);
    }

    // ---- GetBitmapBuffer ----
    {
        cIGZBuffer* buffer = nullptr;
        CHECK(!sim->GetBitmapBuffer(&buffer));
        CHECK(host.bitmapCalls >= 1);
        CHECK(buffer == nullptr);
    }

    // ---- Write/Read of an inactive sim (the record's advisor part only) ----
    {
        cSC4MySim* fresh = new cSC4MySim();
        fresh->AddRef();
        CHECK(fresh->Init(1));
        PortMemoryOStream out;
        CHECK(fresh->Write(out));
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(fresh->Read(in));
        CHECK(fresh->State() == 0);
        fresh->Release();
    }

    // ---- Shutdown ----
    CHECK(host.registryAdds == 2);
    CHECK(sim->Shutdown());
    CHECK(host.registryRemoves == 1);
    CHECK(host.atmUnregisters == 1);
    CHECK(simWinProc->shutdowns == 1);
    CHECK(sim->State() == 0);
    sim->Release();

    SetAdvisorHost(nullptr);
    SetMySimHost(nullptr);
    SetAdviceSubjectHost(nullptr);
    if (failures == 0) std::printf("sc4_mysim_test: ok\n");
    return failures == 0 ? 0 : 1;
}
