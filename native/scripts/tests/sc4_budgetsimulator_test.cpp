#include <cmath>
#include <cstdio>

#include "rz/PortMemoryStreams.h"
#include "sc4/BudgetSimulator.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Env final : public IBudgetSimEnvironment {
public:
    int year = 2, month = 5, difficulty = 1;
    std::int64_t ordIncome = 100, ordExpense = 40, nbIncome = 7, nbExpense = 3;
    std::vector<LotTaxInfo> lots;
    std::vector<std::pair<std::uint32_t, std::int64_t>> history;
    std::vector<std::uint32_t> messages;
    void SimulatorDate(int& y, int& m) override { y = year; m = month; }
    std::int64_t OrdinanceMonthlyIncome() override { return ordIncome; }
    std::int64_t OrdinanceMonthlyExpense() override { return ordExpense; }
    std::int64_t NeighborsIncome() override { return nbIncome; }
    std::int64_t NeighborsExpense() override { return nbExpense; }
    int DifficultyLevel() override { return difficulty; }
    void ForEachTaxableLot(const std::function<void(const LotTaxInfo&)>& visit) override { for (auto& l : lots) visit(l); }
    void PostMessage(std::uint32_t type, std::uint32_t, std::int64_t, std::int64_t) override { messages.push_back(type); }
    void AddHistory(std::uint32_t id, std::int64_t v) override { history.push_back({id, v}); }
    void PostBudgetMessage(std::uint32_t, cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override {}
    void NotifyLocalFundingChanged(cISCPropertyHolder*) override {}
    std::string LoadLocalizedString(std::uint32_t g, std::uint32_t i) override { return "s" + std::to_string(g) + ":" + std::to_string(i); }
    std::string BuildingName(std::uint32_t t) override { return "b" + std::to_string(t); }
};
}  // namespace

int main() {
    Env env;
    cSC4BudgetSimulator::SetEnvironment(&env);
    auto* sim = new cSC4BudgetSimulator();
    sim->AddRef();
    SetBudgetHost(sim);
    CHECK(sim->GetGZCLSID() == 0xe990be01 && sim->GetSimulatorType() == 0x6772bd23);

    // funds
    sim->SetTotalFunds(1000);
    CHECK(sim->GetTotalFunds() == 1000);
    CHECK(sim->DepositFunds(500) && sim->GetTotalFunds() == 1500);
    CHECK(!sim->WithdrawFunds(2000) && sim->GetTotalFunds() == 1500);
    CHECK(sim->WithdrawFunds(400) && sim->GetTotalFunds() == 1100);
    CHECK(sim->DepositFunds(-100) && sim->GetTotalFunds() == 1000);        // negative deposit withdraws
    CHECK(sim->WithdrawFunds(-50) && sim->GetTotalFunds() == 1050);        // negative withdrawal deposits
    sim->SetTotalFunds(INT64_MAX - 10);
    CHECK(sim->DepositFunds(100) && sim->GetTotalFunds() == INT64_MAX);    // saturates
    sim->SetTotalFunds(1000);

    // taxes through the land value curve (0..255 -> 0..10)
    auto& t = sim->GetTunables();
    t.difficultyScale = {1000.0f, 1200.0f, 1000.0f};
    t.wealthWeights = {1.0f, 0.5f, 0.25f};
    t.factors[2] = 2.0f;
    sim->SetTaxRate(2, 0.5f);
    LotTaxInfo lot;
    lot.taxType = 2; lot.lotSizeX = 2; lot.lotSizeZ = 3; lot.landValue = 255; lot.assessedValue = 40; lot.wealthDelta = 1;
    env.lots.push_back(lot);
    // area = round(3 * 10 * 2) = 60; assessed = 2.0 * (60 + 40) * 0.5 = 100
    const std::int64_t tax = sim->ComputeTax();
    CHECK(sim->GetWeightedAssessedTaxValue(2) == 100);
    // tax = 100 * 0.5 * 1200 / 1200 = 50
    CHECK(tax == 50);
    CHECK(sim->GetTaxIncome(2) == 50);
    CHECK(sim->GetTaxRate(2) == 0.5f && sim->GetTaxRate(12) == 0.0f);

    // bonds
    t.bondIncrement = 1000; t.bondLimit = 10000; t.maxOutstandingBonds = 2; t.loanInterest = 0.12f; t.loanMonths = 12;
    CHECK(!sim->IssueBond(0));
    CHECK(sim->IssueBond(2500));                                           // rounds down to 2000
    CHECK(sim->GetTotalBorrowed() == 2000 && sim->GetTotalFunds() == 3000);
    CHECK(sim->Bonds().size() == 1 && sim->Bonds().front().principal == 2000);
    CHECK(sim->Bonds().front().month == 5 && sim->Bonds().front().year == 3);   // one year ahead, same month
    CHECK(sim->GetMonthlyPaymentForLoan(2000) > 0 && sim->GetMonthlyPaymentForLoan(2000) < 2000);
    CHECK(sim->GetTotalMonthlyBondPayments() == sim->Bonds().front().monthlyPayment);
    CHECK(sim->IssueBond(500));                                            // below the increment -> one increment
    CHECK(!sim->IssueBond(1000));                                          // two bonds outstanding is the limit
    std::vector<cSC4BudgetSimulator::LoanInfo> loans;
    sim->GetAllLoans(loans);
    CHECK(loans.size() == 2 && loans[0].monthsLeft == 0 && loans[0].yearsLeft == 1);

    // departments, groups and budget items
    cSC4DepartmentBudget* police = sim->CreateDepartmentBudget(0x11, 0x5);
    CHECK(sim->CreateDepartmentBudget(0x11, 0x5) == police);
    CHECK(police->GetBudgetGroup() == 5 && police->GetFundingPercentage() == 1.0f);
    CHECK(sim->GetDepartmentBudget(0x11) == police && sim->GetDepartmentBudget(0x12) == nullptr);
    police->SetDepartmentName(0x10, 0x20);
    CHECK(sim->GetDepartmentBudget(std::string("S16")) == police);
    cSC4LineItem* line = police->CreateLineItem(1, false);
    line->SetFullExpenses(200);
    sim->DoBudgetGroupChanged(5);
    std::vector<cSC4BudgetSimulator::BudgetGroupInfo> groups;
    sim->GetAllGroups(groups);
    CHECK(groups.size() == 1 && groups[0].id == 5 && groups[0].expense == 200 && groups[0].name == "s" + std::to_string(0x6a3ff01cu) + ":5");
    std::vector<cSC4DepartmentBudget*> inGroup;
    sim->GetDepartmentBudgetsInGroup(5, inGroup);
    CHECK(inGroup.size() == 1 && inGroup[0] == police);
    inGroup[0]->Release();

    auto* building = new cSCPropertyHolder();
    building->AddRef();
    cSCPropertyHelp::SetPropertyValue(building, kBudgetItemPropDepartments, std::vector<std::uint32_t>{0x11, 0x11});
    cSCPropertyHelp::SetPropertyValue(building, kBudgetItemPropLineItems, std::vector<std::uint32_t>{1, 1});
    cSCPropertyHelp::SetPropertyValue(building, kBudgetItemPropCosts, std::vector<std::int64_t>{300, 50});
    cSCPropertyHelp::SetPropertyValue(building, kBudgetItemPropReserved, std::vector<std::uint32_t>{0xaaaa, 0xbbbb});   // purpose ids
    std::vector<BudgetItem> items;
    CHECK(sim->GetBudgetItemInfo(building, items) && items.size() == 2);
    CHECK(items[0].cost == 300 && items[1].purpose == 1 && std::fabs(items[1].localFunding - 1.0f) < 1e-6f);
    sim->ChangeBudgetItemCost(building, 1, 75);
    sim->ChangeBudgetItemLocalFunding(building, 1, 0.25f);
    BudgetItem item;
    // budget.md 7: the first item whose purpose id matches; the item's index is BudgetItem::purpose
    CHECK(sim->GetBudgetItemForPurpose(building, 0xbbbb, item) && item.purpose == 1 && item.cost == 75 && std::fabs(item.localFunding - 0.25f) < 1e-6f);
    CHECK(!sim->GetBudgetItemForPurpose(building, 1, item));
    CHECK(sim->GetLineItemFromBudgetItem(item) == line);
    // Add / RemoveBudgetItemFromBudget(holder, index) move the item's cost into / out of its line's full expenses; nothing registers
    sim->AddBudgetItemToBudget(building, 0);
    CHECK(!police->IsLocallyFundedObjectInDepartment(building, 0xaaaa) && line->GetFullExpenses() == 500);
    sim->RemoveBudgetItemFromBudget(building, 0);
    CHECK(line->GetFullExpenses() == 200);
    sim->AddBudgetItemToBudget(building, 7);   // out of range: nothing
    CHECK(line->GetFullExpenses() == 200);
    auto* copy = new cSCPropertyHolder();
    copy->AddRef();
    cSC4BudgetSimulator::CopyBudgetItemProperties(building, copy);
    CHECK(sim->GetBudgetItemInfo(copy, items) && items.size() == 2 && items[1].cost == 75 && items[0].cost == 300);
    copy->Release();
    building->Release();

    // monthly roll-up
    env.history.clear();
    env.messages.clear();
    sim->SetTotalFunds(5000);
    const std::int64_t before = sim->GetTotalFunds();
    sim->DoMonthlySimulation();
    CHECK(sim->GetTaxIncome() == 50);
    CHECK(sim->GetYTDIncome() > 0 && sim->GetYTDExpenses() > 0);
    CHECK(env.history.size() == 3 && env.history[2].first == kBudgetHistoryFunds);
    CHECK(sim->GetTotalFunds() != before || sim->GetYTDIncome() == sim->GetYTDExpenses());
    sim->DoDailyHistory();
    CHECK(env.history.size() == 15 && env.history[3].first == 0xaabaaa9f);

    // persistence of the plain parts
    PortMemoryOStream out;
    CHECK(sim->Write(out));
    auto* loaded = new cSC4BudgetSimulator();
    loaded->AddRef();
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(loaded->Read(in));
    CHECK(loaded->GetTotalFunds() == sim->GetTotalFunds() && loaded->GetTotalBorrowed() == sim->GetTotalBorrowed());
    CHECK(loaded->GetTaxRate(2) == 0.5f && loaded->GetWeightedAssessedTaxValue(2) == 100);
    CHECK(loaded->Bonds().size() == sim->Bonds().size());
    loaded->Release();
    SetBudgetHost(nullptr);
    sim->Release();
    std::printf(failures ? "FAILED\n" : "sc4_budgetsimulator: all passed\n");
    return failures ? 1 : 0;
}
