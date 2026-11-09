#include "sc4/SubBudgetWindow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gz/WinBtn.h"
#include "gz/WinFont.h"
#include "gz/WinGen.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "rz/cRZString.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SLString.h"

namespace sc4 {

namespace {
ISubBudgetWindowHost* g_host = nullptr;
bool Trace() {
    static const bool on = std::getenv("SC4_SUBBUDGETDEBUG") != nullptr;
    return on;
}

constexpr std::uint32_t kStringGroup = 0x6a3ff01c;
constexpr std::uint32_t kResultClose = 0x42b7c353;            // end_dialog's code for OK, Cancel and Esc
constexpr std::uint32_t kDialogId = 0x423278f, kBodyId = 0x423278e;
constexpr std::uint32_t kSliderMessage = 0x887113a4;          // cIGZWinSlider value changed (p2 = the slider id)
constexpr std::uint32_t kButtonMessage = 0x287259f6;
constexpr std::uint32_t kButtonOk = 0x67, kButtonCancel = 0x6d;
// Fonts and colours of the rows.
constexpr std::uint32_t kFontTitle = 0x4a809916, kFontHeader = 0xea85d304, kFontItem = 0xea85d305, kFontDepartment = 0xea85d306;
// Segments (UI art, group 0x46a006b0) of the department dialog, top to bottom.
constexpr std::uint32_t kSegTop = 0x140155d0, kSegIncomeHead = 0x140155d1, kSegBody = 0x140155d2, kSegIncomeFoot = 0x140155d3,
                        kSegExpenseHead = 0x140155d4, kSegExpenseBody = 0x140155d5, kSegExpenseFoot = 0x140155d6, kSegBottom = 0x140155d7;
constexpr std::uint32_t kImageDepartmentButton = 0x140155b7, kImageFundingBar = 0x140155c8, kImageOkCancel = 0x144161eb;
constexpr int kMaxRows = 9;
// cSC4Tank::sfBarrelMaxVelocity / sfBarrelVelocityScalingFactor (the constants the slider borrows): percent <-> fraction.
constexpr float kPercent = 100.0f, kFraction = 0.01f;

// The three purposes whose departments are listed only when they have money in them.
bool QuietPurpose(std::uint32_t purpose) { return purpose == 0x6a357b96 || purpose == 0xea597195 || purpose == 0x0a5a72d1; }

// The expense rows' button: enabled when the department has locally funded items of the purpose its id names.
std::uint32_t LocalPurposeOf(std::uint32_t departmentId) {
    switch (departmentId) {
        case 0x4a538cc6: case 0x09188f4c: return 0xea5654b6;
        case 0x28f55a9f: return 0xea567bc3;
        case 0x09188f42: case 0xaa538cb3: return 0xca565486;
        case 0x8910bc8a: return 0xca58e540;
        case 0xa2963983: case 0xa2963984: return 0x0a567baa;
        default: return 0;
    }
}

cRZString Text(std::uint32_t instance) { return cSLString(kStringGroup, instance, true); }

void ReleaseItems(std::vector<cSC4LineItem*>& items) {
    for (cSC4LineItem* item : items) item->Release();
    items.clear();
}
}  // namespace

ISubBudgetWindowHost* SubBudgetWindowHost() { return g_host; }
void SetSubBudgetWindowHost(ISubBudgetWindowHost* host) { g_host = host; }

// i386 0x00269b7c: the total row's caption is set once.
cSC4SubBudgetWindow::cSC4SubBudgetWindow() : totalCaption_(Text(0x8a5796d1)) {}

cSC4SubBudgetWindow::~cSC4SubBudgetWindow() {
    if (body_) body_->Release();
    if (dialog_) dialog_->Release();
}

bool cSC4SubBudgetWindow::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == gz::kGZIID_cIGZWinProc || iid == 1) {
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4SubBudgetWindow::Release() {
    if (refs_ > 1) return --refs_;
    delete this;
    return 0;
}

// CreateSubBudgetWindow (i386 0x00265358): a dialog window gen with this as its proc.
bool cSC4SubBudgetWindow::Create(cIGZWin** window, std::uint32_t purpose, std::uint32_t period, cIGZWin* parent) {
    if (window) *window = nullptr;
    gz::cIGZWinGen* gen = g_host ? g_host->CreateWinGen() : nullptr;
    if (!gen) return false;
    cIGZWin* win = gen->AsIGZWin();
    win->AddRef();
    if (window) *window = win;
    if (dialog_) dialog_->Release();
    dialog_ = win;
    win->AddRef();
    gen->SetWinGenFlag(1, true);
    gen->SetWinGenFlag(2, false);
    gen->SetWinGenFlag(0x10000, false);
    gen->SetWinGenFlag(0x20, false);
    gen->SetWinGenFlag(0x40, false);
    gen->SetWinGenFlag(0x1000, true);
    gen->SetWinGenFlag(0x2000, false);
    gen->SetWinGenFlag(0x4000, false);
    gen->SetWinProc(this);
    win->SetFlag(0x100, true);
    win->SetFlag(0x10000, false);
    win->SetFlag(4, true);
    win->SetFlag(0x800, false);
    win->SetID(kDialogId);
    purpose_ = purpose;
    individual_ = parent;
    period_ = period;
    if (Trace()) std::fprintf(stderr, "sub budget: created for purpose 0x%08x period %u\n", purpose, period);
    gen->Release();
    if (!window) win->Release();
    return true;
}

// DestroySubBudgetWindow (i386 0x0026961a): the neighbor-deal lists are dropped (PARTIAL: none are made yet), then end_dialog.
void cSC4SubBudgetWindow::Destroy() {
    // PARTIAL: the simulator pause around the dialog (spSimulator 0x28 in on_init, 0x34 here) is not applied.
    if (Trace())
        for (const BudgetRestoreInfo& info : restore_)
            if (info.department) std::fprintf(stderr, "sub budget: destroy: department %08x funding %.2f (opened at %.2f)\n", info.department->GetDepartmentID(),
                                              info.department->GetFundingPercentage(), info.funding);
    if (dialog_) EndDialog(dialog_, kResultClose);   // the window's destroyed message runs on_shutdown, which drops the references
}

// DoWinMsg (i386 0x00279028).
bool cSC4SubBudgetWindow::DoWinMsg(cIGZWin* window, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t) {
    if (type == 5) {   // key: Esc, or Return / s with modifier 4
        const std::uint64_t key = p1;
        if (key == 0x1b || ((key == 0x73 || key == 0xd) && (p2 & 4) != 0)) {
            EndDialog(window, kResultClose);
            return true;
        }
        return false;
    }
    if (type == 3) {
        if (p1 == kSliderMessage) {
            static bool reentered = false;   // the original's function-static bReentrancyFlag
            void* raw = nullptr;
            if (!window->GetChildAsRecursive(static_cast<std::uint32_t>(p2), gz::kGZIID_cIGZWinSlider, &raw) || !raw) return false;
            auto* slider = static_cast<gz::cIGZWinSlider*>(raw);
            if (!reentered) {
                reentered = true;
                if (!individual_) {
                    for (const SliderInfo& info : sliders_) {
                        if (info.id == p2 && info.department) {
                            info.department->SetFundingPercentage(static_cast<float>(slider->GetValue()) * kFraction, 0);
                            if (Trace()) std::fprintf(stderr, "sub budget: slider %08x -> department %08x funding %.2f\n", info.id,
                                                      info.department->GetDepartmentID(), info.department->GetFundingPercentage());
                        }
                    }
                    UpdateValues(window);
                }
                reentered = false;
            }
            slider->Release();
            return false;
        }
        if (p1 == kButtonMessage) return OnButtonActivated(window, static_cast<std::uint32_t>(p2));
        return false;
    }
    if (type == gz::kWinProcCreated) return OnInit(window);
    if (type == gz::kWinProcDestroyed) return OnShutdown();
    return false;
}

// end_dialog (i386 0x00261c86): a modal dialog ends through the window manager (EndModal 0xf with the code).
void cSC4SubBudgetWindow::EndDialog(cIGZWin* window, std::uint32_t result) {
    if (!window) return;
    result_ = result;
    cIGZWinMgr* manager = window->GetWindowManager();
    if (!manager) return;
    if (window->GetFlag(0x1000)) manager->EndDialog(0xf, window, result);
    else manager->DestroyWindow(window);
}

// on_shutdown (i386 0x0026a20e).
bool cSC4SubBudgetWindow::OnShutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    sliders_.clear();
    itemRows_.clear();
    for (BudgetRestoreInfo& info : restore_)
        if (info.department) info.department->Release();
    restore_.clear();
    AddRef();   // the original holds itself across the releases below
    if (dialog_) { dialog_->Release(); dialog_ = nullptr; }
    if (body_) { body_->Release(); body_ = nullptr; }
    Release();
    return true;
}

// on_button_activated (i386 0x0027825c): OK keeps the funding, Cancel restores it; both close the dialog.
//   PARTIAL: the department buttons' individual-building windows, the ordinance / neighbor deal buttons and the scroll
//   arrows.
bool cSC4SubBudgetWindow::OnButtonActivated(cIGZWin* window, std::uint32_t id) {
    if (Trace()) std::fprintf(stderr, "sub budget: button %08x\n", id);
    if (id == kButtonCancel) {
        RestoreStartSettings();
        EndDialog(window, kResultClose);
        return true;
    }
    if (id == kButtonOk) {
        EndDialog(window, kResultClose);
        return true;
    }
    return false;
}

// RestoreStartSettings (i386 0x002638fc): every department back to the funding the dialog opened with.
void cSC4SubBudgetWindow::RestoreStartSettings() {
    if (Trace()) std::fprintf(stderr, "sub budget: restore start settings\n");
    if (individual_) return;   // PARTIAL: the per-building restore
    for (BudgetRestoreInfo& info : restore_)
        if (info.department) info.department->SetFundingPercentage(info.funding, 0);
}

// GetBudgetSegment (i386 0x0026279a): one art strip as a bitmap child at (x, y); answers its height.
int cSC4SubBudgetWindow::GetBudgetSegment(std::uint32_t image, int x, int y, cIGZWin* parent) {
    cIGZBuffer* buffer = g_host ? g_host->UiImage(image) : nullptr;
    if (!buffer || !parent) return 0;
    cIGZWin* bitmap = g_host->CreateBitmap(buffer, x, y);
    if (!bitmap) return 0;
    const int height = bitmap->GetH();
    bitmap->SetFlag(0x800, false);
    parent->ChildAdd(bitmap);
    bitmap->Release();
    return height;
}

// SetupWindowBig (i386 0x002658fe): the strips for `incomeRows` and `expenseRows` rows, the dialog sized to them and
// centred in its parent.
void cSC4SubBudgetWindow::SetupWindowBig(cIGZWin* window, std::uint32_t incomeRows, std::uint32_t expenseRows) {
    height_ += GetBudgetSegment(kSegTop, 0, height_, body_);
    if (incomeRows != 0) {
        height_ += GetBudgetSegment(kSegIncomeHead, 0, height_, body_);
        incomeTop_ = height_;
        for (std::uint32_t i = 0; i < incomeRows >> 1; ++i) height_ += GetBudgetSegment(kSegBody, 0, height_, body_);
        height_ += GetBudgetSegment(kSegIncomeFoot, 0, height_, body_);
    }
    if (expenseRows != 0) {
        height_ += GetBudgetSegment(kSegExpenseHead, 0, height_, body_);
        expenseTop_ = height_;
        for (std::uint32_t i = 0; i < expenseRows >> 1; ++i) height_ += GetBudgetSegment(kSegExpenseBody, 0, height_, body_);
        height_ += GetBudgetSegment(kSegExpenseFoot, 0, height_, body_);
    }
    height_ += GetBudgetSegment(kSegBottom, 0, height_, body_);
    window->SetSize(width_, height_);
    if (cIGZWin* parent = window->GetParentWin())
        window->GZWinMoveTo((parent->GetW() - window->GetW()) / 2, (parent->GetH() - window->GetH()) / 2);
    if (body_) body_->SetSize(width_, height_);
}

// SetEmptyWindow (i386 0x00265ef4): a group without rows says so. PARTIAL: the empty dialog keeps the default size.
void cSC4SubBudgetWindow::SetEmptyWindow(cIGZWin*, const cIGZString& title, const cIGZString& text) {
    AddText(body_, 0xabcde00, 0x14, 8, title, 0, kFontTitle, 'D', 'U', 'f');
    AddText(body_, 0xabcde01, 0x14, 0x30, text, 0, kFontHeader, 'D', 'U', 'f');
}

// add_text (i386 0x002616ca): a label of `text` in a font style and colour; align 0 = left edge at x, 6 = right edge at x
// (99, the full-width variant, has no caller in the department path).
void cSC4SubBudgetWindow::AddText(cIGZWin* parent, std::uint32_t id, int x, int y, const cIGZString& text, int align,
                                  std::uint32_t font, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (!parent || !g_host) return;
    gz::cIGZWinText* label = g_host->CreateLabel(id, text);
    if (!label) return;
    cIGZWin* win = label->AsIGZWin();
    win->SetID(id);
    if (gz::IWinFont* face = g_host->Font(font)) label->SetFont(face);
    label->SetTextColor(g_host->MakeColor(r, g, b));
    label->FitWindowToText(false, false);
    if (Trace()) {
        cRZString caption(text);
        std::fprintf(stderr, "sub budget: text %08x \"%s\" %dx%d at %d,%d font %08x %s\n", id, caption.ToChar(), win->GetW(), win->GetH(), x, y, font,
                     g_host->Font(font) ? "found" : "MISSING");
    }
    // the original then widens the label to the area (fitted width, top, 1000, bottom): a box 1000 - width wide that later,
    // longer captions fit in; right-aligned labels end at x
    const cRZRect fitted = win->GetArea();
    win->SetArea(fitted.right, fitted.top, 1000, fitted.bottom);
    if (align == 6) win->GZWinMoveTo(x - win->GetW(), y);
    else win->GZWinMoveTo(x, y);
    label->SetAlignment(static_cast<std::uint32_t>(align));
    win->SetFlag(0x800, false);
    win->SetFlag(0x8000, false);
    parent->ChildAdd(win);
    label->Release();
}

// add_slider (i386 0x002613ec).
void cSC4SubBudgetWindow::AddSlider(cIGZWin* parent, std::uint32_t id, int x, int y, int width, int direction, int minimum,
                                    int maximum, int value) {
    if (!parent || !g_host) return;
    gz::cIGZWinSlider* slider = g_host->CreateSlider(id, direction, minimum, maximum, value);
    if (!slider) return;
    cIGZWin* win = slider->AsIGZWin();
    win->SetFlag(0x10000, false);
    win->SetFlag(4, true);
    win->SetArea(x, y, x + width, y + 0xe);
    win->SetFlag(0x800, false);
    parent->ChildAdd(win);
    slider->Release();
}

// add_image_button2 (i386 0x002620f8). PARTIAL: the framed variant's highlight frame (GZCOM 0xc2afa76e) and the button's
// text colours and sound.
void cSC4SubBudgetWindow::AddImageButton(cIGZWin* parent, std::uint32_t id, int x, int y, std::uint32_t image, bool framed) {
    if (!parent || !g_host) return;
    cIGZBuffer* buffer = g_host->UiImage(image);
    gz::cIGZWinBtn* button = g_host->CreateButton(2, id, cRZString(), buffer);
    if (!button) return;
    cIGZWin* win = button->AsIGZWin();
    if (!buffer) win->SetSize(0x1a, 0x10);
    if (framed) {
        win->SetFlag(0x10000, false);
        win->SetFlag(0x20000, true);
        win->SetFlag(0x40000, true);
        win->SetFlag(0x800, false);
    }
    win->SetFlag(4, true);
    win->GZWinMoveTo(x, y);
    parent->ChildAdd(win);
    button->Release();
}

// add_Bitmap (i386 0x002625d2).
void cSC4SubBudgetWindow::AddBitmap(std::uint32_t image, int x, int y, cIGZWin* parent, std::uint32_t id) {
    cIGZBuffer* buffer = g_host ? g_host->UiImage(image) : nullptr;
    if (!buffer || !parent) return;
    cIGZWin* bitmap = g_host->CreateBitmap(buffer, x, y);
    if (!bitmap) return;
    bitmap->SetID(id);
    bitmap->SetFlag(4, true);
    parent->ChildAdd(bitmap);
    bitmap->Release();
}

void cSC4SubBudgetWindow::ReleaseDepartments(std::vector<cSC4DepartmentBudget*>& departments) {
    for (cSC4DepartmentBudget* department : departments) department->Release();
    departments.clear();
}

// The budget group of the dialog's purpose (GetAllGroups, else the first group) and its departments (counted).
void cSC4SubBudgetWindow::Departments(std::vector<cSC4DepartmentBudget*>& out) {
    out.clear();
    cSC4BudgetSimulator* budget = g_host ? g_host->BudgetSimulator() : nullptr;
    if (!budget) return;
    std::vector<cSC4BudgetSimulator::BudgetGroupInfo> groups;
    budget->GetAllGroups(groups);
    if (groups.empty()) return;
    std::size_t index = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (groups[i].id == purpose_) { index = i; break; }
    budget->GetDepartmentBudgetsInGroup(groups[index].id, out);
}

// on_init (i386 0x0026e80e), the department path: count the rows, build the strips, then one row per department (its
// total) or per listed line item, on the income side and the expense side; OK and Cancel at the bottom.
bool cSC4SubBudgetWindow::OnInit(cIGZWin* window) {
    if (!window) return false;
    if (initialized_) return true;
    if (Trace()) std::fprintf(stderr, "sub budget: on_init purpose 0x%08x\n", purpose_);
    if (period_ == 0) {
        incomeTitle_ = Text(0xaa5690b0); incomeHeader_ = Text(0xca569b75);
        expenseTitle_ = Text(0xca569897); expenseHeader_ = Text(0xca569b75);
        multiplier_ = 1;
    } else {
        incomeTitle_ = Text(0xaa569dcd); incomeHeader_ = Text(0x0a569c81);
        expenseTitle_ = Text(0xea569c2f); expenseHeader_ = Text(0x0a569c81);
        multiplier_ = 12;
    }
    // the inner window gen that carries the rows
    if (gz::cIGZWinGen* gen = g_host ? g_host->CreateWinGen() : nullptr) {
        body_ = gen->AsIGZWin();
        body_->AddRef();
        gen->SetWinGenFlag(1, false);
        gen->SetWinGenFlag(2, false);
        gen->SetWinGenFlag(0x10000, false);
        gen->SetWinGenFlag(0x20, false);
        gen->SetWinGenFlag(0x40, false);
        gen->SetWinGenFlag(0x1000, true);
        gen->SetWinGenFlag(0x2000, true);
        gen->SetWinGenFlag(0x4000, true);
        body_->SetFlag(0x100, true);
        body_->SetFlag(0x10000, true);
        body_->SetFlag(0x200000, false);
        body_->SetFlag(0x800, false);
        body_->SetFlag(0x8000, false);
        body_->SetFlag(4, true);
        body_->SetID(kBodyId);
        window->ChildAdd(body_);
        gen->Release();
    }
    if (!body_) return false;
    if (purpose_ == 1 || purpose_ == 2 || individual_) {
        // PARTIAL: the ordinance, neighbor deal and individual building dialogs.
        cIGZBuffer* strip = g_host->UiImage(kSegBody);
        width_ = strip ? static_cast<int>(strip->Width()) : 0x1c8;
        height_ = 0x60;
        SetEmptyWindow(window, cRZString(), cRZString());
    } else {
        std::vector<cSC4DepartmentBudget*> departments;
        Departments(departments);
        // the group's name and the funding the dialog opened with
        cRZString groupName;
        if (cSC4BudgetSimulator* budget = g_host->BudgetSimulator()) {
            std::vector<cSC4BudgetSimulator::BudgetGroupInfo> groups;
            budget->GetAllGroups(groups);
            for (const auto& group : groups)
                if (group.id == purpose_) groupName = cRZString(group.name.c_str());
        }
        for (BudgetRestoreInfo& info : restore_)
            if (info.department) info.department->Release();
        restore_.clear();
        incomeRows_ = expenseRows_ = 0;
        std::vector<cSC4LineItem*> items;
        for (cSC4DepartmentBudget* department : departments) {
            department->AddRef();
            restore_.push_back({department->GetFundingPercentage(), department});
            if (Trace()) {
                cRZString name;
                department->GetDepartmentName(name);
                std::fprintf(stderr, "sub budget: department %08x \"%s\" funding %.2f of %.2f fixed %d\n", department->GetDepartmentID(), name.ToChar(),
                             department->GetFundingPercentage(), department->GetMaxAllowedFundingPercentage(), department->GetIsFixedFunding() ? 1 : 0);
            }
            department->GetAllLineItems(items);
            bool incomeCounted = false, expenseCounted = false;
            for (cSC4LineItem* item : items) {
                const bool income = item->GetType() == 1;
                std::uint32_t& rows = income ? incomeRows_ : expenseRows_;
                bool& counted = income ? incomeCounted : expenseCounted;
                if (!counted) {
                    const std::int64_t money = income ? department->GetTotalIncome() : department->GetIdealMonthlyFunding();
                    if (!QuietPurpose(purpose_) || money != 0) ++rows;
                    counted = true;
                }
                if (item->GetDisplayFlags() & 1) {
                    if (item->GetID() < 0x65 || (income ? item->GetIncome() : item->GetFullExpenses()) != 0) ++rows;
                }
            }
            ReleaseItems(items);
        }
        cIGZBuffer* strip = g_host->UiImage(kSegBody);
        width_ = strip ? static_cast<int>(strip->Width()) : 0;
        rowStep_ = strip ? static_cast<int>(strip->Height()) / 2 : 0;
        if (incomeRows_ == 0 && expenseRows_ == 0) {
            height_ = 0x60;
            SetEmptyWindow(window, groupName, Text(0x4a5cb4b6));
            ReleaseDepartments(departments);
            initialized_ = true;
        } else {
            const std::uint32_t shownIncome = std::min<std::uint32_t>(incomeRows_, kMaxRows);
            const std::uint32_t shownExpense = std::min<std::uint32_t>(expenseRows_, kMaxRows);
            SetupWindowBig(window, shownIncome, shownExpense);
            if (Trace()) std::fprintf(stderr, "sub budget: %zu departments, %u income / %u expense rows, %dx%d\n", departments.size(), incomeRows_, expenseRows_, width_, height_);
            const int incomeTop = incomeTop_, expenseTop = expenseTop_;
            sliders_.clear();
            AddText(body_, 0xabcde00, 0x14, 8, groupName, 0, kFontTitle, 'D', 'U', 'f');
            std::uint32_t textId = 0xabcde01;
            // ---- income
            if (incomeRows_ != 0) {
                AddText(body_, 0xabcde01, 0x12, incomeTop_ - rowStep_, incomeTitle_, 0, kFontHeader, 'D', 'U', 'f');
                AddText(body_, 0xabcde02, width_ - 0x26, incomeTop_ - rowStep_, incomeHeader_, 6, kFontHeader, 'D', 'U', 'f');
                total_ = 0;
                textId = 0xabcde03;
                int shown = 0;
                for (cSC4DepartmentBudget* department : departments) {
                    department->GetAllLineItems(items);
                    bool rowDone = false;
                    for (cSC4LineItem* item : items) {
                        if (item->GetType() != 1) continue;
                        if (!rowDone) {
                            if (shown > 8) {
                                total_ += department->GetTotalIncome() * multiplier_;
                                break;
                            }
                            if (!QuietPurpose(purpose_) || department->GetTotalIncome() != 0) {
                                cRZString name, money;
                                department->GetDepartmentName(name);
                                AddText(body_, textId, 0x30, incomeTop_, name, 0, kFontDepartment, 'D', 'U', 'f');
                                SC4MakeMoneyString(department->GetTotalIncome() * multiplier_, money);
                                total_ += department->GetTotalIncome() * multiplier_;
                                AddText(body_, textId + 1, width_ - 0x26, incomeTop_, money, 6, kFontDepartment, 0, 'f', 0);
                                AddText(body_, textId + 2, 0x102, expenseTop_, cRZString(), 0, kFontItem, 'D', 'U', 'f');
                                if (cIGZWin* blank = body_->GetChildWindowFromIDRecursive(textId + 2)) blank->SetFlag(1, false);
                                textId += 3;
                                incomeTop_ += rowStep_;
                                ++shown;
                                rowDone = true;
                                if (shown > 8) break;
                            } else {
                                rowDone = false;
                            }
                        }
                        if (item->GetID() > 100 && item->GetIncome() == 0) break;
                        if (item->GetDisplayFlags() & 1) {
                            cRZString name, money, count;
                            item->GetName(name);
                            AddText(body_, textId, 0x30, incomeTop_, name, 0, kFontItem, 'D', 'U', 'f');
                            SC4MakeMoneyString(item->GetIncome() * multiplier_, money);
                            AddText(body_, textId + 1, width_ - 0x26, incomeTop_, money, 6, kFontItem, 0, 'f', 0);
                            char text[32];
                            std::snprintf(text, sizeof(text), "%d", static_cast<int>(item->GetSecondaryInfoField()));
                            count = cRZString(text);
                            AddText(body_, textId + 2, 0x102, expenseTop_, count, 0, kFontItem, 'D', 'U', 'f');
                            if (cIGZWin* number = body_->GetChildWindowFromIDRecursive(textId + 2))
                                number->SetFlag(1, (item->GetDisplayFlags() & 2) != 0);
                            textId += 3;
                            incomeTop_ += rowStep_;
                            ++shown;
                        }
                    }
                    ReleaseItems(items);
                }
                if ((shownIncome & 1) == 0) incomeTop_ += rowStep_;
                AddText(body_, textId, 0xfa, incomeTop_, totalCaption_, 0, kFontHeader, 'D', 'U', 'f');
                cRZString money;
                SC4MakeMoneyString(total_, money);
                AddText(body_, textId + 1, width_ - 0x26, incomeTop_, money, 6, kFontHeader, 0, 'f', 0);
                textId += 2;
            }
            // ---- expenses
            if (expenseRows_ != 0) {
                AddText(body_, textId, 0x12, expenseTop_ - rowStep_, expenseTitle_, 0, kFontHeader, 'D', 'U', 'f');
                AddText(body_, textId + 1, width_ - 0x26, expenseTop_ - rowStep_, expenseHeader_, 6, kFontHeader, 'D', 'U', 'f');
                textId += 2;
                total_ = 0;
                std::uint32_t bitmapId = 0xabce200, sliderId = 0xabcdf00, buttonId = 0xabce600;
                int shown = 0;
                for (cSC4DepartmentBudget* department : departments) {
                    department->GetAllLineItems(items);
                    bool rowDone = false;
                    for (cSC4LineItem* item : items) {
                        if (item->GetType() != 0) continue;
                        if (!rowDone) {
                            if (shown > 8) {
                                total_ += department->GetTotalExpenses() * multiplier_;
                                break;
                            }
                            rowDone = true;
                            if (!QuietPurpose(purpose_) || department->GetIdealMonthlyFunding() != 0) {
                                cRZString name, money;
                                department->GetDepartmentName(name);
                                AddImageButton(window, buttonId, 0x12, expenseTop_, kImageDepartmentButton, false);
                                AddText(body_, textId, 0x30, expenseTop_, name, 0, kFontDepartment, 'D', 'U', 'f');
                                SC4MakeMoneyString(department->GetTotalExpenses() * multiplier_, money);
                                total_ += department->GetTotalExpenses() * multiplier_;
                                AddText(body_, textId + 1, width_ - 0x26, expenseTop_, money, 6, kFontDepartment, 'w', 0, 0);
                                AddText(body_, textId + 2, 0x102, expenseTop_, cRZString(), 0, kFontItem, 'D', 'U', 'f');
                                if (cIGZWin* blank = body_->GetChildWindowFromIDRecursive(textId + 2)) blank->SetFlag(1, false);
                                textId += 3;
                                const float maximum = department->GetMaxAllowedFundingPercentage();
                                float barX = 11000.0f / (maximum * kPercent) + 260.0f - 12.0f;
                                if (barX <= 0.0f) barX = 0.0f;
                                AddBitmap(kImageFundingBar, static_cast<int>(static_cast<std::uint32_t>(barX)), expenseTop_, window, bitmapId);
                                AddSlider(window, sliderId, 0x104, expenseTop_, 0x6e, 1, 0, static_cast<int>(kPercent * maximum),
                                          static_cast<int>(department->GetFundingPercentage() * kPercent));
                                sliders_.push_back({sliderId, department, textId - 2, true});
                                const bool fixed = department->GetIsFixedFunding();
                                if (cIGZWin* bar = window->GetChildWindowFromIDRecursive(bitmapId)) bar->SetFlag(1, !fixed);
                                if (cIGZWin* slider = window->GetChildWindowFromIDRecursive(sliderId)) slider->SetFlag(1, !fixed);
                                const std::uint32_t local = LocalPurposeOf(department->GetDepartmentID());
                                const bool hasLocal = local != 0 && department->HasLocallyFundedItemsByPurpose(local);
                                if (cIGZWin* button = window->GetChildWindowFromIDRecursive(buttonId)) button->SetFlag(1, hasLocal);
                                ++bitmapId; ++sliderId; ++buttonId;
                                expenseTop_ += rowStep_;
                                ++shown;
                                if (shown > 8) break;
                            }
                        }
                        if (item->GetID() > 100 && item->GetFullExpenses() == 0) break;
                        if (item->GetDisplayFlags() & 1) {
                            cRZString name, money, count;
                            item->GetName(name);
                            AddImageButton(window, buttonId, 0x12, expenseTop_, kImageDepartmentButton, false);
                            AddText(body_, textId, 0x30, expenseTop_, name, 0, kFontItem, 'D', 'U', 'f');
                            SC4MakeMoneyString(item->GetCurrentExpenses() * multiplier_, money);
                            AddText(body_, textId + 1, width_ - 0x26, expenseTop_, money, 6, kFontItem, 'w', 0, 0);
                            itemRows_.push_back({textId + 1, department, item->GetID()});
                            char text[32];
                            std::snprintf(text, sizeof(text), "%d", static_cast<int>(item->GetSecondaryInfoField()));
                            count = cRZString(text);
                            AddText(body_, textId + 2, 0x102, expenseTop_, count, 0, kFontItem, 'D', 'U', 'f');
                            if (cIGZWin* number = body_->GetChildWindowFromIDRecursive(textId + 2))
                                number->SetFlag(1, (item->GetDisplayFlags() & 2) != 0);
                            textId += 3;
                            // the item rows get a hidden bar, slider and button (as the original)
                            const float maximum = department->GetMaxAllowedFundingPercentage();
                            float barX = 11000.0f / (maximum * kPercent) + 260.0f - 12.0f;
                            if (barX <= 0.0f) barX = 0.0f;
                            AddBitmap(kImageFundingBar, static_cast<int>(static_cast<std::uint32_t>(barX)), expenseTop_, window, bitmapId);
                            AddSlider(window, sliderId, 0x104, expenseTop_, 0x6e, 1, 0, static_cast<int>(kPercent * maximum),
                                      static_cast<int>(department->GetFundingPercentage() * kPercent));
                            sliders_.push_back({sliderId, department, textId - 2, false});
                            for (std::uint32_t hidden : {bitmapId, sliderId, buttonId})
                                if (cIGZWin* child = window->GetChildWindowFromIDRecursive(hidden)) child->SetFlag(1, false);
                            ++bitmapId; ++sliderId; ++buttonId;
                            expenseTop_ += rowStep_;
                            ++shown;
                        }
                    }
                    ReleaseItems(items);
                }
                if ((shownExpense & 1) == 0) expenseTop_ += rowStep_;
                AddText(body_, textId, 0xfa, expenseTop_, totalCaption_, 0, kFontHeader, 'D', 'U', 'f');
                cRZString money;
                SC4MakeMoneyString(total_, money);
                AddText(body_, textId + 1, width_ - 0x26, expenseTop_, money, 6, kFontHeader, 'w', 0, 0);
                expenseTotalText_ = textId + 1;
            }
            (void)incomeTop;
            (void)expenseTop;
            ReleaseDepartments(departments);
        }
    }
    // OK and Cancel
    AddImageButton(window, kButtonOk, 0xe, height_ - 0x28, kImageOkCancel, true);
    if (cIGZWin* ok = window->GetChildWindowFromIDRecursive(kButtonOk)) {
        ok->SetSize(0xb4, 0x1e);
        ok->SetCaption(cSLString(0x0a554ae8, 0x2a6cd8fd, true));
        ok->SetNotificationTarget(window);
    }
    AddImageButton(window, kButtonCancel, width_ - 0xc3, height_ - 0x28, kImageOkCancel, true);
    if (cIGZWin* cancel = window->GetChildWindowFromIDRecursive(kButtonCancel)) {
        cancel->SetSize(0xb4, 0x1e);
        cancel->SetCaption(cSLString(0x6a231eaa, 0x6a56b0bb, true));
        cancel->SetNotificationTarget(window);
    }
    if (width_ > 0 && height_ > 0 && window->GetW() == 0) window->SetSize(width_, height_);
    for (const SliderInfo& info : sliders_)
        if (cIGZWin* slider = window->GetChildWindowFromIDRecursive(info.id)) slider->SetNotificationTarget(window);
    initialized_ = true;
    return true;
}

// UpdateValues (i386 0x00271a46), the department path: the money texts and the totals again after a funding change.
//   The original walks the rows exactly as on_init does and re-captions them; the port keeps the money text id of each
//   slider's row (a department's expenses are the only figures a slider changes) and the total's.
void cSC4SubBudgetWindow::UpdateValues(cIGZWin*) {
    if (updating_ || !body_) return;
    updating_ = true;
    std::vector<cSC4DepartmentBudget*> departments;
    Departments(departments);
    std::int64_t total = 0;
    for (cSC4DepartmentBudget* department : departments) total += department->GetTotalExpenses() * multiplier_;
    for (const SliderInfo& info : sliders_) {
        if (!info.departmentRow || !info.department) continue;
        cRZString money;
        SC4MakeMoneyString(info.department->GetTotalExpenses() * multiplier_, money);
        if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(info.moneyText)) text->SetCaption(money);
    }
    for (const ItemRow& row : itemRows_) {
        cSC4LineItem* item = row.department ? row.department->GetLineItem(row.item) : nullptr;
        if (!item) continue;
        cRZString money;
        SC4MakeMoneyString(item->GetCurrentExpenses() * multiplier_, money);
        if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(row.moneyText)) text->SetCaption(money);
    }
    if (expenseTotalText_ != 0) {
        cRZString money;
        SC4MakeMoneyString(total, money);
        if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(expenseTotalText_)) text->SetCaption(money);
    }
    ReleaseDepartments(departments);
    updating_ = false;
}

}  // namespace sc4
