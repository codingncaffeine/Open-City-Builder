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
#include "sc4/MySimDispatch.h"
#include "sc4/MyStreetSim.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControl.h"
#include "sc4/SLString.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

// The street sim reads the advisor system's random generator; AdvisorSystem.cpp is not linked, so no system runs here.
namespace sc4 {
cSC4AdvisorSystem* cSC4AdvisorSystem::instance_ = nullptr;
cRZRandom* cSC4AdvisorSystem::GetRandom() { return nullptr; }
}

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
    // as the port's Lua 5 binding: a tagged pointer is boxed, ToUserData gives the box and ToUserTag the pointer in it
    void* ToUserData(int index) override { return (!IsNil(index) && At(index).kind == LuaValue::UserData) ? static_cast<void*>(&At(index).user) : nullptr; }
    void* ToUserTag(int index) override { return (!IsNil(index) && At(index).kind == LuaValue::UserData) ? At(index).user : nullptr; }
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
    std::uintptr_t ToDataRegistryEntryID(cISC4GameDataRegistry*, int) override { return 0; }
    cIGZUnknown* ToDataRegistryNode(cISC4GameDataRegistry*, int) override { return nullptr; }
    void* ToDataRegistryValue(cISC4GameDataRegistry*, int) override { return nullptr; }
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
struct FakeOccupant final : sc4::cISC4Occupant {
    int refs = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    SC4CellRect cells{0, 0, 0, 0};
    std::uint32_t type = kOccupantDefaultType;
    std::uint32_t group = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4Occupant || iid == kGZIID_cIGZUnknown) {
            *out = static_cast<cIGZUnknown*>(static_cast<sc4::cISC4Occupant*>(this));
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
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
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
    bool IsOccupantGroup(std::uint32_t g) const override { return group != 0 && g == group; }
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

struct FakeBuilding final : cISC4BuildingOccupant, sc4::cISC4Occupant {
    int refs = 0;
    BuildingProfile profile;
    cRZString name{"Fake Building"};
    cRZString exemplar{"fakeexemplar"};
    float position[3] = {0.0f, 0.0f, 0.0f};
    SC4CellRect cells{1, 2, 3, 4};
    float completion = 1.0f;
    bool lit = false;
    std::uint32_t group = 0;   // the one occupant group IsOccupantGroup answers
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4BuildingOccupant) {
            *out = static_cast<cIGZUnknown*>(static_cast<cISC4BuildingOccupant*>(this));
            AddRef();
            return true;
        }
        if (iid == kGZIID_cISC4Occupant) {
            *out = static_cast<cIGZUnknown*>(static_cast<sc4::cISC4Occupant*>(this));
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
    sc4::cISC4Occupant* AsOccupant() override { return static_cast<sc4::cISC4Occupant*>(this); }
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
    // sc4::cISC4Occupant
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
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
    bool IsOccupantGroup(std::uint32_t g) const override { return group != 0 && g == group; }
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
    void SetLinkedObject(sc4::cISC4Occupant*) override { ++log->linked; }
    sc4::cISC4Occupant* GetLinkedOccupant() override { return occupant; }
    void SetLinkedAgent(cIGZUnknown*) override { ++log->agents; }
    bool IsDead() const override { return false; }
    sc4::cISC4Occupant* AsOccupant() override { return occupant; }
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
    std::map<std::pair<int, int>, sc4::cISC4Occupant*> occupantsAtCell;
    std::vector<sc4::cISC4Occupant*> cityOccupants;
    float terrainHeight = 12.5f;
    float seaLevel = 10.0f;
    bool paused = false;
    bool simulatorTick = true;
    int hour = 10;
    int residentialAge = 30;
    float eq = 60.0f, hq = 70.0f;
    std::vector<sc4::cISC4Occupant*> schools, hospitals;
    std::map<sc4::cISC4Occupant*, float> schoolGrades, hospitalGrades;
    int lifeExpectancy = 71, crime = 33, policeCoverage = 44, fireCoverage = 45, desirability = -12;
    bool powered = true, watered = false;
    int crimeCell[2] = {-1, -1}, coverageRect[4] = {0, 0, 0, 0};
    std::uint32_t desirabilityType = 0;
    int fitWidth = 0, congestionCalls = 0;
    std::vector<Station> policeStations, fireStations;
    bool cameraCalled = false;
    float cameraPosition[3] = {};
    std::string dialogTitle, dialogBody;
    int dialogCalls = 0;
    int registryAdds = 0, registryRemoves = 0;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> posts;
    std::vector<cIGZUnknown*> postedObjects;
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
        for (sc4::cISC4Occupant* o : cityOccupants) o->Release();
        for (sc4::cISC4Occupant* o : schools) o->Release();
        for (sc4::cISC4Occupant* o : hospitals) o->Release();
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
    sc4::cISC4Occupant* OccupantAtCell(int x, int z) override {
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
    int TrafficCongestion(int x, int z) override { ++congestionCalls; return (x + 2 * z) & 0xff; }
    bool SimulatorTickEnabled() override { return simulatorTick; }
    bool GamePaused() override { return paused; }
    void SpecialBuildings(std::vector<sc4::cISC4Occupant*>& out) override { out = cityOccupants; }
    void BuildingOccupantGroups(std::uint32_t, std::vector<std::uint32_t>& out) override { out = {0x1001, 0x1002}; }
    void ResidentialServiceBuildings(bool hospital, std::vector<sc4::cISC4Occupant*>& out) override { out = hospital ? hospitals : schools; }
    bool ResidentialServiceGrade(bool hospital, sc4::cISC4Occupant* building, float& grade) override {
        const auto& grades = hospital ? hospitalGrades : schoolGrades;
        const auto it = grades.find(building);
        if (it == grades.end()) return false;
        grade = it->second;
        return true;
    }
    int ResidentialLifeExpectancy(int, int) override { return lifeExpectancy; }
    std::uint8_t BuildingCondition(cISC4BuildingOccupant* building) override { return building == jobBuilding ? 3 : 2; }
    bool OccupantPowered(sc4::cISC4Occupant*) override { return powered; }
    bool BuildingWatered(sc4::cISC4Occupant*) override { return watered; }
    int CrimeAtCell(int x, int z) override { crimeCell[0] = x; crimeCell[1] = z; return crime; }
    int ProtectionCoverage(bool police, int x, int z, int width, int height) override {
        coverageRect[0] = x; coverageRect[1] = z; coverageRect[2] = width; coverageRect[3] = height;
        return police ? policeCoverage : fireCoverage;
    }
    int PollutionAtCell(int type, int, int) override { return -5 + type; }
    bool DesirabilityAtCell(std::uint32_t type, int, int, int& value) override { desirabilityType = type; value = desirability; return true; }
    void ProtectionStations(bool police, std::vector<Station>& out) override { out = police ? policeStations : fireStations; }
    // the trip surroundings: each grid answers its own value; the last sampled position and rect are kept
    bool tripSimulators = true;
    std::map<int, double> tripGrids;
    float tripPosition[2] = {0.0f, 0.0f};
    int tripRect[4] = {0, 0, 0, 0};
    std::vector<TripLot> tripLots;
    std::vector<TripDisaster> tripDisasters;
    float landmarkFunding = 0.0f;
    std::uint32_t fundingPurpose = 0;
    float travelRatio = 1.0f;
    int travelRatioType = -1;
    bool TripSimulatorsPresent() override { return tripSimulators; }
    IMySimDispatchVehicleView* streetView = nullptr;
    IMySimDispatchVehicleView* DispatchVehicleView() override { return streetView; }
    double TripGridValue(TripGrid grid, float x, float z) override {
        tripPosition[0] = x; tripPosition[1] = z;
        const auto it = tripGrids.find(static_cast<int>(grid));
        return it == tripGrids.end() ? 0.0 : it->second;
    }
    float TravelTimeRatio(int, int, int type) override { travelRatioType = type; return travelRatio; }
    void LotsInCellRect(int x0, int z0, int x1, int z1, std::vector<TripLot>& out) override {
        tripRect[0] = x0; tripRect[1] = z0; tripRect[2] = x1; tripRect[3] = z1;
        out = tripLots;
    }
    float BuildingLocalFunding(cISC4BuildingOccupant*, std::uint32_t purpose) override { fundingPurpose = purpose; return landmarkFunding; }
    void DisasterLocations(std::vector<TripDisaster>& out) override { out = tripDisasters; }
    bool NeighborJobCityName(cIGZUnknown*, cIGZString& out) override { out.AssignFromCString("Neighborville"); return true; }
    void FitTextToWidth(cIGZString& text, int width) override {
        fitWidth = width;
        if (text.Strlen() > 8) { cRZString whole(text); cRZString cut(whole, 0, 8); text.Copy(cut); }
    }
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
    cISC4AdviceSubject* CreateLuaSubject(sc4::cISC4Occupant* occupant, bool) override {
        auto* subject = new cSC4AdviceSubject();
        subject->AddRef();
        Keep(static_cast<cIGZUnknown*>(static_cast<cISC4AdviceSubject*>(subject)));
        if (occupant) subject->SetObject(static_cast<cIGZUnknown*>(occupant));
        return subject;
    }
    cIGZUnknown* Messages2() { return nullptr; }
    void PostMessage2(std::uint32_t type, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::uint32_t d4, cIGZUnknown* unknown) override {
        posts.emplace_back(type, d1, d2, d3, d4);
        postedObjects.push_back(unknown);
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


// ---- the street-sim tool's surroundings: the window manager the control base asks for, the view and the tool host ----
struct ToolWinMgr final : cIGZWinMgr {
    struct Dialog {
        std::uint32_t code = 0;
        cIGZWin* window = nullptr;
        std::int64_t result = 0;
    };
    std::vector<cIGZWin*> popups;
    std::vector<Dialog> dialogs;
    std::vector<cIGZWin*> destroyed;
    cIGZWin* focus = nullptr;
    cIGZWin* mainWindow = nullptr;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZWin* GetMainWindow() override { return mainWindow; }
    void EndDialog(std::uint32_t code, cIGZWin* window, std::int64_t result) override {
        dialogs.push_back({code, window, result});
    }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin*) override {}
    void RemoveWindowFromValidList(cIGZWin*) override {}
    void CleanUpWindowReferences(cIGZWin*) override {}
    bool DestroyWindow(cIGZWin* window) override {
        destroyed.push_back(window);
        return true;
    }
    bool IsWindowValid(cIGZWin* window) override { return window != nullptr; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void GetCursorPos(cIGZWin*, int& x, int& y) override { x = y = 0; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return nullptr; }
    void GZSetCapture(cIGZWin*) override {}
    void GZReleaseCapture(cIGZWin*) override {}
    cIGZCursorManager* GetCursorManager() override { return nullptr; }
    cIGZWin* GZGetFocus() override { return focus; }
    void GZSetFocus(cIGZWin* window) override { focus = window; }
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
    int RunPopup(cIGZWin* popup) override {
        popups.push_back(popup);
        return 0;
    }
};

struct ToolInputHost final : IViewInputHost {
    ToolWinMgr manager;
    IViewInputView* View3D() override { return nullptr; }
    cIGZWinMgr* WindowManager() override { return &manager; }
    void PlaySound(int, std::uint32_t) override {}
};
struct ToolView final : cISC4View3DWin {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cSC4ViewInputControl* GetCurrentViewInputControl() override { return current; }
    bool SetCurrentViewInputControl(cSC4ViewInputControl* control, int operation) override {   // the stack holds a reference
        if (control) control->AddRef();
        pushed = control; pushOp = operation; current = control; return true;
    }
    bool RemoveCurrentViewInputControl(bool) override { ++removals; current = nullptr; return true; }
    bool PickOccupant(int, int, sc4::cISC4Occupant*& out) override { out = picked; return picked != nullptr; }
    bool PickTerrain(int, int, float* out, bool) override { out[0] = ground[0]; out[1] = ground[1]; out[2] = ground[2]; return terrain; }
    cSC4ViewInputControl* current = nullptr;
    cSC4ViewInputControl* pushed = nullptr;
    int pushOp = -1, removals = 0;
    sc4::cISC4Occupant* picked = nullptr;
    bool terrain = false;
    float ground[3] = {0.0f, 0.0f, 0.0f};
};

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
        std::vector<sc4::cISC4Occupant*> city;
        auto* special = new FakeBuilding();
        special->AddRef();
        special->position[0] = 100.0f;
        special->position[2] = 200.0f;
        host.cityOccupants.push_back(special);
        CHECK(sim->DistanceToClosestSpecial(0x1001) == 0);
        CHECK(sim->DistanceToClosestSpecial(0x2222) == 0xffffffffu);
        // The grade of the closest school of the group (i386 0x00543a48), the hospital list for hospitals.
        auto school = [&host](float x, float z, std::uint32_t group, float grade) {
            auto* o = new FakeOccupant();
            o->AddRef();
            o->position[0] = x;
            o->position[2] = z;
            o->group = group;
            host.schools.push_back(o);
            host.schoolGrades[o] = grade;
        };
        school(130.0f, 240.0f, 0x4149, 0.8f);                           // distance 50
        school(100.0f, 210.0f, 0x4149, 0.3f);                           // distance 10: the closest of the group
        school(100.0f, 201.0f, 0x9999, 0.9f);                           // closer, another group
        CHECK(sim->LocalSchoolGrade(0x4149) == 0.3f);
        CHECK(sim->LocalSchoolGrade(0x9999) == 0.9f);
        CHECK(sim->LocalSchoolGrade(0x1234) == 0.0f);                   // none of the group: kSC4PercentageZero
        CHECK(sim->LocalHospitalGrade(0x4149) == 0.0f);                 // the hospital list is empty
        // ---- UpdateVariables fills the opinion fields (i386 0x005481e6) ----
        {
            host.policeStations = {{{130.0f, 0.0f, 240.0f}, true, 0.5f},     // distance 50
                                   {{101.0f, 0.0f, 200.0f}, false, 0.9f},    // closest, its funding does not answer
                                   {{110.0f, 0.0f, 200.0f}, true, 0.75f}};   // distance 10: the closest funded one
            FakeBuilding* home = dynamic_cast<FakeBuilding*>(sim->GetResidence());
            CHECK(home != nullptr);
            if (home) home->name = cRZString("A Very Long Home Name");
            sim->UpdateVariables();
            CHECK(sim->LuaTableRef() >= 1);
            const LuaTable& t = *host.lua.refs[static_cast<std::size_t>(sim->LuaTableRef() - 1)].table;
            auto num = [&t](const char* key) {
                const auto it = t.fields.find(key);
                return it != t.fields.end() && it->second.kind == LuaValue::Number ? it->second.number : -99999.0;
            };
            auto str = [&t](const char* key) -> std::string {
                const auto it = t.fields.find(key);
                if (it == t.fields.end() || it->second.kind != LuaValue::UserData || !it->second.user) return "<none>";
                return static_cast<cIGZString*>(it->second.user)->ToChar();
            };
            CHECK(num("eq") == 60.0 && num("hq") == 70.0);
            CHECK(num("le") == 71.0);
            CHECK(num("home_condition") == 2.0);
            CHECK(str("home_building") == "A Very Long Home Name");          // the home's name is not fitted
            CHECK(num("home_has_power") == 1.0 && num("home_has_water") == 0.0);
            CHECK(num("local_crime") == 33.0);
            std::int32_t rect[4] = {};
            host.lot->GetBoundingRect(rect);
            CHECK(host.coverageRect[0] == rect[0] && host.coverageRect[1] == rect[1]);
            CHECK(host.coverageRect[2] == rect[2] - rect[0] + 1 && host.coverageRect[3] == rect[3] - rect[1] + 1);
            CHECK(num("home_police_coverage") == 44.0 && num("home_fire_coverage") == 45.0);
            CHECK(num("local_air_pollution") == -5.0 && num("local_water_pollution") == -4.0);
            CHECK(num("local_garbage") == -3.0 && num("local_radiation") == -2.0);
            CHECK(sim->GetWealth() == 0 && host.desirabilityType == 0);   // wealth 0: no developer, no field
            CHECK(t.fields.count("desirability") == 0);
            CHECK(num("home_occupant_wealth") == 0.0);
            CHECK(num("local_police_funding") == 75.0);
            CHECK(num("local_fire_funding") == 0.0);                         // no fire station
            // The highest congestion on the ring one cell outside the lot (the fake answers x + 2z).
            // (the corner x1, z1 of the ring is the highest; the lot is small, below the byte limit)
            const int x1 = std::min(rect[2] + 1, 127), z1 = std::min(rect[3] + 1, 127);
            CHECK(num("local_traffic_congestion") == static_cast<double>(x1 + 2 * z1));
            CHECK(host.congestionCalls > 0);
            CHECK(num("last_commute_time") >= 0.0);
            // The fake's job is across a neighbor connection: the neighbor fields, the "neighbor city" job building
            // (fitted by the host to 200 pixels; the fake cuts at 8 characters) and the regional city's name.
            CHECK(sim->GetJob() == nullptr);
            CHECK(num("employed") == 1.0 && num("unemployed_days") == 0.0);
            CHECK(num("job_is_neighbor") == 1.0 && num("job_condition") == 0.0);
            CHECK(num("job_has_power") == 1.0 && num("job_has_water") == 1.0);
            CHECK(str("job_building") == "str:6a66");
            CHECK(str("job_city") == "Neighborville");
            CHECK(host.fitWidth == 200);
            CHECK(t.fields.count("job_title") == 1);
        }
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

    // ---- FindNewResidence starts RandomUint32Uniform(count) - 1 nodes into the candidates (i386 0x0054a6d9) ----
    {
        std::vector<FakeBuilding*> pool;
        for (int i = 0; i < 7; ++i) {
            auto* building = new FakeBuilding();
            building->AddRef();
            building->profile.purpose = 1;
            pool.push_back(building);
        }
        bool sawFirstNode = false, sawOffset = false;
        for (int round = 0; round < 12; ++round) {
            for (FakeBuilding* b : pool) { b->AddRef(); host.candidates.push_back(b); }
            cRZRandom predict = sim->Random();
            const std::uint32_t draw = predict.RandomUint32Uniform(static_cast<std::uint32_t>(pool.size()));
            cISC4BuildingOccupant* before = sim->GetResidence();
            // every candidate is free; the walk skips the current residence, so start after it if the start is it
            std::size_t index = draw == 0 ? 0 : draw - 1;
            if (pool[index] == before) index = (index + 1) % pool.size();
            sim->FindNewResidence();
            CHECK(sim->GetResidence() == pool[index]);
            if (draw == 0) sawFirstNode = true; else sawOffset = true;
            for (FakeBuilding* b : host.candidates) b->Release();
            host.candidates.clear();
        }
        CHECK(sawOffset);
        (void)sawFirstNode;
        for (FakeBuilding* b : pool) b->Release();
    }

    // ---- UpdateTripVariables: the trip state and the flag / travel-type block (i386 0x00546b1a) ----
    {
        sim->SetStateForTest(2);
        if (sim->LuaTableRef() < 1) sim->UpdateVariables();
        CHECK(sim->LuaTableRef() >= 1);
        auto num = [&](const char* key) {
            if (sim->LuaTableRef() < 1) return -99999.0;
            const LuaTable& t = *host.lua.refs[static_cast<std::size_t>(sim->LuaTableRef() - 1)].table;
            const auto it = t.fields.find(key);
            return it != t.fields.end() && it->second.kind == LuaValue::Number ? it->second.number : -99999.0;
        };
        auto notify = [&](std::uint32_t event, std::uint64_t data2, std::uint64_t data3) {
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(kMessageMySimDispatchNotify);
            message->SetData1(event);
            message->SetData2(data2);
            message->SetData3(data3);
            sim->DoMessage(message);
            message->Release();
        };
        host.dispatchLog.travelType = 1;
        host.dispatchLog.checkFlags = true;
        const std::size_t events = sim->Events().size();
        notify(kDispatchNotifyTripEnd, 0x1000, 0x9000);   // old flags, new flags: 0x8000 (near the destination) set
        CHECK(num("trip_idle") == 1.0 && num("trip_at_transit_switch") == 1.0);
        CHECK(num("trip_in_car") == 1.0 && num("trip_on_foot") == 0.0 && num("trip_on_bus") == 0.0);
        CHECK(num("last_trip_type") == 1.0);
        CHECK(sim->Events().size() == events + 1 && sim->Events().back().first == static_cast<int>(kEventMySimLeftWork));
        host.dispatchLog.travelType = 0xfe;
        notify(kDispatchNotifyTripEnd, 0x9000, 0x9100);   // only the tracking bit: the block is skipped
        CHECK(num("trip_in_car") == 1.0);
        notify(kDispatchNotifyTripEnd, 0x9100, 0x1100);
        CHECK(num("trip_in_car") == 0.0 && num("last_trip_type") == 255.0);
        CHECK(sim->Events().size() == events + 1);
        notify(kDispatchNotifyTripLeg, 3, 2);              // old state, new state: leaving
        CHECK(num("trip_in_transit") == 1.0 && num("trip_at_location") == 0.0);
        notify(kDispatchNotifyTripLeg, 2, 3);              // arriving
        CHECK(num("trip_in_transit") == 0.0 && num("trip_at_location") == 1.0);
        // the arrival bookkeeping of the state block (i386 0x00546994)
        MySimLocation home, work, elsewhere;
        home.SetCellPosition(5, 5);
        work.SetCellPosition(9, 9);
        elsewhere.SetCellPosition(20, 20);
        host.dispatchLog.defaultDestination = home;
        host.dispatchLog.commuteDestination = work;
        host.dispatchLog.current = home;
        sim->SetStateForTest(1);                           // moving in: the first arrival home announces it
        sim->SetLastSoundTimeForTest(-100);
        const std::size_t before = sim->Events().size();
        const std::size_t posted = host.posts.size();
        notify(kDispatchNotifyTripLeg, 2, 3);
        CHECK(sim->State() == 2);
        CHECK(sim->Events().size() > before && sim->Events().back().first == static_cast<int>(kEventMySimMovedIn));
        // a state change without the flag block posts "this sim changed" with the sim itself as the object
        CHECK(host.posts.size() == posted + 1 && std::get<0>(host.posts.back()) == kMessageMySimChanged);
        CHECK(host.postedObjects.back() == static_cast<cIGZUnknown*>(static_cast<cISC4MySim*>(sim)));
        sim->SetStateForTest(3);                           // back from work: the home sound
        sim->SetLastSoundTimeForTest(-100);
        const int sounds = host.homeSounds;
        notify(kDispatchNotifyTripLeg, 2, 3);
        CHECK(sim->State() == 2 && host.homeSounds == sounds + 1);
        host.dispatchLog.current = work;                   // at work
        notify(kDispatchNotifyTripLeg, 2, 3);
        CHECK(sim->State() == 3);
        host.dispatchLog.current = elsewhere;              // elsewhere: the state stays, the subject is the location
        notify(kDispatchNotifyTripLeg, 2, 3);
        CHECK(sim->State() == 3);

        // the surroundings: grids at the location's position, the local area's lots and disasters
        host.dispatchLog.current = work;                   // cell 9,9 -> the local area (MYSIM_TRIP_LOCAL_AREA_SIZE 10) 4..14
        host.dispatchLog.state = 2;
        host.dispatchLog.travelType = 1;
        host.dispatchLog.checkFlags = false;
        host.tripGrids = {{static_cast<int>(IMySimHost::TripGrid::Congestion), 300.0},
                          {static_cast<int>(IMySimHost::TripGrid::PollutionAir), -7.0},
                          {static_cast<int>(IMySimHost::TripGrid::PollutionGarbage), 12.0},
                          {static_cast<int>(IMySimHost::TripGrid::Crime), 40.0},
                          {static_cast<int>(IMySimHost::TripGrid::MayorRating), -56.0},
                          {static_cast<int>(IMySimHost::TripGrid::EducationQuotient), 87.9},
                          {static_cast<int>(IMySimHost::TripGrid::HealthQuotient), 55.2}};
        host.travelRatio = 2.5f;
        auto* tower = new FakeBuilding();
        tower->AddRef();
        tower->group = kOccupantGroupLandmark;
        tower->name = cRZString("Big Tower");
        auto* plain = new FakeBuilding();
        plain->AddRef();
        host.tripLots = {{plain, 2, 1}, {nullptr, 5, 3}, {tower, 3, 2}, {plain, 5, 3}};
        host.landmarkFunding = 0.75f;
        host.tripDisasters = {{kDisasterIdFire, 7, 11}, {kDisasterIdRiot, 30, 30}};
        const int top = host.lua.GetTop();
        notify(kDispatchNotifyTripEnd, 0, 0);              // no state or flag change: only the sampling
        CHECK(host.lua.GetTop() == top);
        CHECK(host.tripPosition[0] == 144.0f && host.tripPosition[1] == 144.0f);
        CHECK(num("trip_traffic_congestion") == 44.0);
        CHECK(num("trip_time_ratio") == 2.0 && host.travelRatioType == 1);
        CHECK(num("trip_wait_length") == 0.0);
        CHECK(num("trip_pollution_air") == -7.0 && num("trip_pollution_water") == 0.0 && num("trip_pollution_garbage") == 12.0);
        CHECK(num("trip_crime") == 40.0 && num("trip_mayor_rating") == -56.0);
        CHECK(num("trip_eq") == 87.0 && num("trip_hq") == 55.0);
        CHECK(host.tripRect[0] == 4 && host.tripRect[1] == 4 && host.tripRect[2] == 14 && host.tripRect[3] == 14);
        CHECK(num("trip_distress1_p") == 33.0 && num("trip_distress2_p") == 33.0 && num("trip_distress3_p") == 33.0);
        CHECK(num("trip_wealth_low_p") == 33.0 && num("trip_wealth_med_p") == 33.0 && num("trip_wealth_high_p") == 33.0);
        CHECK(num("trip_landmark_nearby") == 1.0 && num("trip_landmark_funding_p") == 75.0);
        CHECK(host.fundingPurpose == kBudgetPurposeLandmark);
        CHECK(num("trip_fire_nearby") == 1.0 && num("trip_riot_nearby") == 0.0 && num("trip_any_disaster_nearby") == 1.0);
        // travelling by bus and idle-free: no local area; a 0xfe travel type rates 1 without asking the simulator
        host.tripLots.clear();
        host.tripDisasters.clear();
        host.tripRect[0] = -1;
        host.dispatchLog.travelType = 0xfe;
        host.travelRatioType = -1;
        notify(kDispatchNotifyTripEnd, 0, 0);
        CHECK(num("trip_time_ratio") == 1.0 && host.travelRatioType == -1);
        host.dispatchLog.travelType = 2;
        notify(kDispatchNotifyTripEnd, 0, 0);
        CHECK(host.tripRect[0] == -1 && num("trip_landmark_nearby") == 1.0);
        host.dispatchLog.state = 3;                        // at a location: the local area again, now empty
        notify(kDispatchNotifyTripEnd, 0, 0);
        CHECK(host.tripRect[0] == 4 && num("trip_landmark_nearby") == 0.0 && num("trip_fire_nearby") == 0.0);
        host.tripLots.clear();
        tower->Release();
        plain->Release();
        host.dispatchLog.current = MySimLocation();
        host.dispatchLog.state = 0;
        host.dispatchLog.travelType = 0;
        host.dispatchLog.checkFlags = false;
    }

    // ---- Shutdown ----
    CHECK(host.registryAdds == 2);
    CHECK(sim->Shutdown());
    CHECK(host.registryRemoves == 1);
    CHECK(host.atmUnregisters == 1);
    CHECK(simWinProc->shutdowns == 1);
    CHECK(sim->State() == 0);
    sim->Release();

    // ---- MySimSetTableOccupantSubject: a second call finds the subject stored under the key and reuses it ----
    {
        cISCLua* lua = &host.lua;
        lua->NewTable();
        MySimSetTableOccupantSubject(lua, "destination_subject", nullptr);
        lua->PushString("destination_subject");
        lua->RawGet(-2);
        void* first = lua->ToUserTag(-1);
        lua->Pop(1);
        CHECK(first != nullptr);
        MySimSetTableOccupantSubject(lua, "destination_subject", nullptr);
        lua->PushString("destination_subject");
        lua->RawGet(-2);
        CHECK(lua->ToUserTag(-1) == first);
        lua->Pop(2);
    }

    // ---- cSC4MyStreetSim: attach / detach, the trip kind from the occupant group, the sampling at the automaton ----
    {
        struct StreetView final : IMySimDispatchVehicleView {
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return 1; }
            std::uint32_t Release() override { return 1; }
            int TrackOccupant(sc4::cISC4Occupant* occupant, int type, std::uint32_t number, const cRZString*) override {
                tracked = occupant; trackType = type; trackNumber = number; return 77;
            }
            void UntrackOccupant(int id, bool force) override { untracked.push_back(id); untrackForce = force; }
            sc4::cISC4Occupant* tracked = nullptr; int trackType = -1; std::uint32_t trackNumber = 0;
            std::vector<int> untracked; bool untrackForce = true;
        };
        struct StreetAutomaton final : IMySimDispatchAutomaton, IMySimDispatchRoute {
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return ++refs; }
            std::uint32_t Release() override { return --refs; }
            sc4::cISC4Occupant* AsOccupant() override { return occupant; }
            void Remove(bool) override {}
            bool PlayAnim(std::uint32_t) override { return true; }
            void SetAutomataFlag(std::uint32_t flag, bool on) override { flags = on ? (flags | flag) : (flags & ~flag); }
            bool CheckAutomataFlag(std::uint32_t flag) override { return (flags & flag) != 0; }
            void AddCallback(cIGZMessageTarget2* target) override { callback = target; }
            void RemoveCallback(cIGZMessageTarget2* target) override { if (callback == target) callback = nullptr; }
            int GetStateId() const override { return 0; }
            std::uint32_t GetBehavior() const override { return behavior; }
            bool SetState(std::uint32_t, IMySimDispatchAutomaton*, float, bool, std::uint32_t, std::uint32_t, const void*) override { return true; }
            void SetPriority(std::uint8_t) override {}
            IMySimDispatchRoute* RouteInterface() override { if (!car) return nullptr; AddRef(); return this; }
            bool GetRouteInfo(std::deque<RouteData>&) const override { return false; }
            bool SetRouteInfo(const std::deque<RouteData>&, bool, bool) override { return false; }
            void SetCellLifespan(std::uint8_t lifespan) override { cellLifespan = lifespan; }
            int refs = 1; sc4::cISC4Occupant* occupant = nullptr; std::uint32_t flags = 0, behavior = 0; bool car = false;
            cIGZMessageTarget2* callback = nullptr; int cellLifespan = -1;
        };
        StreetView view;
        host.streetView = &view;
        auto* street = new cSC4MyStreetSim();
        street->AddRef();
        CHECK(street->GetBitmapID() == kMyStreetSimBitmapId && street->get_track_handle() == -1);
        CHECK(street->Init(1));
        CHECK(street->LuaTableRef() >= 1);
        auto snum = [&](const char* key) {
            const LuaTable& t = *host.lua.refs[static_cast<std::size_t>(street->LuaTableRef() - 1)].table;
            const auto it = t.fields.find(key);
            return it != t.fields.end() && it->second.kind == LuaValue::Number ? it->second.number : -99999.0;
        };
        auto* walker = new FakeOccupant();
        walker->AddRef();
        walker->group = 0x4001;
        walker->position[0] = 160.0f;   // cell 10,10
        walker->position[2] = 160.0f;
        StreetAutomaton person;
        person.occupant = walker;
        person.behavior = 7;
        host.tripSimulators = true;
        host.tripGrids = {{static_cast<int>(IMySimHost::TripGrid::Crime), 300.0}};
        host.travelRatio = 3.5f;
        host.tripLots.clear();
        host.tripDisasters.clear();
        const int top = host.lua.GetTop();
        CHECK(street->AttachAutomaton(&person));
        CHECK(host.lua.GetTop() == top);
        CHECK(street->Attached() && street->Automaton() == &person && street->State() == 6 && street->TravelType() == 0);
        CHECK(snum("trip_on_foot") == 1.0 && snum("trip_idle") == 1.0 && snum("trip_in_car") == 0.0 && snum("state") == 6.0);
        CHECK(snum("wealth") == 2.0 && snum("trip_in_transit") == 1.0);
        CHECK(snum("trip_crime") == 44.0 && snum("trip_time_ratio") == 3.0 && host.travelRatioType == 0);   // 300 & 0xff
        CHECK(host.tripPosition[0] == 160.0f && host.tripPosition[1] == 160.0f);
        CHECK(view.tracked == walker && view.trackType == 3 && view.trackNumber == kMyStreetSimBitmapId && street->get_track_handle() == 77);
        CHECK(person.callback != nullptr && person.cellLifespan == -1 && person.flags == 0);   // a pedestrian: no car fields
        // a bus: the car fields are set, travel type 2, state 4 or 5 (the advisor system's random number)
        walker->group = 0x4200;
        StreetAutomaton bus;
        bus.occupant = walker;
        bus.car = true;
        CHECK(street->AttachAutomaton(&bus));
        CHECK(person.callback == nullptr && view.untracked.size() == 1 && view.untracked[0] == 77 && !view.untrackForce);
        CHECK(bus.cellLifespan == 0xff && (bus.flags & 0x2000000) != 0);
        CHECK(street->TravelType() == 2 && (street->State() == 4 || street->State() == 5) && snum("trip_on_bus") == 1.0);
        CHECK(snum("trip_on_foot") == 0.0 && snum("trip_idle") == 0.0);
        // HideLocation keeps the automaton; ShowLocation attaches again; the removal message detaches for good
        street->HideLocation();
        CHECK(!street->Attached() && street->Automaton() == &bus && street->get_track_handle() == -1 && street->State() == 0);
        street->ShowLocation();
        CHECK(street->Attached() && view.trackType == 3);
        auto* removed = new cRZMessage2Standard();
        removed->AddRef();
        removed->SetType(kMessageMyStreetSimRemoved);
        removed->SetData2(4);   // not 2, 3 or 5
        street->DoMessage(removed);
        CHECK(street->Attached());
        removed->SetData2(3);
        street->DoMessage(removed);
        CHECK(!street->Attached() && street->Automaton() == nullptr);
        removed->Release();
        // ---- the street-sim tool: the button creates it on the view, hover marks, a click attaches, Escape and HideLocation end it
        {
            struct ToolHost final : IMyStreetSimToolHost {
                ToolView view;
                std::vector<sc4::cISC4Occupant*> box;
                std::vector<std::pair<sc4::cISC4Occupant*, IMySimDispatchAutomaton*>> automata;
                float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
                cISC4View3DWin* View() override { return &view; }
                void OccupantsInBox(const float min[3], const float max[3], std::vector<sc4::cISC4Occupant*>& out) override {
                    for (int i = 0; i < 3; ++i) { boxMin[i] = min[i]; boxMax[i] = max[i]; }
                    out = box;
                }
                IMySimDispatchAutomaton* StreetAutomaton(sc4::cISC4Occupant* occupant) override {
                    for (auto& entry : automata) if (entry.first == occupant) { entry.second->AddRef(); return entry.second; }
                    return nullptr;
                }
            } toolHost;
            ToolInputHost inputHost;
            SetViewInputHost(&inputHost);
            SetMyStreetSimToolHost(&toolHost);
            CHECK(street->CreateMyStreetSimTool());
            auto* tool = static_cast<cSC4MyStreetSimInputControl*>(toolHost.view.pushed);
            CHECK(tool && toolHost.view.pushOp == 2 && tool->GetID() == kMyStreetSimToolId && !tool->ShouldStack());
            if (tool) {
                // a bus under the pointer is marked; a flagged one (automata flag 1) is not
                walker->group = 0x4200;
                StreetAutomaton busUnder;
                busUnder.occupant = walker;
                toolHost.automata = {{walker, &busUnder}};
                toolHost.view.picked = walker;
                tool->OnMouseMove(5, 5, 0);
                CHECK(tool->Hovered() == &busUnder);
                busUnder.flags = 1;
                tool->OnMouseMove(5, 5, 0);
                CHECK(tool->Hovered() == nullptr);
                busUnder.flags = 0;
                // nothing under the pointer: a person within 3 x 7 x 3 of the picked ground
                toolHost.view.picked = nullptr;
                toolHost.view.terrain = true;
                toolHost.view.ground[0] = 100.0f; toolHost.view.ground[1] = 50.0f; toolHost.view.ground[2] = 200.0f;
                auto* nearby = new FakeOccupant();
                nearby->AddRef();
                nearby->group = 0x4001;
                StreetAutomaton personNear;
                personNear.occupant = nearby;
                toolHost.automata.push_back({nearby, &personNear});
                toolHost.box = {walker, nearby};   // the bus is not a person: skipped
                tool->OnMouseMove(5, 5, 0);
                CHECK(tool->Hovered() == &personNear);
                CHECK(toolHost.boxMin[0] == 97.0f && toolHost.boxMin[1] == 43.0f && toolHost.boxMax[2] == 203.0f);
                // a click when the tool is not on top does nothing; on top it attaches
                toolHost.view.current = nullptr;
                CHECK(!tool->OnMouseDownL(5, 5, 0));
                CHECK(!street->Attached());
                toolHost.view.current = tool;
                CHECK(tool->OnMouseDownL(5, 5, 0));
                CHECK(street->Attached() && street->Automaton() == &personNear);
                // HideLocation ends the tool when it is the current control
                street->HideLocation();
                CHECK(toolHost.view.removals == 1 && toolHost.view.current == nullptr);
                CHECK(tool->OnKeyDown(0x1b, 0) && !tool->OnKeyDown(0x20, 0));
                street->DetachAutomaton();
                tool->Shutdown();
                tool->Release();   // the fake stack's reference
                nearby->Release();
            }
            SetMyStreetSimToolHost(nullptr);
            SetViewInputHost(nullptr);
        }
        // no automaton: the day does nothing (the base would advance the sim)
        street->simulate_day();
        street->Shutdown();
        street->Release();
        walker->Release();
        host.streetView = nullptr;
    }

    SetAdvisorHost(nullptr);
    SetMySimHost(nullptr);
    SetAdviceSubjectHost(nullptr);
    if (failures == 0) std::printf("sc4_mysim_test: ok\n");
    return failures == 0 ? 0 : 1;
}
