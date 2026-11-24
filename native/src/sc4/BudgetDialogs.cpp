#include "sc4/BudgetDialogs.h"

#include "gz/WinSlider.h"
#include "gz/WinSpinner.h"
#include "gz/WinText.h"
#include "rz/cRZString.h"
#include "sc4/DialogWinProcs.h"
#include "sc4/SC4Utilities.h"

namespace sc4 {

namespace {
IBudgetDialogHost* g_host = nullptr;
constexpr std::uint32_t kSenderSlider = 0x887113a3, kSenderButton = 0x287259f6, kMessageSpinner = 0x682876c9;
constexpr std::uint32_t kExpenseScript = 0x691a0fb4, kExpenseWindowId = 0x10000000, kIncomeScript = 0xe91a0fa3;
constexpr std::uint32_t kDepartmentTable[7][4] = {
    {152604492, 769, 533, 534}, {152604482, 770, 535, 536}, {2727754115u, 771, 537, 544}, {687168159, 772, 545, 546},
    {2299575434u, 773, 547, 548}, {3374436074u, 774, 549, 550}, {3374436075u, 775, 551, 552}};
constexpr int kTaxTypes[3] = {1, 4, 10};                         // residential, commercial, industrial
constexpr std::uint32_t kYearlyTexts[3] = {22, 24, 32};

void SetAmount(cIGZWin* window, std::uint32_t id, std::int64_t amount) {
    if (!window) return;
    cIGZWin* control = window->GetChildWindowFromID(id);
    gz::cIGZWinText* text = nullptr;
    if (control && control->QueryInterface(gz::kGZIID_cIGZWinText, reinterpret_cast<void**>(&text))) {
        cRZString money;
        SC4MakeMoneyString(amount, money);
        text->SetCaption(money);
        text->Release();
    }
}
}  // namespace

void SetBudgetDialogHost(IBudgetDialogHost* host) { g_host = host; }

void SC4ShowBudgetWindow(bool expense) {
    gz::cIGZWinProc* window = expense ? static_cast<gz::cIGZWinProc*>(new cSC4BudgetUIExpenseWinProc())
                                      : static_cast<gz::cIGZWinProc*>(new cSC4BudgetUIIncomeWinProc());
    window->AddRef();
    window->Release();
}

// ---- expense dialog --------------------------------------------------------------------------------------

cSC4BudgetUIExpenseWinProc::cSC4BudgetUIExpenseWinProc() {
    for (int i = 0; i < kDepartmentCount; ++i)
        departments_[i] = {kDepartmentTable[i][0], kDepartmentTable[i][1], kDepartmentTable[i][2], kDepartmentTable[i][3], 0, 0, false};
    const cGZPersistResourceKey key{0, kUIScriptGroup, kExpenseScript};
    if (SC4CreateWindowFromScript(key, nullptr, kExpenseWindowId, gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&dialog_))) dialog_->SetWinProc(this);
}

cSC4BudgetUIExpenseWinProc::~cSC4BudgetUIExpenseWinProc() {
    if (budget_) budget_->Release();
    if (dialog_) dialog_->Release();
}

bool cSC4BudgetUIExpenseWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != gz::kGZIID_cIGZWinProc) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4BudgetUIExpenseWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cSC4BudgetUIExpenseWinProc::SetFormattedAmountInTextControl(std::uint32_t id, std::int64_t amount) {
    if (dialog_) SetAmount(dialog_->AsIGZWin(), id, amount);
}

void cSC4BudgetUIExpenseWinProc::DisplayDepartmentInfo(cUIDepartmentInfo& info) {
    if (cIGZWin* control = dialog_->AsIGZWin()->GetChildWindowFromID(info.sliderId)) {
        gz::cIGZWinSlider* slider = nullptr;
        if (control->QueryInterface(gz::kGZIID_cIGZWinSlider, reinterpret_cast<void**>(&slider))) {
            slider->SetValue(static_cast<int>(info.percent));
            slider->Release();
        }
    }
    SetFormattedAmountInTextControl(info.monthlyTextId, info.amount);
    SetFormattedAmountInTextControl(info.yearlyTextId, info.amount * 12);
    RecalculateTotals();
}

void cSC4BudgetUIExpenseWinProc::InitDialog() {
    cSC4BudgetSimulator* budget = g_host->Budget();
    if (budget_ != budget) {
        if (budget) budget->AddRef();
        cSC4BudgetSimulator* old = budget_;
        budget_ = budget;
        if (old) old->Release();
    }
    ordinanceMonthly_ = g_host->OrdinanceMonthlyExpense();
    for (cUIDepartmentInfo& info : departments_) {
        cSC4DepartmentBudget* department = budget_->GetDepartmentBudget(info.departmentId);
        const std::int64_t ideal = department->GetIdealMonthlyFunding();
        const float funding = department->GetFundingPercentage();
        info.changed = false;
        info.percent = static_cast<std::uint32_t>(static_cast<int>(funding * 100.0f));
        info.amount = static_cast<std::int64_t>(static_cast<float>(ideal) * funding);
        DisplayDepartmentInfo(info);
    }
    if (!budget_) {
        SetFormattedAmountInTextControl(0x246, 0);
        SetFormattedAmountInTextControl(0x240, 0);
        SetFormattedAmountInTextControl(0x241, 0);
    } else {
        SetFormattedAmountInTextControl(0x246, budget_->GetTotalFunds());
        SetFormattedAmountInTextControl(0x240, budget_->GetTotalMonthlyIncome());
        SetFormattedAmountInTextControl(0x241, budget_->GetTotalYearlyIncome());
    }
    SetFormattedAmountInTextControl(0x229, ordinanceMonthly_);
    SetFormattedAmountInTextControl(0x230, ordinanceMonthly_ * 12);
    RecalculateTotals();
    if (cIGZWin* apply = dialog_->AsIGZWin()->GetChildWindowFromID(0x101)) apply->SetFlag(2, false);
}

void cSC4BudgetUIExpenseWinProc::SaveChanges() {
    std::int64_t total = 0;
    for (cUIDepartmentInfo& info : departments_) {
        total += info.amount;
        if (info.changed) {
            if (cSC4DepartmentBudget* department = budget_->GetDepartmentBudget(info.departmentId))
                department->SetFundingPercentage(static_cast<float>(static_cast<int>(info.percent)) * 0.01f, 0);
        }
    }
    if (budget_) {
        budget_->SetTotalMonthlyExpense(total + ordinanceMonthly_);
        budget_->SetTotalYearlyExpense((total + ordinanceMonthly_) * 12);
    }
}

void cSC4BudgetUIExpenseWinProc::EndDialog() {
    AddRef();
    if (dialog_) {
        cIGZWin* window = dialog_->AsIGZWin();
        if (cIGZWinMgr* manager = window->GetWindowManager()) manager->DestroyWindow(window);
        gz::cIGZWinGen* dialog = dialog_;
        dialog_ = nullptr;
        dialog->Release();
    }
    if (budget_) {
        cSC4BudgetSimulator* budget = budget_;
        budget_ = nullptr;
        budget->Release();
    }
    Release();
}

void cSC4BudgetUIExpenseWinProc::SetInfoPerSlider(std::uint32_t sliderId) {
    cIGZWin* control = dialog_->AsIGZWin()->GetChildWindowFromID(sliderId);
    gz::cIGZWinSlider* slider = nullptr;
    if (!control || !control->QueryInterface(gz::kGZIID_cIGZWinSlider, reinterpret_cast<void**>(&slider))) return;
    const std::uint32_t value = static_cast<std::uint32_t>(slider->GetValue());
    slider->Release();
    for (cUIDepartmentInfo& info : departments_) {
        if (info.sliderId != sliderId) continue;
        cSC4DepartmentBudget* department = budget_->GetDepartmentBudget(info.departmentId);
        if (!department) return;
        info.changed = true;
        info.percent = value;
        info.amount = static_cast<std::int64_t>(static_cast<std::uint64_t>(department->GetIdealMonthlyFunding()) * value) / 100;
        SetFormattedAmountInTextControl(info.monthlyTextId, info.amount);
        SetFormattedAmountInTextControl(info.yearlyTextId, info.amount * 12);
        RecalculateTotals();
        return;
    }
}

void cSC4BudgetUIExpenseWinProc::RecalculateTotals() {
    std::int64_t total = 0;
    for (const cUIDepartmentInfo& info : departments_) total += info.amount;
    total += ordinanceMonthly_;
    SetFormattedAmountInTextControl(0x242, total);
    SetFormattedAmountInTextControl(0x243, total * 12);
    std::int64_t balance = budget_ ? budget_->GetTotalMonthlyIncome() - total : 0;
    SetFormattedAmountInTextControl(0x244, balance);
    std::int64_t yearly = budget_ ? budget_->GetTotalYearlyIncome() + total * -12 : 0;
    SetFormattedAmountInTextControl(0x245, yearly);
    SetFormattedAmountInTextControl(0x247, (budget_ ? budget_->GetTotalFunds() : 0) + yearly);
}

bool cSC4BudgetUIExpenseWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (dialog_->AsIGZWin() != window) return false;
    if (message.type == kGZMsgCommand) {
        if (message.param1 == kSenderSlider) {
            SetInfoPerSlider(static_cast<std::uint32_t>(message.param2));
        } else if (message.param1 == kSenderButton) {
            if (message.param2 != 0x111) {
                if (message.param2 != 0x102) return false;
                g_host->Announce(kMessageBudgetDialog, 0xc92230ee);
            }
            SaveChanges();
            EndDialog();
            return true;
        }
    } else if (message.type == gz::kWinProcInit) {
        InitDialog();
    }
    return false;
}

// ---- income dialog ---------------------------------------------------------------------------------------

bool cSC4BudgetUIIncomeWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != gz::kGZIID_cIGZWinProc) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4BudgetUIIncomeWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cSC4BudgetUIIncomeWinProc::SetFormattedAmountInTextControl(std::uint32_t id, std::int64_t amount) {
    SetAmount(dialog_ ? dialog_->AsIGZWin() : nullptr, id, amount);
}

void cSC4BudgetUIIncomeWinProc::SetInfoPerSpinner(std::uint32_t spinnerId) {
    cIGZWin* control = dialog_->AsIGZWin()->GetChildWindowFromID(spinnerId);
    gz::cIGZWinSpinner* spinner = nullptr;
    if (!control || !control->QueryInterface(gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) return;
    const std::uint32_t value = static_cast<std::uint32_t>(spinner->GetValue());
    spinner->Release();
    const std::uint32_t index = spinnerId - 0x501;
    if (index > 2) return;
    const int taxType = kTaxTypes[index];
    budget_->SetTaxRate(taxType, static_cast<float>(value));
    const std::int64_t weighted = budget_->GetWeightedAssessedTaxValue(taxType);
    const std::int64_t monthly = static_cast<std::int64_t>(static_cast<std::uint64_t>(weighted) * value) / 1200;
    monthly_[index] = monthly;
    SetFormattedAmountInTextControl(spinnerId - 0x4ec + index, monthly);
    const std::int64_t yearly = static_cast<std::int64_t>(static_cast<std::uint64_t>(weighted) * value) / 100;
    yearly_[index] = yearly;
    SetFormattedAmountInTextControl(kYearlyTexts[index], yearly);
    RecalculateTotals();
}

void cSC4BudgetUIIncomeWinProc::SetInfoPerTaxType(int taxType) {
    std::uint32_t spinnerId, monthlyText, yearlyText;
    if (taxType == 1) { spinnerId = 0x501; monthlyText = 0x15; yearlyText = 0x16; }
    else if (taxType == 10) { spinnerId = 0x503; monthlyText = 0x19; yearlyText = 0x20; }
    else if (taxType == 4) { spinnerId = 0x502; monthlyText = 0x17; yearlyText = 0x18; }
    else return;
    const float rate = budget_->GetTaxRate(taxType);
    if (cIGZWin* control = dialog_ ? dialog_->AsIGZWin()->GetChildWindowFromID(spinnerId) : nullptr) {
        gz::cIGZWinSpinner* spinner = nullptr;
        if (control->QueryInterface(gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) {
            spinner->SetValue(static_cast<int>(static_cast<long>(rate)));
            spinner->Release();
        }
    }
    const float weighted = static_cast<float>(budget_->GetWeightedAssessedTaxValue(taxType));
    SetFormattedAmountInTextControl(monthlyText, static_cast<std::int64_t>((weighted * rate) / 100.0f));
    SetFormattedAmountInTextControl(yearlyText, static_cast<std::int64_t>(((weighted * rate) / 100.0f) * 12.0f));
    RecalculateTotals();
}

cSC4BudgetUIIncomeWinProc::cSC4BudgetUIIncomeWinProc() {
    const cGZPersistResourceKey key{0, kUIScriptGroup, kIncomeScript};
    if (SC4CreateWindowFromScript(key, nullptr, kExpenseWindowId, gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&dialog_))) dialog_->SetWinProc(this);
}

void cSC4BudgetUIIncomeWinProc::InitDialog() {
    ordinanceMonthly_ = ordinanceYearly_ = 0;
    for (std::int64_t& v : monthly_) v = 0;
    for (std::int64_t& v : yearly_) v = 0;
    SetInfoPerTaxType(1);
    SetInfoPerTaxType(4);
    SetInfoPerTaxType(10);
    std::int64_t yearlyExpense = 0;
    if (!budget_) {
        SetFormattedAmountInTextControl(0x38, 0);
        SetFormattedAmountInTextControl(0x34, 0);
    } else {
        SetFormattedAmountInTextControl(0x38, budget_->GetTotalFunds());
        SetFormattedAmountInTextControl(0x34, budget_->GetTotalMonthlyExpense());
        yearlyExpense = budget_->GetTotalYearlyExpense();
    }
    SetFormattedAmountInTextControl(0x35, yearlyExpense);
    if (g_host->HasOrdinanceSimulator()) {
        ordinanceMonthly_ = g_host->OrdinanceMonthlyIncome();
        ordinanceYearly_ = ordinanceMonthly_ * 12;
    }
    SetFormattedAmountInTextControl(0x21, ordinanceMonthly_);
    SetFormattedAmountInTextControl(0x22, ordinanceYearly_);
    RecalculateTotals();
}

void cSC4BudgetUIIncomeWinProc::RecalculateTotals() {
    const std::int64_t total = monthly_[0] + ordinanceMonthly_ + monthly_[1] + monthly_[2];
    if (budget_) budget_->SetTotalMonthlyIncome(total);
    SetFormattedAmountInTextControl(0x32, total);
    if (budget_) budget_->SetTotalYearlyIncome(total * 12);
    SetFormattedAmountInTextControl(0x33, total * 12);
    const std::int64_t monthlyBalance = budget_ ? budget_->GetTotalMonthlyIncome() - budget_->GetTotalMonthlyExpense() : 0;
    SetFormattedAmountInTextControl(0x36, monthlyBalance);
    const std::int64_t yearlyBalance = budget_ ? budget_->GetTotalYearlyIncome() - budget_->GetTotalYearlyExpense() : 0;
    SetFormattedAmountInTextControl(0x37, yearlyBalance);
    SetFormattedAmountInTextControl(0x39, (budget_ ? budget_->GetTotalFunds() : 0) + yearlyBalance);
}

void cSC4BudgetUIIncomeWinProc::EndDialog() {
    AddRef();
    if (dialog_) {
        cIGZWin* window = dialog_->AsIGZWin();
        if (cIGZWinMgr* manager = window->GetWindowManager()) manager->DestroyWindow(window);
    }
    budget_ = nullptr;
    gz::cIGZWinGen* dialog = dialog_;
    dialog_ = nullptr;
    if (dialog) dialog->Release();
    Release();
}

bool cSC4BudgetUIIncomeWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (dialog_->AsIGZWin() != window) return false;
    if (message.type == kMessageSpinner) {
        if (message.param1 - 0x501 < 3) SetInfoPerSpinner(static_cast<std::uint32_t>(message.param1));
    } else if (message.type == kGZMsgCommand) {
        if (message.param1 == kSenderButton) {
            if (message.param2 != 0x107) {
                if (message.param2 != 0x101) return false;
                g_host->Announce(kMessageBudgetDialog, 0x892230f2);
            }
            EndDialog();
            return true;
        }
    } else if (message.type == gz::kWinProcInit) {
        if (cIGZWin* apply = dialog_->AsIGZWin()->GetChildWindowFromID(0x102)) apply->SetFlag(2, false);
        budget_ = g_host->Budget();
        InitDialog();
    }
    return false;
}

}  // namespace sc4
