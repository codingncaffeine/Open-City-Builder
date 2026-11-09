// cSC4SubBudgetWindow (the department budget dialog, Mac i386 0x002613ec..): the dialog is built on the real window classes
// (cGZWinGen / cGZWinText / cGZWinSlider / cGZWinBtn / cGZWinBMP on a fake window manager) over a real cSC4BudgetSimulator.
// Checked: Create's window and proc, the rows on_init lays out for a budget group, a slider moving a department's funding
// and re-captioning its money and the total (UpdateValues), Cancel restoring the opening funding, OK keeping the change.
#include <cstdio>
#include <string>

#include "fake_winmgr2.h"
#include "gz/Buffer.h"
#include "gz/WinBMP.h"
#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SubBudgetWindow.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct TestFormatter final : sc4::ILanguageFormatter {
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString&) override { out.AssignFromCString(std::to_string(value).c_str()); }
    void FormatMoney(double value, cIGZString& out, const cIGZString&, const cRZString&) override {
        out.AssignFromCString(std::to_string(value).c_str());
    }
    void FormatNumber(std::int64_t value, cRZString& out) override { out = cRZString(std::to_string(value).c_str()); }
};

struct TestEnv final : sc4::IBudgetSimEnvironment {
    void SimulatorDate(int& year, int& month) override { year = 2000; month = 0; }
    std::int64_t OrdinanceMonthlyIncome() override { return 0; }
    std::int64_t OrdinanceMonthlyExpense() override { return 0; }
    std::int64_t NeighborsIncome() override { return 0; }
    std::int64_t NeighborsExpense() override { return 0; }
    int DifficultyLevel() override { return 0; }
    void ForEachTaxableLot(const std::function<void(const sc4::LotTaxInfo&)>&) override {}
    void PostMessage(std::uint32_t, std::uint32_t, std::int64_t, std::int64_t) override {}
    void AddHistory(std::uint32_t, std::int64_t) override {}
    void PostBudgetMessage(std::uint32_t, sc4::cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override {}
    void NotifyLocalFundingChanged(sc4::cISCPropertyHolder*) override {}
    std::string LoadLocalizedString(std::uint32_t, std::uint32_t) override { return std::string(); }
    std::string BuildingName(std::uint32_t) override { return std::string(); }
};

struct Host final : sc4::ISubBudgetWindowHost {
    FakeMgr2* mgr = nullptr;
    sc4::cSC4BudgetSimulator* budget = nullptr;
    cGZBuffer strip;   // every art strip: 500 x 20 (rows of 10)
    Host() {
        cGZBufferColorType type;
        type.type = kBufferColorARGB8888;
        type.bitsPerPixel = 0x20;
        strip.Init(500, 20, type);
    }
    sc4::cSC4BudgetSimulator* BudgetSimulator() override { return budget; }
    cIGZWinMgr* WindowManager() override { return mgr; }
    gz::cIGZWinGen* CreateWinGen() override {
        auto* gen = new gz::cGZWinGen(mgr);
        auto* view = static_cast<gz::cIGZWinGen*>(gen);
        view->AddRef();
        return view;
    }
    cIGZBuffer* UiImage(std::uint32_t) override { return &strip; }
    gz::IWinFont* Font(std::uint32_t) override { return nullptr; }
    std::uint32_t MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return 0xff000000u | (r << 16) | (g << 8) | b; }
    gz::cIGZWinText* CreateLabel(std::uint32_t id, const cIGZString& caption) override {
        auto* view = static_cast<gz::cIGZWinText*>(new gz::cGZWinText(mgr));
        view->AddRef();
        view->AsIGZWin()->SetID(id);
        view->AsIGZWin()->SetCaption(caption);
        return view;
    }
    gz::cIGZWinSlider* CreateSlider(std::uint32_t id, int, int minimum, int maximum, int value) override {
        auto* view = static_cast<gz::cIGZWinSlider*>(new gz::cGZWinSlider(mgr));
        view->AddRef();
        view->AsIGZWin()->SetID(id);
        view->SetMinimumValue(minimum);
        view->SetMaximumValue(maximum);
        view->SetValue(value);
        return view;
    }
    gz::cIGZWinBtn* CreateButton(std::uint32_t, std::uint32_t id, const cIGZString& caption, cIGZBuffer*) override {
        auto* view = static_cast<gz::cIGZWinBtn*>(new gz::cGZWinBtn(mgr));
        view->AddRef();
        view->AsIGZWin()->SetID(id);
        view->AsIGZWin()->SetCaption(caption);
        return view;
    }
    cIGZWin* CreateBitmap(cIGZBuffer* image, int x, int y) override {
        auto* bitmap = new gz::cGZWinBMP(mgr);
        cIGZWin* win = bitmap;
        win->AddRef();
        win->SetArea(x, y, x + static_cast<int>(image->Width()), y + static_cast<int>(image->Height()));
        return win;
    }
};

std::string Caption(cIGZWin* root, std::uint32_t id) {
    cIGZWin* win = root ? root->GetChildWindowFromIDRecursive(id) : nullptr;
    return win ? std::string(win->GetCaption().ToChar()) : std::string("<none>");
}

constexpr std::uint32_t kGroup = 0x4a357b40;

}  // namespace

int main() {
    TestFormatter formatter;
    sc4::SetLanguageFormatter(&formatter);
    TestEnv env;
    sc4::cSC4BudgetSimulator::SetEnvironment(&env);
    auto* budget = new sc4::cSC4BudgetSimulator();
    budget->AddRef();
    // Two departments in one group: A (1000 full expenses), B (500), each with one listed expense item.
    auto* a = budget->CreateDepartmentBudget(0x1111, kGroup);
    auto* itemA = a->CreateLineItem(0x2222, false);
    itemA->SetType(0);   // an expense item (type 1 = income)
    itemA->SetFullExpenses(1000);
    itemA->SetDisplayFlag(1, true);
    a->SetFundingPercentage(1.0f, 0, true);
    auto* b = budget->CreateDepartmentBudget(0x3333, kGroup);
    auto* itemB = b->CreateLineItem(0x4444, false);
    itemB->SetType(0);
    itemB->SetFullExpenses(500);
    itemB->SetDisplayFlag(1, true);
    b->SetFundingPercentage(1.0f, 0, true);
    budget->DoBudgetGroupChanged(kGroup);

    FakeMgr2 mgr;
    Host host;
    host.mgr = &mgr;
    host.budget = budget;
    sc4::SetSubBudgetWindowHost(&host);
    auto* view = new cGZWin(&mgr);
    view->AddRef();
    view->SetArea(0, 0, 1600, 1200);

    // ---- Create: a window gen with the dialog as its proc
    auto* dialog = new sc4::cSC4SubBudgetWindow();
    dialog->AddRef();
    cIGZWin* window = nullptr;
    CHECK(dialog->Create(&window, kGroup, 0, nullptr));
    CHECK(window != nullptr);
    view->ChildAdd(window);
    CHECK(window->Init());   // the created message runs on_init

    // ---- on_init: two department rows and two item rows on the expense side, a slider per row
    CHECK(dialog->IncomeRows() == 0);
    CHECK(dialog->ExpenseRows() == 4);
    CHECK(dialog->SliderCount() == 4);
    // ids: the title 0xabcde00, the expense headers 0xabcde01/02, then three texts per row (name, money, count)
    CHECK(Caption(window, 0xabcde04) == "1000");   // department A's expenses
    CHECK(Caption(window, 0xabcde07) == "1000");   // its item
    CHECK(Caption(window, 0xabcde0a) == "500");    // department B
    CHECK(Caption(window, 0xabcde10) == "1500");   // the subtotal
    CHECK(window->GetChildWindowFromIDRecursive(0xabcdf00) != nullptr);   // A's slider

    // ---- a slider: funding = value / 100, the money texts and the total follow
    void* raw = nullptr;
    CHECK(window->GetChildAsRecursive(0xabcdf00, gz::kGZIID_cIGZWinSlider, &raw) && raw);
    if (raw) {
        static_cast<gz::cIGZWinSlider*>(raw)->SetValue(50);
        static_cast<gz::cIGZWinSlider*>(raw)->Release();
    }
    dialog->DoWinMsg(window, 3, 0x887113a4, 0xabcdf00, 0);
    CHECK(a->GetFundingPercentage() > 0.49f && a->GetFundingPercentage() < 0.51f);
    CHECK(Caption(window, 0xabcde04) == "500");
    CHECK(Caption(window, 0xabcde07) == "500");    // the item row follows its department
    CHECK(Caption(window, 0xabcde10) == "1000");

    // ---- Cancel restores the funding the dialog opened with
    dialog->DoWinMsg(window, 3, 0x287259f6, 0x6d, 0);
    CHECK(a->GetFundingPercentage() > 0.99f);
    CHECK(dialog->Result() == 0x42b7c353);

    // ---- OK keeps a change
    if (window->GetChildAsRecursive(0xabcdf00, gz::kGZIID_cIGZWinSlider, &raw) && raw) {
        static_cast<gz::cIGZWinSlider*>(raw)->SetValue(80);
        static_cast<gz::cIGZWinSlider*>(raw)->Release();
    }
    dialog->DoWinMsg(window, 3, 0x887113a4, 0xabcdf00, 0);
    dialog->DoWinMsg(window, 3, 0x287259f6, 0x67, 0);
    CHECK(a->GetFundingPercentage() > 0.79f && a->GetFundingPercentage() < 0.81f);

    window->Release();
    dialog->Destroy();
    dialog->Release();
    view->Release();
    budget->Release();
    sc4::SetSubBudgetWindowHost(nullptr);
    sc4::SetLanguageFormatter(nullptr);
    if (failures == 0) std::printf("sc4_subbudgetwindow: all passed\n");
    return failures == 0 ? 0 : 1;
}
