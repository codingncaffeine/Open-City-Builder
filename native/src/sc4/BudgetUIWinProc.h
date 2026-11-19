// cSC4BudgetUIWinProc — the window procedure of the balance-sheet ("budget") panel (40 symbols,
// 0x1002e3966..0x1002e8bb8; 32 real functions plus the QI / AddRef / Release / DoWinProcMessage / DoMessage thunks).
// cSC4View3DWin::AddGUI constructs it for the script window {0, 0x96a006b0, 0xcbc3c2b9}, id 0xaa3ac000 (the window
// itself is the income/monthly page; the yearly/expense page 0xaa3ac001 is its sibling), and attaches it with
// cIGZWinGen::SetWinProc.
//
// The panel shows the city treasury: funds, this month's / this year's estimated income and expense, the tax income of
// the three wealth groups (types 0..2, 3..7, 8..11), the ordinance and neighbour income / expense, the department
// budget groups and the bond payments. It has an income and an expense page (0xaa3ac000 / 0xaa3ac001) and a
// monthly / yearly switch (period 0 / 1). Every number goes through SetFormattedAmountInTextControl, which formats it
// with SC4MakeMoneyString into the cIGZWinText control of that id. The Taxes button opens the taxes dialog
// (0xaa3ac002: one spinner + one text edit per tax type, OK / Cancel), the bond button opens the loan dialog
// (0xca4c332d: amount spinner, four rows of existing loans, take / scroll buttons), and the eight department rows open
// a cSC4SubBudgetWindow with the group id as its purpose.
//
// Original layout (0x1c8 bytes, allocated by AddGUI; the first three vtables come from the three interfaces):
//   +0x00 cRZUnknown (refcount +0x08)   +0x10 cIGZWinProc vptr   +0x18 cIGZMessageTarget2 vptr
//   +0x20 cIGZWinGen* balance-sheet window (id 0xaa3ac000)        +0x28 cSC4BudgetSimulator*
//   +0x30 cSC4SubBudgetWindow* (only while a department window is open)
//   +0x38 std::vector<cISC4BudgetSimulator::LoanInfo> the loans shown / pending (stride 0x20)
//   +0x50 funds   +0x58 monthly income   +0x60 estimated income   +0x68 monthly expense   +0x70 estimated expense
//   +0x78 / +0x80 tax group 1 (types 0..2) monthly / yearly     +0x88 / +0x90 tax group 2 (types 3..7)
//   +0x98 / +0xa0 tax group 3 (types 8..11)                     +0xa8 / +0xb0 ordinance income monthly / yearly
//   +0xb8 / +0xc0 ordinance expense monthly / yearly            +0xc8 / +0xd0 neighbour income / expense monthly
//   +0xd8 / +0xe0 neighbour income / expense yearly
//   +0xe8 / +0xf0 budget group 0xa5a72d1 income monthly / yearly
//   +0xf8 / +0x100 budget group 0xaa369059 income monthly / yearly
//   +0x108 / +0x110 budget group 0xaa369059 expense monthly / yearly
//   +0x118 / +0x120 group 0x4a357b40   +0x128 / +0x130 group 0x6a357b7f   +0x138 / +0x140 group 0x4a357eaf
//   +0x148 / +0x150 group 0x6a357b96   +0x158 / +0x160 groups 0xea597195 / 0xea357ecb (same row)
//   +0x168 / +0x170 total monthly bond payments / x12
//   +0x178 float savedTaxRates[12] (tenths, restored by Cancel)  +0x1a8 i32 first shown loan   +0x1ac u32 loan count
//   +0x1b0 u32 loans already issued   +0x1b4 i32 pending loan total   +0x1b8 i32 current loan amount
//   +0x1bc bool expense page   +0x1c0 i32 period (0 monthly, 1 yearly)
//   +0x1c4 bool message notifications registered   +0x1c5 bool visible   +0x1c6 bool update pending
//
// Quirks kept (addresses are the original's):
//  * The constructor only zeroes +0x20..+0x178 and a few tail fields; savedTaxRates (+0x178..+0x1a7) is left
//    uninitialised. Only DisplayTaxesWindow writes it, and only CancelTaxesWindow reads it, so it is always filled
//    before use (0x1002e3966).
//  * InitBalanceSheet (called only from the constructor, always with period 0) relabels the column headers for the
//    monthly period; switching to the yearly period does not relabel them, so the headers keep saying "monthly"
//    (0x1002e3b4e).
//  * The spinner handler computes the yearly loan payment as GetMonthlyPaymentForLoan(amount) * 12, while
//    DisplayLoanWindow passes GetMonthlyPaymentForLoan(amount * 12) for the same label (0x1002e4d8a vs 0x1002e77a7).
//  * The pending-loan monthly payment is a 32-bit unsigned division of GetFullCostOfLoan(amount) by the loan time in
//    months (0x1002e52e9: divl), truncating the 64-bit cost; the original divides by zero if there is no loan time.
//  * The loan list total is accumulated in a wrap-around 32-bit unsigned counter and truncated back to int
//    (0x1002e5554 / 0x1002e7a53).
//  * The tax text edit of a type is mapped back to a tax type through the 3x3 table {0,3,9,1,4,10,2,5,11} followed by
//    three padding zeroes (0x100a1cac0), while DisplayTaxesWindow displays GetTaxRate(slot+1) on the matching spinner;
//    the two orders disagree from slot 1 on (0x1002e4eaa). see the BUDGETUI-5 to-do row (pending).
//  * The key-down handler and the spinner message answer the window manager's IsModal() (0x150), i.e. the panel
//    swallows them only while a modal dialog is up.
//  * Returning in a tax text edit selects all of the *next* field's text (the (id - 0xff) % 12 wrap) and focuses it
//    (0x1002e4f45).
//  * EndDialog destroys the four top-level windows it knows, then destroys its own window, then drops the references
//    it holds (0x1002e4984).
//
// The three calls cSC4View3DWin::AddGUI / Shutdown make on the object are its primary-vtable slots 0x50 (IsVisible,
// the host's BudgetDialogOpen), 0x48 (Update, RefreshBudgetDialog) and cSC4BudgetUIWinProc::Shutdown (0x1002e4872).
//
// Every engine service (SL::spCity vslot 0x1a0, SL::spNeighborsSim, SL::spOrdinanceSim, SL::spMS2, SL::ATM(),
// SL::spSimulator vslots 0x50 / 0x68, SL::spWinLocationSaver vslots 0x28 / 0x48, SL::spView3D vslot 0x18, RZ::WM(),
// RZ::GS() vslot 0x118, RZ::LM() -> GetLanguageUtility(0) -> GetDecimalSeparator, SC4CreateWindowFromScript and
// nsSC4TutorialSystem::FilterUICommandMessage) goes through IBudgetUIWinProcHost; neighbouring systems that are not
// part of this class's own data (the ordinance simulator, the neighbours simulator and cSC4SubBudgetWindow) are the
// small I* seams below. The budget data itself uses the recovered cSC4BudgetSimulator directly.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinInterfaces.h"
#include "gz/WinProc.h"
#include "gz/WinSpinner.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "rz/cRZUnknown.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/DialogWinProcs.h"

namespace sc4 {

// cIGZMessageTarget2 (the +0x18 view; also declared in sc4/OccupantManager.h and other recovered headers).
constexpr std::uint32_t kBudgetUIMessageTargetIid = 0x452294aa;

// ---- the script window and the control-id families (all values are the original ids) ----------------------
constexpr std::uint32_t kBudgetUIWindowScript = 0xcbc3c2b9;   // UI script instance (group kUIScriptGroup)
constexpr std::uint32_t kBudgetUIWindowId = 0xaa3ac000;       // the balance-sheet window = income / monthly page
constexpr std::uint32_t kBudgetUIYearlyPageId = 0xaa3ac001;   // the yearly / expense page
constexpr std::uint32_t kBudgetUITaxesWindowId = 0xaa3ac002;  // the taxes dialog
constexpr std::uint32_t kBudgetUILoanWindowId = 0xca4c332d;   // the bond / loan dialog

// Command ids of the two page families: the 0xaa3ac4xx and 0xaa3ac5xx ids reach the same handlers (the second family
// is the first + 0x100). Handled relative ids: 0 taxes, 1 / 2 / 3 / 4 / 5 / 6 / 7 / 8 / 9 / a / b / c department
// windows, d loans, e expense page, f income page.
constexpr std::uint32_t kBudgetUICommandPageBase = 0xaa3ac400;
constexpr std::uint32_t kBudgetUICommandPageBase2 = 0xaa3ac500;
constexpr std::uint32_t kBudgetUIExpensePageCommand = 0x4a8879cb;   // switch to the expense page
constexpr std::uint32_t kBudgetUIIncomePageCommand = 0x4a8879db;    // switch to the income page
constexpr std::uint32_t kBudgetUIMonthlyButtonA = 0x4a8879cc;       // monthly buttons (A / B)
constexpr std::uint32_t kBudgetUIMonthlyButtonB = 0x4a8879dc;
constexpr std::uint32_t kBudgetUIYearlyButtonA = 0x4a8879cd;        // yearly buttons (A / B)
constexpr std::uint32_t kBudgetUIYearlyButtonB = 0x4a8879dd;
constexpr std::uint32_t kBudgetUIHideCommand = 0x6a5bb0fb;          // close the panel
constexpr std::uint32_t kBudgetUIHideCommand2 = 0x4a5bb1b5;
constexpr std::uint32_t kBudgetUILoanCloseCommand = 0xa4c3354;      // close the loan dialog
constexpr std::uint32_t kBudgetUILoanUpCommand = 0x2a4c33a6;        // loan list one page up
constexpr std::uint32_t kBudgetUILoanDownCommand = 0xaa4c339c;      // loan list one page down
constexpr std::uint32_t kBudgetUITakeLoanCommand = 0xaa4c33e6;      // take the pending loan
constexpr std::uint32_t kBudgetUITaxesOkCommand = 0xaa4c353b;       // taxes dialog OK
constexpr std::uint32_t kBudgetUITaxesCancelCommand = 0xca4c352f;   // taxes dialog Cancel

// The budget department groups the eight department rows open (the id is also the DoSubBudgetWindow purpose).
constexpr std::uint32_t kBudgetGroupUtilities = 0xa5a72d1;
constexpr std::uint32_t kBudgetGroupTransport = 0xaa369059;     // airport / seaport group
constexpr std::uint32_t kBudgetGroupGroup3 = 0x4a357b40;
constexpr std::uint32_t kBudgetGroupGroup4 = 0x6a357b7f;
constexpr std::uint32_t kBudgetGroupGroup5 = 0x4a357eaf;
constexpr std::uint32_t kBudgetGroupParks = 0x6a357b96;         // civic / park group
constexpr std::uint32_t kBudgetGroupCivic2 = 0xea597195;
constexpr std::uint32_t kBudgetGroupCivic2Alt = 0xea357ecb;     // the same row as kBudgetGroupCivic2

// The balance-sheet amount / colour controls.
constexpr std::uint32_t kBudgetUIFundsText = 0xaa3ac100;
constexpr std::uint32_t kBudgetUIMonthlyIncomeText = 0xaa3ac101;
constexpr std::uint32_t kBudgetUIMonthlyExpenseText = 0xaa3ac102;
constexpr std::uint32_t kBudgetUIBalanceText = 0xaa3ac103;
constexpr std::uint32_t kBudgetUIBalanceTextAlt = 0xaa3ac104;
constexpr std::uint32_t kBudgetUIFundsTextAlt = 0xaa3ac107;
constexpr std::uint32_t kBudgetUIBalanceColorExpense = 0xaa3ac10a;
constexpr std::uint32_t kBudgetUIFundsColorExpense = 0xaa3ac10d;
constexpr std::uint32_t kBudgetUIFundsTextExpense = 0xaa3ac10e;
constexpr std::uint32_t kBudgetUIIncomeTextExpense = 0xaa3ac10f;
constexpr std::uint32_t kBudgetUIExpenseTextExpense = 0xaa3ac110;
constexpr std::uint32_t kBudgetUIBalanceTextExpense = 0xaa3ac111;
constexpr std::uint32_t kBudgetUITaxGroup1Text = 0xaa3ac112;
constexpr std::uint32_t kBudgetUITaxGroup2Text = 0xaa3ac113;
constexpr std::uint32_t kBudgetUITaxGroup3Text = 0xaa3ac114;
constexpr std::uint32_t kBudgetUIOrdinanceIncomeText = 0xaa3ac115;
constexpr std::uint32_t kBudgetUIOrdinanceExpenseText = 0xaa3ac11f;
constexpr std::uint32_t kBudgetUINeighborsIncomeText = 0xaa3ac120;
constexpr std::uint32_t kBudgetUINeighborsExpenseText = 0xaa3ac121;
constexpr std::uint32_t kBudgetUIGroup1IncomeText = 0xaa3ac122;
constexpr std::uint32_t kBudgetUIGroup2IncomeText = 0xaa3ac123;
constexpr std::uint32_t kBudgetUIGroup2ExpenseText = 0xaa3ac124;
constexpr std::uint32_t kBudgetUIGroup3ExpenseText = 0xaa3ac125;
constexpr std::uint32_t kBudgetUIGroup4ExpenseText = 0xaa3ac126;
constexpr std::uint32_t kBudgetUIGroup5ExpenseText = 0xaa3ac127;
constexpr std::uint32_t kBudgetUIGroup6ExpenseText = 0xaa3ac128;
constexpr std::uint32_t kBudgetUIGroup7ExpenseText = 0xaa3ac129;
constexpr std::uint32_t kBudgetUIBondText = 0xaa3ac12a;

// The taxes dialog controls: spinner 0x200 + type and text edit 0x100 + type (types 0..11).
constexpr std::uint32_t kBudgetUITaxSpinnerBase = 0x200;
constexpr std::uint32_t kBudgetUITaxEditBase = 0x100;
constexpr std::uint32_t kBudgetUITaxCount = 12;

// The loan dialog controls.
constexpr std::uint32_t kBudgetUILoanAmountText = 0xa4c3409;      // pending amount (set by Init from the spinner)
constexpr std::uint32_t kBudgetUILoanLimitText = 0xea4c34a6;      // current borrowing limit
constexpr std::uint32_t kBudgetUILoanBorrowedText = 0xca4c349d;   // total borrowed (also refreshed by the list)
constexpr std::uint32_t kBudgetUILoanPaymentText = 0xca4c34e2;    // monthly payment for the amount
constexpr std::uint32_t kBudgetUILoanTotalText = 0x6a4c3494;      // total monthly payment of all loans
constexpr std::uint32_t kBudgetUILoanTitleText = 0xa4c34be;       // "bonds" title (monthly / yearly text)
constexpr std::uint32_t kBudgetUILoanTermText = 0xa4c34db;        // loan time caption
constexpr std::uint32_t kBudgetUILoanSpinner = 0x8a4c34d4;        // loan amount spinner
constexpr std::uint32_t kBudgetUILoanTakeButton = 0xaa4c33e6;     // take-loan button (enabled flag 2)
constexpr std::uint32_t kBudgetUILoanUpButton = 0x2a4c33a6;       // scroll up (visible flag 1)
constexpr std::uint32_t kBudgetUILoanDownButton = 0xaa4c339c;     // scroll down (visible flag 1)
// Four loan rows: term, principal, remaining principal, payment (four ids each, + row).
constexpr std::uint32_t kBudgetUILoanRowTerm[4] = {0x4a4c33ad, 0x4a4c33ae, 0x4a4c33af, 0x4a4c33b0};
constexpr std::uint32_t kBudgetUILoanRowPrincipal[4] = {0x8a4c33d0, 0x8a4c33d1, 0x8a4c33d2, 0x8a4c33d3};
constexpr std::uint32_t kBudgetUILoanRowRemaining[4] = {0xa4c3405, 0xa4c3406, 0xa4c3407, 0xa4c3408};
constexpr std::uint32_t kBudgetUILoanRowPayment[4] = {0xca4c3426, 0xca4c3427, 0xca4c3428, 0xca4c3429};

// The static label ids InitBalanceSheet fills (monthly / yearly string instances below).
constexpr std::uint32_t kBudgetUILabelIds[] = {
    0xaa3ac108, 0xaa3ac109, 0xaa3ac104, 0xaa3ac10a, 0xaa3ac105, 0xaa3ac10b, 0xaa3ac106, 0xaa3ac10c, 0xaa3ac107,
    0xaa3ac10d, 0xaa3ad120, 0xaa3ad121, 0xaa3ad122, 0xaa3ad123, 0xaa3ad200, 0xaa3ad201, 0xaa3ad202, 0xaa3ad203};
// The string instances of those labels for period 0 (monthly) and period 1 (yearly); group 0x6a3ff01c.
constexpr std::uint32_t kBudgetUIMonthlyLabelStrings[] = {
    0xa569e48, 0xa569e48, 0x4a569ef8, 0x4a569ef8, 0xaa5690b0, 0xaa5690b0, 0x4a569f44, 0x4a569f44, 0xca569fa8,
    0xca569fa8, 0xaa5690b0, 0xca569b75, 0x4a569f44, 0xca569b75, 0xaa5690b0, 0xca569b75, 0x4a569f44, 0xca569b75};
constexpr std::uint32_t kBudgetUIYearlyLabelStrings[] = {
    0x6a569ffd, 0x6a569ffd, 0x4a569ef8, 0x4a569ef8, 0xaa569dcd, 0xaa569dcd, 0xaa56a0f8, 0xaa56a0f8, 0x4a56a15b,
    0x4a56a15b, 0xaa569dcd, 0xa569c81, 0xaa56a0f8, 0xa569c81, 0xaa569dcd, 0xa569c81, 0xaa56a0f8, 0xa569c81};
constexpr std::uint32_t kBudgetUILabelStringGroup = 0x6a3ff01c;

// The two localized format strings ("<years><sep><months>"): the loan term caption and each loan row.
constexpr std::uint32_t kBudgetUILoanTermFormat = 0xea569d4b;
constexpr std::uint32_t kBudgetUILoanTitleMonthly = 0x8a569bd2;   // 0xa4c34be in the monthly period
constexpr std::uint32_t kBudgetUILoanTitleYearly = 0x4a569ce4;    // 0xa4c34be in the yearly period

// The 3x3 tax-type table of the text-edit path, followed by three padding zeroes exactly as laid out in the
// binary (0x100a1cac0: 0, 3, 9, 1, 4, 10, 2, 5, 11, 0, 0, 0). Only the first nine entries are meaningful.
constexpr std::int32_t kBudgetUITaxTypeTable[12] = {0, 3, 9, 1, 4, 10, 2, 5, 11, 0, 0, 0};

// Messages.
constexpr std::uint32_t kBudgetUIMsgFundsChanged = 0x8dac3e7;     // marks the panel dirty
constexpr std::uint32_t kBudgetUIMsgFundsChanged2 = 0x8dac3e8;
constexpr std::uint32_t kBudgetUIMsgAnimationTick = 0x86ad10ee;   // animation tick: refresh when dirty
constexpr std::uint32_t kBudgetUITickChannel = 0xc0;              // SL::ATM() notification channel
constexpr std::uint32_t kBudgetUISpinnerMessage = 0x682876c9;     // cIGZWinSpinner value changed
constexpr std::uint32_t kBudgetUISenderButton = 0x287259f6;       // cIGZWinBtn command sender class
constexpr std::uint32_t kBudgetUITextEditKey = 0x9b1552d5;        // cIGZWinTextEdit key notification (p2 = key)
constexpr std::uint32_t kBudgetUITextEditKillFocus = 0x9b1552d9;  // cIGZWinTextEdit lost the focus
constexpr std::uint32_t kBudgetUIReturnKey = 0xd;

// ---- seams ---------------------------------------------------------------------------------------------

// A department budget window (cSC4SubBudgetWindow, ctor @ 0x10033358c, object size 0x230). DoSubBudgetWindow builds it
// on the heap, calls CreateSubBudgetWindow(&window, purpose, period, 0) @ 0x100333826, runs the window modally and then
// calls DestroySubBudgetWindow @ 0x100333a1a.
class ISubBudgetWindow : public cIGZUnknown {
public:
    // `department`: 0 for a group's dialog, else the department whose buildings the single-building dialog lists (+0x24).
    virtual bool Create(cIGZWin** window, std::uint32_t purpose, std::uint32_t period, std::uint32_t department) = 0;
    virtual void Destroy() = 0;
};

// The ordinance simulator's calls in CalculateValues: vslot 0x58 GetOrdinanceAvailableCount, 0x60
// GetOrdinanceAvailableArray, 0x40 IsOrdinanceOn and 0xa0 GetOrdinanceByID.
class IOrdinanceBudgetItem {
public:
    virtual ~IOrdinanceBudgetItem() = default;
    virtual std::int64_t CurrentMonthlyIncome() = 0;   // cISC4Ordinance vslot 0x28 (cSC4OrdinanceSimple)
};
class IOrdinanceBudgetApi {
public:
    virtual ~IOrdinanceBudgetApi() = default;
    virtual std::uint32_t AvailableCount() = 0;                                  // 0x58
    virtual void AvailableIds(std::uint32_t* out, std::uint32_t& count) = 0;     // 0x60
    virtual IOrdinanceBudgetItem* OrdinanceById(std::uint32_t id) = 0;           // 0xa0 (borrowed)
    virtual bool IsOrdinanceOn(std::uint32_t id) = 0;                            // 0x40
};

// cSC4NeighborsSimulator vslot 0x100 / 0x108.
class INeighborsBudgetApi {
public:
    virtual ~INeighborsBudgetApi() = default;
    virtual std::int64_t MonthlyDealIncome() = 0;    // GetTotalMonthlyNeighborDealIncome
    virtual std::int64_t MonthlyDealExpense() = 0;   // GetTotalMonthlyNeighborDealExpense
};

// What the panel needs from the running game (the original global / call chain is in the comment). The test installs
// its own; there is no default host, so a missing hook-up leaves the panel inert (like cSC4MySimWinProc).
class IBudgetUIWinProcHost {
public:
    virtual ~IBudgetUIWinProcHost() = default;
    virtual cIGZWin* ViewWindow() = 0;                          // SL::spView3D vslot 0x18 (cISC4View3DWin::AsIGZWin)
    // SC4CreateWindowFromScript(key, parent, id, iid, out) @ 0x100039bf7; one reference on success.
    virtual bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                        std::uint32_t iid, void** out) = 0;
    virtual cIGZWinMgr* WindowManager() = 0;                    // RZ::WM()
    virtual bool WindowManagerIsModal() = 0;                    // RZ::WM() vslot 0x150 (IsModal)
    virtual cSC4BudgetSimulator* Budget() = 0;                  // SL::spCity vslot 0x1a0 (borrowed)
    virtual IOrdinanceBudgetApi* Ordinances() = 0;              // SL::spOrdinanceSim (borrowed)
    virtual INeighborsBudgetApi* Neighbors() = 0;               // SL::spNeighborsSim (borrowed)
    virtual cIGZMessageServer2* Messages() = 0;                 // SL::spMS2
    virtual void AddTickNotification(cIGZMessageTarget2* target, std::uint32_t channel) = 0;    // SL::ATM() vslot 0x28
    virtual void RemoveTickNotification(cIGZMessageTarget2* target) = 0;                       // SL::ATM() vslot 0x30
    virtual void HiddenPause() = 0;                             // SL::spSimulator vslot 0x50
    virtual void HiddenResume() = 0;                            // SL::spSimulator vslot 0x68
    virtual bool SaveWindowLocation(cIGZWin* window) = 0;       // SL::spWinLocationSaver vslot 0x28
    virtual bool RestoreWindowLocation(cIGZWin* window, bool keepOnScreen) = 0;   // vslot 0x48
    // RZ::LM() vslot 0x128(0) -> vslot 0x78: the decimal separator PrintPercentageString puts between the two digits.
    virtual void DecimalSeparator(cIGZString& out) = 0;
    virtual std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;   // RZ::GS() vslot 0x118
    virtual bool FilterUICommandMessage(cGZMessage& message) = 0;   // nsSC4TutorialSystem::FilterUICommandMessage
    virtual ISubBudgetWindow* CreateSubBudgetWindow() = 0;      // new cSC4SubBudgetWindow (0x230 bytes)
};
IBudgetUIWinProcHost* BudgetUIWinProcHost();
void SetBudgetUIWinProcHost(IBudgetUIWinProcHost* host);

// ---- the window procedure ------------------------------------------------------------------------------
class cSC4BudgetUIWinProc final : public gz::cIGZWinProc, public cIGZMessageTarget2, public cRZUnknown {
public:
    cSC4BudgetUIWinProc();                   // @ 0x1002e3966
    ~cSC4BudgetUIWinProc() override;         // @ 0x1002e4776

    // cIGZUnknown — one reference count behind the three views (QI 0x22e85d8e / 0x452294aa; else unknown).
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1002e4ae6
    std::uint32_t AddRef() override { return cRZUnknown::AddRef(); }
    std::uint32_t Release() override { return cRZUnknown::Release(); }

    // gz::cIGZWinProc
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x1002e4b80
    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;                        // @ 0x1002e7eb8

    // the class's own virtuals (primary vtable slots 7..10 of the original)
    void ShowWindow();                       // @ 0x1002e84a8
    void HideWindow();                       // @ 0x1002e85aa
    void Update();                           // @ 0x1002e8ba2
    bool IsVisible() const;                  // @ 0x1002e8668

    // life cycle
    void Init();                             // @ 0x1002e7f18
    void Shutdown();                         // @ 0x1002e4872 (ends with a Release: the object frees itself)
    void RegisterMessageNotification(bool on);   // @ 0x1002e489c
    void EndDialog();                        // @ 0x1002e4984

    // the balance sheet
    void InitBalanceSheet();                 // @ 0x1002e3b4e
    void SetFormattedAmountInTextControl(std::uint32_t id, std::int64_t amount);   // @ 0x1002e58bc
    void PrintPercentageString(int whole, int fraction, cRZString& out);          // @ 0x1002e59b2
    void SetTaxes();                         // @ 0x1002e5a70
    void UpdateBalanceSheetInfo();           // @ 0x1002e5b7e
    void CalculateValues();                  // @ 0x1002e8674

    // the taxes / loans / department dialogs
    void RemoveLoanWindow();                 // @ 0x1002e6240
    void UpdateLoanDisplay();                // @ 0x1002e62ba
    void TakePendingLoans();                 // @ 0x1002e6ac0
    void CancelTaxesWindow();                // @ 0x1002e6b66
    void RemoveTaxesWindow();                // @ 0x1002e6c00
    void DisplayTaxesWindow();               // @ 0x1002e6c7a
    void DoSubBudgetWindow(std::uint32_t purpose);   // @ 0x1002e700a
    void DisplayLoanWindow();                // @ 0x1002e7226

    // state (for the test and for the future AddGUI hook-up)
    gz::cIGZWinGen* Window() const { return window_; }
    cSC4BudgetSimulator* Budget() const { return budget_; }
    ISubBudgetWindow* SubBudgetWindow() const { return subBudget_; }
    const std::vector<cSC4BudgetSimulator::LoanInfo>& Loans() const { return loans_; }
    std::int32_t Period() const { return period_; }
    bool ExpenseView() const { return expenseView_; }
    bool NotificationsRegistered() const { return notificationsRegistered_; }
    bool PendingUpdate() const { return pendingUpdate_; }
    std::int32_t LoanScroll() const { return loanScroll_; }
    std::uint32_t LoanCount() const { return loanCount_; }
    std::uint32_t PendingLoanCount() const { return pendingLoanCount_; }
    std::int32_t PendingLoanTotal() const { return pendingLoanTotal_; }
    std::int32_t CurrentLoanAmount() const { return currentLoanAmount_; }
    float SavedTaxRate(int index) const { return (index >= 0 && index < 12) ? savedTaxRates_[index] : 0.0f; }

    // the values CalculateValues produced (the test reads them; the original has these at +0x50..+0x170)
    struct Values {
        std::int64_t funds = 0;                              // +0x50
        std::int64_t monthlyIncome = 0;                      // +0x58
        std::int64_t estimatedIncome = 0;                    // +0x60
        std::int64_t monthlyExpense = 0;                     // +0x68
        std::int64_t estimatedExpense = 0;                   // +0x70
        std::int64_t taxGroup1 = 0, taxGroup1Yearly = 0;     // +0x78 / +0x80
        std::int64_t taxGroup2 = 0, taxGroup2Yearly = 0;     // +0x88 / +0x90
        std::int64_t taxGroup3 = 0, taxGroup3Yearly = 0;     // +0x98 / +0xa0
        std::int64_t ordinanceIncome = 0, ordinanceIncomeYearly = 0;     // +0xa8 / +0xb0
        std::int64_t ordinanceExpense = 0, ordinanceExpenseYearly = 0;   // +0xb8 / +0xc0
        std::int64_t neighborsIncome = 0, neighborsExpense = 0;          // +0xc8 / +0xd0
        std::int64_t neighborsIncomeYearly = 0, neighborsExpenseYearly = 0;   // +0xd8 / +0xe0
        std::int64_t groupIncome1 = 0, groupIncome1Yearly = 0;           // +0xe8 / +0xf0 (0xa5a72d1)
        std::int64_t groupIncome2 = 0, groupIncome2Yearly = 0;           // +0xf8 / +0x100 (0xaa369059)
        std::int64_t groupExpense2 = 0, groupExpense2Yearly = 0;         // +0x108 / +0x110 (0xaa369059)
        std::int64_t groupExpense3 = 0, groupExpense3Yearly = 0;         // +0x118 / +0x120 (0x4a357b40)
        std::int64_t groupExpense4 = 0, groupExpense4Yearly = 0;         // +0x128 / +0x130 (0x6a357b7f)
        std::int64_t groupExpense5 = 0, groupExpense5Yearly = 0;         // +0x138 / +0x140 (0x4a357eaf)
        std::int64_t groupExpense6 = 0, groupExpense6Yearly = 0;         // +0x148 / +0x150 (0x6a357b96)
        std::int64_t groupExpense7 = 0, groupExpense7Yearly = 0;         // +0x158 / +0x160 (0xea597195)
        std::int64_t bondPayments = 0, bondPaymentsYearly = 0;           // +0x168 / +0x170
    };
    const Values& CalculatedValues() const { return values_; }

private:
    // --- the original's call sites, spelled out once -----------------------------------------------------
    cIGZWin* Controls() const;     // window->AsIGZWin()->GetParentWin()
    cIGZWin* Control(std::uint32_t id) const;               // Controls()->GetChildWindowFromIDRecursive(id)
    void EnsureBudget();                                    // +0x28 = SL::spCity vslot 0x1a0 (one reference)
    void SetCaptionById(std::uint32_t id, const cIGZString& caption);
    void SetVisibleById(std::uint32_t id, bool visible);
    void SetEnabledById(std::uint32_t id, bool enabled);
    void SetTextColorById(std::uint32_t id, std::uint32_t color);
    void SetButtonOn(std::uint32_t id);                     // cIGZWinBtn vslot 6 ToggleOn
    void SetButtonOff(std::uint32_t id);                    // cIGZWinBtn vslot 7 ToggleOff
    void ClearLoanLabels();
    void ShowLoanRows(std::int64_t paymentScale);
    void SetMonthlyPeriod();
    void SetYearlyPeriod();
    void TakeLoan();                                        // the 0xaa4c33e6 command body
    bool HandlePageCommand(std::uint32_t relativeId);       // the 0xaa3ac400 + n family
    bool HandleSpinnerMessage(const cGZMessage& message);
    bool HandleTextEditKey(const cGZMessage& message);
    bool HandleTextEditKillFocus(const cGZMessage& message);

    gz::cIGZWinGen* window_ = nullptr;             // +0x20 (referenced)
    cSC4BudgetSimulator* budget_ = nullptr;        // +0x28 (referenced)
    ISubBudgetWindow* subBudget_ = nullptr;        // +0x30 (referenced)
    std::vector<cSC4BudgetSimulator::LoanInfo> loans_;   // +0x38
    Values values_;                                // +0x50..+0x170
    float savedTaxRates_[12];                      // +0x178 (deliberately uninitialised: see the quirks)
    std::int32_t loanScroll_ = 0;                  // +0x1a8
    std::uint32_t loanCount_ = 0;                  // +0x1ac
    std::uint32_t pendingLoanCount_ = 0;           // +0x1b0
    std::int32_t pendingLoanTotal_ = 0;            // +0x1b4
    std::int32_t currentLoanAmount_ = 0;           // +0x1b8
    bool expenseView_ = false;                     // +0x1bc
    std::int32_t period_ = 0;                      // +0x1c0
    bool notificationsRegistered_ = false;         // +0x1c4
    bool visible_ = false;                         // +0x1c5
    bool pendingUpdate_ = false;                   // +0x1c6
};

}  // namespace sc4
