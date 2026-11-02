// cSC4CivicBuildingSimulator: department set-up, the landmark/zoo registry and its budget bookkeeping, the monthly
// funding pass (lot condition and ape escape), the conditional building status table, exclusion and save/load.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/CivicBuildingSimulator.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

namespace SL {
cIGZMessageServer2* spMS2 = nullptr;
}
using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

// --------------------------------------------------------------------------------------------------- fakes
struct BudgetEnv final : IBudgetSimEnvironment {
    void SimulatorDate(int& year, int& month) override { year = 1; month = 1; }
    std::int64_t OrdinanceMonthlyIncome() override { return 0; }
    std::int64_t OrdinanceMonthlyExpense() override { return 0; }
    std::int64_t NeighborsIncome() override { return 0; }
    std::int64_t NeighborsExpense() override { return 0; }
    int DifficultyLevel() override { return 0; }
    void ForEachTaxableLot(const std::function<void(const LotTaxInfo&)>&) override {}
    void PostMessage(std::uint32_t, std::uint32_t, std::int64_t, std::int64_t) override {}
    void AddHistory(std::uint32_t, std::int64_t) override {}
    void PostBudgetMessage(std::uint32_t, cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override {}
    void NotifyLocalFundingChanged(cISCPropertyHolder*) override {}
    std::string LoadLocalizedString(std::uint32_t, std::uint32_t) override { return {}; }
    std::string BuildingName(std::uint32_t) override { return {}; }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
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

struct FakeParamSet final : cIGZCommandParameterSet {
    std::map<std::uint32_t, cRZVariant> parameters;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    std::uint32_t GetParameterCount() const override { return static_cast<std::uint32_t>(parameters.size()); }
    void SetParameterCount(std::uint32_t) override {}
    cIGZVariant& operator[](std::uint32_t id) override { return parameters[id]; }
    cIGZVariant* GetParameter(std::uint32_t id) override {
        auto it = parameters.find(id);
        return it == parameters.end() ? nullptr : &it->second;
    }
    void SetParameter(std::uint32_t id, const cIGZVariant& value) override { parameters[id] = cRZVariant(value); }
    void AppendParameter(const cIGZVariant& value) override { parameters[0xffffffffu] = cRZVariant(value); }
    void RemoveParameter(std::uint32_t id) override { parameters.erase(id); }
    void SetStatusParameterValue(std::int32_t) override {}
    bool CompareParameterString(std::uint32_t, const char*, bool) override { return false; }
    bool ValidateParameter(std::uint32_t, std::uint32_t) override { return true; }
    std::uint32_t GetFirstParameterID() const override {
        return parameters.empty() ? 0xffffffffu : parameters.begin()->first;
    }
    std::uint32_t GetNextParameterID(std::uint32_t) const override { return 0xffffffffu; }
};

// The game-data registry the original gets from RZGetFramework(): records the node/value registrations and runs a
// registered command the way DoValueCall does.
struct FakeRegistry final : cISC4GameDataRegistry {
    struct Entry {
        void* handle = nullptr;
        std::string name;
        EntryId parent = 0;
        int flags = 0;
        void* context = nullptr;
        bool isNode = false;
        GameDataValueFn valueFn = nullptr;
        GameDataCommandFn commandFn = nullptr;
    };
    static constexpr EntryId kRoot = 0x1000;
    std::vector<Entry> entries;
    std::vector<std::string> removed;

    std::size_t IndexOf(EntryId id) const {
        const std::size_t index = static_cast<std::size_t>(id - kRoot);
        return index < entries.size() ? index : entries.size();
    }
    EntryId AddChildEntry(void* handle, const char* name, EntryId parent, int flags, void* context, bool isNode) {
        Entry entry;
        entry.handle = handle;
        entry.name = name ? name : "";
        entry.parent = parent;
        entry.flags = flags;
        entry.context = context;
        entry.isNode = isNode;
        entries.push_back(entry);
        return kRoot + entries.size() - 1;
    }

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    EntryId AddChildEntry(cIGZUnknown* node, const char* name, EntryId parent, int flags, void* context) override {
        return AddChildEntry(static_cast<void*>(node), name, parent, flags, context, true);
    }
    EntryId AddChildEntry(void* handle, const char* name, GameDataValueFn fn, EntryId parent, int flags,
                          void* context) override {
        const EntryId id = AddChildEntry(handle, name, parent, flags, context, false);
        entries.back().valueFn = fn;
        return id;
    }
    EntryId AddChildEntry(void* handle, const char* name, GameDataCommandFn fn, EntryId parent, int flags,
                          void* context) override {
        const EntryId id = AddChildEntry(handle, name, parent, flags, context, false);
        entries.back().commandFn = fn;
        return id;
    }
    bool RemoveChildEntry(cIGZUnknown* node) override { return RemoveChildEntry(static_cast<void*>(node)); }
    bool RemoveChildEntry(void* handle) override {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].handle == handle) {
                removed.push_back(entries[i].name);
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }
    EntryId GetEntryID(cIGZUnknown* node) const override { return GetEntryID(static_cast<void*>(node)); }
    EntryId GetEntryID(void* handle) const override {
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].handle == handle) return kRoot + i;
        return 0;
    }
    cIGZUnknown* GetNode(EntryId) const override { return nullptr; }
    void* GetValue(EntryId) const override { return nullptr; }
    bool IsValidEntry(EntryId id) const override { return id >= kRoot && id - kRoot < entries.size(); }
    bool IsNodeEntry(EntryId id) const override { return IsValidEntry(id) && entries[IndexOf(id)].isNode; }
    const cRZString* GetEntryName(EntryId) const override { return nullptr; }
    bool DoValueExchange(EntryId, cIGZVariant&, bool) override { return false; }
    bool DoValueCall(EntryId id, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out) override {
        if (!IsValidEntry(id)) return false;
        Entry& entry = entries[IndexOf(id)];
        if (!entry.commandFn) return false;
        return entry.commandFn(entry.handle, in, out, this);
    }
    bool SetEntryContext(EntryId, void*) override { return true; }
    void* GetEntryContext(EntryId id) const override {
        if (!IsValidEntry(id)) return nullptr;
        return entries[IndexOf(id)].context;
    }
    bool CheckEntryFlags(EntryId, int) const override { return true; }
    EntryId GetParentNodeEntry(EntryId id) const override {
        return IsValidEntry(id) ? entries[IndexOf(id)].parent : 0;
    }
    EntryId GetRootNodeEntry() const override { return kRoot; }
    bool VisitBreadthFirst(EntryId, EntryVisitFn, void*) override { return true; }
    bool VisitImmediateChildren(EntryId, EntryVisitFn, void*) override { return true; }
    bool AddNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return true; }
    bool AddNotificationTarget(cIGZMessageTarget2*) override { return true; }
    bool RemoveNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return true; }
    bool RemoveNotificationTarget(cIGZMessageTarget2*) override { return true; }
    void DebugDumpRecursive(EntryId) override {}
};

struct FakeLot final : ICivicLot {
    cISC4BuildingOccupant* building = nullptr;
    std::uint32_t condition = 0;
    int setConditionCalls = 0;
    cISC4BuildingOccupant* GetBuilding() override { return building; }
    std::uint32_t GetCondition() const override { return condition; }
    void SetCondition(std::uint32_t value) override {
        condition = value;
        ++setConditionCalls;
    }
};

// A building occupant that answers both the occupant and the building-occupant views (like cSC4BuildingOccupant).
struct FakeBuilding final : cISC4BuildingOccupant, cISC4Occupant {
    cISCPropertyHolder* holder = nullptr;
    std::uint32_t buildingType = 0;
    std::uint32_t group = 0;
    int refs = 1;
    std::uint32_t type = kOccupantTypeBuilding;
    float completion = 1.0f;
    BuildingProfile profile;
    cRZString name;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cISC4BuildingOccupant) *out = static_cast<cISC4BuildingOccupant*>(this);
        else if (iid == kGZIID_cISC4Occupant) *out = static_cast<cISC4Occupant*>(this);
        else return false;
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }

    // cISC4BuildingOccupant
    cISC4Occupant* AsOccupant() override { return static_cast<cISC4Occupant*>(this); }
    std::uint32_t GetBuildingType() const override { return buildingType; }
    void SetBuildingType(std::uint32_t value) override { buildingType = value; }
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { return completion; }
    void SetCompletionPercent(float value) override { completion = value; }
    const BuildingProfile& GetBuildingProfile() const override { return profile; }
    const cRZString& GetBuildingName() const override { return name; }
    const cRZString& GetExemplarName() const override { return name; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return true; }
    bool IsLit() const override { return true; }
    void SetLit(bool) override {}
    void SetName(const cRZString& value) override { name = value; }
    bool GetName(cRZString& out) override { out = name; return true; }

    // cISC4Occupant
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float*) override {}
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
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
    bool IsOccupantGroup(std::uint32_t value) const override { return value == group; }
    void AddOccupantGroup(std::uint32_t value) override { group = value; }
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

struct FakeHost final : ICivicHost {
    cSC4BudgetSimulator* budget = nullptr;
    FakeRegistry* registry = nullptr;
    cIGZMessageTarget2* agentTarget = nullptr;
    std::string agentName;
    int agentInterval = 0;
    std::uint32_t agentFlags = 0;
    int agentsAdded = 0, agentsRemoved = 0;

    bool hasKey = false;
    std::uint32_t keyInstance = 0;
    ICivicLot* messageLot = nullptr;
    std::map<cISC4Occupant*, ICivicLot*> lots;
    int escapes = 0;
    std::string escapeGroup;
    cISC4Occupant* escapeOccupant = nullptr;

    cSC4BudgetSimulator* BudgetSimulator() override { return budget; }
    cISC4GameDataRegistry* GameDataRegistry() override { return registry; }
    bool AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {
        agentTarget = target;
        agentInterval = interval;
        agentName = name ? name : "";
        agentFlags = flags;
        ++agentsAdded;
        return true;
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override {
        CHECK(target == agentTarget);
        ++agentsRemoved;
    }
    bool GetBuildingKeyFromType(std::uint32_t buildingType, cGZPersistResourceKey& key) override {
        if (!hasKey) return false;
        key.type = 0x6534284a;
        key.group = buildingType;
        key.instance = keyInstance;
        return true;
    }
    ICivicLot* MessageLot(cIGZMessage2*) override { return messageLot; }
    ICivicLot* LotForOccupant(cISC4Occupant* occupant) override {
        auto it = lots.find(occupant);
        return it == lots.end() ? nullptr : it->second;
    }
    bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) override {
        ++escapes;
        escapeGroup = group.ToChar();
        escapeOccupant = occupant;
        return true;
    }
};

void AddBudgetItemProperties(cISCPropertyHolder* holder, std::uint32_t department, std::uint32_t landmarkLine) {
    // Real civic exemplars carry these parallel arrays; the purpose ids of the two items live in the "reserved" one
    // (the original matches GetBudgetItemForPurpose against that word, 0x100222797).
    cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropDepartments,
                                      std::vector<std::uint32_t>{department, department, department});
    cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropLineItems,
                                      std::vector<std::uint32_t>{landmarkLine, 0, 0});
    cSCPropertyHelp::SetPropertyValue(
        holder, kBudgetItemPropReserved,
        std::vector<std::uint32_t>{kCivicBudgetPurposeLandmark, kCivicBudgetPurposeIncome, 0});
    cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropCosts, std::vector<std::int64_t>{120, 45, 0});
    cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropLocalFunding,
                                      std::vector<float>{100.0f, 100.0f, 100.0f});
}

std::uint32_t ReadU32(const std::vector<unsigned char>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

}  // namespace

int main() {
    BudgetEnv budgetEnv;
    cSC4BudgetSimulator::SetEnvironment(&budgetEnv);
    auto* budget = new cSC4BudgetSimulator();
    budget->AddRef();

    FakeMessageServer server;
    SL::spMS2 = &server;
    FakeRegistry registry;
    FakeHost host;
    host.budget = budget;
    host.registry = &registry;
    SetCivicBuildingHost(&host);

    auto* sim = new cSC4CivicBuildingSimulator();
    sim->AddRef();

    // ---- Init: the civic department, the two further departments, messages, agent and game-data node.
    CHECK(sim->Init());
    CHECK(sim->IsInitialised());
    cSC4DepartmentBudget* civic = budget->GetDepartmentBudget(kCivicDepartment);
    CHECK(civic != nullptr);
    CHECK(sim->Department() == civic);
    CHECK(civic && civic->GetMaxAllowedFundingPercentage() == 1.0f);
    CHECK(budget->GetDepartmentBudget(kCivicDepartment2) != nullptr);
    CHECK(budget->GetDepartmentBudget(kCivicDepartment3) != nullptr);
    CHECK(server.added.size() == 3);
    if (server.added.size() == 3) {
        CHECK(server.added[0].second == kCivicMessageLotStateChanged);
        CHECK(server.added[1].second == kCivicMessageOccupantInserted);
        CHECK(server.added[2].second == kCivicMessageOccupantRemoved);
        CHECK(server.added[0].first == static_cast<cIGZMessageTarget2*>(sim));
    }
    CHECK(host.agentsAdded == 1);
    CHECK(host.agentName == kCivicSimulatorName);
    CHECK(host.agentInterval == kCivicAgentInterval);
    CHECK(host.agentFlags == kCivicAgentFlags);
    CHECK(host.agentTarget == static_cast<cIGZMessageTarget2*>(sim));
    CHECK(registry.entries.size() == 2);
    if (registry.entries.size() == 2) {
        CHECK(registry.entries[0].name == "civic" && registry.entries[0].isNode);
        CHECK(registry.entries[0].handle == static_cast<cIGZUnknown*>(static_cast<cISC4CivicBuildingSimulator*>(sim)));
        CHECK(registry.entries[0].context == nullptr);      // AddNode passes no context
        CHECK(registry.entries[1].name == "get_landmark_count" && registry.entries[1].commandFn != nullptr);
        CHECK(registry.entries[1].context == sim);
    }
    CHECK(sim->Init() && server.added.size() == 3 && host.agentsAdded == 1);   // second Init is a no-op

    // ---- QueryInterface / AddRef / Release / CLSID
    void* iface = nullptr;
    CHECK(sim->QueryInterface(1, &iface) && iface == static_cast<cISC4CivicBuildingSimulator*>(sim));
    sim->Release();
    CHECK(sim->QueryInterface(kGZIID_cISC4CivicBuildingSimulator, &iface) &&
          iface == static_cast<cISC4CivicBuildingSimulator*>(sim));
    sim->Release();
    CHECK(sim->QueryInterface(0x452294aa, &iface) && iface == static_cast<cIGZMessageTarget2*>(sim));
    sim->Release();
    CHECK(sim->QueryInterface(0xe4fda3d4, &iface) && iface == static_cast<cIGZSerializable*>(sim));
    sim->Release();
    CHECK(!sim->QueryInterface(0x12345678, &iface));
    CHECK(!sim->QueryInterface(0, &iface));
    CHECK(sim->GetGZCLSID() == kGZCLSID_cSC4CivicBuildingSimulator);
    CHECK(sim->RefCount() == 1);

    // ---- conditional building status table
    {
        cISC4CivicBuildingSimulator::ConditionalBuildingStatus status;
        status.state = 6;
        status.text = cRZString("fired");
        CHECK(sim->GetConditionalBuildingStatus(0x555) == nullptr);
        sim->UpdateConditionalBuildingStatus(0x555, &status);
        auto* record = sim->GetConditionalBuildingStatus(0x555);
        CHECK(record != nullptr);
        if (record) {
            CHECK(record->state == 6 && record->BuildingCount() == 0);
            CHECK(record->reserved == 0);                       // not copied by Update
            CHECK(std::string(record->text.ToChar()) == "fired");
            record->AddToBuildingCount(1);
            CHECK(record->BuildingCount() == 1 && record->state == (6u | (1u << 16)));
            record->AddToBuildingCount(-1);
            CHECK(record->BuildingCount() == 0 && record->state == 6u);
        }
    }

    // ---- occupant insert / remove: the count only follows an existing record, the exclusion id is tracked.
    auto* holderA = new cSCPropertyHolder();
    holderA->AddRef();
    auto* holderB = new cSCPropertyHolder();
    holderB->AddRef();
    auto* buildingA = new FakeBuilding();
    auto* buildingB = new FakeBuilding();
    buildingA->holder = holderA;
    buildingB->holder = holderB;
    buildingA->buildingType = 0x1234;
    buildingB->buildingType = 0x4321;
    buildingB->group = kCivicOccupantGroupZoo;
    cSCPropertyHelp::SetPropertyValue(holderA, kCivicPropertyConditionalBuilding, true);
    cSCPropertyHelp::SetPropertyValue(holderA, kCivicPropertyExclusionId, static_cast<std::uint32_t>(0xbeef));
    cSCPropertyHelp::SetPropertyValue(holderB, kCivicPropertyConditionalBuilding, true);

    host.hasKey = true;
    host.keyInstance = 0x555;
    {
        cRZMessage2Standard inserted;
        inserted.SetType(kCivicMessageOccupantInserted);
        inserted.SetVoid1(static_cast<cISC4Occupant*>(buildingA));
        CHECK(sim->DoMessage(&inserted));
        auto* record = sim->GetConditionalBuildingStatus(0x555);
        CHECK(record && record->BuildingCount() == 1);
        CHECK(sim->IsBuildingExcluded(0xbeef));
        CHECK(!sim->IsBuildingExcluded(0x1234));
        // a building whose exemplar key is unknown does not create a record
        host.keyInstance = 0x999;
        inserted.SetVoid1(static_cast<cISC4Occupant*>(buildingB));
        CHECK(sim->DoMessage(&inserted));
        CHECK(sim->GetConditionalBuildingStatus(0x999) == nullptr);
        host.keyInstance = 0x555;
        cRZMessage2Standard removed;
        removed.SetType(kCivicMessageOccupantRemoved);
        removed.SetVoid1(static_cast<cISC4Occupant*>(buildingA));
        CHECK(sim->DoMessage(&removed));
        record = sim->GetConditionalBuildingStatus(0x555);
        CHECK(record && record->BuildingCount() == 0);
        CHECK(!sim->IsBuildingExcluded(0xbeef));
        // a non-building occupant is ignored
        buildingA->type = 5;
        inserted.SetVoid1(static_cast<cISC4Occupant*>(buildingA));
        CHECK(sim->DoMessage(&inserted));
        CHECK(sim->GetConditionalBuildingStatus(0x555)->BuildingCount() == 0);
        buildingA->type = kOccupantTypeBuilding;
        // A removal for a building that was never counted decrements anyway; the 16-bit count wraps (the original
        // stores it as a u16 at +2 and adds 0xffff, 0x1000e9dd8).
        CHECK(sim->DoMessage(&removed));
        CHECK(sim->GetConditionalBuildingStatus(0x555)->BuildingCount() == 0xffff);
        sim->GetConditionalBuildingStatus(0x555)->AddToBuildingCount(1);
        CHECK(sim->GetConditionalBuildingStatus(0x555)->BuildingCount() == 0);
    }

    // ---- DoLotActivated through the lot-state message: budget items, landmark and zoo lists.
    auto* lotA = new FakeLot();
    auto* lotB = new FakeLot();
    lotA->building = buildingA;
    lotB->building = buildingB;
    host.lots[static_cast<cISC4Occupant*>(buildingA)] = lotA;
    host.lots[static_cast<cISC4Occupant*>(buildingB)] = lotB;
    AddBudgetItemProperties(holderA, kCivicDepartment, 0x777);
    AddBudgetItemProperties(holderB, kCivicDepartment, 0);

    const int refsA = buildingA->refs, refsB = buildingB->refs;
    host.messageLot = lotA;
    {
        cRZMessage2Standard lotMessage;
        lotMessage.SetType(kCivicMessageLotStateChanged);
        lotMessage.SetData2(3);
        CHECK(sim->DoMessage(&lotMessage));
        lotMessage.SetData2(2);                                 // occupied, not livable: nothing
        CHECK(sim->DoMessage(&lotMessage));
    }
    CHECK(sim->Landmarks().size() == 1 && sim->Landmarks().front() == static_cast<cISC4Occupant*>(buildingA));
    CHECK(sim->GetLandmarkCount() == 1);
    CHECK(buildingA->refs == refsA + 1);
    CHECK(sim->Zoos().empty());
    {
        cSC4LineItem* line = civic->GetLineItem(0x777);        // the landmark item's line item
        CHECK(line != nullptr);
        if (line) CHECK(line->GetFullExpenses() == 120);
        cSC4LineItem* income = civic->GetLineItem(0x1234);     // the income item uses the building type
        CHECK(income != nullptr);
        if (income) CHECK(income->GetIncome() == 45);
    }

    host.messageLot = lotB;
    {
        cRZMessage2Standard lotMessage;
        lotMessage.SetType(kCivicMessageLotStateChanged);
        lotMessage.SetData2(3);
        CHECK(sim->DoMessage(&lotMessage));
    }
    CHECK(sim->Landmarks().size() == 2 && sim->GetLandmarkCount() == 2);
    CHECK(buildingB->refs == refsB + 2);       // one reference for each list
    CHECK(sim->Zoos().size() == 1 && sim->Zoos().front() == static_cast<cISC4Occupant*>(buildingB));
    {
        std::vector<std::uint32_t> lineItems;
        CHECK(cSCPropertyHelp::GetPropertyValue(holderB, kBudgetItemPropLineItems, lineItems) &&
              lineItems.size() == 3 && lineItems[0] == 0x4321u);   // ChangeBudgetItemLine wrote the type back
        cSC4LineItem* line = civic->GetLineItem(0x4321);
        CHECK(line != nullptr);
        if (line) CHECK(line->GetFullExpenses() == 120 && line->GetIncome() == 45);
    }

    // ---- the registry command "get_landmark_count"
    if (registry.entries.size() == 2) {
        FakeParamSet result;
        CHECK(registry.DoValueCall(FakeRegistry::kRoot + 1, nullptr, &result));
        cIGZVariant* value = result.GetParameter(1);
        CHECK(value != nullptr);
        if (value) {
            std::uint32_t count = 0;
            CHECK(value->GetValUint32(count) && count == 2);
        }
    }

    // ---- the save format: version, tables, then the two list counts
    {
        PortMemoryOStream out;
        CHECK(sim->Write(out));
        const std::vector<unsigned char>& bytes = out.bytes();
        CHECK(bytes.size() > 10 + 8);
        CHECK(bytes[0] == kCivicSaveVersion && bytes[1] == 0);
        CHECK(ReadU32(bytes, 2) == 0);                         // no excluded ids yet
        CHECK(ReadU32(bytes, 6) == 1);                         // one conditional building status (0x555)
        CHECK(ReadU32(bytes, 10) == 0x555u);
        CHECK(ReadU32(bytes, bytes.size() - 8) == 1);          // one zoo
        CHECK(ReadU32(bytes, bytes.size() - 4) == 2);          // two landmarks
    }

    // ---- monthly pass: the landmark department's funding drives the lot condition
    civic->SetFundingPercentage(0.25f, 0, true);
    CHECK(lotA->condition == 0 && lotB->condition == 0);
    {
        cRZMessage2Standard month;
        month.SetType(kCivicMessageMonth);
        CHECK(sim->DoMessage(&month));
    }
    CHECK(lotA->condition == kCivicLotConditionWarning && lotB->condition == kCivicLotConditionWarning);
    CHECK(lotA->setConditionCalls == 1 && lotB->setConditionCalls == 1);
    sim->DoMonthlySimulation();                                // already warned: no further writes
    CHECK(lotA->setConditionCalls == 1 && lotB->setConditionCalls == 1);
    civic->SetFundingPercentage(1.0f, 0, true);
    sim->DoMonthlySimulation();
    CHECK(lotA->condition == kCivicLotConditionNormal && lotB->condition == kCivicLotConditionNormal);
    CHECK(lotA->setConditionCalls == 2 && lotB->setConditionCalls == 2);
    host.lots.clear();                                         // a landmark without a lot is skipped
    sim->DoMonthlySimulation();
    CHECK(lotA->setConditionCalls == 2);
    host.lots[static_cast<cISC4Occupant*>(buildingA)] = lotA;
    host.lots[static_cast<cISC4Occupant*>(buildingB)] = lotB;

    // ---- monthly pass: the 1% ape escape from the zoo list (the generator is seeded from the clock)
    for (int i = 0; i < 5000 && host.escapes == 0; ++i) sim->DoMonthlySimulation();
    CHECK(host.escapes > 0);
    CHECK(host.escapeGroup == kCivicAutomataGroupApeEscape);
    CHECK(host.escapeOccupant == static_cast<cISC4Occupant*>(buildingB));

    // ---- DoBuildingRemoved: the landmark item leaves the budget, the lists drop the building, the income line item
    // goes away once its income reaches zero.
    {
        cRZMessage2Standard removed;
        removed.SetType(kCivicMessageOccupantRemoved);
        removed.SetVoid1(static_cast<cISC4Occupant*>(buildingA));
        CHECK(sim->DoMessage(&removed));
    }
    CHECK(sim->Landmarks().size() == 1 && sim->GetLandmarkCount() == 1);
    // the landmark item leaves the budget through RemoveBudgetItemFromBudget, which takes its cost back out of the line
    // the activation added it to (budget.md 7, 12: removal reverses activation)
    CHECK(!civic->IsLocallyFundedObjectInDepartment(holderA, 0));
    {
        cSC4LineItem* line = civic->GetLineItem(0x777);
        CHECK(line != nullptr && line->GetFullExpenses() == 0);
    }
    CHECK(civic->GetLineItem(0x1234) == nullptr);              // income 45-45 = 0: removed
    CHECK(buildingA->refs == refsA);                           // the list's reference was given back
    {
        cRZMessage2Standard removed;
        removed.SetType(kCivicMessageOccupantRemoved);
        removed.SetVoid1(static_cast<cISC4Occupant*>(buildingB));
        CHECK(sim->DoMessage(&removed));
    }
    CHECK(sim->Landmarks().empty() && sim->Zoos().empty());
    CHECK(buildingB->refs == refsB);
    CHECK(civic->GetLineItem(0x4321) == nullptr);

    // ---- Read: a hand-built version 6 stream with one id, one status record and two empty lists
    auto* loaded = new cSC4CivicBuildingSimulator();
    loaded->AddRef();
    {
        std::vector<unsigned char> bytes;
        auto put16 = [&bytes](std::uint16_t v) { bytes.push_back(static_cast<unsigned char>(v)); bytes.push_back(static_cast<unsigned char>(v >> 8)); };
        auto put32 = [&bytes](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(v >> (8 * i)));
        };
        put16(kCivicSaveVersion);
        put32(1);                                              // one excluded id
        put32(0xbeef);
        put32(1);                                              // one conditional building status
        put32(0xabc);
        bytes.push_back(4);                                    // state byte
        put16(3);                                              // building count
        bytes.push_back(6);                                    // 7-bit varint length, then "reward"
        for (const char* p = "reward"; *p; ++p) bytes.push_back(static_cast<unsigned char>(*p));
        put32(0);                                              // zoos
        put32(0);                                              // landmarks
        PortMemoryIStream in(bytes.data(), bytes.size());
        CHECK(loaded->Read(in));
        CHECK(loaded->IsBuildingExcluded(0xbeef));
        auto* record = loaded->GetConditionalBuildingStatus(0xabc);
        CHECK(record != nullptr);
        if (record) {
            CHECK(record->state == (4u | (3u << 16)));
            CHECK(record->BuildingCount() == 3);
            CHECK(std::string(record->text.ToChar()) == "reward");
        }
        CHECK(loaded->Landmarks().empty() && loaded->Zoos().empty());
        CHECK(loaded->Department() == nullptr);
        // the same bytes with a different version are refused
        bytes[0] = 7;
        PortMemoryIStream bad(bytes.data(), bytes.size());
        CHECK(!loaded->Read(bad));
    }
    loaded->Release();

    // ---- Shutdown
    CHECK(sim->Shutdown());
    CHECK(!sim->IsInitialised());
    CHECK(server.removed.size() == 3);
    CHECK(host.agentsRemoved == 1);
    CHECK(sim->Department() == nullptr);
    CHECK(sim->Landmarks().empty() && sim->Zoos().empty());
    CHECK(registry.removed.size() == 1 && registry.removed[0] == "civic");
    CHECK(sim->Shutdown());                                    // second call is a no-op
    CHECK(sim->Release() == 0);                                // destroys the simulator

    SetCivicBuildingHost(nullptr);
    holderA->Release();
    holderB->Release();
    delete lotA;
    delete lotB;
    delete buildingA;
    delete buildingB;
    budget->Release();
    std::printf(failures ? "FAILED\n" : "sc4_civicbuildingsimulator: all passed\n");
    return failures ? 1 : 0;
}
