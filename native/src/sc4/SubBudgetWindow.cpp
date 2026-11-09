#include "sc4/SubBudgetWindow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gz/WinBtn.h"
#include "gz/WinCombo.h"
#include "gz/WinFont.h"
#include "gz/WinGen.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "rz/cRZString.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/NeighborDeal.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/RegionalCity.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/Ordinance.h"
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

// DestroySubBudgetWindow (i386 0x0026961a): the neighbor-deal proposals are dropped (ReleaseDeals), then end_dialog.
void cSC4SubBudgetWindow::Destroy() {
    // PARTIAL: the simulator pause around the dialog (spSimulator 0x28 in on_init, 0x34 here) is not applied.
    if (Trace())
        for (const BudgetRestoreInfo& info : restore_)
            if (info.department) std::fprintf(stderr, "sub budget: destroy: department %08x funding %.2f (opened at %.2f)\n", info.department->GetDepartmentID(),
                                              info.department->GetFundingPercentage(), info.funding);
    ReleaseDeals();
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
        if (p1 == gz::kComboChanged && purpose_ == 2) return OnComboChanged(window, static_cast<std::uint32_t>(p2));
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
    if (id == 0xcd) {   // the ordinance dialog's Accept: every changed ordinance switched (spOrdinanceSim 0x34)
        if (cSC4OrdinanceSimulator* ordinances = g_host ? g_host->OrdinanceSimulator() : nullptr)
            for (std::size_t i = 0; i < ordinanceIds_.size() && i < ordinanceOn_.size(); ++i)
                if (ordinanceOn_[i].second != ordinanceOn_[i].first) {
                    ordinances->SetOrdinanceOn(ordinanceIds_[i], ordinanceOn_[i].second);
                    if (Trace()) std::fprintf(stderr, "sub budget: ordinance %08x -> %s (now %d)\n", ordinanceIds_[i], ordinanceOn_[i].second ? "on" : "off",
                                              ordinances->IsOrdinanceOn(ordinanceIds_[i]) ? 1 : 0);
                }
        EndDialog(window, kResultClose);
        return false;
    }
    if (id >= 300 && id - 300 < 200 && id - 300 < ordinanceRows_.size()) {   // an ordinance's check button
        const std::uint32_t index = ordinanceRows_[id - 300];
        void* raw = nullptr;
        if (window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
            if (index < ordinanceOn_.size()) ordinanceOn_[index].second = static_cast<gz::cIGZWinBtn*>(raw)->IsChecked();
            static_cast<gz::cIGZWinBtn*>(raw)->Release();
        }
        UpdateOrdinanceBudget(window);
        return false;
    }
    if (id == kButtonCancel) {
        RestoreStartSettings();
        EndDialog(window, kResultClose);
        return true;
    }
    if (id == 0xcf && purpose_ == 2) {   // the neighbor deal dialog's Cancel: the deals as they were, then close
        RestoreDeals();
        EndDialog(window, kResultClose);
        return true;
    }
    if (id >= 0x451 && id <= 0x454 && purpose_ == 2) {   // the scroll arrows (two rows a click)
        const bool income = id <= 0x452, down = id == 0x452 || id == 0x454;
        std::uint32_t& scroll = income ? incomeScroll_ : expenseScroll_;
        const std::uint32_t rows = income ? incomeRows_ : expenseRows_;
        const std::uint32_t up = income ? 0x451 : 0x453, downId = up + 1;
        auto show = [&](std::uint32_t arrow, bool on) {
            if (cIGZWin* button = window->GetChildWindowFromIDRecursive(arrow)) button->SetFlag(1, on);
        };
        if (down) {
            if (rows <= scroll + 9 || (scroll += 2, rows <= scroll + 9)) show(downId, false);
            show(up, true);
        } else {
            if (scroll == 0 || (scroll -= 2, scroll == 0)) show(up, false);
            if (scroll + 9 < rows) show(downId, true);
        }
        UpdateNeighborBudget(window);
        return false;
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

// SetupWindowBig (i386 0x002658fe) / SetupWindow (0x002655a4): the strips first+0 .. first+7 (top, income head, a body
// strip per two income rows, income foot, the same for the expenses, bottom), the dialog sized to them and centred.
void cSC4SubBudgetWindow::StackSegments(cIGZWin* window, std::uint32_t first, std::uint32_t incomeRows, std::uint32_t expenseRows) {
    height_ += GetBudgetSegment(first + 0, 0, height_, body_);
    if (incomeRows != 0) {
        height_ += GetBudgetSegment(first + 1, 0, height_, body_);
        incomeTop_ = height_;
        for (std::uint32_t i = 0; i < incomeRows >> 1; ++i) height_ += GetBudgetSegment(first + 2, 0, height_, body_);
        height_ += GetBudgetSegment(first + 3, 0, height_, body_);
    }
    if (expenseRows != 0) {
        height_ += GetBudgetSegment(first + 4, 0, height_, body_);
        expenseTop_ = height_;
        for (std::uint32_t i = 0; i < expenseRows >> 1; ++i) height_ += GetBudgetSegment(first + 5, 0, height_, body_);
        height_ += GetBudgetSegment(first + 6, 0, height_, body_);
    }
    height_ += GetBudgetSegment(first + 7, 0, height_, body_);
    window->SetSize(width_, height_);
    if (cIGZWin* parent = window->GetParentWin())
        window->GZWinMoveTo((parent->GetW() - window->GetW()) / 2, (parent->GetH() - window->GetH()) / 2);
    if (body_) body_->SetSize(width_, height_);
}
void cSC4SubBudgetWindow::SetupWindowBig(cIGZWin* window, std::uint32_t incomeRows, std::uint32_t expenseRows) {
    StackSegments(window, kSegTop, incomeRows, expenseRows);
}
void cSC4SubBudgetWindow::SetupWindow(cIGZWin* window, std::uint32_t incomeRows, std::uint32_t expenseRows) {
    StackSegments(window, 0x140155f0, incomeRows, expenseRows);
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

// add_image_button1 (i386 0x00261e94): a check button (WCM button style 4) showing `checked`.
void cSC4SubBudgetWindow::AddCheckButton(cIGZWin* parent, std::uint32_t id, int x, int y, std::uint32_t image, bool checked) {
    if (!parent || !g_host) return;
    gz::cIGZWinBtn* button = g_host->CreateButton(4, id, cRZString(), g_host->UiImage(image));
    if (!button) return;
    cIGZWin* win = button->AsIGZWin();
    if (!g_host->UiImage(image)) win->SetSize(0x1a, 0x10);
    button->SetBtnFlag(4, false);
    win->SetFlag(0x10000, true);
    win->SetFlag(0x20000, true);
    win->SetFlag(0x40000, true);
    win->SetFlag(4, true);
    button->SetChecked(checked);
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
    if (purpose_ == 1 && !individual_) {
        cIGZBuffer* strip = g_host->UiImage(0x140155f2);
        width_ = strip ? static_cast<int>(strip->Width()) : 0;
        rowStep_ = strip ? static_cast<int>(strip->Height()) / 2 : 0;
        InitOrdinanceBudget(window);
        initialized_ = true;
        return true;
    }
    if (purpose_ == 2 && !individual_) {
        cIGZBuffer* strip = g_host->UiImage(kSegBody);
        width_ = strip ? static_cast<int>(strip->Width()) : 0x1c8;
        rowStep_ = strip ? static_cast<int>(strip->Height()) / 2 : 0;
        InitNeighborBudget(window);
        initialized_ = true;
        return true;
    }
    if (individual_) {
        // PARTIAL: the individual building dialog.
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

// initOrdinanceBudget (i386 0x002678fe): every available ordinance as a row with a check button (on / off), an info button,
// its name and, while it is on, its monthly money; income ordinances above, expense ordinances below. Accept (0xcd) switches
// the changed ones, Cancel leaves them.
void cSC4SubBudgetWindow::InitOrdinanceBudget(cIGZWin* window) {
    cSC4OrdinanceSimulator* ordinances = g_host ? g_host->OrdinanceSimulator() : nullptr;
    ordinanceIds_.clear();
    ordinanceOn_.clear();
    ordinanceRows_.clear();
    ordinanceMoney_.clear();
    incomeRows_ = expenseRows_ = 0;
    if (ordinances) {
        std::uint32_t count = static_cast<std::uint32_t>(ordinances->GetOrdinanceAvailableCount());
        ordinanceIds_.resize(count);
        if (count) ordinances->GetOrdinanceAvailableArray(ordinanceIds_.data(), count);
        ordinanceIds_.resize(count);
        for (std::uint32_t id : ordinanceIds_) {
            cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(id);
            if (ordinance && ordinance->IsIncomeOrdinance()) ++incomeRows_;
            else ++expenseRows_;
            const bool on = ordinances->IsOrdinanceOn(id);
            ordinanceOn_.push_back({on, on});
        }
    }
    if (incomeRows_ == 0 && expenseRows_ == 0) {
        height_ = 0x60;
        SetEmptyWindow(window, Text(0x8a579f31), Text(0x0a5cb1a1));
    } else {
        const std::uint32_t shownIncome = std::min<std::uint32_t>(incomeRows_, kMaxRows);
        const std::uint32_t shownExpense = std::min<std::uint32_t>(expenseRows_, kMaxRows);
        SetupWindow(window, shownIncome, shownExpense);
        AddText(body_, 0xabcde00, 0x14, 8, Text(0x8a579f31), 0, kFontTitle, 'D', 'U', 'f');
        std::uint32_t textId = 0xabcde01, checkId = 300, infoId = 500;
        auto monthly = [&](std::uint32_t index) -> std::int64_t {
            auto* simple = dynamic_cast<cSC4OrdinanceSimple*>(ordinances->GetOrdinanceByID(ordinanceIds_[index]));
            return simple ? simple->GetCurrentMonthlyIncome() : 0;
        };
        for (int side = 0; side < 2; ++side) {
            const bool income = side == 0;
            if ((income ? incomeRows_ : expenseRows_) == 0) continue;
            int& top = income ? incomeTop_ : expenseTop_;
            const std::uint32_t headerFont = income ? kFontHeader : 0xaa0f4a9e;
            AddText(body_, textId, 0x12, top - rowStep_ - 2, income ? incomeTitle_ : expenseTitle_, 0, headerFont, 'D', 'U', 'f');
            AddText(body_, textId + 1, width_ - 0x26, top - rowStep_ - 2, income ? incomeHeader_ : expenseHeader_, 6, headerFont, 'D', 'U', 'f');
            textId += 2;
            std::int64_t total = 0;
            int shown = 0;
            for (std::uint32_t index = 0; index < ordinanceIds_.size(); ++index) {
                cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(ordinanceIds_[index]);
                if (!ordinance || ordinance->IsIncomeOrdinance() != income) continue;
                const bool on = ordinanceOn_[index].second;
                const std::int64_t money = (income ? monthly(index) : -monthly(index)) * multiplier_;
                if (shown >= kMaxRows) {
                    if (on) total += money;
                    continue;
                }
                AddCheckButton(window, checkId++, 0x12, top, 0x144161ea, on);
                ordinanceRows_.push_back(index);
                AddImageButton(window, infoId, 0x22, top, kImageDepartmentButton, false);
                if (cIGZWin* info = window->GetChildWindowFromIDRecursive(infoId)) info->SetNotificationTarget(window);
                ++infoId;
                AddText(body_, textId, 0x44, top, ordinance->GetName(), 0, kFontItem, 'D', 'U', 'f');
                cRZString text;
                if (on) {
                    SC4MakeMoneyString(money, text);
                    total += money;
                }
                if (income) AddText(body_, textId + 1, width_ - 0x26, top, text, 6, kFontItem, 0, 'f', 0);
                else AddText(body_, textId + 1, width_ - 0x26, top, text, 6, kFontItem, 'w', 0, 0);
                ordinanceMoney_.push_back({textId + 1, index, income});
                textId += 2;
                top += rowStep_;
                ++shown;
            }
            if (((income ? shownIncome : shownExpense) & 1) == 0) top += rowStep_;
            AddText(body_, textId, 0x96, top, totalCaption_, 0, kFontHeader, 'D', 'U', 'f');
            cRZString text;
            SC4MakeMoneyString(total, text);
            if (income) AddText(body_, textId + 1, width_ - 0x26, top, text, 6, kFontHeader, 0, 'f', 0);
            else AddText(body_, textId + 1, width_ - 0x26, top, text, 6, kFontHeader, 'w', 0, 0);
            (income ? incomeTotalText_ : expenseTotalText_) = textId + 1;
            textId += 2;
        }
    }
    // Accept (0xcd) and Cancel
    AddImageButton(window, 0xcd, 0xe, height_ - 0x28, kImageOkCancel, true);
    if (cIGZWin* ok = window->GetChildWindowFromIDRecursive(0xcd)) {
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
    for (std::uint32_t id = 300; id < 300 + ordinanceRows_.size(); ++id)
        if (cIGZWin* check = window->GetChildWindowFromIDRecursive(id)) check->SetNotificationTarget(window);
}

// updateOrdinanceBudget (i386 0x002628da): the rows' money (shown only while the ordinance is checked) and both totals.
void cSC4SubBudgetWindow::UpdateOrdinanceBudget(cIGZWin*) {
    cSC4OrdinanceSimulator* ordinances = g_host ? g_host->OrdinanceSimulator() : nullptr;
    if (!ordinances || !body_) return;
    std::int64_t totals[2] = {0, 0};
    for (std::uint32_t index = 0; index < ordinanceIds_.size(); ++index) {
        cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(ordinanceIds_[index]);
        if (!ordinance || !ordinanceOn_[index].second) continue;
        auto* simple = dynamic_cast<cSC4OrdinanceSimple*>(ordinance);
        const std::int64_t income = simple ? simple->GetCurrentMonthlyIncome() : 0;
        totals[ordinance->IsIncomeOrdinance() ? 0 : 1] += (ordinance->IsIncomeOrdinance() ? income : -income) * multiplier_;
    }
    for (const OrdinanceRow& row : ordinanceMoney_) {
        cRZString text;
        if (ordinanceOn_[row.index].second) {
            auto* simple = dynamic_cast<cSC4OrdinanceSimple*>(ordinances->GetOrdinanceByID(ordinanceIds_[row.index]));
            const std::int64_t income = simple ? simple->GetCurrentMonthlyIncome() : 0;
            SC4MakeMoneyString((row.income ? income : -income) * multiplier_, text);
        }
        if (cIGZWin* win = body_->GetChildWindowFromIDRecursive(row.moneyText)) win->SetCaption(text);
    }
    for (int side = 0; side < 2; ++side) {
        const std::uint32_t id = side == 0 ? incomeTotalText_ : expenseTotalText_;
        if (id == 0) continue;
        cRZString text;
        SC4MakeMoneyString(totals[side], text);
        if (cIGZWin* win = body_->GetChildWindowFromIDRecursive(id)) win->SetCaption(text);
    }
}

// =====================================================================================================================
// Neighbor deals (purpose 2)
// =====================================================================================================================
namespace {
constexpr std::uint32_t kNeighborDealCombo = 0xe8711255;   // cIGZWinCombo selection changed (p2 = the combo id)
constexpr std::uint32_t kButtonDealsCancel = 0xcf;
constexpr std::uint32_t kDealIncomeBitmap = 0x140155b9, kDealExpenseBitmap = 0x140155b8;
constexpr std::uint32_t kArrowUp = 0x140155cb, kArrowDown = 0x140155cc;

// One block of rows: the deals of one type under a header (the category, a column word and the block's running total).
struct DealSection {
    std::uint32_t type, opposite;            // the deal type and the one that cannot run with the same city
    std::uint32_t headerGroup, headerText;   // "Power" / "Water" / "Garbage"
    std::uint32_t columnText;                // the header's second column
    int mode;                                // set_comboBox's unit: 0 power, 1 water, 2 garbage
};
// initNeighborBudget's order: sales (income) power, water, garbage; purchases (expense) the same.
constexpr DealSection kIncomeSections[3] = {
    {0x4773ffe1, 0x4773ffe0, kStringGroup, 0x4a579570, 0xca579b63, 0},
    {0x4773ffe3, 0x4773ffe2, 0x0a554ae8, 0x0a6cd8f9, 0xca579b63, 1},
    {0x4773ffe5, 0x4773ffe4, kStringGroup, 0x4a579c5b, 0x6a579ca4, 2},
};
constexpr DealSection kExpenseSections[3] = {
    {0x4773ffe0, 0x4773ffe1, kStringGroup, 0x4a579570, 0xea579e1a, 0},
    {0x4773ffe2, 0x4773ffe3, 0x0a554ae8, 0x0a6cd8f9, 0xea579e1a, 1},
    {0x4773ffe4, 0x4773ffe5, kStringGroup, 0x4a579c5b, 0x8a579e78, 2},
};
constexpr std::uint32_t kDealRowColumnText = 0xca579b63;

std::uint32_t CitySerialOf(cISC4NeighborDeal* deal) {
    auto* city = deal ? static_cast<cSC4RegionalCity*>(deal->GetCityOnOtherSideOfConnection()) : nullptr;
    return city ? city->GetCitySerialNumber() : 0;   // regional city vslot 0x6c
}
}  // namespace

// initNeighborBudget (i386 0x0026a2e2): the simulator stops trading while the dialog is open (SetUIDisableFlag), proposes the
// deals the neighbours offer (ProposeDeals from the UI), then every deal of the six types is listed: a header per type that has
// deals and a row per deal. The rows are the first nine of each side; the arrows scroll the rest.
void cSC4SubBudgetWindow::InitNeighborBudget(cIGZWin* window) {
    cSC4NeighborsSimulator* neighbors = g_host ? g_host->NeighborsSimulator() : nullptr;
    incomeRows_ = expenseRows_ = 0;
    combos_.clear();
    dealCities_.clear();
    dealList_.clear();
    if (neighbors) {
        neighbors->SetUIDisableFlag(true);
        DealList found;
        neighbors->ProposeDeals(found, true);
        found.clear();
        for (const DealSection* sides : {kIncomeSections, kExpenseSections}) {
            for (int i = 0; i < 3; ++i) {
                const DealSection& section = sides[i];
                std::uint32_t& rows = sides == kIncomeSections ? incomeRows_ : expenseRows_;
                found.clear();
                neighbors->FindDeals(section.type, found);
                bool counted = false;
                for (auto& entry : found) {
                    cISC4NeighborDeal* deal = entry.get();
                    if (!deal) continue;
                    if (!counted) { ++rows; counted = true; }   // the header row
                    ++rows;
                    NDList item;
                    item.type = section.type;
                    item.deal = cRZAutoRefCount<cISC4NeighborDeal>(deal);
                    dealList_.push_back(std::move(item));
                    if (deal->GetFlags() & kDealFlagActive) {
                        // the restore point: the amount and step the deal had when the dialog opened
                        deal->SetLastAmount(deal->GetAmount());
                        deal->SetLastIncAmount(deal->GetIncAmount());
                        dealCities_.push_back({CitySerialOf(deal), section.type});
                    }
                }
            }
        }
    }
    if (Trace()) std::fprintf(stderr, "sub budget: neighbor deals %zu listed (%u income rows, %u expense rows)\n", dealList_.size(), incomeRows_, expenseRows_);
    const cRZString title = Text(0x2a579ad2);
    if (incomeRows_ == 0 && expenseRows_ == 0) {
        height_ = 0x60;
        SetEmptyWindow(window, title, Text(0xca5cb52d));
        return;
    }
    SetupWindowBig(window, std::min<std::uint32_t>(kMaxRows, incomeRows_), std::min<std::uint32_t>(kMaxRows, expenseRows_));
    AddText(body_, 0xabcde00, 0x14, 8, title, 0, kFontTitle, 'D', 'U', 'f');
    const int incomeTop = incomeTop_, expenseTop = expenseTop_;
    incomeScroll_ = expenseScroll_ = 0;
    LayoutNeighborRows(window, true);
    // OK keeps the deals, the second button restores them (on_button_activated 0xcf -> RestoreDeals)
    AddImageButton(window, kButtonOk, 0xe, height_ - 0x28, kImageOkCancel, true);
    if (cIGZWin* ok = window->GetChildWindowFromIDRecursive(kButtonOk)) {
        ok->SetSize(0xb4, 0x1e);
        ok->SetCaption(cSLString(0x0a554ae8, 0x2a6cd8fd, true));
        ok->SetNotificationTarget(window);
    }
    AddImageButton(window, kButtonDealsCancel, width_ - 0xc3, height_ - 0x28, kImageOkCancel, true);
    if (cIGZWin* cancel = window->GetChildWindowFromIDRecursive(kButtonDealsCancel)) {
        cancel->SetSize(0xb4, 0x1e);
        cancel->SetCaption(cSLString(0x6a231eaa, 0x6a56b0bb, true));
        cancel->SetNotificationTarget(window);
    }
    // the scroll arrows: up hidden at the top, down shown when a side has more than nine rows
    const struct { std::uint32_t id; int y; std::uint32_t image; bool shown; } arrows[4] = {
        {0x451, incomeTop + 4, kArrowUp, false},
        {0x452, incomeTop + 4 + rowStep_ * 8, kArrowDown, incomeRows_ >= 10},
        {0x453, expenseTop + 4, kArrowUp, false},
        {0x454, expenseTop + 4 + rowStep_ * 8, kArrowDown, expenseRows_ >= 10},
    };
    for (const auto& arrow : arrows) {
        AddImageButton(window, arrow.id, width_ - 0x21, arrow.y, arrow.image, false);
        if (cIGZWin* button = window->GetChildWindowFromIDRecursive(arrow.id)) {
            button->SetNotificationTarget(window);
            if (!arrow.shown) button->SetFlag(1, false);
        }
    }
    for (const ComboInfo& info : combos_)
        if (cIGZWin* combo = window->GetChildWindowFromIDRecursive(info.id)) combo->SetNotificationTarget(window);
}

// The rows of both sides from the scroll offsets: on init each visible slot's controls are made (text ids from 0xabcde01,
// combos from 0xabce100, bitmaps from 0xabce200, in row order); updateNeighborBudget (i386 0x0027385e) walks the same rows and
// re-captions the slots. A header slot hides its bitmap and combo; a deal slot hides its column word.
void cSC4SubBudgetWindow::LayoutNeighborRows(cIGZWin* window, bool create) {
    cSC4NeighborsSimulator* neighbors = g_host ? g_host->NeighborsSimulator() : nullptr;
    combos_.clear();
    if (!create) {
        dealCities_.clear();
        for (const NDList& item : dealList_)
            if (item.deal && (item.deal->GetFlags() & kDealFlagActive)) dealCities_.push_back({CitySerialOf(item.deal.get()), item.type});
    }
    std::uint32_t textId = 0xabcde01, comboId = 0xabce100, bitmapId = 0xabce200;
    for (int side = 0; side < 2; ++side) {
        const bool income = side == 0;
        const DealSection* sections = income ? kIncomeSections : kExpenseSections;
        const std::uint32_t rows = income ? incomeRows_ : expenseRows_;
        DealSlots& slots = income ? incomeSlots_ : expenseSlots_;
        const std::uint32_t scroll = income ? incomeScroll_ : expenseScroll_;
        const int sideTop = income ? incomeTop_ : expenseTop_;
        if (rows == 0) continue;
        if (create) {
            AddText(body_, textId, 0x12, sideTop - rowStep_ - 2, income ? incomeTitle_ : expenseTitle_, 0, kFontHeader, 'D', 'U', 'f');
            AddText(body_, textId + 1, width_ - 0x26, sideTop - rowStep_ - 2, income ? incomeHeader_ : expenseHeader_, 6, kFontHeader, 'D', 'U', 'f');
            textId += 2;
            slots.text = textId;
            slots.bitmap = bitmapId;
            slots.combo = comboId;
            slots.top = sideTop;
            slots.count = 0;
        }
        // the side's rows in order: a header per type with deals, then its deals
        struct Row { const DealSection* section; cISC4NeighborDeal* deal; bool header; std::int64_t total; };
        std::vector<Row> list;
        for (int i = 0; i < 3; ++i) {
            const DealSection& section = sections[i];
            std::int64_t total = 0;
            for (const NDList& item : dealList_)
                if (item.type == section.type && item.deal && (item.deal->GetFlags() & kDealFlagActive)) total += item.deal->GetCost();
            bool header = false;
            for (const NDList& item : dealList_) {
                if (item.type != section.type || !item.deal) continue;
                if (!header) { list.push_back({&section, item.deal.get(), true, total}); header = true; }
                list.push_back({&section, item.deal.get(), false, 0});
            }
        }
        int top = slots.top;
        int slot = 0;
        for (std::size_t r = scroll; r < list.size() && slot < kMaxRows; ++r, ++slot, top += rowStep_) {
            const Row& row = list[r];
            const std::uint32_t name = slots.text + 3 * slot, column = name + 1, money = name + 2;
            const std::uint32_t bitmap = slots.bitmap + slot, combo = slots.combo + slot;
            cRZString caption, columnCaption, moneyCaption;
            const std::uint32_t font = row.header ? kFontDepartment : kFontItem;
            if (row.header) {
                caption = cSLString(row.section->headerGroup, row.section->headerText, true);
                columnCaption = Text(row.section->columnText);
                SC4MakeMoneyString(row.total, moneyCaption);
            } else {
                if (auto* city = static_cast<cSC4RegionalCity*>(row.deal->GetCityOnOtherSideOfConnection())) city->GetCityName(caption);
                columnCaption = Text(kDealRowColumnText);
                SC4MakeMoneyString((row.deal->GetFlags() & kDealFlagActive) ? row.deal->GetCost() : 0, moneyCaption);
            }
            if (create) {
                AddText(body_, name, 0x12, top, caption, 0, font, 'D', 'U', 'f');
                AddText(body_, column, 0xda, top, columnCaption, 0, font, 'D', 'U', 'f');
                AddText(body_, money, width_ - 0x26, top, moneyCaption, 6, font, 'D', 'U', 'f');
                AddBitmap(income ? kDealIncomeBitmap : kDealExpenseBitmap, 0xce, top, window, bitmap);
                AddComboBox(window, combo, 0xda, top + 1);
                ++slots.count;
            } else {
                if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(name)) text->SetCaption(caption);
                if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(column)) text->SetCaption(columnCaption);
                if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(money)) text->SetCaption(moneyCaption);
            }
            if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(column)) text->SetFlag(1, row.header);
            if (cIGZWin* image = window->GetChildWindowFromIDRecursive(bitmap)) image->SetFlag(1, !row.header);
            if (cIGZWin* box = window->GetChildWindowFromIDRecursive(combo)) box->SetFlag(1, !row.header);
            ComboInfo info;
            info.id = combo;
            info.deal = cRZAutoRefCount<cISC4NeighborDeal>(row.deal);
            combos_.push_back(std::move(info));
            if (!row.header) {
                int state = ((row.deal->GetFlags() & kDealFlagActive) && !(row.deal->GetFlags() & kDealFlagCancelled)) ? 1 : 0;
                const std::uint32_t city = CitySerialOf(row.deal);
                for (const NDCityList& running : dealCities_)
                    if (running.type == row.section->opposite && running.city == city) state = 2;
                SetComboBox(window, combo, row.section->type, row.deal, row.section->mode, state);
            }
        }
        if (create) {
            textId = slots.text + 3 * slots.count;
            bitmapId = slots.bitmap + slots.count;
            comboId = slots.combo + slots.count;
            // the side's total: a blank half row when the strip count is even, then the caption and the simulator's total
            const std::uint32_t shown = std::min<std::uint32_t>(kMaxRows, rows);
            int totalTop = slots.top + rowStep_ * slots.count;
            if ((shown & 1) == 0) totalTop += rowStep_;
            AddText(body_, textId, 0xfa, totalTop, totalCaption_, 0, kFontHeader, 'D', 'U', 'f');
            slots.total = textId + 1;
            textId += 2;
            const std::int64_t total = !neighbors ? 0 : income ? neighbors->GetTotalMonthlyNeighborDealIncome() : neighbors->GetTotalMonthlyNeighborDealExpense();
            cRZString money;
            SC4MakeMoneyString(total, money);
            if (income) AddText(body_, slots.total, width_ - 0x26, totalTop, money, 6, kFontHeader, 0, 'f', 0);
            else AddText(body_, slots.total, width_ - 0x26, totalTop, money, 6, kFontHeader, 'w', 0, 0);
        } else if (neighbors) {
            cRZString money;
            SC4MakeMoneyString(income ? neighbors->GetTotalMonthlyNeighborDealIncome() : neighbors->GetTotalMonthlyNeighborDealExpense(), money);
            if (cIGZWin* text = body_->GetChildWindowFromIDRecursive(slots.total)) text->SetCaption(money);
        }
    }
}

void cSC4SubBudgetWindow::UpdateNeighborBudget(cIGZWin* window) {
    if (!body_) return;
    LayoutNeighborRows(window, false);
}

// add_comboBox (i386 0x002691ee): a 0x78 x 0xf combo box, not editable, the dialog's colours, font style 0x4a809917.
void cSC4SubBudgetWindow::AddComboBox(cIGZWin* parent, std::uint32_t id, int x, int y) {
    gz::cIGZWinCombo* combo = g_host ? g_host->CreateComboBox() : nullptr;
    if (!combo || !parent) {
        if (combo) combo->Release();
        return;
    }
    cIGZWin* win = combo->AsIGZWin();
    win->SetID(id);
    win->SetArea(x, y, x + 0x78, y + 0xf);
    if (gz::IWinFont* font = g_host->Font(0x4a809917)) combo->SetFont(font->AsFont());
    combo->SetFontColor(gz::cIGZWinCombo::kColorText, g_host->MakeColor(0x3f, 0x49, 0x67));
    combo->SetFontColor(gz::cIGZWinCombo::kColorSelected, g_host->MakeColor(0xff, 0xff, 0xff));
    combo->SetHighlightColor(g_host->MakeColor(0x11, 0x22, 0x66));
    win->SetFillColor(g_host->MakeColor(0xde, 0xe8, 0xe3));
    combo->SetDropDownOutlineColor(g_host->MakeColor(0, 0, 0));
    win->SetFlag(0x10000, true);
    win->SetFlag(0x800, false);
    win->SetFlag(4, true);
    combo->SetEditable(false);
    if (gz::cIGZWinTextEdit* edit = combo->GetTextEdit()) {   // borrowed (the original holds it in an auto pointer)
        edit->SetFillColor(g_host->MakeColor(0xde, 0xe8, 0xe3));                                       // text edit vslot 0x50
        edit->SetFontColor(gz::cIGZWinTextEdit::kColorText, g_host->MakeColor(0x3f, 0x49, 0x67));   // vslot 0x34
    }
    parent->ChildAdd(win);
    combo->Release();
}

// set_comboBox (i386 0x002639be). The steps: a tenth of the most the deal can trade (GetDealMaxAmount, else its amount), at least
// the tuning exemplar's smallest step for the type; entry 0 is "none" (not running) or "cancel" (running), then one entry per
// step. A deal whose amounts are many steps wide lists the steps up to its amount and then tenths of the maximum (IncAmount2).
void cSC4SubBudgetWindow::SetComboBox(cIGZWin* window, std::uint32_t id, std::uint32_t type, cISC4NeighborDeal* deal, int mode, int state) {
    void* raw = nullptr;
    if (!deal || !window || !window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinCombo, &raw) || !raw) return;
    auto* combo = static_cast<gz::cIGZWinCombo*>(raw);
    float unitScale = 1.0f;   // the simulator exemplar's garbage display scale (0x09135482)
    if (cISCPropertyHolder* simulator = g_host->Exemplar(0xe7e2c2db, 0xc911e35b)) cSCPropertyHelp::GetPropertyValue(simulator, 0x09135482, unitScale);
    cSC4NeighborsSimulator* neighbors = g_host->NeighborsSimulator();
    int maximum = neighbors ? static_cast<int>(static_cast<std::uint32_t>(neighbors->GetDealMaxAmount(type, static_cast<cISC4RegionalCity*>(deal->GetCityOnOtherSideOfConnection())))) : 0;
    if (maximum < 1) maximum = deal->GetAmount();
    int step = maximum / 10;
    std::int32_t smallest = 0;
    if (cISCPropertyHolder* tuning = g_host->Exemplar(0x7a4a8458, 0x6a469021)) {
        const std::uint32_t offset = type - 0x4773ffe0;
        const std::uint32_t property = offset < 2 ? 0xea46932c : offset < 4 ? 0x6a469337 : offset < 6 ? 0x8a46933d : 0;
        if (property) cSCPropertyHelp::GetPropertyValue(tuning, property, smallest);
    }
    int increment = std::max<int>(smallest, step);
    combo->AsIGZWin()->SetFlag(2, true);
    combo->RemoveAllStrings();
    deal->SetIncAmount2(0);
    int selection = 0;
    int upper = maximum;
    auto unavailable = [&]() {
        combo->InsertString(Text(0xca5793c1), -2);
        combo->SetSelection(0, false);
        combo->AsIGZWin()->SetFlag(2, false);
        combo->Release();
    };
    if (state == 0) {
        combo->InsertString(Text(0xca5793c0), -2);
        deal->SetIncAmount(increment);
    } else if (state == 2) {
        unavailable();
        return;
    } else if (!deal->CheckFlagsOr(kDealFlagFrozen)) {
        combo->InsertString(Text(0x2a5794a0), -2);
        int dealStep = deal->GetIncAmount();
        const int amount = deal->GetAmount();
        if ((dealStep == amount && smallest < dealStep) || dealStep == 0) {
            deal->SetIncAmount(step);
            increment = step;
            if (step == 0) {
                unavailable();
                return;
            }
        } else {
            increment = dealStep;
        }
        selection = amount / increment;
        upper = std::max(selection * increment, maximum);
    } else {   // a frozen deal: its amount is the only choice
        combo->InsertString(Text(0x2a5794a0), -2);
        upper = deal->GetAmount();
        deal->SetIncAmount(upper);
        deal->SetIncAmount2(upper);
        selection = 1;
        increment = upper;
        upper = std::max(upper, maximum);
    }
    if (upper < increment) upper = increment;
    auto label = [&](int value) {
        cRZString text;
        if (mode == 1) text.Sprintf(Text(0x6a579c00).ToChar(), value);
        else if (mode == 0) text.Sprintf(Text(0xaa579b3a).ToChar(), value);
        else {
            float scaled = static_cast<float>(value) * unitScale;
            if (scaled <= 0.0f) scaled = 0.0f;
            text.Sprintf("%d", static_cast<int>(static_cast<std::uint32_t>(scaled)));
        }
        if (value >= 0) combo->InsertString(text, -2);
    };
    if (increment > 0) {
        if (deal->GetAmount() == 0 || static_cast<std::uint32_t>(upper / deal->GetIncAmount()) < 0x10) {
            for (int value = increment; value <= upper; value += increment) label(value);
        } else {
            const int coarse = upper / 10;
            deal->SetIncAmount2(coarse);
            for (int value = increment; value <= deal->GetAmount(); value += increment) label(value);
            if (coarse > 0)
                for (int value = coarse; value <= upper; value += coarse)
                    if (deal->GetAmount() < value) label(value);
        }
    }
    combo->SetSelection(selection, false);
    combo->Release();
}

// DoWinMsg's combo branch (i386 0x00279028, 0xe8711255): entry 0 cancels a running deal (it becomes a proposal again); any other
// entry starts the deal if needed and sets its amount to that many steps (past the fine steps, the coarse ones count).
bool cSC4SubBudgetWindow::OnComboChanged(cIGZWin* window, std::uint32_t id) {
    for (const ComboInfo& info : combos_) {
        if (info.id != id || !info.deal) continue;
        void* raw = nullptr;
        if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinCombo, &raw) || !raw) break;
        auto* combo = static_cast<gz::cIGZWinCombo*>(raw);
        cISC4NeighborDeal* deal = info.deal.get();
        const int selection = combo->GetSelection();
        if (selection == 0) {
            if (deal->GetFlags() & kDealFlagActive) {
                deal->Cancel();
                combo->RemoveString(0);
                combo->InsertString(Text(0xca5793c0), 0);
                combo->SetSelection(0, true);
                deal->SetFlags(kDealFlagProposed, true);
            }
        } else if (!deal->CheckFlagsOr(kDealFlagLockedState)) {
            if (!(deal->GetFlags() & kDealFlagActive)) deal->Activate();
            const int coarse = deal->GetIncAmount2(), step = deal->GetIncAmount();
            if (coarse != 0 && step != 0 && static_cast<std::uint32_t>(deal->GetAmount()) < static_cast<std::uint32_t>(step * selection)) {
                const int amount = deal->GetAmount();
                deal->SetAmount((amount / coarse + (selection - amount / step)) * coarse);
                deal->SetIncAmount(coarse);
            } else {
                deal->SetAmount(selection * step);
            }
            combo->RemoveString(0);
            combo->InsertString(Text(0x2a5794a0), 0);
        }
        if (Trace()) std::fprintf(stderr, "sub budget: deal %08x combo %08x -> entry %d: flags %x amount %d cost %lld\n", deal->GetType(), id, selection,
                                  deal->GetFlags(), deal->GetAmount(), static_cast<long long>(deal->GetCost()));
        combo->Release();
        break;
    }
    UpdateNeighborBudget(window);
    return true;
}

// RestoreDeals (i386 0x0026a0da): every listed deal back to how the dialog found it (the amounts kept in Last* on init).
void cSC4SubBudgetWindow::RestoreDeals() {
    for (const NDList& item : dealList_) {
        cISC4NeighborDeal* deal = item.deal.get();
        if (!deal) continue;
        if (deal->GetFlags() & kDealFlagActive) {
            if (deal->GetLastAmount() == 0) {
                deal->Cancel();
                deal->SetLastAmount(0);
                deal->SetLastIncAmount(0);
            } else {
                deal->SetAmount(deal->GetLastAmount());
                deal->SetIncAmount(deal->GetLastIncAmount());
            }
        }
        if ((deal->GetFlags() & kDealFlagCancelled) && deal->GetLastAmount() != 0) {
            deal->Activate();
            deal->SetAmount(deal->GetLastAmount());
            deal->SetIncAmount(deal->GetLastIncAmount());
        }
    }
}

// DestroySubBudgetWindow's deal part (i386 0x0026961a): proposals nobody took are cancelled, then the simulator trades again.
void cSC4SubBudgetWindow::ReleaseDeals() {
    if (purpose_ != 2) return;
    cSC4NeighborsSimulator* neighbors = g_host ? g_host->NeighborsSimulator() : nullptr;
    for (const NDList& item : dealList_)
        if (item.deal && (item.deal->GetFlags() & kDealFlagProposed)) item.deal->Cancel();
    if (neighbors) {
        neighbors->SetUIDisableFlag(false);
        neighbors->MarkDealsDirty();
    }
    combos_.clear();
    dealList_.clear();
    dealCities_.clear();
}

}  // namespace sc4
