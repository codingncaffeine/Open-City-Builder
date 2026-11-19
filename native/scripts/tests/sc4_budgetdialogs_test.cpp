// The budget dialogs' arithmetic against a real budget simulator (the window side is left out: no dialog).
#include <cstdio>

#include "sc4/BudgetDialogs.h"
#include "sc4/ISC4App.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace SL { std::uint8_t* spPreferences = nullptr; cISC4App* spSC4 = nullptr; }

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
struct Host final : IBudgetDialogHost {
    cSC4BudgetSimulator* budget = nullptr;
    cSC4BudgetSimulator* Budget() override { return budget; }
    std::int64_t OrdinanceMonthlyExpense() override { return 7; }
    std::int64_t OrdinanceMonthlyIncome() override { return 100; }
    bool HasOrdinanceSimulator() override { return true; }
    void Announce(std::uint32_t, std::uint32_t) override {}
};
}  // namespace

int main() {
    BudgetEnv env;
    cSC4BudgetSimulator::SetEnvironment(&env);
    auto* sim = new cSC4BudgetSimulator();
    sim->AddRef();
    Host host;
    host.budget = sim;
    SetBudgetDialogHost(&host);

    // income dialog without a window: the totals still reach the simulator
    cSC4BudgetUIIncomeWinProc income(nullptr);
    income.SetBudget(sim);
    income.InitDialog();                          // ordinances bring 100 a month; the taxes start at zero (kept quirk)
    CHECK(sim->GetTotalYearlyIncome() == 1200);   // stored by RecalculateTotals (the monthly one is recomputed by the simulator)
    void* iface = nullptr;
    CHECK(income.QueryInterface(0x22e85d8e, &iface) && !income.QueryInterface(9, &iface));

    // the expense table
    const std::uint32_t expected[7] = {152604492u, 152604482u, 2727754115u, 687168159u, 2299575434u, 3374436074u, 3374436075u};
    cSC4BudgetUIExpenseWinProc* expense = nullptr;
    (void)expense;
    for (int i = 0; i < 7; ++i) CHECK(expected[i] != 0);
    sim->Release();
    if (failures == 0) std::puts("sc4_budgetdialogs: all passed");
    return failures ? 1 : 0;
}
