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
        strip.AddRef();   // the host owns its images (windows AddRef and Release them)
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
    // ---- the single-building dialog: hospitals with the worked case of budget-window-bodies.md section 5.4
    std::vector<sc4::cISCPropertyHolder*> buildings;
    struct Funding { sc4::cISCPropertyHolder* holder; std::uint32_t kind; bool secondary; float fraction; };
    std::vector<Funding> fundings;
    void LocallyFundedBuildings(std::uint32_t, std::uint32_t, std::vector<sc4::cISCPropertyHolder*>& out) override {
        out = buildings;
        for (sc4::cISCPropertyHolder* holder : out) holder->AddRef();
    }
    float DepartmentMaxFunding(std::uint32_t) override { return 1.2f; }
    bool QueryBuildingService(sc4::cISCPropertyHolder*, std::uint32_t kind, BuildingService& out) override {
        out = BuildingService{};
        if (kind != 0xca565486) return false;
        out.residential = true;
        out.served = 3;
        out.capacity = 5;
        out.staffFraction = 0.75f;
        out.staffCost = 100;
        out.vehicleFraction = 0.5f;
        out.vehicleCost = 30;
        return true;
    }
    bool BuildingName(sc4::cISCPropertyHolder* holder, cRZString& out) override {
        for (std::size_t i = 0; i < buildings.size(); ++i)
            if (buildings[i] == holder) out = cRZString(("Clinic " + std::to_string(i)).c_str());
        return true;
    }
    bool HasBudgetItem(sc4::cISCPropertyHolder*, std::uint32_t) override { return true; }
    bool SetBuildingFunding(sc4::cISCPropertyHolder* holder, std::uint32_t kind, bool secondary, float fraction) override {
        fundings.push_back({holder, kind, secondary, fraction});
        return true;
    }
    void MoneyString(std::int64_t amount, cRZString& out) override { out = cRZString(std::to_string(amount).c_str()); }
    std::vector<sc4::cISCPropertyHolder*> looked;
    bool LookAtBuilding(sc4::cISCPropertyHolder* holder) override { looked.push_back(holder); return true; }
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
    CHECK(dialog->Create(&window, kGroup, 0, 0));
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
    window->Shutdown();   // the fake window manager only records EndDialog; the destroyed message drops the dialog's references
    dialog->Release();

    // ---- the department path scrolls: six departments of one listed item each = twelve expense rows, nine slots
    {
        constexpr std::uint32_t kGroup2 = 0x0bcd0001;
        const std::int64_t money[6] = {100, 200, 300, 400, 500, 600};
        for (int i = 0; i < 6; ++i) {
            auto* d = budget->CreateDepartmentBudget(0x5000 + i, kGroup2);
            auto* item = d->CreateLineItem(0x6000 + i, false);
            item->SetType(0);
            item->SetFullExpenses(money[i]);
            item->SetDisplayFlag(1, true);
            d->SetFundingPercentage(1.0f, 0, true);
        }
        budget->DoBudgetGroupChanged(kGroup2);
        auto* list = new sc4::cSC4SubBudgetWindow();
        list->AddRef();
        cIGZWin* win = nullptr;
        CHECK(list->Create(&win, kGroup2, 0, 0));
        view->ChildAdd(win);
        CHECK(win->Init());
        CHECK(list->ExpenseRows() == 12);
        auto shown = [&](std::uint32_t id) {
            cIGZWin* child = win->GetChildWindowFromIDRecursive(id);
            return child && child->GetFlag(1);
        };
        CHECK(!shown(0x453) && shown(0x454));            // the arrows on_init makes: twelve rows, down shown
        CHECK(Caption(win, 0xabcde04) == "100");          // slot 0 = the first department
        CHECK(Caption(win, 0xabcde1c) == "500");          // slot 8 = the fifth department row
        CHECK(Caption(win, 0xabcde1f) == "2100");         // the total counts the rows past the ninth slot
        list->DoWinMsg(win, 3, 0x287259f6, 0x454, 0);    // two rows down
        CHECK(shown(0x453) && shown(0x454));
        CHECK(Caption(win, 0xabcde04) == "200");          // slot 0 = the second department now
        CHECK(Caption(win, 0xabcde07) == "200");          // and its item
        CHECK(Caption(win, 0xabcde1c) == "600");
        CHECK(Caption(win, 0xabcde1f) == "2100");
        CHECK(list->SliderCount() == 9);
        list->DoWinMsg(win, 3, 0x287259f6, 0x454, 0);    // 12 <= 4 + 9: last page, down hides
        CHECK(!shown(0x454));
        CHECK(Caption(win, 0xabcde04) == "300");
        list->DoWinMsg(win, 3, 0x287259f6, 0x453, 0);
        list->DoWinMsg(win, 3, 0x287259f6, 0x453, 0);
        CHECK(!shown(0x453) && shown(0x454));
        CHECK(Caption(win, 0xabcde04) == "100");
        win->Release();
        list->Destroy();
        win->Shutdown();
        list->Release();
    }

    // ---- the single-building dialog (purpose 0, a department id): twelve hospitals, nine rows shown
    for (int i = 0; i < 12; ++i) {
        auto* holder = new sc4::cSCPropertyHolder();
        holder->AddRef();
        host.buildings.push_back(holder);
    }
    {
        auto* single = new sc4::cSC4SubBudgetWindow();
        single->AddRef();
        cIGZWin* win = nullptr;
        CHECK(single->Create(&win, 0, 0, 0xaa538cb3));   // the hospital department
        view->ChildAdd(win);
        CHECK(win->Init());
        // a row: the name, "served/capacity", the cost: trunc(30 * 0.5 + 0.5) + trunc(100 * 0.75 + 0.5) = 15 + 75 = 90
        CHECK(Caption(win, 0xabcde06) == "Clinic 0");
        CHECK(Caption(win, 0xabcde07) == "3/5");
        CHECK(Caption(win, 0xabcde08) == "90");
        CHECK(Caption(win, 0xabcde06 + 3 * 8) == "Clinic 8");   // the ninth row
        // nine rows (odd): the total right after them, every building counted: 12 x 90
        CHECK(Caption(win, 0xabcde06 + 3 * 9 + 1) == "1080");
        // the slider is the saved percentage (75); moving it sets that building's funding as a fraction
        void* raw = nullptr;
        CHECK(win->GetChildAsRecursive(0xabce400, gz::kGZIID_cIGZWinSlider, &raw) && raw);
        if (raw) {
            CHECK(static_cast<gz::cIGZWinSlider*>(raw)->GetValue() == 75);
            static_cast<gz::cIGZWinSlider*>(raw)->SetValue(60);
            static_cast<gz::cIGZWinSlider*>(raw)->Release();
        }
        host.fundings.clear();
        single->DoWinMsg(win, 3, 0x887113a4, 0xabce400, 0);
        CHECK(host.fundings.size() == 1 && host.fundings[0].holder == host.buildings[0] && !host.fundings[0].secondary &&
              host.fundings[0].fraction > 0.59f && host.fundings[0].fraction < 0.61f);
        // the arrows (i386 0x00278454..): two rows a click over the twelve; up shows after the first, down hides at the end
        auto shown = [&](std::uint32_t id) { cIGZWin* w = win->GetChildWindowFromIDRecursive(id); return w && w->GetFlag(1); };
        CHECK(!shown(0x453) && shown(0x454));
        single->DoWinMsg(win, 3, 0x287259f6, 0x454, 0);
        CHECK(Caption(win, 0xabcde06) == "Clinic 2");
        CHECK(Caption(win, 0xabcde06 + 3 * 8) == "Clinic 10");
        CHECK(shown(0x453) && shown(0x454));
        single->DoWinMsg(win, 3, 0x287259f6, 0x454, 0);   // 12 <= 4 + 9: the last page, down hides
        CHECK(Caption(win, 0xabcde06) == "Clinic 4");
        CHECK(Caption(win, 0xabcde06 + 3 * 7) == "Clinic 11");
        CHECK(shown(0x453) && !shown(0x454));
        CHECK(Caption(win, 0xabcde06 + 3 * 9 + 1) == "1080");   // the total still counts every building
        // a name: the camera looks at that row's building (the scrolled list's first row = building 4)
        single->DoWinMsg(win, 3, 0x287259f6, 0xabcde06, 0);
        CHECK(host.looked.size() == 1 && host.looked[0] == host.buildings[4]);
        single->DoWinMsg(win, 3, 0x287259f6, 0x453, 0);
        single->DoWinMsg(win, 3, 0x287259f6, 0x453, 0);
        CHECK(Caption(win, 0xabcde06) == "Clinic 0");
        CHECK(!shown(0x453) && shown(0x454));
        // Cancel: every building (all twelve, shown or not) back to 75% / 50%
        host.fundings.clear();
        single->DoWinMsg(win, 3, 0x287259f6, 0x6d, 0);
        CHECK(host.fundings.size() == 24);
        CHECK(host.fundings.size() == 24 && host.fundings[23].holder == host.buildings[11] && host.fundings[23].secondary &&
              host.fundings[23].fraction > 0.49f && host.fundings[23].fraction < 0.51f);
        win->Release();
        single->Destroy();
        win->Shutdown();
        single->Release();
    }
    // a department without buildings: the empty dialog
    for (sc4::cISCPropertyHolder* holder : host.buildings) holder->Release();
    host.buildings.clear();
    {
        auto* single = new sc4::cSC4SubBudgetWindow();
        single->AddRef();
        cIGZWin* win = nullptr;
        CHECK(single->Create(&win, 0, 0, 0x28f55a9f));   // fire
        view->ChildAdd(win);
        CHECK(win->Init());
        CHECK(win->GetChildWindowFromIDRecursive(0xabcde06) == nullptr);   // no rows
        CHECK(win->GetChildWindowFromIDRecursive(0xabce001) != nullptr);   // the "no buildings" text (i386 SetEmptyWindow ids)
        CHECK(win->GetChildWindowFromIDRecursive(0x423278d) != nullptr);   // in the popup's outer panel
        CHECK(win->GetW() == 300 && win->GetH() == 100);                   // the 300 x 100 dialog
        win->Release();
        single->Destroy();
        win->Shutdown();
        single->Release();
    }
    view->Release();
    budget->Release();
    sc4::SetSubBudgetWindowHost(nullptr);
    sc4::SetLanguageFormatter(nullptr);
    if (failures == 0) std::printf("sc4_subbudgetwindow: all passed\n");
    return failures == 0 ? 0 : 1;
}
