// The two budget detail dialogs (cSC4BudgetUIExpenseWinProc 17, cSC4BudgetUIIncomeWinProc 16 functions,
// 0x100242076..0x1002432ec; UI scripts 0x691a0fb4 / window id 0x10000000 for the expense dialog).
//  * Expense: seven department sliders (percent of ideal funding) show their monthly and yearly cost; the totals,
//    income, balance and resulting funds update live; OK stores the chosen funding percentages and the new expense
//    totals in the budget simulator (command 0x102 also announces message 0x29222f02 / 0xc92230ee), 0x111 just stores.
//  * Income: three tax spinners (residential 1, commercial 4, industrial 10) set the tax rates; the income totals go back
//    to the simulator on every change; commands 0x101 (announces 0x29222f02 / 0x892230f2) and 0x107 close it.
// Quirk kept: the income dialog's first display does not add the tax incomes to its totals (they are zero until a
// spinner is touched).
#pragma once
#include <cstdint>

#include "gz/WinGen.h"
#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "sc4/BudgetSimulator.h"

namespace sc4 {

class IBudgetDialogHost {
public:
    virtual ~IBudgetDialogHost() = default;
    virtual cSC4BudgetSimulator* Budget() = 0;                  // spCity->GetBudgetSimulator()
    virtual std::int64_t OrdinanceMonthlyExpense() = 0;         // ordinance simulator vslot 0xb0
    virtual std::int64_t OrdinanceMonthlyIncome() = 0;          // ordinance simulator vslot 0xb8 (0 without one)
    virtual bool HasOrdinanceSimulator() = 0;
    virtual void Announce(std::uint32_t type, std::uint32_t data) = 0;       // message server MessageSend
};
void SetBudgetDialogHost(IBudgetDialogHost* host);

constexpr std::uint32_t kMessageBudgetDialog = 0x29222f02;

class cSC4BudgetUIExpenseWinProc final : public gz::cIGZWinProc {
public:
    struct cUIDepartmentInfo {                      // 0x28 bytes
        std::uint32_t departmentId, sliderId, monthlyTextId, yearlyTextId;
        std::uint32_t percent;                      // +0x10
        std::int64_t amount;                        // +0x18
        bool changed;                               // +0x20
    };
    cSC4BudgetUIExpenseWinProc();
    ~cSC4BudgetUIExpenseWinProc();
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;
    bool DoMessage(cIGZMessage2*) { return true; }

    void InitDialog();
    void SaveChanges();
    void EndDialog();
    void SetInfoPerSlider(std::uint32_t sliderId);
    void DisplayDepartmentInfo(cUIDepartmentInfo& info);
    void SetFormattedAmountInTextControl(std::uint32_t id, std::int64_t amount);
    void RecalculateTotals();
    cUIDepartmentInfo* Departments() { return departments_; }
    static constexpr int kDepartmentCount = 7;

private:
    std::uint32_t refCount_ = 0;                    // cRZUnknown
    gz::cIGZWinGen* dialog_ = nullptr;              // +0x20
    cSC4BudgetSimulator* budget_ = nullptr;         // +0x28
    bool flag_ = true;                              // +0x30
    std::int64_t ordinanceMonthly_ = 0;             // +0x38
    cUIDepartmentInfo departments_[kDepartmentCount];
};

class cSC4BudgetUIIncomeWinProc final : public gz::cIGZWinProc {
public:
    cSC4BudgetUIIncomeWinProc();   // @ 0x001aa526: makes its window from script 0xe91a0fa3 and becomes its proc
    cSC4BudgetUIIncomeWinProc(gz::cIGZWinGen* dialog) : dialog_(dialog) {}
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;

    void InitDialog();
    void EndDialog();
    void SetInfoPerSpinner(std::uint32_t spinnerId);
    void SetInfoPerTaxType(int taxType);
    void SetFormattedAmountInTextControl(std::uint32_t id, std::int64_t amount);
    void RecalculateTotals();
    void SetBudget(cSC4BudgetSimulator* budget) { budget_ = budget; }
    std::int64_t Monthly(int index) const { return monthly_[index]; }

private:
    std::uint32_t refCount_ = 0;
    gz::cIGZWinGen* dialog_ = nullptr;                      // +0x28
    cSC4BudgetSimulator* budget_ = nullptr;         // +0x30
    std::int64_t ordinanceMonthly_ = 0, ordinanceYearly_ = 0;   // +0x38, +0x40
    std::int64_t monthly_[3] = {};                  // +0x48, +0x58, +0x68 (residential, commercial, industrial)
    std::int64_t yearly_[3] = {};                   // +0x50, +0x60, +0x70
};

// cSC4BudgetSimulator::ShowBudgetWindow (i386 0x0018d1ec): the expense or the income budget window; each lives on through
// the window proc its dialog holds.
void SC4ShowBudgetWindow(bool expense);

}  // namespace sc4
