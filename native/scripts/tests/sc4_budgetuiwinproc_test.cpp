// cSC4BudgetUIWinProc (src/sc4/BudgetUIWinProc.{h,cpp}) — the balance-sheet window procedure of the budget panel.
//
// The rig is real: real cGZWin / cGZWinGen / cGZWinText / cGZWinTextEdit / cGZWinSpinner / cGZWinBtn controls on the
// shared FakeMgr2, the real recovered cSC4BudgetSimulator (with a fake IBudgetSimEnvironment), the real nSC4UI window-set
// helpers and the real nsGZWinUtils notification-target helper. Only the engine services and the unrecovered
// neighbours are faked through IBudgetUIWinProcHost and its small I* seams (ordinance / neighbours simulators and
// cSC4SubBudgetWindow).
//
// Covered: the constructor's script-window creation and window-set alignment, InitBalanceSheet's label sets (monthly /
// yearly, including the second-column guard), Init's notification targets / page hiding / month toggles / loan amount
// label, ShowWindow / HideWindow / IsVisible, the command dispatch (page family 0xaa3ac4xx and 0xaa3ac5xx, expense and
// income pages, monthly / yearly toggles, hide, taxes, loans, scroll, take-loan, department windows), the spinner
// message (loan amount and the twelve tax spinners, including the yearly payment quirk), the tax text edit's parse /
// fallback / Return paths (including the 3x3 tax-type table quirk), SetTaxes, UpdateBalanceSheetInfo for both periods
// and both pages (amounts and the two text colours), CalculateValues (tax groups, ordinance / neighbour figures,
// department groups, bond payments), the taxes dialog and Cancel / Remove, the loan dialog, UpdateLoanDisplay,
// TakePendingLoans, RemoveLoanWindow, DoSubBudgetWindow, the message dirty flag / Update and Shutdown / EndDialog.
//
// Build (from the repository root; the full source list is scripts/tests/registry/sc4_budgetuiwinproc.txt):
//   /usr/bin/clang++ -std=c++20 -Wall -Wextra -O1 -fsanitize=address,undefined -Isrc -Iscripts/tests/stubs \
//       -Iscripts/tests scripts/tests/sc4_budgetuiwinproc_test.cpp src/sc4/BudgetUIWinProc.cpp \
//       src/sc4/BudgetSimulator.cpp src/sc4/Budget.cpp ... -o /tmp/bui_test && /tmp/bui_test
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fake_winmgr2.h"
#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinSpinner.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BudgetUIWinProc.h"
#include "sc4/SC4Utilities.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const long long va = static_cast<long long>(a); const long long vb = static_cast<long long>(b); \
    if (va != vb) { std::printf("FAIL line %d: %s == %s (%lld vs %lld)\n", __LINE__, #a, #b, va, vb); ++failures; } } while (0)

namespace {

// ---- engine services -----------------------------------------------------------------------------------

struct TestFormatter final : sc4::ILanguageFormatter {
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString&) override {
        out.AssignFromCString(std::to_string(value).c_str());
    }
    void FormatMoney(double value, cIGZString& out, const cIGZString&, const cRZString&) override {
        out.AssignFromCString(std::to_string(value).c_str());
    }
    void FormatNumber(std::int64_t value, cRZString& out) override { out = cRZString(std::to_string(value).c_str()); }
};

struct TestEnv final : sc4::IBudgetSimEnvironment {
    int month = 0;
    std::int64_t ordinanceIncome = 111, ordinanceExpense = 222, neighborsIncome = 333, neighborsExpense = 444;
    int difficulty = 0;
    int messages = 0, budgetMessages = 0, history = 0;
    void SimulatorDate(int& year, int& m) override { year = 2000; m = month; }
    std::int64_t OrdinanceMonthlyIncome() override { return ordinanceIncome; }
    std::int64_t OrdinanceMonthlyExpense() override { return ordinanceExpense; }
    std::int64_t NeighborsIncome() override { return neighborsIncome; }
    std::int64_t NeighborsExpense() override { return neighborsExpense; }
    int DifficultyLevel() override { return difficulty; }
    void ForEachTaxableLot(const std::function<void(const sc4::LotTaxInfo&)>&) override {}
    void PostMessage(std::uint32_t, std::uint32_t, std::int64_t, std::int64_t) override { ++messages; }
    void AddHistory(std::uint32_t, std::int64_t) override { ++history; }
    void PostBudgetMessage(std::uint32_t, sc4::cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override {
        ++budgetMessages;
    }
    void NotifyLocalFundingChanged(sc4::cISCPropertyHolder*) override {}
    std::string LoadLocalizedString(std::uint32_t, std::uint32_t) override { return std::string(); }
    std::string BuildingName(std::uint32_t) override { return std::string(); }
};

struct FakeNotification {
    cIGZMessageTarget2* target = nullptr;
    std::uint32_t type = 0;
    bool add = false;
};

struct FakeMessages final : cIGZMessageServer2 {
    std::vector<FakeNotification> log;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        log.push_back({target, type, true});
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        log.push_back({target, type, false});
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
    int AddCount(std::uint32_t type) const {
        int n = 0;
        for (const FakeNotification& entry : log) if (entry.add && entry.type == type) ++n;
        return n;
    }
    int RemoveCount(std::uint32_t type) const {
        int n = 0;
        for (const FakeNotification& entry : log) if (!entry.add && entry.type == type) ++n;
        return n;
    }
};

// cISC4OrdinanceSimple's GetCurrentMonthlyIncome (vslot 0x28) as the panel uses it.
struct FakeOrdinanceItem : sc4::IOrdinanceBudgetItem {
    std::int64_t income = 0;
    std::int64_t CurrentMonthlyIncome() override { return income; }
};

struct FakeOrdinances final : sc4::IOrdinanceBudgetApi {
    std::vector<std::uint32_t> ids;
    std::map<std::uint32_t, FakeOrdinanceItem> items;
    std::map<std::uint32_t, bool> on;
    int countCalls = 0, arrayCalls = 0, lookupCalls = 0, onCalls = 0;
    std::uint32_t AvailableCount() override {
        ++countCalls;
        return static_cast<std::uint32_t>(ids.size());
    }
    void AvailableIds(std::uint32_t* out, std::uint32_t& count) override {
        ++arrayCalls;
        const std::uint32_t n = count < ids.size() ? count : static_cast<std::uint32_t>(ids.size());
        for (std::uint32_t i = 0; i < n; ++i) out[i] = ids[i];
    }
    sc4::IOrdinanceBudgetItem* OrdinanceById(std::uint32_t id) override {
        ++lookupCalls;
        auto it = items.find(id);
        return it == items.end() ? nullptr : &it->second;
    }
    bool IsOrdinanceOn(std::uint32_t id) override {
        ++onCalls;
        return on[id];
    }
};

struct FakeNeighbors final : sc4::INeighborsBudgetApi {
    std::int64_t income = 0, expense = 0;
    int calls = 0;
    std::int64_t MonthlyDealIncome() override { ++calls; return income; }
    std::int64_t MonthlyDealExpense() override { ++calls; return expense; }
};

// The modal loop ends when the dialog hides; the fake RunPopup hides it, like the real one does after EndDialog.
struct TestMgr final : FakeMgr2 {
    int RunPopup(cIGZWin* window) override {
        const int result = FakeMgr2::RunPopup(window);
        if (window) window->HideWindow();
        return result;
    }
};

// ---- cSC4SubBudgetWindow ------------------------------------------------------------------------------

struct SubBudgetLog {
    int created = 0, destroyed = 0;
    std::uint32_t purpose = 0, period = 0;
    cIGZWin* lastWindow = nullptr;
};

struct FakeSubBudget final : sc4::ISubBudgetWindow {
    FakeSubBudget(SubBudgetLog* log, TestMgr* mgr, cIGZWin* view) : log_(log), mgr_(mgr), view_(view) {}
    std::uint32_t refs = 0;
    SubBudgetLog* log_;
    TestMgr* mgr_;
    cIGZWin* view_;
    cIGZWin* window_ = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        const std::uint32_t result = refs ? --refs : 0;
        if (refs == 0) {
            delete this;
            return 0;
        }
        return result;
    }
    bool Create(cIGZWin** window, std::uint32_t purpose, std::uint32_t period, std::uint32_t) override {
        ++log_->created;
        log_->purpose = purpose;
        log_->period = period;
        auto* dialog = new gz::cGZWinGen(mgr_);
        mgr_->valid.insert(dialog);
        dialog->SetWinGenFlag(gz::kGenDialog, true);
        dialog->SetID(0x5b000000u + purpose);
        dialog->SetArea(0, 0, 300, 220);
        if (view_) view_->ChildAdd(dialog);
        dialog->AddRef();   // the reference handed to CreateSubBudgetWindow's caller
        window_ = dialog;
        log_->lastWindow = dialog;
        if (window) *window = dialog;   // the ctor's reference
        return true;
    }
    void Destroy() override { ++log_->destroyed; }
};

// ---- the fake window tree -------------------------------------------------------------------------------

struct RecordingText final : gz::cGZWinText {
    using cGZWinText::cGZWinText;
    std::uint32_t lastColor = 0xffffffffu;
    int colorCalls = 0;
    bool SetTextColor(tFontColorIdx index, std::uint32_t native) override {
        lastColor = native;
        ++colorCalls;
        return cGZWinText::SetTextColor(index, native);
    }
};


template <typename T>
T* AddWindow(TestMgr& mgr, cIGZWin* parent, std::uint32_t id) {
    T* window = new T(&mgr);
    mgr.valid.insert(window);
    window->SetID(id);
    window->SetArea(0, 0, 120, 20);
    if (parent) parent->ChildAdd(window);
    return window;
}

gz::cGZWinGen* AddGenWindow(TestMgr& mgr, cIGZWin* parent, std::uint32_t id) {
    auto* window = new gz::cGZWinGen(&mgr);
    mgr.valid.insert(window);
    window->SetWinGenFlag(gz::kGenDialog, true);
    window->SetID(id);
    window->SetArea(0, 0, 320, 240);
    if (parent) parent->ChildAdd(window);
    return window;
}

struct Rig {
    TestMgr mgr;
    cIGZWin* view = nullptr;
    gz::cGZWinGen* yearlyPage = nullptr;
    gz::cGZWinGen* taxesWindow = nullptr;
    gz::cGZWinGen* loanWindow = nullptr;
    gz::cGZWinGen* budgetWindow = nullptr;
    std::map<std::uint32_t, RecordingText*> texts;
    std::map<std::uint32_t, gz::cGZWinSpinner*> spinners;
    std::map<std::uint32_t, gz::cGZWinTextEdit*> edits;
    std::map<std::uint32_t, gz::cGZWinBtn*> buttons;
    std::map<std::uint32_t, cIGZWin*> windows;

    RecordingText* Text(std::uint32_t id) const {
        auto it = texts.find(id);
        return it == texts.end() ? nullptr : it->second;
    }
    gz::cGZWinSpinner* Spinner(std::uint32_t id) const {
        auto it = spinners.find(id);
        return it == spinners.end() ? nullptr : it->second;
    }
    gz::cGZWinTextEdit* Edit(std::uint32_t id) const {
        auto it = edits.find(id);
        return it == edits.end() ? nullptr : it->second;
    }
    gz::cGZWinBtn* Button(std::uint32_t id) const {
        auto it = buttons.find(id);
        return it == buttons.end() ? nullptr : it->second;
    }
    cIGZWin* Window(std::uint32_t id) const {
        auto it = windows.find(id);
        return it == windows.end() ? nullptr : it->second;
    }
    std::string Caption(std::uint32_t id) const {
        cIGZWin* window = Window(id);
        return window ? std::string(window->GetCaption().ToChar()) : std::string("<missing>");
    }
};

struct TestHost final : sc4::IBudgetUIWinProcHost {
    TestMgr* mgr = nullptr;
    cIGZWin* view = nullptr;
    Rig* rig = nullptr;
    TestEnv* env = nullptr;
    FakeMessages* messages = nullptr;
    FakeOrdinances* ordinances = nullptr;
    FakeNeighbors* neighbors = nullptr;
    SubBudgetLog* subLog = nullptr;
    sc4::cSC4BudgetSimulator* budget = nullptr;
    cGZPersistResourceKey lastKey{};
    std::uint32_t lastWindowId = 0;
    int pauses = 0, resumes = 0, addTicks = 0, removeTicks = 0, removedChannels = 0;
    bool modal = false;
    int decimalCalls = 0;
    std::vector<cIGZWin*> restored, saved;
    std::vector<std::uint32_t> colors;
    bool filterResult = true;
    int filterCalls = 0;

    cIGZWin* ViewWindow() override { return view; }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                std::uint32_t iid, void** out) override {
        lastKey = key;
        lastWindowId = id;
        (void)iid;
        auto* window = new gz::cGZWinGen(mgr);
        mgr->valid.insert(window);
        window->SetWinGenFlag(gz::kGenDialog, true);
        window->SetID(id);
        window->SetArea(0, 0, 320, 240);
        if (parent) parent->ChildAdd(window);
        window->AddRef();   // the script service hands over one counted reference
        // The script service answers the requested interface (0x5386d516), not the cGZWin base pointer.
        *out = static_cast<gz::cIGZWinGen*>(window);
        if (rig) rig->budgetWindow = window;
        return true;
    }
    cIGZWinMgr* WindowManager() override { return mgr; }
    bool WindowManagerIsModal() override { return modal; }
    sc4::cSC4BudgetSimulator* Budget() override { return budget; }
    sc4::IOrdinanceBudgetApi* Ordinances() override { return ordinances; }
    sc4::INeighborsBudgetApi* Neighbors() override { return neighbors; }
    cIGZMessageServer2* Messages() override { return messages; }
    void AddTickNotification(cIGZMessageTarget2*, std::uint32_t) override { ++addTicks; }
    void RemoveTickNotification(cIGZMessageTarget2*) override { ++removeTicks; ++removedChannels; }
    void HiddenPause() override { ++pauses; }
    void HiddenResume() override { ++resumes; }
    bool SaveWindowLocation(cIGZWin* window) override {
        saved.push_back(window);
        return true;
    }
    bool RestoreWindowLocation(cIGZWin* window, bool) override {
        restored.push_back(window);
        return true;
    }
    void DecimalSeparator(cIGZString& out) override {
        ++decimalCalls;
        out.AssignFromCString(".");
    }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        const std::uint32_t color = 0xff000000u | (static_cast<std::uint32_t>(r) << 16) |
                                    (static_cast<std::uint32_t>(g) << 8) | b;
        colors.push_back(color);
        return color;
    }
    bool FilterUICommandMessage(cGZMessage&) override {
        ++filterCalls;
        return filterResult;
    }
    sc4::ISubBudgetWindow* CreateSubBudgetWindow() override {
        auto* sub = new FakeSubBudget(subLog, mgr, view);
        return sub;
    }
};

cGZMessage Command(std::uint64_t id) {
    cGZMessage message;
    message.type = kGZMsgCommand;
    message.param1 = 0x287259f6;
    message.param2 = id;
    return message;
}

cGZMessage SpinnerMessage(std::uint64_t id, std::int64_t value) {
    cGZMessage message;
    message.type = 0x682876c9;
    message.param1 = id;
    message.param2 = static_cast<std::uint64_t>(value);
    return message;
}

struct FakeMessage2 final : cIGZMessage2 {
    explicit FakeMessage2(std::uint32_t value) : type(value) {}
    std::uint32_t type = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
};

}  // namespace

int main() {
    // ---- the real simulator, the fake environment and the services ------------------------------------
    TestFormatter formatter;
    sc4::SetLanguageFormatter(&formatter);
    TestEnv env;
    sc4::cSC4BudgetSimulator::SetEnvironment(&env);
    auto* budget = new sc4::cSC4BudgetSimulator();
    budget->AddRef();   // the test's own reference (the panel takes one more when it adopts it)

    FakeMessages messages;
    FakeOrdinances ordinances;
    FakeNeighbors neighbors;
    SubBudgetLog subLog;
    Rig rig;
    TestHost host;
    host.mgr = &rig.mgr;
    host.rig = &rig;
    host.env = &env;
    host.messages = &messages;
    host.ordinances = &ordinances;
    host.neighbors = &neighbors;
    host.subLog = &subLog;
    host.budget = budget;

    // A real view window that hosts every script window and control.
    rig.view = new cGZWin(&rig.mgr);
    rig.mgr.valid.insert(rig.view);
    rig.view->SetID(0x00c0ffee);
    rig.view->SetArea(0, 0, 800, 600);
    host.view = rig.view;

    auto addText = [&](std::uint32_t id, cIGZWin* parent) {
        auto* text = new RecordingText(&rig.mgr);
        rig.mgr.valid.insert(text);
        text->SetID(id);
        text->SetArea(0, 0, 120, 20);
        if (parent) parent->ChildAdd(text);
        rig.texts[id] = text;
        rig.windows[id] = text;
        return text;
    };
    auto addWindow = [&](std::uint32_t id, cIGZWin* parent) {
        cIGZWin* window = new cGZWin(&rig.mgr);
        rig.mgr.valid.insert(window);
        window->SetID(id);
        window->SetArea(0, 0, 60, 20);
        if (parent) parent->ChildAdd(window);
        rig.windows[id] = window;
        return window;
    };
    auto addButton = [&](std::uint32_t id, cIGZWin* parent) {
        auto* button = AddWindow<gz::cGZWinBtn>(rig.mgr, parent, id);
        rig.buttons[id] = button;
        rig.windows[id] = button;
        return button;
    };
    auto addSpinner = [&](std::uint32_t id, cIGZWin* parent, int low, int high) {
        auto* spinner = AddWindow<gz::cGZWinSpinner>(rig.mgr, parent, id);
        spinner->SetMinMax(low, high);
        rig.spinners[id] = spinner;
        rig.windows[id] = spinner;
        return spinner;
    };
    auto addEdit = [&](std::uint32_t id, cIGZWin* parent) {
        auto* edit = AddWindow<gz::cGZWinTextEdit>(rig.mgr, parent, id);
        rig.edits[id] = edit;
        rig.windows[id] = edit;
        return edit;
    };

    // The balance-sheet labels and amount fields (direct children of the view window, the original's layout).
    for (std::uint32_t id : {0xaa3ac104u, 0xaa3ac105u, 0xaa3ac106u, 0xaa3ac107u, 0xaa3ac108u, 0xaa3ac109u,
                             0xaa3ac10au, 0xaa3ac10bu, 0xaa3ac10cu, 0xaa3ac10du, 0xaa3ad120u, 0xaa3ad121u,
                             0xaa3ad122u, 0xaa3ad123u, 0xaa3ad200u, 0xaa3ad201u, 0xaa3ad202u, 0xaa3ad203u})
        addText(id, rig.view);
    for (std::uint32_t id : {0xaa3ac100u, 0xaa3ac101u, 0xaa3ac102u, 0xaa3ac103u, 0xaa3ac10eu, 0xaa3ac10fu,
                             0xaa3ac110u, 0xaa3ac111u, 0xaa3ac112u, 0xaa3ac113u, 0xaa3ac114u, 0xaa3ac115u,
                             0xaa3ac11fu, 0xaa3ac120u, 0xaa3ac121u, 0xaa3ac122u, 0xaa3ac123u, 0xaa3ac124u,
                             0xaa3ac125u, 0xaa3ac126u, 0xaa3ac127u, 0xaa3ac128u, 0xaa3ac129u, 0xaa3ac12au})
        addText(id, rig.view);
    // The per-type tax incomes SetTaxes writes (0x300 + type) and the three group totals.
    for (std::uint32_t type = 0; type < 12; ++type) addText(0x300 + type, rig.view);
    for (std::uint32_t id : {0x500u, 0x501u, 0x502u}) addText(id, rig.view);

    // The yearly page and the two dialogs.
    rig.yearlyPage = AddGenWindow(rig.mgr, rig.view, 0xaa3ac001);
    rig.taxesWindow = AddGenWindow(rig.mgr, rig.view, 0xaa3ac002);
    rig.loanWindow = AddGenWindow(rig.mgr, rig.view, 0xca4c332d);

    // Taxes dialog: twelve spinners 0x200..0x20b and twelve text edits 0x100..0x10b.
    for (std::uint32_t type = 0; type < 12; ++type) addSpinner(0x200 + type, rig.taxesWindow, 0, 1000);
    for (std::uint32_t type = 0; type < 12; ++type) addEdit(0x100 + type, rig.taxesWindow);

    // Loan dialog controls.
    addSpinner(0x8a4c34d4, rig.loanWindow, 0, 100000);
    addWindow(0xaa4c33e6, rig.loanWindow);
    addWindow(0x2a4c33a6, rig.loanWindow);
    addWindow(0xaa4c339c, rig.loanWindow);
    addWindow(0xa4c34be, rig.loanWindow);
    addWindow(0xa4c34db, rig.loanWindow);
    addText(0xa4c3409, rig.loanWindow);
    addText(0xea4c34a6, rig.loanWindow);
    addText(0xca4c349d, rig.loanWindow);
    addText(0xca4c34e2, rig.loanWindow);
    addText(0x6a4c3494, rig.loanWindow);
    for (std::uint32_t row = 0; row < 4; ++row) {
        addText(0x4a4c33ad + row, rig.loanWindow);
        addText(0x8a4c33d0 + row, rig.loanWindow);
        addText(0xa4c3405 + row, rig.loanWindow);
        addText(0xca4c3426 + row, rig.loanWindow);
    }
    // The taxes dialog's OK / Cancel buttons and the page / month buttons.
    addButton(0xaa4c353b, rig.view);
    addButton(0xca4c352f, rig.view);
    addButton(0x4a8879cb, rig.view);
    addButton(0x4a8879db, rig.view);
    addButton(0x4a8879cc, rig.view);
    addButton(0x4a8879cd, rig.view);
    addButton(0x4a8879dc, rig.view);
    addButton(0x4a8879dd, rig.view);
    addWindow(0x6a5bb0fb, rig.view);
    addWindow(0x4a5bb1b5, rig.view);
    addWindow(0xa4c3354, rig.view);
    for (std::uint32_t i = 0; i < 14; ++i) {
        addWindow(0xaa3ac400 + i, rig.view);
        addWindow(0xaa3ac500 + i, rig.view);
    }

    sc4::SetBudgetUIWinProcHost(&host);
    SetWinGraphics(nullptr);   // not used by this rig

    // ---- construction ---------------------------------------------------------------------------------
    auto* proc = new sc4::cSC4BudgetUIWinProc();
    CHECK(proc->Window() != nullptr);
    CHECK(rig.budgetWindow != nullptr);
    CHECK_EQ(host.lastKey.group, 0x96a006b0u);
    CHECK_EQ(host.lastKey.instance, 0xcbc3c2b9u);
    CHECK_EQ(host.lastWindowId, 0xaa3ac000u);
    CHECK(rig.budgetWindow->GetID() == 0xaa3ac000u);
    CHECK(rig.budgetWindow->GetParentWin() == rig.view);
    CHECK(rig.budgetWindow->GetWinProc() != nullptr);
    CHECK(proc->NotificationsRegistered());        // SetWinProc's 0xa2bf8ad5 registered the notifications
    CHECK_EQ(messages.AddCount(0x8dac3e8), 1);
    CHECK_EQ(messages.AddCount(0x8dac3e7), 1);
    CHECK_EQ(messages.AddCount(0x86ad10ee), 1);
    CHECK_EQ(host.addTicks, 1);
    for (std::uint32_t id : {0xaa3ac108u, 0xaa3ac104u, 0xaa3ac10du, 0xaa3ad120u, 0xaa3ad200u, 0xaa3ad203u}) {
        CHECK(rig.Text(id) != nullptr);
        CHECK(rig.Window(id)->GetCaption().Strlen() <= 1);   // the strings are not available in the test
    }
    CHECK_EQ(proc->Period(), 0);
    CHECK(!proc->IsVisible());

    // ---- Init -----------------------------------------------------------------------------------------
    rig.Spinner(0x8a4c34d4)->SetValue(1500);
    proc->Init();
    CHECK(!rig.budgetWindow->IsVisible());
    CHECK(!rig.yearlyPage->IsVisible());
    CHECK(!rig.taxesWindow->IsVisible());
    CHECK(!rig.loanWindow->IsVisible());
    CHECK(rig.Button(0x4a8879cc)->IsOn());
    CHECK(!rig.Button(0x4a8879cd)->IsOn());
    CHECK(rig.Button(0x4a8879dc)->IsOn());
    CHECK(!rig.Button(0x4a8879dd)->IsOn());
    CHECK(rig.Text(0xa4c3409)->GetCaption().IsEqualToStringRef(cRZString("1500"), true));
    // Notification targets: a page button, the loan spinner and one tax spinner / edit point at the budget window.
    CHECK(rig.Button(0x4a8879cb)->GetNotificationTarget() == rig.budgetWindow);
    CHECK(rig.Spinner(0x8a4c34d4)->GetNotificationTarget() == rig.budgetWindow);
    CHECK(rig.Spinner(0x203)->GetNotificationTarget() == rig.budgetWindow);
    CHECK(rig.Edit(0x103)->GetNotificationTarget() == rig.budgetWindow);

    // ---- ShowWindow / HideWindow / Update -------------------------------------------------------------
    proc->ShowWindow();
    CHECK(proc->IsVisible());
    CHECK(rig.budgetWindow->IsVisible());
    CHECK(!rig.yearlyPage->IsVisible());
    CHECK(rig.mgr.focus == rig.view);
    CHECK(rig.Text(0xaa3ac100)->GetCaption().IsEqualToStringRef(
        cRZString(std::to_string(budget->GetTotalFunds()).c_str()), true));
    proc->Update();
    CHECK(proc->PendingUpdate());
    proc->HideWindow();
    CHECK(!proc->IsVisible());
    CHECK(!rig.budgetWindow->IsVisible());
    CHECK(!rig.taxesWindow->IsVisible());
    CHECK(!rig.loanWindow->IsVisible());

    // ---- command dispatch -----------------------------------------------------------------------------
    auto* winProc = static_cast<gz::cIGZWinProc*>(proc);
    {
        cGZMessage message = Command(0xaa3ac40e);   // expense page
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(proc->ExpenseView());
        proc->ShowWindow();
        CHECK(rig.yearlyPage->IsVisible());
        CHECK(!rig.budgetWindow->IsVisible());
        message = Command(0xaa3ac40f);              // income page
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(!proc->ExpenseView());
        proc->ShowWindow();
        CHECK(rig.budgetWindow->IsVisible());
        // Same commands through the 0xaa3ac5xx family.
        message = Command(0xaa3ac50e);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(proc->ExpenseView());
        message = Command(0xaa3ac50f);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(!proc->ExpenseView());
    }
    {
        cGZMessage message = Command(0x4a8879cd);   // yearly
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(proc->Period(), 1);
        CHECK(rig.Button(0x4a8879cd)->IsOn());
        CHECK(!rig.Button(0x4a8879cc)->IsOn());
        CHECK(rig.Button(0x4a8879dd)->IsOn());
        CHECK(!rig.Button(0x4a8879dc)->IsOn());
        CHECK(rig.Text(0xaa3ac101)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetEstIncome()).c_str()), true));
        message = Command(0x4a8879dd);   // yearly again (the second button)
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(proc->Period(), 1);
        message = Command(0x4a8879cc);   // monthly
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(proc->Period(), 0);
        CHECK(rig.Text(0xaa3ac101)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetTotalMonthlyIncome()).c_str()), true));
        message = Command(0x4a8879dc);   // monthly again (the second button)
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(proc->Period(), 0);
        // The hide commands.
        proc->ShowWindow();
        message = Command(0x6a5bb0fb);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(!proc->IsVisible());
        proc->ShowWindow();
        message = Command(0x4a5bb1b5);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(!proc->IsVisible());
    }
    {
        // A command from another sender, an unknown command, and the tutorial filter.
        cGZMessage message = Command(0xaa3ac40e);
        message.param1 = 0x1234;
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        message = Command(0xdeadbeef);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        host.filterResult = false;
        const int calls = host.filterCalls;
        message = Command(0xaa3ac40e);
        CHECK(winProc->DoWinProcMessage(rig.budgetWindow, message));   // swallowed
        CHECK_EQ(host.filterCalls, calls + 1);
        CHECK(!proc->ExpenseView());
        host.filterResult = true;
        // A message for another window is ignored.
        cGZMessage foreign = Command(0xaa3ac40e);
        CHECK(!winProc->DoWinProcMessage(rig.view, foreign));
        // The modal key-down gate.
        cGZMessage key;
        key.type = kGZMsgKeyDown;
        host.modal = false;
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, key));
        host.modal = true;
        CHECK(winProc->DoWinProcMessage(rig.budgetWindow, key));
        host.modal = false;
    }

    // ---- spinner message: loan amount and tax rates ---------------------------------------------------
    proc->ShowWindow();
    {
        cGZMessage message = SpinnerMessage(0x8a4c34d4, 2500);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(proc->CurrentLoanAmount(), 2500);
        CHECK(rig.Text(0xa4c3409)->GetCaption().IsEqualToStringRef(cRZString("2500"), true));
        CHECK(rig.Text(0xca4c34e2)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetMonthlyPaymentForLoan(2500)).c_str()), true));
        // Yearly period: the payment is multiplied by 12 (the quirk).
        cGZMessage yearly = Command(0x4a8879cd);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, yearly));
        message = SpinnerMessage(0x8a4c34d4, 2500);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(rig.Text(0xca4c34e2)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetMonthlyPaymentForLoan(2500) * 12).c_str()), true));
        cGZMessage monthly = Command(0x4a8879cc);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, monthly));
    }
    {
        // Tax spinner: value in tenths of a percent, written to the lower tax type.
        cGZMessage message = SpinnerMessage(0x203, 75);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(budget->GetTaxRate(3) > 7.49f && budget->GetTaxRate(3) < 7.51f);
        CHECK(rig.Edit(0x103)->GetCaption().IsEqualToStringRef(cRZString("7.5"), true));
        CHECK_EQ(host.decimalCalls, 1);
        // The tax totals went through SetTaxes (factor 1 in the monthly period).
        {
            const cRZString expected(std::to_string(static_cast<int>(budget->GetTaxIncome(3))).c_str());
            CHECK(rig.Text(0x303)->GetCaption().IsEqualToStringRef(expected, true));
        }
        // Fractional taxes keep the fraction (0.5 -> 5 tenths -> "0.5" for tax type 4).
        message = SpinnerMessage(0x204, 5);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(rig.Edit(0x104)->GetCaption().IsEqualToStringRef(cRZString("0.5"), true));
    }

    // ---- tax text edit: kill focus, parse fallback and Return -----------------------------------------
    {
        budget->SetTaxRate(1, 3.0f);
        rig.Edit(0x101)->SetText(cRZString("4.5"), false);
        cGZMessage message;
        message.type = 0x9b1552d9;
        message.param1 = reinterpret_cast<std::uintptr_t>(static_cast<gz::cIGZWinTextEdit*>(rig.Edit(0x101)));
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(rig.Spinner(0x201)->GetValue(), 45);
        CHECK(rig.Edit(0x101)->GetCaption().IsEqualToStringRef(cRZString("4.5"), true));
        CHECK(budget->GetTaxRate(1) > 4.49f && budget->GetTaxRate(1) < 4.51f);
        // Garbage text: the spinner falls back to the rate of kBudgetUITaxTypeTable[2] = tax type 9.
        budget->SetTaxRate(9, 6.0f);
        budget->SetTaxRate(2, 2.0f);
        rig.Edit(0x102)->SetText(cRZString("garbage"), false);
        message.param1 = reinterpret_cast<std::uintptr_t>(static_cast<gz::cIGZWinTextEdit*>(rig.Edit(0x102)));
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(rig.Spinner(0x202)->GetValue(), 60);
        // The spinner notification then writes the *slot* tax type (2) with that value; tax type 9 stays.
        CHECK(budget->GetTaxRate(2) > 5.99f && budget->GetTaxRate(2) < 6.01f);
        CHECK(budget->GetTaxRate(9) > 5.99f && budget->GetTaxRate(9) < 6.01f);
        // Return: select all of the next field (0x102 -> 0x103) and focus it.
        rig.Edit(0x103)->SetText(cRZString("1234"), false);
        message.type = 0x9b1552d5;
        message.param1 = reinterpret_cast<std::uintptr_t>(static_cast<gz::cIGZWinTextEdit*>(rig.Edit(0x102)));
        message.param2 = 0xd;
        rig.mgr.focus = nullptr;
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(rig.mgr.focus == rig.Edit(0x103)->AsIGZWin());
        std::uint32_t start = 0, end = 0;
        CHECK(rig.Edit(0x103)->GetSelection(start, end));
        CHECK_EQ(start, 0);
        CHECK_EQ(end, 4);
        // A different key is ignored.
        message.type = 0x9b1552d5;
        message.param2 = 0x41;
        rig.mgr.focus = nullptr;
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(rig.mgr.focus == nullptr);
    }

    // ---- CalculateValues / UpdateBalanceSheetInfo -----------------------------------------------------
    {
        // A department in group 0x4a357b40 with a 1000 shekel expense, and one in 0xaa369059 with 400 income.
        auto* expenseDepartment = budget->CreateDepartmentBudget(0x1111, 0x4a357b40);
        auto* expenseItem = expenseDepartment->CreateLineItem(0x2222, false);
        expenseItem->SetFullExpenses(1000);
        expenseDepartment->SetFundingPercentage(1.0f, 0, false);
        budget->DoBudgetGroupChanged(0x4a357b40);
        auto* incomeDepartment = budget->CreateDepartmentBudget(0x3333, 0xaa369059);
        auto* incomeItem = incomeDepartment->CreateLineItem(0x4444, false);
        incomeItem->SetIncome(400);
        budget->DoBudgetGroupChanged(0xaa369059);

        // Ordinance: one income (+100, on), one expense (-60, on) and one off (+999).
        ordinances.ids = {0x9001, 0x9002, 0x9003};
        ordinances.items[0x9001].income = 100;
        ordinances.items[0x9002].income = -60;
        ordinances.items[0x9003].income = 999;
        ordinances.on[0x9001] = true;
        ordinances.on[0x9002] = true;
        ordinances.on[0x9003] = false;
        neighbors.income = 55;
        neighbors.expense = 66;

        proc->CalculateValues();
        const sc4::cSC4BudgetUIWinProc::Values& values = proc->CalculatedValues();
        CHECK_EQ(values.funds, budget->GetTotalFunds());
        CHECK_EQ(values.monthlyIncome, budget->GetTotalMonthlyIncome());
        CHECK_EQ(values.estimatedIncome, budget->GetEstIncome());
        CHECK_EQ(values.monthlyExpense, budget->GetTotalMonthlyExpense());
        CHECK_EQ(values.estimatedExpense, budget->GetEstExpenses());
        CHECK_EQ(values.taxGroup1, budget->GetTaxIncome(0) + budget->GetTaxIncome(1) + budget->GetTaxIncome(2));
        CHECK_EQ(values.taxGroup2, budget->GetTaxIncome(3) + budget->GetTaxIncome(4) + budget->GetTaxIncome(5) +
                                       budget->GetTaxIncome(6) + budget->GetTaxIncome(7));
        CHECK_EQ(values.taxGroup3, budget->GetTaxIncome(8) + budget->GetTaxIncome(9) + budget->GetTaxIncome(10) +
                                       budget->GetTaxIncome(11));
        CHECK_EQ(values.taxGroup1Yearly, values.taxGroup1 * 12);
        CHECK_EQ(values.ordinanceIncome, 100);
        CHECK_EQ(values.ordinanceExpense, 60);
        CHECK_EQ(values.ordinanceIncomeYearly, 1200);
        CHECK_EQ(values.neighborsIncome, 55);
        CHECK_EQ(values.neighborsExpense, 66);
        CHECK_EQ(values.groupExpense3, 1000);
        CHECK_EQ(values.groupExpense3Yearly, 12000);
        CHECK_EQ(values.groupIncome2, 400);
        CHECK_EQ(values.groupExpense2, 0);
        CHECK_EQ(values.bondPayments, budget->GetTotalMonthlyBondPayments());
        CHECK_EQ(values.bondPaymentsYearly, values.bondPayments * 12);
        CHECK(ordinances.countCalls > 0);
        CHECK_EQ(ordinances.countCalls, ordinances.arrayCalls);
        CHECK_EQ(ordinances.lookupCalls, 3);   // the three ids of the last CalculateValues
        CHECK_EQ(ordinances.onCalls, ordinances.lookupCalls);

        // Monthly income page: funds, income, expense, balance and the two text colours.
        proc->UpdateBalanceSheetInfo();
        CHECK(rig.Text(0xaa3ac100)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.funds).c_str()), true));
        CHECK(rig.Text(0xaa3ac101)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.monthlyIncome).c_str()), true));
        CHECK(rig.Text(0xaa3ac102)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.monthlyExpense).c_str()), true));
        CHECK(rig.Text(0xaa3ac103)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.monthlyIncome + values.funds - values.monthlyExpense).c_str()), true));
        const std::uint32_t normal = 0xff000000u | (0x44u << 16) | (0x55u << 8) | 0x66u;
        const std::uint32_t red = 0xff000000u | (0x77u << 16);
        CHECK_EQ(rig.Text(0xaa3ac100)->lastColor, values.funds < 0 ? red : normal);
        CHECK_EQ(rig.Text(0xaa3ac107)->lastColor, values.funds < 0 ? red : normal);
        CHECK_EQ(rig.Text(0xaa3ac103)->lastColor,
                 values.monthlyIncome + values.funds < values.monthlyExpense ? red : normal);

        // Expense page, monthly: the extra rows.
        cGZMessage command = Command(0xaa3ac40e);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, command));
        CHECK(rig.Text(0xaa3ac10e)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.funds).c_str()), true));
        CHECK(rig.Text(0xaa3ac112)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.taxGroup1).c_str()), true));
        CHECK(rig.Text(0xaa3ac11f)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.ordinanceExpense).c_str()), true));
        CHECK(rig.Text(0xaa3ac120)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.neighborsIncome).c_str()), true));
        CHECK(rig.Text(0xaa3ac122)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.groupIncome1).c_str()), true));
        CHECK(rig.Text(0xaa3ac125)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.groupExpense3).c_str()), true));
        CHECK_EQ(rig.Text(0xaa3ac10d)->lastColor, values.funds < 0 ? red : normal);

        // Yearly expense page: the same ids get the x12 values.
        command = Command(0x4a8879cd);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, command));
        CHECK(rig.Text(0xaa3ac10f)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.estimatedIncome).c_str()), true));
        CHECK(rig.Text(0xaa3ac112)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.taxGroup1Yearly).c_str()), true));
        CHECK(rig.Text(0xaa3ac125)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.groupExpense3Yearly).c_str()), true));
        CHECK(rig.Text(0xaa3ac12a)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.bondPaymentsYearly).c_str()), true));
        // Yearly income page.
        command = Command(0xaa3ac40f);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, command));
        CHECK(rig.Text(0xaa3ac101)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.estimatedIncome).c_str()), true));
        CHECK(rig.Text(0xaa3ac102)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.estimatedExpense).c_str()), true));
        CHECK(rig.Text(0xaa3ac103)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(values.funds + values.estimatedIncome - values.estimatedExpense).c_str()), true));
        command = Command(0x4a8879cc);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, command));
    }

    // ---- taxes dialog ---------------------------------------------------------------------------------
    {
        budget->SetTaxRate(0, 5.0f);
        budget->SetTaxRate(11, 9.9f);
        proc->DisplayTaxesWindow();
        CHECK_EQ(host.pauses, 1);
        CHECK_EQ(host.mgr->popups, 1);   // FakeMgr::RunPopup
        CHECK(!host.restored.empty());
        CHECK(host.restored.front() == rig.taxesWindow);
        CHECK(!host.saved.empty());
        CHECK(host.saved.front() == rig.taxesWindow);
        CHECK(rig.mgr.focus == rig.view);
        CHECK_EQ(proc->SavedTaxRate(0), 50.0f);   // the tenths as a float
        CHECK_EQ(proc->SavedTaxRate(11), 99.0f);
        CHECK_EQ(rig.Spinner(0x200)->GetValue(), 50);
        CHECK_EQ(rig.Spinner(0x20b)->GetValue(), 99);
        CHECK(rig.Edit(0x100)->GetCaption().IsEqualToStringRef(cRZString("5.0"), true));
        // The modal loop hid the dialog, so the panel did not cancel it on its own.
        CHECK(!rig.taxesWindow->IsVisible());
        CHECK_EQ(host.resumes, 0);
        CHECK_EQ(host.mgr->dialogs.size(), 0u);
        // Cancel restores the saved rates over the current ones, Remove ends the dialog and resumes.
        budget->SetTaxRate(0, 8.0f);
        proc->CancelTaxesWindow();
        CHECK(budget->GetTaxRate(0) > 4.99f && budget->GetTaxRate(0) < 5.01f);
        CHECK_EQ(host.mgr->dialogs.size(), 1u);
        CHECK_EQ(host.mgr->dialogs[0].code, 0xf);
        CHECK(host.mgr->dialogs[0].window == rig.taxesWindow);
        CHECK_EQ(host.resumes, 0);
        proc->RemoveTaxesWindow();
        CHECK_EQ(host.resumes, 1);
        CHECK_EQ(host.mgr->dialogs.size(), 2u);
    }

    // ---- loan dialog ----------------------------------------------------------------------------------
    {
        CHECK(budget->IssueBond(1000));
        CHECK(budget->IssueBond(2000));
        CHECK_EQ(budget->GetTotalBorrowed(), 3000);
        const std::uint64_t dialogsBefore = host.mgr->dialogs.size();
        const int pausesBefore = host.pauses;
        proc->DisplayLoanWindow();
        CHECK_EQ(host.pauses, pausesBefore + 1);
        CHECK_EQ(host.mgr->popups, 2);
        CHECK(!rig.loanWindow->IsVisible());   // the modal loop hid it, like the real EndDialog path
        CHECK(rig.Text(0xea4c34a6)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetCurrentBorrowingLimit()).c_str()), true));
        CHECK(rig.Text(0xca4c349d)->GetCaption().IsEqualToStringRef(cRZString("3000"), true));
        CHECK(rig.Text(0xca4c34e2)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetMonthlyPaymentForLoan(1000)).c_str()), true));
        CHECK_EQ(proc->LoanCount(), 2);
        CHECK_EQ(proc->PendingLoanCount(), 2);
        CHECK_EQ(proc->CurrentLoanAmount(), 1000);
        CHECK(rig.Text(0x6a4c3494)->GetCaption().IsEqualToStringRef(
            cRZString(std::to_string(budget->GetMonthlyPaymentForLoan(1000) +
                                    budget->GetMonthlyPaymentForLoan(2000)).c_str()), true));
        CHECK(rig.Text(0x8a4c33d0)->GetCaption().IsEqualToStringRef(cRZString("1000"), true));
        CHECK(rig.Text(0x8a4c33d1)->GetCaption().IsEqualToStringRef(cRZString("2000"), true));
        CHECK(!rig.Window(0x2a4c33a6)->IsVisible());
        CHECK(!rig.Window(0xaa4c339c)->IsVisible());
        // A pending loan: take 1500 (bond increment 1000 -> IssueBond rounds to 1000), then the dialog closes.
        cGZMessage message = SpinnerMessage(0x8a4c34d4, 1500);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(proc->CurrentLoanAmount(), 1500);
        const int resumesBefore = host.resumes;
        message = Command(0xaa4c33e6);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK(host.resumes == resumesBefore + 1);
        CHECK(!rig.loanWindow->IsVisible());
        CHECK_EQ(budget->GetTotalBorrowed(), 4000);   // IssueBond(1500) rounds down to 1000
        CHECK(proc->Loans().empty());                 // RemoveLoanWindow cleared the list
        CHECK(host.mgr->dialogs.size() > dialogsBefore);
    }
    {
        // Five outstanding loans: the down button appears and scrolls by two rows.
        budget->GetTunables().maxOutstandingBonds = 10;
        CHECK(budget->IssueBond(1000));
        CHECK(budget->IssueBond(1000));
        proc->DisplayLoanWindow();
        CHECK_EQ(proc->LoanCount(), 5);
        CHECK(rig.Window(0xaa4c339c)->IsVisible());
        CHECK(!rig.Window(0x2a4c33a6)->IsVisible());
        cGZMessage down = Command(0xaa4c339c);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, down));
        CHECK_EQ(proc->LoanScroll(), 2);
        CHECK(rig.Window(0x2a4c33a6)->IsVisible());
        CHECK(!rig.Window(0xaa4c339c)->IsVisible());
        cGZMessage downAgain = Command(0xaa4c339c);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, downAgain));
        CHECK_EQ(proc->LoanScroll(), 2);   // count - 4 <= offset: no further scroll
        cGZMessage up = Command(0x2a4c33a6);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, up));
        CHECK_EQ(proc->LoanScroll(), 0);
        cGZMessage upAgain = Command(0x2a4c33a6);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, upAgain));
        CHECK_EQ(proc->LoanScroll(), 0);
        cGZMessage close = Command(0xa4c3354);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, close));
        CHECK(!rig.loanWindow->IsVisible());
    }

    // ---- department sub-budget window ------------------------------------------------------------------
    {
        cGZMessage message = Command(0xaa3ac403);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(subLog.created, 1);
        CHECK_EQ(subLog.purpose, 0xa5a72d1u);
        CHECK_EQ(subLog.period, 0u);
        CHECK_EQ(subLog.destroyed, 1);
        CHECK(proc->SubBudgetWindow() == nullptr);
        CHECK_EQ(host.mgr->popups, 4);
        CHECK(subLog.lastWindow != nullptr);
        // The 0xaa3ac5xx family reaches the same handler.
        message = Command(0xaa3ac509);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(subLog.created, 2);
        CHECK_EQ(subLog.purpose, 1u);
        // While no department window object is held, the same command builds another one.
        message = Command(0xaa3ac406);
        CHECK(!winProc->DoWinProcMessage(rig.budgetWindow, message));
        CHECK_EQ(subLog.created, 3);
        CHECK_EQ(subLog.purpose, 0x4a357b40u);
    }

    // ---- message dirty flag / Update ------------------------------------------------------------------
    {
        FakeMessage2 funds(0x8dac3e7);
        CHECK(proc->DoMessage(&funds));
        CHECK(proc->PendingUpdate());
        FakeMessage2 other(0x12345678);
        CHECK(proc->DoMessage(&other));
        CHECK(proc->PendingUpdate());
        FakeMessage2 tick(0x86ad10ee);
        CHECK(proc->DoMessage(&tick));
        CHECK(!proc->PendingUpdate());
        // A tick without the dirty flag does not refresh; Update sets it again while visible.
        CHECK(proc->DoMessage(&tick));
        proc->ShowWindow();
        proc->Update();
        CHECK(proc->PendingUpdate());
        CHECK(proc->DoMessage(&tick));
        CHECK(!proc->PendingUpdate());
    }

    // ---- Shutdown / EndDialog -------------------------------------------------------------------------
    {
        const int dialogues = static_cast<int>(host.mgr->dialogs.size());
        const std::size_t destroyedBefore = host.mgr->destroyed.size();
        const int removes = messages.RemoveCount(0x8dac3e8) + messages.RemoveCount(0x8dac3e7) +
                            messages.RemoveCount(0x86ad10ee);
        proc->Shutdown();   // releases the last reference: the object is gone now
        (void)dialogues;
        CHECK_EQ(messages.RemoveCount(0x8dac3e8), 1);
        CHECK_EQ(messages.RemoveCount(0x8dac3e7), 1);
        CHECK_EQ(messages.RemoveCount(0x86ad10ee), 1);
        CHECK_EQ(removes, 0);
        CHECK_EQ(host.removeTicks, 1);
        // The five windows of the panel were destroyed: the two pages, the taxes dialog, the loan dialog and itself.
        CHECK(host.mgr->destroyed.size() >= destroyedBefore + 5);
        CHECK(std::find(host.mgr->destroyed.begin(), host.mgr->destroyed.end(), rig.budgetWindow) !=
              host.mgr->destroyed.end());
        CHECK(std::find(host.mgr->destroyed.begin(), host.mgr->destroyed.end(), rig.yearlyPage) !=
              host.mgr->destroyed.end());
        CHECK(std::find(host.mgr->destroyed.begin(), host.mgr->destroyed.end(), rig.taxesWindow) !=
              host.mgr->destroyed.end());
        CHECK(std::find(host.mgr->destroyed.begin(), host.mgr->destroyed.end(), rig.loanWindow) !=
              host.mgr->destroyed.end());
    }

    rig.view->Release();   // the view window owns every script window and control added under it
    budget->Release();
    sc4::cSC4BudgetSimulator::SetEnvironment(nullptr);
    sc4::SetLanguageFormatter(nullptr);
    sc4::SetBudgetUIWinProcHost(nullptr);
    if (failures == 0) std::printf("cSC4BudgetUIWinProc: all checks passed\n");
    else std::printf("cSC4BudgetUIWinProc: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
