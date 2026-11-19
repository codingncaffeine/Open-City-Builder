// cSC4ParkManager: department set-up, parks/zoos tracking, monthly run-down switching and zoo escapes.
#include <cstdio>

#include "fake_occupant.h"
#include "sc4/ParkManager.h"

namespace SL { cIGZMessageServer2* spMS2 = nullptr; }
using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct BudgetEnv final : IBudgetSimEnvironment {
    void SimulatorDate(int& y, int& m) override { y = 1; m = 1; }
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
    std::string LoadLocalizedString(std::uint32_t, std::uint32_t) override { return ""; }
    std::string BuildingName(std::uint32_t) override { return ""; }
};
struct Park final : cISC4Occupant {
    std::uint32_t type = 0x278128a0, group = 0; int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
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
    bool IsOccupantGroup(std::uint32_t g) const override { return g == group; }
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
struct Env final : IParkEnvironment {
    cSC4BudgetSimulator* budget = nullptr;
    std::vector<std::pair<cISC4Occupant*, std::uint32_t>> conditions;
    int escapes = 0, agents = 0, removed = 0;
    cSC4BudgetSimulator* Budget() override { return budget; }
    cISCPropertyHolder* TuningExemplar() override { return nullptr; }
    void SetLotCondition(cISC4Occupant* o, std::uint32_t c) override { conditions.push_back({o, c}); }
    void StartZooEscape(cISC4Occupant*) override { ++escapes; }
    void AddMonthlyAgent(cIGZMessageTarget2*) override { ++agents; }
    void RemoveAgent(cIGZMessageTarget2*) override { ++removed; }
};
}  // namespace

int main() {
    BudgetEnv budgetEnv;
    cSC4BudgetSimulator::SetEnvironment(&budgetEnv);
    auto* sim = new cSC4BudgetSimulator();
    sim->AddRef();
    Env env;
    SetParkEnvironment(&env);

    // with a budget simulator the department is created and shared
    env.budget = sim;
    auto* manager = new cSC4ParkManager;
    manager->AddRef();
    CHECK(manager->Init() && env.agents == 1);
    cSC4DepartmentBudget* department = sim->GetDepartmentBudget(kDepartmentParks);
    CHECK(department && department->GetMaxAllowedFundingPercentage() == 1.0f);
    void* iface = nullptr;
    CHECK(manager->QueryInterface(0x452294aa, &iface) && iface == static_cast<cIGZMessageTarget2*>(manager));
    CHECK(manager->GetGZCLSID() == 0xaa3fe6bc);
    manager->Release();                              // count back to 1
    manager->Shutdown();
    CHECK(env.removed == 1);

    manager->Release();
    // without one: tracking and the monthly switch
    env.budget = nullptr;
    auto* m2 = new cSC4ParkManager;
    m2->AddRef();
    m2->Init();
    Park parkA, parkB, zoo, other;
    parkA.group = 0x1006; parkB.group = 0x1006; zoo.group = 0x1702; other.group = 0x1006; other.type = 5;
    m2->DoOccupantInserted(&parkA);
    m2->DoOccupantInserted(&parkB);
    m2->DoOccupantInserted(&zoo);
    m2->DoOccupantInserted(&other);                  // not a building: ignored
    CHECK(m2->Parks().size() == 2 && m2->Zoos().size() == 1 && parkA.refs == 2);
    m2->EscapeCurve().SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {100.0f, 0.0f}});
    m2->DoMonthlySimulation();                       // funding 0 <= threshold: run down
    CHECK(m2->ParksRunDown() && env.conditions.size() == 2 && env.conditions[0].second == 2);
    m2->DoMonthlySimulation();                       // still run down: nothing more
    CHECK(env.conditions.size() == 2 && env.escapes == 0);
    m2->EscapeCurve().SetResponseCurveData(std::vector<SC4Point>{{0.0f, 100.0f}, {100.0f, 100.0f}});
    m2->DoMonthlySimulation();
    CHECK(env.escapes == 1);
    m2->DoOccupantRemoved(&parkA);
    CHECK(m2->Parks().size() == 1 && parkA.refs == 1);
    m2->Shutdown();
    CHECK(parkB.refs == 1 && zoo.refs == 1 && m2->Parks().empty());
    m2->Release();
    {   // the lot lookup goes through the environment; without one there is no lot
        struct LotEnv final : IParkEnvironment {
            int lot = 0;
            cSC4BudgetSimulator* Budget() override { return nullptr; }
            cISCPropertyHolder* TuningExemplar() override { return nullptr; }
            void SetLotCondition(cISC4Occupant*, std::uint32_t) override {}
            void StartZooEscape(cISC4Occupant*) override {}
            void* LotWhereOccupantResides(cISC4Occupant*) override { return &lot; }
            void AddMonthlyAgent(cIGZMessageTarget2*) override {}
            void RemoveAgent(cIGZMessageTarget2*) override {}
        } lotEnv;
        cSC4ParkManager pm;
        IParkEnvironment* previous = ParkEnvironment();
        SetParkEnvironment(nullptr);
        CHECK(pm.GetLotWhereOccupantResides(nullptr) == nullptr);
        SetParkEnvironment(&lotEnv);
        CHECK(pm.GetLotWhereOccupantResides(nullptr) == &lotEnv.lot);
        SetParkEnvironment(previous);
    }
    sim->Release();
    if (failures == 0) std::puts("sc4_parkmanager: all passed");
    return failures ? 1 : 0;
}
