#include "sc4/BudgetUIWinProc.h"

#include <bit>
#include <cstddef>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "gz/WinUtils.h"
#include "rz/LanguageManager.h"
#include "sc4/SC4UI.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SLString.h"

namespace sc4 {

namespace {

IBudgetUIWinProcHost* g_host = nullptr;

// The original's float-to-int conversion: it adds 12582912.0f (1.5 * 2^23, whose bit pattern is 0x4b400000) so the
// integer value lands in the low mantissa bits, reinterprets the float as an int and subtracts that bit pattern
// (0x1002e6d60: addss 12582912.0f, movd, addl 0xb4c00000 = -0x4b400000).
std::int32_t TenthsFromRate(float rate) {
    return std::bit_cast<std::int32_t>(rate * 10.0f + 12582912.0f) - 0x4b400000;
}

}  // namespace

IBudgetUIWinProcHost* BudgetUIWinProcHost() { return g_host; }
void SetBudgetUIWinProcHost(IBudgetUIWinProcHost* host) { g_host = host; }

// ============================================================================================== construction

// @ 0x1002e3966. Builds the script window (id 0xaa3ac000) inside the 3D view window and aligns the windows the script
// created with it; the balance-sheet labels are then filled by InitBalanceSheet.
cSC4BudgetUIWinProc::cSC4BudgetUIWinProc() {
    // The original zeroes +0x20..+0x178 and the tail fields explicitly; savedTaxRates (+0x178..+0x1a7) stays
    // uninitialised (see the header). The member initialisers cover the rest.
    std::set<cIGZWin*> before;
    cIGZWin* view = g_host ? g_host->ViewWindow() : nullptr;
    if (view) nSC4UI::GetWindowChildrenSet(view, before);

    // The original releases a previous window (+0x20) here; a fresh object has none.
    if (window_) {
        gz::cIGZWinGen* previous = window_;
        window_ = nullptr;
        previous->Release();
    }

    gz::cIGZWinGen* created = nullptr;
    const cGZPersistResourceKey key{0, kUIScriptGroup, kBudgetUIWindowScript};
    const bool ok = g_host && g_host->CreateWindowFromScript(key, view, kBudgetUIWindowId, gz::kGZIID_cIGZWinGen,
                                                             reinterpret_cast<void**>(&created));
    window_ = created;
    if (ok && created) {
        if (view) {
            nSC4UI::GetWindowChildrenNotInSet(view, before, before);
            nSC4UI::AlignWindowSetToWindow(before, nullptr);
        }
        created->SetWinProc(this);
        nSC4UI::ClearAcceptFocusFlagRecursive(created->AsIGZWin());
        InitBalanceSheet();
    }
}

// @ 0x1002e4776. Releases the sub-budget window, the budget simulator and the script window (in that order).
cSC4BudgetUIWinProc::~cSC4BudgetUIWinProc() {
    if (subBudget_) {
        ISubBudgetWindow* sub = subBudget_;
        subBudget_ = nullptr;
        sub->Release();
    }
    if (budget_) {
        cSC4BudgetSimulator* budget = budget_;
        budget_ = nullptr;
        budget->Release();
    }
    if (window_) {
        gz::cIGZWinGen* window = window_;
        window_ = nullptr;
        window->Release();
    }
}

// ============================================================================================== COM

// @ 0x1002e4ae6: 0x22e85d8e -> the cIGZWinProc view, 0x452294aa -> the message target, everything else unknown.
bool cSC4BudgetUIWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == gz::kGZIID_cIGZWinProc) {
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else if (iid == kBudgetUIMessageTargetIid) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return cRZUnknown::QueryInterface(iid, out);
    }
    AddRef();
    return true;
}

// ============================================================================================== helpers

// The original re-reads window->AsIGZWin()->GetParentWin() at every control lookup.
cIGZWin* cSC4BudgetUIWinProc::Controls() const {
    if (!window_) return nullptr;
    cIGZWin* self = window_->AsIGZWin();
    return self ? self->GetParentWin() : nullptr;
}

// The original uses cIGZWin vslot 0x118, i.e. GetChildWindowFromIDRecursive.
cIGZWin* cSC4BudgetUIWinProc::Control(std::uint32_t id) const {
    cIGZWin* root = Controls();
    return root ? root->GetChildWindowFromIDRecursive(id) : nullptr;
}

// Adopts SL::spCity vslot 0x1a0 (one reference) when +0x28 is still empty.
void cSC4BudgetUIWinProc::EnsureBudget() {
    if (budget_) return;
    if (!g_host) return;
    cSC4BudgetSimulator* budget = g_host->Budget();
    cSC4BudgetSimulator* previous = budget_;
    if (previous == budget) return;
    if (budget) budget->AddRef();
    budget_ = budget;
    if (previous) previous->Release();
}

// cIGZWin vslot 0x250 (SetCaption) on the (recursive) child of that id.
void cSC4BudgetUIWinProc::SetCaptionById(std::uint32_t id, const cIGZString& caption) {
    if (cIGZWin* control = Control(id)) control->SetCaption(caption);
}

// cIGZWin vslot 0x220 (SetFlag 1 = visible).
void cSC4BudgetUIWinProc::SetVisibleById(std::uint32_t id, bool visible) {
    if (cIGZWin* control = Control(id)) control->SetFlag(kWinVisible, visible);
}

// cIGZWin vslot 0x220 (SetFlag 2 = enabled).
void cSC4BudgetUIWinProc::SetEnabledById(std::uint32_t id, bool enabled) {
    if (cIGZWin* control = Control(id)) control->SetFlag(kWinEnabled, enabled);
}

// GetChildAsRecursive(id, cIGZWinText) then cIGZWinText vslot 0x58 (SetTextColor(index, native)).
void cSC4BudgetUIWinProc::SetTextColorById(std::uint32_t id, std::uint32_t color) {
    cIGZWin* root = Controls();
    if (!root) return;
    gz::cIGZWinText* text = nullptr;
    if (!root->GetChildAsRecursive(id, gz::kGZIID_cIGZWinText, reinterpret_cast<void**>(&text))) return;
    text->SetTextColor(gz::cIGZWinText::kColorText, color);
    text->Release();
}

// GetChildAsRecursive(id, cIGZWinBtn) then cIGZWinBtn vslot 6 (ToggleOn).
void cSC4BudgetUIWinProc::SetButtonOn(std::uint32_t id) {
    cIGZWin* root = Controls();
    if (!root) return;
    gz::cIGZWinBtn* button = nullptr;
    if (!root->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&button))) return;
    button->ToggleOn();
    button->Release();
}

// cIGZWinBtn vslot 7 (ToggleOff).
void cSC4BudgetUIWinProc::SetButtonOff(std::uint32_t id) {
    cIGZWin* root = Controls();
    if (!root) return;
    gz::cIGZWinBtn* button = nullptr;
    if (!root->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&button))) return;
    button->ToggleOff();
    button->Release();
}

// ============================================================================================== messages

// @ 0x1002e4b80.
bool cSC4BudgetUIWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (!window_ || window_->AsIGZWin() != window) return false;

    if (message.type == kBudgetUITextEditKillFocus) return HandleTextEditKillFocus(message);
    if (message.type == kBudgetUITextEditKey) return HandleTextEditKey(message);
    if (message.type == gz::kWinProcInit) {
        RegisterMessageNotification(true);
        return false;
    }
    if (message.type == gz::kWinProcShutdown) {
        if (notificationsRegistered_) RegisterMessageNotification(false);
        return false;
    }
    if (message.type == kBudgetUISpinnerMessage) return HandleSpinnerMessage(message);
    if (message.type == kGZMsgKeyDown) {
        // The panel swallows the key while a modal dialog is up (WM vslot 0x150).
        return g_host && g_host->WindowManagerIsModal();
    }
    if (message.type != kGZMsgCommand) return false;
    if (!g_host || !g_host->FilterUICommandMessage(message)) return true;
    if (static_cast<std::uint32_t>(message.param1) != kBudgetUISenderButton) return false;

    const std::uint32_t command = static_cast<std::uint32_t>(message.param2);
    if (command >= kBudgetUICommandPageBase && command <= kBudgetUICommandPageBase + 0xf)
        return HandlePageCommand(command - kBudgetUICommandPageBase);
    if (command >= kBudgetUICommandPageBase2 && command <= kBudgetUICommandPageBase2 + 0xf)
        return HandlePageCommand(command - kBudgetUICommandPageBase2);

    switch (command) {
        case kBudgetUIExpensePageCommand:
            expenseView_ = true;
            ShowWindow();
            return false;
        case kBudgetUIIncomePageCommand:
            expenseView_ = false;
            ShowWindow();
            return false;
        case kBudgetUIMonthlyButtonA:
        case kBudgetUIMonthlyButtonB:
            SetMonthlyPeriod();
            return false;
        case kBudgetUIYearlyButtonA:
        case kBudgetUIYearlyButtonB:
            SetYearlyPeriod();
            return false;
        case kBudgetUIHideCommand:
        case kBudgetUIHideCommand2:
            HideWindow();
            return false;
        case kBudgetUILoanCloseCommand:
            RemoveLoanWindow();
            return false;
        case kBudgetUILoanUpCommand:
            if (loanScroll_ != 0) loanScroll_ -= 2;
            UpdateLoanDisplay();
            return false;
        case kBudgetUILoanDownCommand:
            if (static_cast<std::uint32_t>(loanCount_ - 4) > static_cast<std::uint32_t>(loanScroll_)) loanScroll_ += 2;
            UpdateLoanDisplay();
            return false;
        case kBudgetUITakeLoanCommand:
            TakeLoan();
            return false;
        case kBudgetUITaxesOkCommand:
            RemoveTaxesWindow();
            return false;
        case kBudgetUITaxesCancelCommand:
            CancelTaxesWindow();
            RemoveTaxesWindow();
            return false;
        default:
            return false;
    }
}

// The 0xaa3ac400 + n / 0xaa3ac500 + n family (n = 0..0xf).
bool cSC4BudgetUIWinProc::HandlePageCommand(std::uint32_t relativeId) {
    switch (relativeId) {
        case 0x0: DisplayTaxesWindow(); return false;
        case 0x1: case 0x9: DoSubBudgetWindow(1); return false;
        case 0x2: case 0xa: DoSubBudgetWindow(2); return false;
        case 0x3: DoSubBudgetWindow(kBudgetGroupUtilities); return false;
        case 0x4: case 0x5: DoSubBudgetWindow(kBudgetGroupTransport); return false;
        case 0x6: DoSubBudgetWindow(kBudgetGroupGroup3); return false;
        case 0x7: DoSubBudgetWindow(kBudgetGroupGroup4); return false;
        case 0x8: DoSubBudgetWindow(kBudgetGroupGroup5); return false;
        case 0xb: DoSubBudgetWindow(kBudgetGroupParks); return false;
        case 0xc: DoSubBudgetWindow(kBudgetGroupCivic2); return false;
        case 0xd: DisplayLoanWindow(); return false;
        case 0xe:
            expenseView_ = true;
            ShowWindow();
            return false;
        case 0xf:
            expenseView_ = false;
            ShowWindow();
            return false;
        default: return false;
    }
}

// The 0x682876c9 spinner notification: the loan amount spinner and the twelve tax spinners.
bool cSC4BudgetUIWinProc::HandleSpinnerMessage(const cGZMessage& message) {
    EnsureBudget();
    if (!budget_) return false;
    const std::uint32_t controlId = static_cast<std::uint32_t>(message.param1);
    const std::int64_t value = static_cast<std::int64_t>(message.param2);

    if (controlId == kBudgetUILoanSpinner) {
        SetFormattedAmountInTextControl(kBudgetUILoanAmountText, value);
        // Quirk: the yearly label multiplies the *payment* by 12 here; DisplayLoanWindow multiplies the amount.
        const std::int64_t payment = budget_->GetMonthlyPaymentForLoan(value);
        SetFormattedAmountInTextControl(kBudgetUILoanPaymentText, period_ == 0 ? payment : payment * 12);
        currentLoanAmount_ = static_cast<std::int32_t>(value);
    }

    if (controlId - kBudgetUITaxSpinnerBase < kBudgetUITaxCount) {
        const std::uint64_t unsignedValue = static_cast<std::uint64_t>(value);
        const std::uint64_t whole = unsignedValue / 10;
        const std::int32_t remainder = static_cast<std::int32_t>(static_cast<std::uint32_t>(value)) -
                                       static_cast<std::int32_t>(static_cast<std::uint32_t>(whole * 10));
        cRZString percentage;
        PrintPercentageString(static_cast<int>(whole), remainder, percentage);
        cIGZWin* root = Controls();
        if (root) {
            gz::cIGZWinTextEdit* edit = nullptr;
            if (root->GetChildAsRecursive(controlId - kBudgetUITaxEditBase, gz::kGZIID_cIGZWinTextEdit,
                                          reinterpret_cast<void**>(&edit))) {
                edit->SetText(percentage, false);
                edit->ScrollToStart();
                edit->Release();
            }
        }
        budget_->SetTaxRate(static_cast<int>(controlId - kBudgetUITaxSpinnerBase), static_cast<float>(value) / 10.0f);
        SetTaxes();
    }
    return false;
}

// 0x9b1552d5 (cIGZWinTextEdit key): Return selects all of the next tax field and focuses it.
bool cSC4BudgetUIWinProc::HandleTextEditKey(const cGZMessage& message) {
    if (message.param2 != kBudgetUIReturnKey) return false;
    gz::cIGZWinTextEdit* edit = reinterpret_cast<gz::cIGZWinTextEdit*>(static_cast<std::uintptr_t>(message.param1));
    if (!edit) return false;   // port guard: the original dereferences the key message's view unconditionally
    cIGZWin* root = Controls();
    cIGZWin* taxes = root ? root->GetChildWindowFromIDRecursive(kBudgetUITaxesWindowId) : nullptr;
    if (!taxes) return false;
    const std::int32_t editId = static_cast<std::int32_t>(edit->AsIGZWin()->GetID());
    const std::int32_t nextId = ((editId - 0xff) % static_cast<std::int32_t>(kBudgetUITaxCount)) + 0x100;
    gz::cIGZWinTextEdit* next = nullptr;
    if (taxes->GetChildAsRecursive(static_cast<std::uint32_t>(nextId), gz::kGZIID_cIGZWinTextEdit,
                                   reinterpret_cast<void**>(&next))) {
        next->SetSelection(0, next->GetTextLength());
        next->ScrollToStart();
        if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) manager->GZSetFocus(next->AsIGZWin());
        next->Release();
    }
    return false;
}

// 0x9b1552d9 (cIGZWinTextEdit lost the focus): parse the typed percentage, otherwise keep the current rate, and write
// the normalised text back.
bool cSC4BudgetUIWinProc::HandleTextEditKillFocus(const cGZMessage& message) {
    gz::cIGZWinTextEdit* edit = reinterpret_cast<gz::cIGZWinTextEdit*>(static_cast<std::uintptr_t>(message.param1));
    if (!edit) return false;   // port guard: the original dereferences the view unconditionally
    cIGZWin* root = Controls();
    cIGZWin* taxes = root ? root->GetChildWindowFromIDRecursive(kBudgetUITaxesWindowId) : nullptr;
    if (!taxes) return false;
    cRZString text;
    edit->GetText(text);
    text.Trim();
    const std::uint32_t editId = edit->AsIGZWin()->GetID();
    gz::cIGZWinSpinner* spinner = nullptr;
    if (!taxes->GetChildAsRecursive(editId + kBudgetUITaxEditBase, gz::kGZIID_cIGZWinSpinner,
                                    reinterpret_cast<void**>(&spinner)))
        return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.ToChar(), &end);
    float rate;
    if (end == text.ToChar() + text.Strlen()) {
        rate = static_cast<float>(parsed);
    } else {
        const std::uint32_t index = editId - kBudgetUITaxEditBase;
        const std::int32_t taxType = index < kBudgetUITaxCount ? kBudgetUITaxTypeTable[index] : 0;   // original: no guard
        rate = budget_ ? budget_->GetTaxRate(taxType) : 0.0f;
    }
    spinner->SetValue(TenthsFromRate(rate));
    const int value = spinner->GetValue();
    cRZString percentage;
    PrintPercentageString(value / 10, value % 10, percentage);
    edit->SetText(percentage, false);
    edit->ScrollToStart();
    spinner->Release();
    return false;
}

// @ 0x1002e7eb8: the budget messages only mark the panel dirty; the animation tick refreshes it.
bool cSC4BudgetUIWinProc::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type - kBudgetUIMsgFundsChanged < 2u) {
        pendingUpdate_ = true;
    } else if (type == kBudgetUIMsgAnimationTick && pendingUpdate_) {
        pendingUpdate_ = false;
        UpdateBalanceSheetInfo();
    }
    return true;
}

// @ 0x1002e8ba2.
void cSC4BudgetUIWinProc::Update() {
    if (visible_) pendingUpdate_ = true;
}

// @ 0x1002e489c: subscribe / unsubscribe the three message types and the animation tick channel.
void cSC4BudgetUIWinProc::RegisterMessageNotification(bool on) {
    if (!g_host) return;
    cIGZMessageServer2* messages = g_host->Messages();
    if (on) {
        if (notificationsRegistered_) return;
        notificationsRegistered_ = true;
        if (messages) {
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUIMsgFundsChanged2);
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUIMsgFundsChanged);
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUIMsgAnimationTick);
        }
        g_host->AddTickNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUITickChannel);
    } else {
        if (!notificationsRegistered_) return;
        notificationsRegistered_ = false;
        if (messages) {
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUIMsgFundsChanged2);
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUIMsgFundsChanged);
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kBudgetUIMsgAnimationTick);
        }
        g_host->RemoveTickNotification(static_cast<cIGZMessageTarget2*>(this));
    }
}

// @ 0x1002e4872.
void cSC4BudgetUIWinProc::Shutdown() {
    if (notificationsRegistered_) RegisterMessageNotification(false);
    EndDialog();
}

// @ 0x1002e4984: detach the window procedure, destroy the four script windows and release everything. The final
// Release drops the caller's reference, so the object frees itself.
void cSC4BudgetUIWinProc::EndDialog() {
    AddRef();
    if (window_) window_->SetWinProc(nullptr);
    cIGZWin* root = window_ ? window_->AsIGZWin()->GetParentWin() : nullptr;
    cIGZWinMgr* manager = window_ ? window_->AsIGZWin()->GetWindowManager() : nullptr;
    if (manager) {
        if (root) {
            for (std::uint32_t id : {kBudgetUIWindowId, kBudgetUIYearlyPageId, kBudgetUITaxesWindowId,
                                     kBudgetUILoanWindowId}) {
                if (cIGZWin* control = root->GetChildWindowFromIDRecursive(id)) manager->DestroyWindow(control);
            }
        }
        if (window_) manager->DestroyWindow(window_->AsIGZWin());
    }
    if (window_) {
        gz::cIGZWinGen* created = window_;
        window_ = nullptr;
        created->Release();
    }
    if (budget_) {
        cSC4BudgetSimulator* budget = budget_;
        budget_ = nullptr;
        budget->Release();
    }
    if (subBudget_) {
        ISubBudgetWindow* sub = subBudget_;
        subBudget_ = nullptr;
        sub->Release();
    }
    RegisterMessageNotification(false);
    Release();
}

// ============================================================================================== init / balance sheet

// @ 0x1002e7f18.
void cSC4BudgetUIWinProc::Init() {
    if (!window_) return;   // port guard: the original dereferences the window
    cIGZWin* self = window_->AsIGZWin();
    cIGZWin* root = self ? self->GetParentWin() : nullptr;
    if (!root) return;
    cIGZWin* yearlyPage = root->GetChildWindowFromIDRecursive(kBudgetUIYearlyPageId);
    cIGZWin* taxes = root->GetChildWindowFromIDRecursive(kBudgetUITaxesWindowId);
    (void)yearlyPage;
    SetVisibleById(kBudgetUIYearlyPageId, false);
    SetVisibleById(kBudgetUIWindowId, false);
    SetVisibleById(kBudgetUITaxesWindowId, false);
    SetVisibleById(kBudgetUILoanWindowId, false);

    // The controls whose commands the panel handles, plus the loan dialog's buttons and spinner.
    static constexpr std::uint32_t kTargets[] = {
        0xaa3ac40e, 0xaa3ac50e, 0x4a8879cb, 0xaa3ac40f, 0xaa3ac50f, 0x4a8879db, 0x4a8879cc, 0x4a8879cd,
        0x4a8879dc, 0x4a8879dd,
        0xaa3ac400, 0xaa3ac401, 0xaa3ac402, 0xaa3ac403, 0xaa3ac404, 0xaa3ac405, 0xaa3ac406, 0xaa3ac407,
        0xaa3ac408, 0xaa3ac409, 0xaa3ac40a, 0xaa3ac40b, 0xaa3ac40c, 0xaa3ac40d,
        0xaa3ac500, 0xaa3ac501, 0xaa3ac502, 0xaa3ac503, 0xaa3ac504, 0xaa3ac505, 0xaa3ac506, 0xaa3ac507,
        0xaa3ac508, 0xaa3ac509, 0xaa3ac50a, 0xaa3ac50b, 0xaa3ac50c, 0xaa3ac50d,
        0x6a5bb0fb, 0x4a5bb1b5, 0xaa4c353b, 0xca4c352f,
        0xa4c3354, 0xaa4c33e6, 0x8a4c34d4, 0x2a4c33a6, 0xaa4c339c};
    for (std::uint32_t id : kTargets) nsGZWinUtils::SetWindowNotificationTarget(root, id, self);

    // The tax dialog's twelve spinners (0x200 + type) and twelve text edits (0x100 + type).
    if (taxes) {
        for (std::uint32_t type = 0; type != kBudgetUITaxCount; ++type) {
            nsGZWinUtils::SetWindowNotificationTarget(taxes, type + kBudgetUITaxSpinnerBase, self);
            nsGZWinUtils::SetWindowNotificationTarget(taxes, type + kBudgetUITaxEditBase, self);
        }
    }

    SetButtonOn(kBudgetUIMonthlyButtonA);
    SetButtonOff(kBudgetUIYearlyButtonA);
    SetButtonOn(kBudgetUIMonthlyButtonB);
    SetButtonOff(kBudgetUIYearlyButtonB);
    period_ = 0;

    gz::cIGZWinSpinner* spinner = nullptr;
    if (root->GetChildAsRecursive(kBudgetUILoanSpinner, gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) {
        SetFormattedAmountInTextControl(kBudgetUILoanAmountText, spinner->GetValue());
        spinner->Release();
    }
}

// @ 0x1002e3b4e: the static labels of the balance sheet (the monthly or the yearly string set) and, when the script
// provides it, the four second-column labels.
void cSC4BudgetUIWinProc::InitBalanceSheet() {
    if (!Controls()) return;
    const std::uint32_t* instances = period_ == 0 ? kBudgetUIMonthlyLabelStrings : kBudgetUIYearlyLabelStrings;
    constexpr std::size_t count = sizeof(kBudgetUILabelIds) / sizeof(kBudgetUILabelIds[0]);
    constexpr std::size_t secondColumn = 14;   // the labels 0xaa3ad200..0xaa3ad203
    for (std::size_t i = 0; i < secondColumn; ++i) {
        cSLString caption(kBudgetUILabelStringGroup, instances[i], true);
        SetCaptionById(kBudgetUILabelIds[i], caption);
    }
    if (!Control(kBudgetUILabelIds[secondColumn])) return;
    for (std::size_t i = secondColumn; i < count; ++i) {
        cSLString caption(kBudgetUILabelStringGroup, instances[i], true);
        SetCaptionById(kBudgetUILabelIds[i], caption);
    }
}

// @ 0x1002e58bc: the cIGZWinText control of that id gets the formatted money string.
void cSC4BudgetUIWinProc::SetFormattedAmountInTextControl(std::uint32_t id, std::int64_t amount) {
    cIGZWin* control = Control(id);
    if (!control) return;
    gz::cIGZWinText* text = nullptr;
    if (!control->QueryInterface(gz::kGZIID_cIGZWinText, reinterpret_cast<void**>(&text))) return;
    cRZString money;
    SC4MakeMoneyString(amount, money);
    text->SetCaption(money);
    text->Release();
}

// @ 0x1002e59b2: "<whole><decimal separator><fraction>" through RZ::LM() -> GetLanguageUtility(0) -> vslot 0x78.
void cSC4BudgetUIWinProc::PrintPercentageString(int whole, int fraction, cRZString& out) {
    cRZString separator;
    if (g_host) g_host->DecimalSeparator(separator);
    out.Sprintf("%d%s%d", whole, separator.ToChar(), fraction);
}

// ============================================================================================== calculation

// @ 0x1002e8674.
void cSC4BudgetUIWinProc::CalculateValues() {
    EnsureBudget();
    if (!budget_) {
        values_ = Values{};
        return;
    }
    values_.funds = budget_->GetTotalFunds();
    values_.monthlyIncome = budget_->GetTotalMonthlyIncome();
    values_.estimatedIncome = budget_->GetEstIncome();
    values_.monthlyExpense = budget_->GetTotalMonthlyExpense();
    values_.estimatedExpense = budget_->GetEstExpenses();

    const std::int64_t tax1 = budget_->GetTaxIncome(0) + budget_->GetTaxIncome(1) + budget_->GetTaxIncome(2);
    values_.taxGroup1 = tax1;
    values_.taxGroup1Yearly = tax1 * 12;
    const std::int64_t tax2 = budget_->GetTaxIncome(3) + budget_->GetTaxIncome(4) + budget_->GetTaxIncome(5) +
                              budget_->GetTaxIncome(6) + budget_->GetTaxIncome(7);
    values_.taxGroup2 = tax2;
    values_.taxGroup2Yearly = tax2 * 12;
    const std::int64_t tax3 = budget_->GetTaxIncome(8) + budget_->GetTaxIncome(9) + budget_->GetTaxIncome(10) +
                              budget_->GetTaxIncome(11);
    values_.taxGroup3 = tax3;
    values_.taxGroup3Yearly = tax3 * 12;

    values_.ordinanceIncome = 0;
    values_.ordinanceExpense = 0;
    if (IOrdinanceBudgetApi* ordinances = g_host ? g_host->Ordinances() : nullptr) {
        std::uint32_t count = ordinances->AvailableCount();
        std::vector<std::uint32_t> ids(count);
        ordinances->AvailableIds(ids.data(), count);
        for (std::uint32_t i = 0; i < count; ++i) {
            IOrdinanceBudgetItem* ordinance = ordinances->OrdinanceById(ids[i]);
            const int income = ordinance ? static_cast<int>(ordinance->CurrentMonthlyIncome()) : 0;
            if (!ordinances->IsOrdinanceOn(ids[i])) continue;
            if (income < 0) values_.ordinanceExpense += -static_cast<std::int64_t>(income);
            else values_.ordinanceIncome += income;
        }
    }
    values_.ordinanceIncomeYearly = values_.ordinanceIncome * 12;
    values_.ordinanceExpenseYearly = values_.ordinanceExpense * 12;

    if (INeighborsBudgetApi* neighbors = g_host ? g_host->Neighbors() : nullptr) {
        values_.neighborsIncome = neighbors->MonthlyDealIncome();
        values_.neighborsExpense = neighbors->MonthlyDealExpense();
    }
    values_.neighborsIncomeYearly = values_.neighborsIncome * 12;
    values_.neighborsExpenseYearly = values_.neighborsExpense * 12;

    std::vector<cSC4BudgetSimulator::BudgetGroupInfo> groups;
    budget_->GetAllGroups(groups);
    for (const cSC4BudgetSimulator::BudgetGroupInfo& group : groups) {
        switch (group.id) {
            case kBudgetGroupUtilities:
                values_.groupIncome1 = group.income;
                values_.groupIncome1Yearly = group.income * 12;
                break;
            case kBudgetGroupTransport:
                values_.groupIncome2 = group.income;
                values_.groupIncome2Yearly = group.income * 12;
                values_.groupExpense2 = group.expense;
                values_.groupExpense2Yearly = group.expense * 12;
                break;
            case kBudgetGroupGroup3:
                values_.groupExpense3 = group.expense;
                values_.groupExpense3Yearly = group.expense * 12;
                break;
            case kBudgetGroupGroup4:
                values_.groupExpense4 = group.expense;
                values_.groupExpense4Yearly = group.expense * 12;
                break;
            case kBudgetGroupGroup5:
                values_.groupExpense5 = group.expense;
                values_.groupExpense5Yearly = group.expense * 12;
                break;
            case kBudgetGroupParks:
                values_.groupExpense6 = group.expense;
                values_.groupExpense6Yearly = group.expense * 12;
                break;
            case kBudgetGroupCivic2:
            case kBudgetGroupCivic2Alt:
                values_.groupExpense7 = group.expense;
                values_.groupExpense7Yearly = group.expense * 12;
                break;
            default:
                break;
        }
    }

    values_.bondPayments = budget_->GetTotalMonthlyBondPayments();
    values_.bondPaymentsYearly = values_.bondPayments * 12;
}

// @ 0x1002e5a70: the twelve per-type tax incomes into 0x300 + type and the three group totals.
void cSC4BudgetUIWinProc::SetTaxes() {
    if (!budget_) return;
    const int factor = period_ == 0 ? 1 : 12;
    std::uint32_t total = 0;
    for (int type = 0; type < 3; ++type) {
        const int income = static_cast<int>(budget_->GetTaxIncome(type));
        SetFormattedAmountInTextControl(static_cast<std::uint32_t>(0x300 + type), static_cast<std::uint32_t>(income * factor));
        total += static_cast<std::uint32_t>(income * factor);
    }
    SetFormattedAmountInTextControl(0x500, total);
    total = 0;
    for (int type = 3; type < 8; ++type) {
        const int income = static_cast<int>(budget_->GetTaxIncome(type));
        SetFormattedAmountInTextControl(static_cast<std::uint32_t>(0x300 + type), static_cast<std::uint32_t>(income * factor));
        total += static_cast<std::uint32_t>(income * factor);
    }
    SetFormattedAmountInTextControl(0x501, total);
    total = 0;
    for (int type = 8; type < 12; ++type) {
        const int income = static_cast<int>(budget_->GetTaxIncome(type));
        SetFormattedAmountInTextControl(static_cast<std::uint32_t>(0x300 + type), static_cast<std::uint32_t>(income * factor));
        total += static_cast<std::uint32_t>(income * factor);
    }
    SetFormattedAmountInTextControl(0x502, total);
}

// @ 0x1002e5b7e.
void cSC4BudgetUIWinProc::UpdateBalanceSheetInfo() {
    CalculateValues();
    const std::uint32_t red = g_host ? g_host->MakeNativeColor(0x77, 0, 0) : 0;
    const std::uint32_t normal = g_host ? g_host->MakeNativeColor(0x44, 0x55, 0x66) : 0;

    if (period_ != 0) {
        if (!expenseView_) {
            SetFormattedAmountInTextControl(kBudgetUIFundsText, values_.funds);
            SetFormattedAmountInTextControl(kBudgetUIMonthlyIncomeText, values_.estimatedIncome);
            SetFormattedAmountInTextControl(kBudgetUIMonthlyExpenseText, values_.estimatedExpense);
            SetFormattedAmountInTextControl(kBudgetUIBalanceText,
                                            values_.funds + values_.estimatedIncome - values_.estimatedExpense);
        } else {
            SetFormattedAmountInTextControl(kBudgetUIFundsTextExpense, values_.funds);
            SetFormattedAmountInTextControl(kBudgetUIIncomeTextExpense, values_.estimatedIncome);
            SetFormattedAmountInTextControl(kBudgetUIExpenseTextExpense, values_.estimatedExpense);
            SetFormattedAmountInTextControl(kBudgetUIBalanceTextExpense,
                                            values_.funds + values_.estimatedIncome - values_.estimatedExpense);
            SetFormattedAmountInTextControl(kBudgetUITaxGroup1Text, values_.taxGroup1Yearly);
            SetFormattedAmountInTextControl(kBudgetUITaxGroup2Text, values_.taxGroup2Yearly);
            SetFormattedAmountInTextControl(kBudgetUITaxGroup3Text, values_.taxGroup3Yearly);
            SetFormattedAmountInTextControl(kBudgetUIOrdinanceIncomeText, values_.ordinanceIncomeYearly);
            SetFormattedAmountInTextControl(kBudgetUIOrdinanceExpenseText, values_.ordinanceExpenseYearly);
            SetFormattedAmountInTextControl(kBudgetUINeighborsIncomeText, values_.neighborsIncomeYearly);
            SetFormattedAmountInTextControl(kBudgetUINeighborsExpenseText, values_.neighborsExpenseYearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup1IncomeText, values_.groupIncome1Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup2IncomeText, values_.groupIncome2Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup2ExpenseText, values_.groupExpense2Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup3ExpenseText, values_.groupExpense3Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup4ExpenseText, values_.groupExpense4Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup5ExpenseText, values_.groupExpense5Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup6ExpenseText, values_.groupExpense6Yearly);
            SetFormattedAmountInTextControl(kBudgetUIGroup7ExpenseText, values_.groupExpense7Yearly);
            SetFormattedAmountInTextControl(kBudgetUIBondText, values_.bondPaymentsYearly);
        }
        return;
    }

    SetFormattedAmountInTextControl(kBudgetUIFundsText, values_.funds);
    SetFormattedAmountInTextControl(kBudgetUIFundsTextExpense, values_.funds);
    const std::uint32_t fundsColor = values_.funds < 0 ? red : normal;
    if (!expenseView_) {
        SetTextColorById(kBudgetUIFundsText, fundsColor);
        SetTextColorById(kBudgetUIFundsTextAlt, fundsColor);
        SetFormattedAmountInTextControl(kBudgetUIMonthlyIncomeText, values_.monthlyIncome);
        SetFormattedAmountInTextControl(kBudgetUIMonthlyExpenseText, values_.monthlyExpense);
        SetFormattedAmountInTextControl(kBudgetUIBalanceText,
                                        values_.monthlyIncome + values_.funds - values_.monthlyExpense);
        const std::uint32_t balanceColor =
            values_.monthlyIncome + values_.funds < values_.monthlyExpense ? red : normal;
        SetTextColorById(kBudgetUIBalanceText, balanceColor);
        SetTextColorById(kBudgetUIBalanceTextAlt, balanceColor);
    } else {
        SetTextColorById(kBudgetUIFundsTextExpense, fundsColor);
        SetTextColorById(kBudgetUIFundsColorExpense, fundsColor);
        SetFormattedAmountInTextControl(kBudgetUIIncomeTextExpense, values_.monthlyIncome);
        SetFormattedAmountInTextControl(kBudgetUIExpenseTextExpense, values_.monthlyExpense);
        SetFormattedAmountInTextControl(kBudgetUIBalanceTextExpense,
                                        values_.monthlyIncome + values_.funds - values_.monthlyExpense);
        const std::uint32_t balanceColor =
            values_.monthlyIncome + values_.funds < values_.monthlyExpense ? red : normal;
        SetTextColorById(kBudgetUIBalanceTextExpense, balanceColor);
        SetTextColorById(kBudgetUIBalanceColorExpense, balanceColor);
        SetFormattedAmountInTextControl(kBudgetUITaxGroup1Text, values_.taxGroup1);
        SetFormattedAmountInTextControl(kBudgetUITaxGroup2Text, values_.taxGroup2);
        SetFormattedAmountInTextControl(kBudgetUITaxGroup3Text, values_.taxGroup3);
        SetFormattedAmountInTextControl(kBudgetUIOrdinanceIncomeText, values_.ordinanceIncome);
        SetFormattedAmountInTextControl(kBudgetUIOrdinanceExpenseText, values_.ordinanceExpense);
        SetFormattedAmountInTextControl(kBudgetUINeighborsIncomeText, values_.neighborsIncome);
        SetFormattedAmountInTextControl(kBudgetUINeighborsExpenseText, values_.neighborsExpense);
        SetFormattedAmountInTextControl(kBudgetUIGroup1IncomeText, values_.groupIncome1);
        SetFormattedAmountInTextControl(kBudgetUIGroup2IncomeText, values_.groupIncome2);
        SetFormattedAmountInTextControl(kBudgetUIGroup2ExpenseText, values_.groupExpense2);
        SetFormattedAmountInTextControl(kBudgetUIGroup3ExpenseText, values_.groupExpense3);
        SetFormattedAmountInTextControl(kBudgetUIGroup4ExpenseText, values_.groupExpense4);
        SetFormattedAmountInTextControl(kBudgetUIGroup5ExpenseText, values_.groupExpense5);
        SetFormattedAmountInTextControl(kBudgetUIGroup6ExpenseText, values_.groupExpense6);
        SetFormattedAmountInTextControl(kBudgetUIGroup7ExpenseText, values_.groupExpense7);
        SetFormattedAmountInTextControl(kBudgetUIBondText, values_.bondPayments);
    }
}

// ============================================================================================== the two pages

// @ 0x1002e84a8.
void cSC4BudgetUIWinProc::ShowWindow() {
    cIGZWin* root = Controls();
    if (!root) return;
    const std::uint32_t shownId = expenseView_ ? kBudgetUIYearlyPageId : kBudgetUIWindowId;
    if (expenseView_) {
        SetVisibleById(kBudgetUIWindowId, false);
        SetVisibleById(kBudgetUIYearlyPageId, true);
    } else {
        SetVisibleById(kBudgetUIYearlyPageId, false);
        SetVisibleById(kBudgetUIWindowId, true);
    }
    if (cIGZWin* shown = root->GetChildWindowFromIDRecursive(shownId)) shown->SetFlag(0x800, false);
    UpdateBalanceSheetInfo();
    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) {
        cIGZWin* view = g_host->ViewWindow();
        if (view) manager->GZSetFocus(view);
    }
    visible_ = true;
}

// @ 0x1002e85aa.
void cSC4BudgetUIWinProc::HideWindow() {
    SetVisibleById(kBudgetUIYearlyPageId, false);
    SetVisibleById(kBudgetUIWindowId, false);
    SetVisibleById(kBudgetUITaxesWindowId, false);
    SetVisibleById(kBudgetUILoanWindowId, false);
    visible_ = false;
}

// @ 0x1002e8668.
bool cSC4BudgetUIWinProc::IsVisible() const {
    return visible_;
}

// The monthly button pair + period 0 (0x1002e53d9).
void cSC4BudgetUIWinProc::SetMonthlyPeriod() {
    SetButtonOn(kBudgetUIMonthlyButtonA);
    SetButtonOff(kBudgetUIYearlyButtonA);
    SetButtonOn(kBudgetUIMonthlyButtonB);
    SetButtonOff(kBudgetUIYearlyButtonB);
    period_ = 0;
    UpdateBalanceSheetInfo();
}

// The yearly button pair + period 1 (0x1002e5594).
void cSC4BudgetUIWinProc::SetYearlyPeriod() {
    SetButtonOff(kBudgetUIMonthlyButtonA);
    SetButtonOn(kBudgetUIYearlyButtonA);
    SetButtonOff(kBudgetUIMonthlyButtonB);
    SetButtonOn(kBudgetUIYearlyButtonB);
    period_ = 1;
    UpdateBalanceSheetInfo();
}

// ============================================================================================== taxes dialog

// @ 0x1002e6c7a.
void cSC4BudgetUIWinProc::DisplayTaxesWindow() {
    cIGZWin* root = Controls();
    if (!root) return;
    cIGZWin* taxes = root->GetChildWindowFromIDRecursive(kBudgetUITaxesWindowId);
    EnsureBudget();
    if (budget_) {
        // Slots -1..10: tax type slot + 1, spinner 0x201 + slot, text edit 0x101 + slot.
        for (int slot = -1; slot < 11; ++slot) {
            gz::cIGZWinSpinner* spinner = nullptr;
            if (!root->GetChildAsRecursive(static_cast<std::uint32_t>(slot + 0x201), gz::kGZIID_cIGZWinSpinner,
                                           reinterpret_cast<void**>(&spinner)))
                continue;
            const float rate = budget_->GetTaxRate(slot + 1);
            const std::int32_t tenths = TenthsFromRate(rate);
            savedTaxRates_[slot + 1] = static_cast<float>(tenths);
            spinner->SetValue(tenths);
            cRZString percentage;
            PrintPercentageString(tenths / 10, tenths % 10, percentage);
            gz::cIGZWinTextEdit* edit = nullptr;
            if (root->GetChildAsRecursive(static_cast<std::uint32_t>(slot + 0x101), gz::kGZIID_cIGZWinTextEdit,
                                          reinterpret_cast<void**>(&edit))) {
                edit->SetText(percentage, false);
                edit->SetFlag(0x8000);
                edit->SetFlag(0x400);
                edit->Release();
            }
            spinner->Release();
        }
        SetTaxes();
    }
    if (taxes) {
        taxes->SetFlag(kWinVisible, true);
        taxes->SetFlag(0x800, true);
        taxes->PullToFront();
        taxes->SetFlag(0x800, false);
    }
    if (g_host) g_host->HiddenPause();
    if (taxes && g_host) g_host->RestoreWindowLocation(taxes, true);
    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) manager->RunPopup(taxes);
    if (taxes && taxes->GetFlag(kWinVisible)) {
        CancelTaxesWindow();
        RemoveTaxesWindow();
    }
    if (g_host) {
        if (cIGZWinMgr* manager = g_host->WindowManager()) {
            if (cIGZWin* view = g_host->ViewWindow()) manager->GZSetFocus(view);
        }
        if (taxes) g_host->SaveWindowLocation(taxes);
    }
}

// @ 0x1002e6b66: restore the saved rates, hide the dialog and end it.
void cSC4BudgetUIWinProc::CancelTaxesWindow() {
    for (std::uint32_t type = 0; type != kBudgetUITaxCount; ++type) {
        if (budget_) budget_->SetTaxRate(static_cast<int>(type), savedTaxRates_[type] / 10.0f);
    }
    cIGZWin* taxes = Control(kBudgetUITaxesWindowId);
    if (taxes) taxes->SetFlag(kWinVisible, false);
    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr)
        manager->EndDialog(0xf, taxes, 0);
    // The original tail-calls EndDialog here; the hidden resume is in RemoveTaxesWindow.
}

// @ 0x1002e6c00: hide the dialog, end it, resume the simulation and refresh the balance sheet.
void cSC4BudgetUIWinProc::RemoveTaxesWindow() {
    cIGZWin* taxes = Control(kBudgetUITaxesWindowId);
    if (taxes) taxes->SetFlag(kWinVisible, false);
    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr)
        manager->EndDialog(0xf, taxes, 0);
    if (g_host) g_host->HiddenResume();
    UpdateBalanceSheetInfo();
}

// ============================================================================================== loan dialog

// The sixteen loan-row labels, in the original's clear order (term, principal, remaining, payment per row).
void cSC4BudgetUIWinProc::ClearLoanLabels() {
    for (int row = 0; row < 4; ++row) {
        const cRZString empty;
        SetCaptionById(kBudgetUILoanRowTerm[row], empty);
        SetCaptionById(kBudgetUILoanRowPrincipal[row], empty);
        SetCaptionById(kBudgetUILoanRowRemaining[row], empty);
        SetCaptionById(kBudgetUILoanRowPayment[row], empty);
    }
}

// The four visible loan rows and the total (0x1002e62ba body, also used by DisplayLoanWindow).
void cSC4BudgetUIWinProc::ShowLoanRows(std::int64_t paymentScale) {
    cIGZWin* root = Controls();
    if (!root) return;
    ClearLoanLabels();
    std::uint32_t total = 0;
    std::uint32_t displayed = 0;
    const std::uint32_t count = static_cast<std::uint32_t>(loans_.size());
    loanCount_ = count;
    for (std::uint32_t i = 0; i < count; ++i) {
        const cSC4BudgetSimulator::LoanInfo& loan = loans_[i];
        if (displayed < 4 && static_cast<std::uint32_t>(loanScroll_) <= i) {
            const cSLString format(kBudgetUILabelStringGroup, kBudgetUILoanTermFormat, true);
            cRZString text;
            text.Sprintf(format.ToChar(), static_cast<std::uint32_t>(loan.yearsLeft),
                         static_cast<std::uint32_t>(loan.monthsLeft));
            SetCaptionById(kBudgetUILoanRowTerm[displayed], text);
            SetFormattedAmountInTextControl(kBudgetUILoanRowPrincipal[displayed], loan.principal);
            SetFormattedAmountInTextControl(kBudgetUILoanRowRemaining[displayed], loan.remainingPrincipal);
            SetFormattedAmountInTextControl(kBudgetUILoanRowPayment[displayed], loan.monthlyPayment * paymentScale);
            ++displayed;
        }
        // Quirk: the total wraps in 32 bits and is truncated back to int.
        total += static_cast<std::uint32_t>(loan.monthlyPayment * paymentScale);
    }
    SetFormattedAmountInTextControl(kBudgetUILoanTotalText, static_cast<std::int32_t>(total));
    SetVisibleById(kBudgetUILoanUpButton, loanScroll_ != 0);
    SetVisibleById(kBudgetUILoanDownButton, count >= 5 && static_cast<std::uint32_t>(count - 4) >
                                                      static_cast<std::uint32_t>(loanScroll_));
}

// @ 0x1002e62ba.
void cSC4BudgetUIWinProc::UpdateLoanDisplay() {
    cIGZWin* root = Controls();
    if (!root) return;
    EnsureBudget();
    const std::int64_t paymentScale = period_ == 0 ? 1 : 12;
    ClearLoanLabels();

    gz::cIGZWinSpinner* spinner = nullptr;
    if (root->GetChildAsRecursive(kBudgetUILoanSpinner, gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) {
        if (budget_) {
            const std::int64_t limit = budget_->GetCurrentBorrowingLimit();
            const std::int64_t borrowed = budget_->GetTotalBorrowed();
            if (limit - borrowed == static_cast<std::int64_t>(pendingLoanTotal_)) {
                spinner->SetMinMax(0, 0);
                spinner->SetValue(0);
                SetEnabledById(kBudgetUILoanTakeButton, false);
            } else {
                spinner->SetMinMax(static_cast<int>(budget_->GetBondIncrement()),
                                   static_cast<int>((limit - borrowed) - pendingLoanTotal_));
                spinner->SetValue(currentLoanAmount_);
                SetEnabledById(kBudgetUILoanTakeButton, true);
            }
        }
        spinner->Release();
    }

    if (budget_) {
        SetFormattedAmountInTextControl(kBudgetUILoanBorrowedText,
                                        budget_->GetTotalBorrowed() + pendingLoanTotal_);
    }

    ShowLoanRows(paymentScale);

    if (budget_ && static_cast<std::int64_t>(loans_.size()) == budget_->GetCurrentMaxOutstandingBondsLimit()) {
        gz::cIGZWinSpinner* again = nullptr;
        if (root->GetChildAsRecursive(kBudgetUILoanSpinner, gz::kGZIID_cIGZWinSpinner,
                                      reinterpret_cast<void**>(&again))) {
            again->SetMinMax(0, 0);
            again->SetValue(0);
            SetEnabledById(kBudgetUILoanTakeButton, false);
            again->Release();
        }
    }
}

// @ 0x1002e7226.
void cSC4BudgetUIWinProc::DisplayLoanWindow() {
    cIGZWin* root = Controls();
    if (!root) return;
    cIGZWin* loanWindow = root->GetChildWindowFromIDRecursive(kBudgetUILoanWindowId);
    loanScroll_ = 0;
    pendingLoanTotal_ = 0;
    currentLoanAmount_ = 0;
    EnsureBudget();
    if (!budget_) return;   // port guard: the original would dereference the missing simulator

    const std::int64_t paymentScale = period_ == 0 ? 1 : 12;
    {
        const cSLString title(kBudgetUILabelStringGroup,
                              period_ == 0 ? kBudgetUILoanTitleMonthly : kBudgetUILoanTitleYearly, true);
        SetCaptionById(kBudgetUILoanTitleText, title);
    }
    ClearLoanLabels();

    gz::cIGZWinSpinner* spinner = nullptr;
    if (root->GetChildAsRecursive(kBudgetUILoanSpinner, gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) {
        const std::int64_t limit = budget_->GetCurrentBorrowingLimit();
        const std::int64_t borrowed = budget_->GetTotalBorrowed();
        if (borrowed == limit) {
            spinner->SetMinMax(0, 0);
            spinner->SetValue(0);
            SetEnabledById(kBudgetUILoanTakeButton, false);
        } else {
            const std::int64_t remaining = limit - borrowed;
            const std::int64_t bondLimit = budget_->GetCurrentBondLimit();
            spinner->SetMinMax(static_cast<int>(budget_->GetBondIncrement()),
                               static_cast<int>(remaining < bondLimit ? remaining : bondLimit));
            spinner->SetValue(static_cast<int>(budget_->GetBondIncrement()));
            SetEnabledById(kBudgetUILoanTakeButton, true);
        }
        spinner->Release();
    }

    SetFormattedAmountInTextControl(kBudgetUILoanLimitText, budget_->GetCurrentBorrowingLimit());
    SetFormattedAmountInTextControl(kBudgetUILoanBorrowedText, budget_->GetTotalBorrowed());
    currentLoanAmount_ = static_cast<std::int32_t>(budget_->GetBondIncrement());
    // Quirk: the yearly label passes amount * 12 to the payment function (the spinner handler multiplies the result).
    SetFormattedAmountInTextControl(kBudgetUILoanPaymentText,
                                    period_ == 0 ? budget_->GetMonthlyPaymentForLoan(currentLoanAmount_)
                                                 : budget_->GetMonthlyPaymentForLoan(currentLoanAmount_ * 12));
    {
        const std::int64_t months = budget_->GetLoanTimeInMonths();
        const cSLString format(kBudgetUILabelStringGroup, kBudgetUILoanTermFormat, true);
        cRZString text;
        text.Sprintf(format.ToChar(), static_cast<std::uint32_t>(months / 12),
                     static_cast<std::uint32_t>(months % 12));
        SetCaptionById(kBudgetUILoanTermText, text);
    }

    loans_.clear();
    budget_->GetAllLoans(loans_);
    loanCount_ = static_cast<std::uint32_t>(loans_.size());
    pendingLoanCount_ = loanCount_;
    ShowLoanRows(paymentScale);

    if (loanWindow) {
        loanWindow->AddRef();
        loanWindow->SetFlag(kWinVisible, true);
        loanWindow->SetFlag(0x800, true);
        loanWindow->PullToFront();
        loanWindow->SetFlag(0x800, false);
        if (cIGZWin* parent = loanWindow->GetParentWin()) {
            const int dx = (parent->GetW() - loanWindow->GetW()) / 2;
            const int dy = (parent->GetH() - loanWindow->GetH()) / 2;
            loanWindow->GZWinMoveTo(dx, dy);
        }
        if (static_cast<std::int64_t>(loans_.size()) == budget_->GetCurrentMaxOutstandingBondsLimit()) {
            gz::cIGZWinSpinner* again = nullptr;
            if (root->GetChildAsRecursive(kBudgetUILoanSpinner, gz::kGZIID_cIGZWinSpinner,
                                          reinterpret_cast<void**>(&again))) {
                again->SetMinMax(0, 0);
                again->SetValue(0);
                SetEnabledById(kBudgetUILoanTakeButton, false);
                again->Release();
            }
        }
    }
    if (g_host) g_host->HiddenPause();
    if (loanWindow && g_host) g_host->RestoreWindowLocation(loanWindow, true);
    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) manager->RunPopup(loanWindow);
    if (loanWindow && loanWindow->GetFlag(kWinVisible)) RemoveLoanWindow();
    if (g_host) {
        if (cIGZWinMgr* manager = g_host->WindowManager()) {
            if (cIGZWin* view = g_host->ViewWindow()) manager->GZSetFocus(view);
        }
        if (loanWindow) g_host->SaveWindowLocation(loanWindow);
    }
    if (loanWindow) loanWindow->Release();
}

// @ 0x1002e6240: hide and end the loan dialog, drop the shown loans and resume the simulation.
void cSC4BudgetUIWinProc::RemoveLoanWindow() {
    cIGZWin* loanWindow = Control(kBudgetUILoanWindowId);
    if (loanWindow) loanWindow->SetFlag(kWinVisible, false);
    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr)
        manager->EndDialog(0xf, loanWindow, 0);
    loans_.clear();
    if (g_host) g_host->HiddenResume();
}

// @ 0x1002e6ac0: issue the bonds of the loans added after the dialog opened.
void cSC4BudgetUIWinProc::TakePendingLoans() {
    if (pendingLoanCount_ >= loanCount_) return;
    EnsureBudget();
    if (!budget_) return;
    for (std::uint32_t i = pendingLoanCount_; i < loanCount_; ++i) {
        budget_->IssueBond(static_cast<std::uint32_t>(loans_[i].principal));
    }
}

// The 0xaa4c33e6 command body (inline in DoWinProcMessage at 0x1002e5297): append a pending loan, show it, issue the
// pending bonds and close the dialog.
void cSC4BudgetUIWinProc::TakeLoan() {
    cSC4BudgetSimulator* budget = g_host ? g_host->Budget() : nullptr;
    if (!budget) return;   // port guard: the original dereferences the missing simulator
    cSC4BudgetSimulator::LoanInfo loan;
    const std::int64_t fullCost = budget->GetFullCostOfLoan(currentLoanAmount_);
    const std::uint32_t months = budget->GetLoanTimeInMonths();
    loan.monthsLeft = static_cast<std::int32_t>(months % 12);
    loan.yearsLeft = static_cast<std::int32_t>(months / 12);
    loan.principal = currentLoanAmount_;
    loan.remainingPrincipal = currentLoanAmount_;
    // Quirk: a 32-bit unsigned division of the full cost (see the header).
    loan.monthlyPayment = months ? static_cast<std::int64_t>(static_cast<std::uint32_t>(fullCost) / months) : 0;
    loans_.push_back(loan);
    pendingLoanTotal_ += currentLoanAmount_;
    ++loanCount_;
    UpdateLoanDisplay();
    TakePendingLoans();
    RemoveLoanWindow();
}

// ============================================================================================== department dialog

// @ 0x1002e700a.
void cSC4BudgetUIWinProc::DoSubBudgetWindow(std::uint32_t purpose) {
    if (subBudget_ || !g_host) return;
    ISubBudgetWindow* sub = g_host->CreateSubBudgetWindow();
    if (!sub) return;
    sub->AddRef();   // the member (+0x30) holds a reference
    subBudget_ = sub;
    cIGZWin* dialog = nullptr;
    if (sub->Create(&dialog, purpose, static_cast<std::uint32_t>(period_), 0) && dialog) {
        dialog->ShowWindow();
        dialog->SetFlag(0x800, true);
        dialog->PullToFront();
        dialog->SetFlag(0x800, false);
        dialog->SetFlag(0x100, true);
        gz::cIGZWinGen* gen = nullptr;
        if (dialog->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen))) {
            gen->SetWinGenFlag(gz::kGenDialog, true);
            gen->SetWinGenFlag(gz::kGenNoBackground, true);
            gen->SetWinGenFlag(gz::kGenFlatFill, true);
            gen->Release();
        }
        if (g_host) g_host->RestoreWindowLocation(dialog, true);
        if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) manager->RunPopup(dialog);
        if (g_host) {
            if (cIGZWinMgr* manager = g_host->WindowManager()) {
                if (cIGZWin* view = g_host->ViewWindow()) manager->GZSetFocus(view);
            }
            g_host->SaveWindowLocation(dialog);
        }
        dialog->Release();
    }
    sub->Destroy();
    subBudget_ = nullptr;
    sub->Release();
    UpdateBalanceSheetInfo();
}

}  // namespace sc4
