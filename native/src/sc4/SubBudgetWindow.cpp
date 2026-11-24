#include "sc4/SubBudgetWindow.h"
#include "gz/WinFlatRect.h"
#include "platform/Unwired.h"

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
    if (popup_) popup_->Release();
    if (popupInner_) popupInner_->Release();
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
bool cSC4SubBudgetWindow::Create(cIGZWin** window, std::uint32_t purpose, std::uint32_t period, std::uint32_t department) {
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
    individual_ = department;
    period_ = period;
    if (Trace()) std::fprintf(stderr, "sub budget: created for purpose 0x%08x period %u\n", purpose, period);
    gen->Release();
    if (!window) win->Release();
    return true;
}

// DestroySubBudgetWindow (i386 0x0026961a): the neighbor-deal proposals are dropped (ReleaseDeals), then end_dialog.
void cSC4SubBudgetWindow::Destroy() {
    if (Trace())
        for (const BudgetRestoreInfo& info : restore_)
            if (info.department) std::fprintf(stderr, "sub budget: destroy: department %08x funding %.2f (opened at %.2f)\n", info.department->GetDepartmentID(),
                                              info.department->GetFundingPercentage(), info.funding);
    ReleaseDeals();
    if (g_host) g_host->SimulatorHiddenPause(false);   // spSimulator vslot 0x34 HiddenResume
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
                if (individual_) {
                    // a building's slider (primary 0xabce400 + row, its second item + 0x100): its building's funding
                    for (const auto& row : rowHolders_) {
                        if (row.first != p2 || !row.second) continue;
                        const bool second = p2 >= 0x0abce500u;
                        SetOccupantFundingLevel(row.second, second, static_cast<float>(slider->GetValue()));
                        if (Trace()) std::fprintf(stderr, "sub budget: building slider %08x -> %d%%\n", static_cast<std::uint32_t>(p2), slider->GetValue());
                        break;
                    }
                    UpdateIndividualBudget(window, false);
                } else {
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
    for (BudgetRestoreInfo& info : restore_)
        if (info.department) info.department->Release();
    restore_.clear();
    ReleaseIndividualRestore();
    rowHolders_.clear();
    AddRef();   // the original holds itself across the releases below
    if (dialog_) { dialog_->Release(); dialog_ = nullptr; }
    if (body_) { body_->Release(); body_ = nullptr; }
    if (popup_) { popup_->Release(); popup_ = nullptr; }
    if (popupInner_) { popupInner_->Release(); popupInner_ = nullptr; }
    Release();
    return true;
}

// on_button_activated (i386 0x0027825c): OK keeps the funding, Cancel restores it; both close the dialog.
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
    if (individual_ && id > 0x0abcddffu && id < 0x0abcdf00u) {   // a building's name (i386 0x002783..): the camera looks at it
        for (const auto& row : rowHolders_)
            if (row.first == id && row.second) {
                const bool moved = g_host && g_host->LookAtBuilding(row.second);
                if (Trace()) std::fprintf(stderr, "sub budget: name %08x -> camera %s\n", id, moved ? "moved" : "unchanged");
                break;
            }
        return false;
    }
    if (id - 0x0abce600u < 0x100 && !individual_) {   // a department row's button: its buildings, one by one
        for (const SliderInfo& info : sliders_) {
            if (info.departmentRow && info.id == id - 0x700 && info.department) {   // the row's slider is 0xabcdf00 + n
                DoIndividualBudgetWindow(info.department->GetDepartmentID());
                return true;
            }
        }
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
    if (id >= 0x451 && id <= 0x454) {   // the scroll arrows (two rows a click), every path
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
        if (purpose_ == 1) UpdateOrdinanceBudget(window);
        else if (purpose_ == 2) UpdateNeighborBudget(window);
        else if (individual_) UpdateIndividualBudget(window, false);   // the single-building list (i386 0x00278532..)
        else UpdateValues(window);
        return false;
    }
    if (id == kButtonOk) {
        EndDialog(window, kResultClose);
        return true;
    }
    if (id >= 500 && id < 700) {   // an ordinance's info button
        ShowOrdinanceInfo(window, id - 500);
        return false;
    }
    if (id == 0x68 || id == 900) {   // the info popup's close button / a click on it
        ClosePopup(window);
        return false;
    }
    if (id == 0xcc || id == 0x385) {   // the empty dialog's close button / a click on it
        EndDialog(window, kResultClose);
        return true;
    }
    return false;
}

// RestoreStartSettings (i386 0x002638fc): every department back to the funding the dialog opened with.
void cSC4SubBudgetWindow::RestoreStartSettings() {
    if (Trace()) std::fprintf(stderr, "sub budget: restore start settings\n");
    if (individual_) {   // every building back to the two fundings the dialog saw last (the +0x44 records)
        for (const IndividualRestore& record : individualRestore_) {
            SetOccupantFundingLevel(record.holder, false, record.primary);
            SetOccupantFundingLevel(record.holder, true, record.secondary);
        }
        return;
    }
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

// The popup's two generic windows (i386 SetEmptyWindow 0x00265ef4 / on_button_activated 500+n): the outer one (id 0x423278d)
// on the dialog, the inner one (id 0x423278f, background image 0x144161e4) inside it.
void cSC4SubBudgetWindow::MakePopupPanels(cIGZWin* window, bool ordinanceStyle) {
    if (!g_host || !window) return;
    auto genFlags = [](gz::cIGZWinGen* gen, bool flag2000) {
        gen->SetWinGenFlag(1, false);
        gen->SetWinGenFlag(2, false);
        gen->SetWinGenFlag(0x10000, false);
        gen->SetWinGenFlag(0x20, false);
        gen->SetWinGenFlag(0x40, false);
        gen->SetWinGenFlag(0x1000, true);
        gen->SetWinGenFlag(0x2000, flag2000);
        gen->SetWinGenFlag(0x4000, true);
    };
    gz::cIGZWinGen* outer = g_host->CreateWinGen();
    if (!outer) return;
    if (popup_) popup_->Release();
    popup_ = outer->AsIGZWin();
    popup_->AddRef();
    genFlags(outer, true);
    if (ordinanceStyle) popup_->SetFlag(0x8000, false);
    popup_->SetFlag(0x10000, false);
    popup_->SetFlag(0x200000, false);
    popup_->SetFlag(0x800, true);
    popup_->PullToFront();
    popup_->SetFlag(0x800, false);
    popup_->SetID(0x423278d);
    window->ChildAdd(popup_);
    gz::cIGZWinGen* inner = g_host->CreateWinGen();
    if (inner) {
        if (popupInner_) popupInner_->Release();
        popupInner_ = inner->AsIGZWin();
        popupInner_->AddRef();
        genFlags(inner, false);
        if (ordinanceStyle) {
            popupInner_->SetFlag(4, true);
            popupInner_->SetFlag(0x8000, false);
            popupInner_->SetFlag(0x100, true);
            popupInner_->SetFlag(0x10000, true);
            popupInner_->SetFlag(0x200000, true);
            popupInner_->SetFlag(0x800, false);
        } else {
            popupInner_->SetFlag(0x100, true);
            popupInner_->SetFlag(0x10000, true);
            popupInner_->SetFlag(0x20000, true);
            popupInner_->SetFlag(0x200000, true);
            popupInner_->SetFlag(0x800, false);
            popupInner_->SetFlag(4, true);
        }
        popupInner_->SetID(0x423278f);
        popup_->ChildAdd(popupInner_);
        inner->SetBackgroundBitmap(static_cast<gz::BGBltType>(2), g_host->UiImage(0x144161e4));
        inner->Release();
    }
    outer->Release();
}

// the popup goes (buttons 0x68 / 900: the dialog's ChildRemove of +0x14)
void cSC4SubBudgetWindow::ClosePopup(cIGZWin* window) {
    if (!popup_) return;
    if (window) window->ChildRemove(popup_);
    popup_->Release();
    popup_ = nullptr;
    if (popupInner_) {
        popupInner_->Release();
        popupInner_ = nullptr;
    }
}

// SetEmptyWindow (i386 0x00265ef4): a group without rows says so in a 300 x 100 dialog, centred, closed by 0xcc or a click on it.
void cSC4SubBudgetWindow::SetEmptyWindow(cIGZWin* window, const cIGZString& title, const cIGZString& text) {
    if (!window) return;
    MakePopupPanels(window, false);
    if (!popup_ || !popupInner_) return;
    popup_->SetSize(300, 100);
    popupInner_->SetSize(300, 100);
    popup_->GZWinMoveTo(0, 0);
    popupInner_->GZWinMoveTo(0, 0);
    window->SetSize(300, 100);
    if (cIGZWin* parent = window->GetParentWin())
        window->GZWinMoveTo((parent->GetW() - window->GetW()) / 2, (parent->GetH() - window->GetH()) / 2);
    AddText(popupInner_, 0xabce000, 10, 5, title, 0, 0xea85d307, 'D', 'U', 'f');
    AddText(popupInner_, 0xabce001, 0xf, 0x19, text, 99, 0xea85d308, 'D', 'U', 'f');
    AddImageButton(popup_, 0x385, 0, 0, 0x14416242, false);
    AddImageButton(popup_, 0xcc, 0x10d, 0xb, 0x140155b4, false);
    if (cIGZWin* back = popup_->GetChildWindowFromIDRecursive(0x385)) back->SetSize(300, 100);
    if (cIGZWin* frame = popup_->GetChildWindowFromIDRecursive(0x485)) frame->SetSize(300, 100);
    if (cIGZWin* back = popup_->GetChildWindowFromIDRecursive(0x385)) back->SetNotificationTarget(window);
    if (cIGZWin* close = popup_->GetChildWindowFromIDRecursive(0xcc)) close->SetNotificationTarget(window);
}

// on_button_activated 500+n (i386 0x0027825c): the ordinance's name and description in a popup beside its row, (width - 0x3c)
// x 0x7d at x 0x1e, as low as the row's info button (up to the dialog's height - 0x7d); 900 / 0x68 close it.
void cSC4SubBudgetWindow::ShowOrdinanceInfo(cIGZWin* window, std::uint32_t row) {
    ClosePopup(window);
    if (!window || row >= ordinanceRows_.size()) return;
    const std::uint32_t index = ordinanceRows_[row];
    cIGZWin* info = window->GetChildWindowFromIDRecursive(row + 500 + 0x100);   // the info button's frame
    if (!info) return;
    const int rowTop = info->GetT();
    MakePopupPanels(window, true);
    if (!popup_ || !popupInner_) return;
    const int width = width_ - 0x3c;
    popup_->SetSize(width, 0x7d);
    popupInner_->SetSize(width, 0x7d);
    popup_->GZWinMoveTo(0x1e, std::min(rowTop, height_ - 0x7d));
    popupInner_->GZWinMoveTo(0, 0);
    cSC4OrdinanceSimulator* ordinances = g_host->OrdinanceSimulator();
    cISC4Ordinance* ordinance = ordinances && index < ordinanceIds_.size() ? ordinances->GetOrdinanceByID(ordinanceIds_[index]) : nullptr;
    if (ordinance) {
        AddText(popupInner_, 0xabce000, 10, 5, ordinance->GetName(), 0, 0xea85d307, 'D', 'U', 'f');
        AddText(popupInner_, 0xabce001, 0xf, 0x19, ordinance->GetDescription(), 99, 0xea85d308, 'D', 'U', 'f');
    }
    AddImageButton(popup_, 900, 0, 0, 0x14416242, false);
    AddImageButton(popup_, 0x68, width_ - 0x5b, 0xb, 0x140155b4, false);
    if (cIGZWin* back = popup_->GetChildWindowFromIDRecursive(900)) back->SetSize(width, 0x7d);
    if (cIGZWin* frame = popup_->GetChildWindowFromIDRecursive(0x484)) frame->SetSize(width, 0x7d);
    if (cIGZWin* back = popup_->GetChildWindowFromIDRecursive(900)) back->SetNotificationTarget(window);
    if (cIGZWin* close = popup_->GetChildWindowFromIDRecursive(0x68)) close->SetNotificationTarget(window);
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
    if (align == 6) {
        win->GZWinMoveTo(x - win->GetW(), y);
    } else if (align == 99) {   // the full-width variant: the parent's width less x on each side, down to its bottom; left aligned
        win->SetArea(x, y, parent->GetW() - 2 * x, parent->GetH() - y);
        win->GZWinMoveTo(x, y);
        align = 0;
    } else {
        win->GZWinMoveTo(x, y);
    }
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

// add_image_button2 (i386 0x002620f8): a WCM style-2 button showing `image` (0x1a x 0x10 without one); a framed one takes
// the framed look and font 0x4a809919. The button's click sound is 0x4a5c31d7 and its text colours (63, 73, 103) normal and
// pressed, (102, 102, 102) disabled. It sits at 0,0 inside a flat rect (class 0xc2afa76e, id + 0x100, fill (255, 0, 255),
// outline flags 0x40) placed at x, y.
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
        button->SetFont(g_host->Font(0x4a809919));
    }
    button->SetBtnFlag(4, framed);
    button->SetSoundID(0, 0x4a5c31d7);
    button->SetFontColor(0, g_host->MakeColor(0x3f, 0x49, 0x67));
    button->SetFontColor(2, g_host->MakeColor(0x3f, 0x49, 0x67));
    button->SetFontColor(1, g_host->MakeColor(0x66, 0x66, 0x66));
    win->SetFlag(4, true);
    win->GZWinMoveTo(x, y);
    auto* frame = new gz::cGZWinFlatRect(g_host->WindowManager());   // GZCOM 0xc2afa76e
    frame->AddRef();
    cIGZWin* frameWin = frame->AsIGZWin();
    frameWin->SetArea(win->GetArea());
    frameWin->GZWinMoveTo(x, y);
    win->GZWinMoveTo(0, 0);
    frameWin->SetFlag(0x10000, false);
    frameWin->SetFlag(0x20000, false);
    frameWin->SetFlag(0x40000, false);
    frameWin->SetFlag(0x800, false);
    frameWin->SetID(id + 0x100);
    frame->SetFillColor(0xff, 0, 0xff);
    frame->SetOutlineFlags(0x40);
    parent->ChildAdd(frameWin);
    frameWin->ChildAdd(win);
    frame->Release();
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
    if (g_host) g_host->SimulatorHiddenPause(true);   // spSimulator vslot 0x28 HiddenPause
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
    int arrowIncomeTop = -1, arrowExpenseTop = -1;   // the department path's scroll arrows (made after OK / Cancel)
    if (individual_) {
        InitIndividualBudget(window);
    } else {
        incomeScroll_ = expenseScroll_ = 0;
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
            }
            arrowIncomeTop = incomeTop;
            arrowExpenseTop = expenseTop;
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
    if (arrowIncomeTop >= 0) {   // the scroll arrows (i386 on_init tail): up hidden, down shown when a side has ten rows or more
        const struct { std::uint32_t id; int y; std::uint32_t image; bool shown; } arrows[] = {
            {0x451, arrowIncomeTop + 4, 0x140155cb, false},
            {0x452, arrowIncomeTop + 4 + rowStep_ * 8, 0x140155cc, incomeRows_ >= 10},
            {0x453, arrowExpenseTop + 4, 0x140155cb, false},
            {0x454, arrowExpenseTop + 4 + rowStep_ * 8, 0x140155cc, expenseRows_ >= 10},
        };
        for (const auto& arrow : arrows) {
            AddImageButton(window, arrow.id, width_ - 0x21, arrow.y, arrow.image, false);
            if (cIGZWin* button = window->GetChildWindowFromIDRecursive(arrow.id)) {
                button->SetNotificationTarget(window);
                if (!arrow.shown) button->SetFlag(1, false);
            }
        }
    }
    if (width_ > 0 && height_ > 0 && window->GetW() == 0) window->SetSize(width_, height_);
    for (const SliderInfo& info : sliders_)
        if (cIGZWin* slider = window->GetChildWindowFromIDRecursive(info.id)) slider->SetNotificationTarget(window);
    initialized_ = true;
    return true;
}

// UpdateValues (i386 0x00271a46), the department path: the rows again from the scroll offsets (+0xec income, +0xf0
//   expenses) after a funding change or a scroll arrow. The slots on_init made (three texts each, ids from 0xabcde03; the
//   expense slots also a funding bar 0xabce200 + n, a slider 0xabcdf00 + n and a button 0xabce600 + n) are re-captioned
//   with the rows from the offset on, a department row in the department font, an item row in the item font; rows above
//   the offset and past the ninth slot only add to the total. A side of more than eight rows that now shows fewer than
//   nine blanks the next slot. The slider list is rebuilt with the rows shown.
void cSC4SubBudgetWindow::UpdateValues(cIGZWin* window) {
    if (updating_ || !body_ || !window) return;
    updating_ = true;
    std::vector<cSC4DepartmentBudget*> departments;
    Departments(departments);
    const std::uint32_t shownIncome = std::min<std::uint32_t>(incomeRows_, kMaxRows);
    sliders_.clear();
    auto caption = [&](std::uint32_t id, const cIGZString& text) {
        if (cIGZWin* child = body_->GetChildWindowFromIDRecursive(id)) child->SetCaption(text);
    };
    auto font = [&](std::uint32_t id, std::uint32_t style) {   // the text's font again (interface 0x212cdc1f vslot 0x48)
        void* raw = nullptr;
        if (!body_->GetChildAsRecursive(id, gz::kGZIID_cIGZWinText, &raw) || !raw) return;
        if (gz::IWinFont* face = g_host->Font(style)) static_cast<gz::cIGZWinText*>(raw)->SetFont(face);
        static_cast<gz::cIGZWinText*>(raw)->Release();
    };
    auto show = [&](cIGZWin* parent, std::uint32_t id, bool on) {
        if (cIGZWin* child = parent->GetChildWindowFromIDRecursive(id)) child->SetFlag(1, on);
    };
    std::vector<cSC4LineItem*> items;
    std::uint32_t textId = 0xabcde01;
    if (incomeRows_ != 0) {
        total_ = 0;
        textId = 0xabcde03;
        std::uint32_t shown = 0, ordinal = 0;
        for (cSC4DepartmentBudget* department : departments) {
            department->GetAllLineItems(items);
            bool rowDone = false;
            for (cSC4LineItem* item : items) {
                if (item->GetType() != 1) continue;
                if (!rowDone) {
                    if (QuietPurpose(purpose_) && department->GetTotalIncome() == 0) {
                        rowDone = false;
                    } else {
                        if (ordinal < incomeScroll_ || shown > 8) {
                            total_ += department->GetTotalIncome() * multiplier_;
                        } else {
                            cRZString name, money;
                            department->GetDepartmentName(name);
                            caption(textId, name);
                            font(textId, kFontDepartment);
                            SC4MakeMoneyString(department->GetTotalIncome() * multiplier_, money);
                            total_ += department->GetTotalIncome() * multiplier_;
                            caption(textId + 1, money);
                            font(textId + 1, kFontDepartment);
                            show(body_, textId + 2, false);
                            textId += 3;
                            ++shown;
                        }
                        ++ordinal;
                        rowDone = true;
                    }
                }
                if (item->GetID() > 100 && item->GetIncome() == 0) break;
                // the income side counts every item in the ordinal and every listed item as shown, above the offset too
                // (the expense side below counts only what it shows: both as the original)
                if ((item->GetDisplayFlags() & 1) == 0) { ++ordinal; continue; }
                if (incomeScroll_ <= ordinal && shown < 9) {
                    cRZString name, money;
                    item->GetName(name);
                    caption(textId, name);
                    font(textId, kFontItem);
                    SC4MakeMoneyString(item->GetIncome() * multiplier_, money);
                    caption(textId + 1, money);
                    font(textId + 1, kFontItem);
                    char text[32];
                    std::snprintf(text, sizeof(text), "%d", static_cast<int>(item->GetSecondaryInfoField()));
                    caption(textId + 2, cRZString(text));
                    show(body_, textId + 2, (item->GetDisplayFlags() & 2) != 0);
                    textId += 3;
                }
                ++shown;
                ++ordinal;
            }
            ReleaseItems(items);
        }
        if ((shownIncome & 1) == 0) incomeTop_ += rowStep_;   // as the original (its on_init already stepped once)
        if (incomeRows_ > 8 && shown < 9) {
            for (std::uint32_t i = 0; i < 3; ++i) caption(textId + i, cRZString());
            textId += 3;
        }
        cRZString money;
        SC4MakeMoneyString(total_, money);
        caption(textId + 1, money);
        textId += 2;
    }
    if (expenseRows_ != 0) {
        textId += 2;
        total_ = 0;
        std::uint32_t sliderId = 0xabcdf00, bitmapId = 0xabce200, buttonId = 0xabce600;
        std::uint32_t shown = 0, ordinal = 0;
        for (cSC4DepartmentBudget* department : departments) {
            department->GetAllLineItems(items);
            bool rowDone = false;
            for (cSC4LineItem* item : items) {
                if (item->GetType() != 0) continue;
                if (!rowDone) {
                    if (QuietPurpose(purpose_) && department->GetIdealMonthlyFunding() == 0) {
                        rowDone = false;
                    } else {
                        if (ordinal < expenseScroll_ || shown > 8) {
                            total_ += department->GetTotalExpenses() * multiplier_;
                        } else {
                            cRZString name, money;
                            department->GetDepartmentName(name);
                            caption(textId, name);
                            font(textId, kFontDepartment);
                            SC4MakeMoneyString(department->GetTotalExpenses() * multiplier_, money);
                            total_ += department->GetTotalExpenses() * multiplier_;
                            caption(textId + 1, money);
                            font(textId + 1, kFontDepartment);
                            show(body_, textId + 2, false);
                            sliders_.push_back({sliderId, department, textId + 1, true});
                            textId += 3;
                            const float maximum = department->GetMaxAllowedFundingPercentage();
                            void* raw = nullptr;
                            if (window->GetChildAsRecursive(sliderId, gz::kGZIID_cIGZWinSlider, &raw) && raw) {
                                auto* slider = static_cast<gz::cIGZWinSlider*>(raw);
                                slider->SetMinimumValue(0);
                                slider->SetMaximumValue(static_cast<int>(maximum * kPercent));
                                slider->SetValue(static_cast<int>(kPercent * department->GetFundingPercentage()));
                                slider->Release();
                            }
                            const bool fixed = department->GetIsFixedFunding();
                            if (cIGZWin* bar = window->GetChildWindowFromIDRecursive(bitmapId)) {
                                bar->SetFlag(1, !fixed);
                                if (!fixed) {
                                    float barX = 11000.0f / (kPercent * maximum) + 260.0f - 12.0f;
                                    bar->GZWinMoveTo(static_cast<int>(barX), bar->GetArea().top);
                                }
                            }
                            show(window, sliderId, !fixed);
                            const std::uint32_t local = LocalPurposeOf(department->GetDepartmentID());
                            show(window, buttonId, local != 0 && department->HasLocallyFundedItemsByPurpose(local));
                            ++bitmapId; ++sliderId; ++buttonId;
                            ++shown;
                        }
                        ++ordinal;
                        rowDone = true;
                    }
                }
                if (item->GetID() > 100 && item->GetFullExpenses() == 0) break;
                if ((item->GetDisplayFlags() & 1) == 0) continue;
                if (expenseScroll_ <= ordinal && shown < 9) {
                    cRZString name, money;
                    item->GetName(name);
                    caption(textId, name);
                    font(textId, kFontItem);
                    SC4MakeMoneyString(item->GetCurrentExpenses() * multiplier_, money);
                    caption(textId + 1, money);
                    font(textId + 1, kFontItem);
                    char text[32];
                    std::snprintf(text, sizeof(text), "%d", static_cast<int>(item->GetSecondaryInfoField()));
                    caption(textId + 2, cRZString(text));
                    show(body_, textId + 2, (item->GetDisplayFlags() & 2) != 0);
                    sliders_.push_back({sliderId, department, textId + 1, false});
                    textId += 3;
                    for (std::uint32_t hidden : {bitmapId, sliderId, buttonId}) show(window, hidden, false);
                    ++bitmapId; ++sliderId; ++buttonId;
                    ++shown;
                }
                ++ordinal;
            }
            ReleaseItems(items);
        }
        if ((expenseRows_ & 1) == 0) expenseTop_ += rowStep_;   // as the original (it tests the row count's low bit here)
        if (expenseRows_ > 8 && shown < 9) {
            for (std::uint32_t i = 0; i < 3; ++i) caption(textId + i, cRZString());
            textId += 3;
            show(window, bitmapId, false);
            show(window, sliderId, false);
        }
        cRZString money;
        SC4MakeMoneyString(total_, money);
        caption(textId + 1, money);
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
    incomeRows_ = expenseRows_ = 0;
    incomeScroll_ = expenseScroll_ = 0;
    int arrowIncomeTop = -1, arrowExpenseTop = -1;
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
        arrowIncomeTop = incomeTop_;
        arrowExpenseTop = expenseTop_;
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
    if (arrowIncomeTop >= 0) {   // the scroll arrows: up hidden, down shown when a side has nine rows or more
        const struct { std::uint32_t id; int y; std::uint32_t image; bool shown; } arrows[] = {
            {0x451, arrowIncomeTop + 4, 0x140155cb, false},
            {0x452, arrowIncomeTop + 4 + rowStep_ * 8, 0x140155cc, incomeRows_ >= 9},
            {0x453, arrowExpenseTop + 4, 0x140155cb, false},
            {0x454, arrowExpenseTop + 4 + rowStep_ * 8, 0x140155cc, expenseRows_ >= 9},
        };
        for (const auto& arrow : arrows) {
            AddImageButton(window, arrow.id, width_ - 0x21, arrow.y, arrow.image, false);
            if (cIGZWin* button = window->GetChildWindowFromIDRecursive(arrow.id)) {
                button->SetNotificationTarget(window);
                if (!arrow.shown) button->SetFlag(1, false);
            }
        }
    }
}

// updateOrdinanceBudget (i386 0x002628da): the rows again from the scroll offsets (+0xec income, +0xf0 expenses). The
//   slots initOrdinanceBudget made (name + money texts from 0xabcde03, check button 300 + n, info button 500 + n) are bound
//   to the ordinances from the offset on (the slot list +0x74 rebuilt), the money shown and the check button set while the
//   ordinance is checked; rows above the offset and past the ninth slot only add to the total. A side of more than eight
//   rows that now shows fewer than nine blanks the next slot and hides its buttons, a full side shows its last slot's.
void cSC4SubBudgetWindow::UpdateOrdinanceBudget(cIGZWin* window) {
    cSC4OrdinanceSimulator* ordinances = g_host ? g_host->OrdinanceSimulator() : nullptr;
    if (!ordinances || !body_ || !window) return;
    // vslot 0x30: the available ordinances again
    if (std::uint32_t count = static_cast<std::uint32_t>(ordinanceIds_.size()); count)
        ordinances->GetOrdinanceAvailableArray(ordinanceIds_.data(), count);
    ordinanceRows_.clear();
    auto monthly = [&](std::uint32_t index) -> std::int64_t {   // vslot 0x14
        auto* simple = dynamic_cast<cSC4OrdinanceSimple*>(ordinances->GetOrdinanceByID(ordinanceIds_[index]));
        return simple ? simple->GetCurrentMonthlyIncome() : 0;
    };
    auto setText = [&](std::uint32_t id, const cIGZString& text) {
        if (cIGZWin* child = body_->GetChildWindowFromIDRecursive(id)) child->SetCaption(text);
    };
    auto setChecked = [&](std::uint32_t id, bool on) {   // interface 0x8810 vslot 0x24
        void* raw = nullptr;
        if (window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
            static_cast<gz::cIGZWinBtn*>(raw)->SetChecked(on);
            static_cast<gz::cIGZWinBtn*>(raw)->Release();
        }
    };
    auto show = [&](std::uint32_t id, bool on) {
        if (cIGZWin* child = window->GetChildWindowFromIDRecursive(id)) child->SetFlag(1, on);
    };
    std::uint32_t textId = 0xabcde01, checkId = 300, infoId = 500;
    for (int side = 0; side < 2; ++side) {
        const bool income = side == 0;
        const std::uint32_t rows = income ? incomeRows_ : expenseRows_;
        if (rows == 0) continue;
        if (income) textId = 0xabcde03;
        else textId += 2;
        const std::uint32_t scroll = income ? incomeScroll_ : expenseScroll_;
        int& top = income ? incomeTop_ : expenseTop_;
        std::int64_t total = 0;
        std::uint32_t shown = 0, ordinal = 0;
        for (std::uint32_t index = 0; index < ordinanceIds_.size(); ++index) {
            cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(ordinanceIds_[index]);
            if (!ordinance || ordinance->IsIncomeOrdinance() != income) continue;
            const bool on = index < ordinanceOn_.size() && ordinanceOn_[index].second;
            const std::int64_t money = (income ? monthly(index) : -monthly(index)) * multiplier_;
            if (ordinal < scroll || shown > 8) {
                if (on) total += money;
            } else {
                setText(textId, ordinance->GetName());
                cRZString text;
                if (on) {
                    SC4MakeMoneyString(money, text);
                    total += money;
                }
                setChecked(checkId, on);
                setText(textId + 1, text);
                textId += 2;
                ordinanceRows_.push_back(index);
                ++checkId;
                ++infoId;
                top += rowStep_;   // as the original (the field is not read again)
                ++shown;
            }
            ++ordinal;
        }
        if (rows > 8) {
            if (shown < 9) {
                setText(textId, cRZString());
                setText(textId + 1, cRZString());
                textId += 2;
                show(checkId, false);
                show(infoId, false);
                ++checkId;
                ++infoId;
            } else {
                show(checkId - 1, true);
                show(infoId - 1, true);
            }
        }
        cRZString text;
        SC4MakeMoneyString(total, text);
        setText(textId + 1, text);
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


// ================================================================== the single-building dialog
// (+0x24 a department id; budget-window-bodies.md section 5, budget.md section 5.1)

namespace {
constexpr std::uint32_t kIndivSegTop = 0x2bfeb0cf, kIndivSegHead = 0x2bfeb0cb, kIndivSegBody = 0x2bfeb0cc,
                        kIndivSegFoot = 0x2bfeb0cd, kIndivSegBottom = 0x2bfeb0ce;
constexpr std::uint32_t kKindHospital = 0xca565486, kKindSchool = 0xea5654b6, kKindPolice = 0x0a567baa,
                        kKindFire = 0xea567bc3, kKindPower = 0xca58e540;
constexpr std::uint32_t kIndivTextFirst = 0x0abcde06, kIndivSliderFirst = 0x0abce400, kIndivSecondOffset = 0x100,
                        kIndivImageFirst = 0x0abce200;
constexpr std::uint32_t kIndivRows = 9;
// binary32 (x87 rounding to nearest) of value * 100.0f, as the saved percentages are stored
float Percent(float fraction) { return static_cast<float>(static_cast<double>(fraction) * 100.0); }
// trunc(cost * fraction + 0.5) kept to its low 32 bits (the x87 product, then _ftol2)
std::int32_t RoundedCost(double cost, float fraction) {
    return static_cast<std::int32_t>(static_cast<std::int64_t>(cost * static_cast<double>(fraction) + 0.5));
}
}  // namespace

void cSC4SubBudgetWindow::ReleaseIndividualRestore() {
    for (IndividualRestore& record : individualRestore_)
        if (record.holder) record.holder->Release();
    individualRestore_.clear();
}

// SetupWindowIndiv (i386 0x00265c58): top strip, the head + a body strip per two rows + the foot when there are rows,
// the bottom; the dialog sized to them and centred in its parent.
void cSC4SubBudgetWindow::SetupWindowIndiv(cIGZWin* window, std::uint32_t rows) {
    height_ += GetBudgetSegment(kIndivSegTop, 0, height_, body_);
    if (rows != 0) {
        height_ += GetBudgetSegment(kIndivSegHead, 0, height_, body_);
        expenseTop_ = height_;
        for (std::uint32_t i = 0; i < rows >> 1; ++i) height_ += GetBudgetSegment(kIndivSegBody, 0, height_, body_);
        height_ += GetBudgetSegment(kIndivSegFoot, 0, height_, body_);
    }
    height_ += GetBudgetSegment(kIndivSegBottom, 0, height_, body_);
    window->SetSize(width_, height_);
    if (cIGZWin* parent = window->GetParentWin())
        window->GZWinMoveTo((parent->GetW() - window->GetW()) / 2, (parent->GetH() - window->GetH()) / 2);
    if (body_) body_->SetSize(width_, height_);
}

// add_fixed_text (i386 0x00261d18): a label of a fixed width (x .. x + width, one line of the font high).
void cSC4SubBudgetWindow::AddFixedText(cIGZWin* parent, std::uint32_t id, int x, int y, int width, const cIGZString& text, int align,
                                       std::uint32_t font, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (!parent || !g_host) return;
    gz::cIGZWinText* label = g_host->CreateLabel(id, text);
    if (!label) return;
    cIGZWin* win = label->AsIGZWin();
    win->SetID(id);
    label->SetTextColor(g_host->MakeColor(r, g, b));
    if (gz::IWinFont* face = g_host->Font(font)) {
        label->SetFont(face);
        win->SetArea(x, y, x + width, y + face->LineHeight(0, 0));
    } else {
        win->GZWinMoveTo(x, y);
    }
    label->SetAlignment(static_cast<std::uint32_t>(align));
    win->SetFlag(0x800, false);
    win->SetFlag(0x8000, false);
    parent->ChildAdd(win);
    label->Release();
}

// add_text_button (i386 0x00261990): a flat text button (the building's name) that notifies the dialog.
void cSC4SubBudgetWindow::AddTextButton(cIGZWin* parent, std::uint32_t id, int x, int y, int width, const cIGZString& text, int capStyle,
                                        std::uint32_t font, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (!parent || !g_host) return;
    gz::cIGZWinBtn* button = g_host->CreateButton(static_cast<std::uint32_t>(capStyle), id, text, nullptr);
    if (!button) return;
    cIGZWin* win = button->AsIGZWin();
    win->SetID(id);
    button->SetFontColor(0, g_host->MakeColor(r, g, b));
    button->SetFontColor(2, g_host->MakeColor(0, 0, 0));
    if (gz::IWinFont* face = g_host->Font(font)) {
        button->SetFontStyle(font);
        win->SetArea(x, y, x + width, y + face->LineHeight(0, 0));
    } else {
        win->GZWinMoveTo(x, y);
    }
    win->SetCaption(text);
    win->SetFlag(0x800, false);
    win->SetFlag(0x8000, false);
    win->SetNotificationTarget(dialog_);
    parent->ChildAdd(win);
    button->Release();
}

// SetOccupantFundingLevel (i386 0x0026364e): one building's funding (percent) through its service's simulator; the second
// item (a hospital's or school's second budget) only for those two.
bool cSC4SubBudgetWindow::SetOccupantFundingLevel(cISCPropertyHolder* holder, bool secondary, float percent) {
    if (!holder || !g_host) return false;
    if (secondary && kind_ != kKindHospital && kind_ != kKindSchool) return false;
    return g_host->SetBuildingFunding(holder, kind_, secondary, percent * kFraction);
}

// initIndividualBudget (i386 0x002666ba): the department's service kind and titles, the list's count, the rows (at most 9),
// the headers, the total, OK / Cancel and the scroll arrows; then the first refresh.
void cSC4SubBudgetWindow::InitIndividualBudget(cIGZWin* window) {
    kind_ = secondKind_ = 0;
    cRZString title, column2;
    switch (individual_) {
        case 0x4a538cc6: case 0x09188f4c:
            kind_ = kKindSchool; secondKind_ = 0x4a5654ba; title = Text(0x6c04a35e); column2 = Text(0x0c04a37a); break;
        case 0xaa538cb3: case 0x09188f42:
            kind_ = kKindHospital; secondKind_ = 0xea56549e; title = Text(0x4c04a355); column2 = Text(0x6c04a36e); break;
        case 0x28f55a9f: kind_ = kKindFire; title = Text(0x4c04a33a); break;
        case 0x8910bc8a: kind_ = kKindPower; title = Text(0x2c04a366); break;
        case 0xa2963983: case 0xa2963984: kind_ = kKindPolice; title = Text(0x8c04a34b); break;
        default: break;
    }
    individualTitle_ = title;
    secondColumn_ = column2;
    std::vector<cISCPropertyHolder*> buildings;
    g_host->LocallyFundedBuildings(individual_, kind_, buildings);
    expenseRows_ = static_cast<std::uint32_t>(buildings.size());
    for (cISCPropertyHolder* holder : buildings)
        if (holder) holder->Release();
    const float maximum = g_host->DepartmentMaxFunding(individual_) * kPercent;
    float barX = 9000.0f / maximum - 12.0f;
    if (barX <= 0.0f) barX = 0.0f;
    const int bar = static_cast<int>(static_cast<std::uint32_t>(barX));
    if (cIGZBuffer* strip = g_host->UiImage(kIndivSegBody)) {
        width_ = static_cast<int>(strip->Width());
        rowStep_ = static_cast<int>(strip->Height()) / 2;
    }
    if (expenseRows_ == 0) {
        SetEmptyWindow(window, title, Text(0x4a5cb4b6));
        return;
    }
    const std::uint32_t rows = std::min(expenseRows_, kIndivRows);
    SetupWindowIndiv(window, rows);
    const int headY = expenseTop_ - rowStep_;
    AddText(body_, 0xabcde00, 0x14, 8, title, 0, 0x4a809916, 'D', 'U', 'f');
    AddText(body_, 0xabcde01, 0x15, headY, Text(0xac04a30f), 0, kFontHeader, 'D', 'U', 'f');
    AddFixedText(body_, 0xabcde02, 0x96, headY, 0xbe, Text(0x4c04a321), 6, kFontHeader, 'D', 'U', 'f');
    AddFixedText(body_, 0xabcde03, 0xff, headY, 0xbe, column2, 6, kFontHeader, 'D', 'U', 'f');
    AddFixedText(body_, 0xabcde04, 0x15e, headY, 0xdc, Text(0xcc04a330), 6, kFontHeader, 'D', 'U', 'f');
    AddFixedText(body_, 0xabcde05, 0x1d6, headY, 0x87, expenseHeader_, 6, kFontHeader, 'D', 'U', 'f');   // +0xd4
    total_ = 0;
    std::uint32_t text = kIndivTextFirst, image = kIndivImageFirst;
    int y = expenseTop_;
    const int maxValue = static_cast<int>(maximum);
    for (std::uint32_t row = 0; row < rows; ++row) {
        AddTextButton(window, text, 0x15, y, 0xb1, cRZString(), 1, 0xac0c7170, 0x44, 0x55, 0x66);
        AddBitmap(kImageFundingBar, bar + 200, y, window, image);
        AddSlider(window, kIndivSliderFirst + row, 200, y, 0x5a, 1, 0, maxValue, 0);
        AddBitmap(kImageFundingBar, bar + 0x131, y, window, image + 1);
        AddSlider(window, kIndivSliderFirst + kIndivSecondOffset + row, 0x131, y, 0x5a, 1, 0, maxValue, 0);
        AddFixedText(body_, text + 1, 400, y, 0x78, cRZString(), 6, kFontItem, 'D', 'U', 'f');
        AddFixedText(body_, text + 2, 0x208, y, 0x55, cRZString(), 6, kFontItem, 'D', 'U', 'f');
        text += 3;
        image += 2;
        y += rowStep_;
    }
    if ((rows & 1) == 0) y += rowStep_;
    AddText(body_, text, 0x159, y, totalCaption_, 0, kFontHeader, 'D', 'U', 'f');
    AddFixedText(body_, text + 1, 0x208, y, 0x55, cRZString(), 6, kFontHeader, 'w', 0, 0);
    AddImageButton(window, kButtonOk, 0xe, height_ - 0x28, 0x144161eb, true);
    AddImageButton(window, kButtonCancel, width_ - 0xc3, height_ - 0x28, 0x144161eb, true);
    for (std::uint32_t id : {kButtonOk, kButtonCancel}) {
        if (cIGZWin* button = window->GetChildWindowFromIDRecursive(id)) {
            button->SetSize(0xb4, 0x1e);
            button->SetNotificationTarget(window);
            button->SetCaption(id == kButtonOk ? cRZString(cSLString(0x0a554ae8, 0x2a6cd8fd, true))
                                               : cRZString(cSLString(0x6a231eaa, 0x6a56b0bb, true)));   // window slot 0x128
        }
        if (cIGZWin* box = window->GetChildWindowFromIDRecursive(id + 0x100)) box->SetSize(0xb4, 0x1e);
    }
    AddImageButton(window, 0x453, width_ - 0x21, expenseTop_ + 4, 0x140155cb, false);
    AddImageButton(window, 0x454, width_ - 0x21, expenseTop_ + 4 + rowStep_ * 8, 0x140155cc, false);
    for (std::uint32_t id : {0x453u, 0x454u}) {
        cIGZWin* arrow = window->GetChildWindowFromIDRecursive(id);
        if (!arrow) continue;
        arrow->SetNotificationTarget(window);
        if (id == 0x453 || expenseRows_ < 10) arrow->SetFlag(1, false);   // up starts hidden; down only with more rows than fit
    }
    UpdateIndividualBudget(window, true);
}

// updateIndividualBudget (i386 0x002646a2): a fresh enumeration; every building contributes to the total and (rebuild) to the
// cancel snapshot; the rows from +0x120 on (at most min(count, 9)) show its name, service text, cost and sliders.
void cSC4SubBudgetWindow::UpdateIndividualBudget(cIGZWin* window, bool rebuild) {
    if (!g_host || !window) return;
    std::vector<cISCPropertyHolder*> buildings;
    g_host->LocallyFundedBuildings(individual_, kind_, buildings);
    listCount_ = static_cast<std::uint32_t>(buildings.size());
    total_ = 0;
    const std::uint32_t quota = std::min(listCount_, kIndivRows);
    if (rebuild) ReleaseIndividualRestore();
    rowHolders_.clear();
    cRZString serviceText;   // carried from one building to the next when a branch leaves it alone (the original's quirk)
    std::uint32_t drawn = 0, text = kIndivTextFirst, image = kIndivImageFirst;
    for (std::uint32_t ordinal = 0; ordinal < buildings.size(); ++ordinal) {
        cISCPropertyHolder* holder = buildings[ordinal];
        std::int32_t amount = 0;
        float primary = 0.0f, secondary = 0.0f;
        ISubBudgetWindowHost::BuildingService q;
        char line[64];
        if (holder && g_host->QueryBuildingService(holder, kind_, q)) {
            switch (kind_) {
                case kKindHospital: case kKindSchool:
                    if (q.residential) {
                        std::snprintf(line, sizeof line, "%d/%d", q.served, q.capacity);
                        serviceText = cRZString(line);
                        primary = Percent(q.staffFraction);
                        secondary = Percent(q.vehicleFraction);
                        amount = static_cast<std::int32_t>(static_cast<std::uint32_t>(RoundedCost(static_cast<double>(q.vehicleCost), q.vehicleFraction)) +
                                                           static_cast<std::uint32_t>(RoundedCost(static_cast<double>(q.staffCost), q.staffFraction)));
                    }
                    break;
                case kKindPolice: case kKindFire:
                    std::snprintf(line, sizeof line, "%2.0f%%", static_cast<double>(q.efficiency * 100.0f));
                    serviceText = cRZString(line);
                    primary = Percent(q.funding);
                    amount = RoundedCost(static_cast<double>(q.baseCost), q.funding);
                    break;
                case kKindPower:
                    std::snprintf(line, sizeof line, "%d/%d", q.numerator, q.denominator);
                    serviceText = cRZString(line);
                    if (q.powerFunding) {
                        primary = Percent(q.funding);
                        if (q.powerCost) amount = RoundedCost(static_cast<double>(q.powerBaseCost), q.funding);
                    }
                    break;
                default: break;
            }
        }
        if (period_ == 1) amount = static_cast<std::int32_t>(static_cast<std::uint32_t>(amount) * 12u);
        total_ += amount;
        if (rebuild && holder) {
            holder->AddRef();
            individualRestore_.push_back(IndividualRestore{holder, primary, secondary});
        }
        if (ordinal < expenseScroll_ || drawn >= quota) continue;
        // the row: name, sliders (the second only when the building has the second budget item), service text, cost
        cRZString name;
        if (!holder || !g_host->BuildingName(holder, name)) name = cRZString("Error: No name");
        const bool second = secondKind_ != 0 && g_host->HasBudgetItem(holder, secondKind_);
        const std::uint32_t slider = kIndivSliderFirst + drawn, slider2 = slider + kIndivSecondOffset;
        auto show = [&](std::uint32_t id, bool on) {
            if (cIGZWin* w = window->GetChildWindowFromIDRecursive(id)) {
                if (on) w->ShowWindow();
                else w->HideWindow();
            }
        };
        show(text, true);
        show(slider, true);
        show(image, true);
        show(slider2, second);
        show(image + 1, second);
        rowHolders_.push_back({text, holder});
        rowHolders_.push_back({slider, holder});
        rowHolders_.push_back({slider2, holder});
        auto setSlider = [&](std::uint32_t id, float value) {
            void* raw = nullptr;
            if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinSlider, &raw) || !raw) return;
            static_cast<gz::cIGZWinSlider*>(raw)->SetValue(static_cast<int>(value));
            static_cast<gz::cIGZWinSlider*>(raw)->Release();
        };
        setSlider(slider, primary);
        if (secondKind_ != 0) setSlider(slider2, secondary);
        auto caption = [&](std::uint32_t id, const cIGZString& value) {
            void* raw = nullptr;
            if (window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinText, &raw) && raw) {
                static_cast<gz::cIGZWinText*>(raw)->SetCaption(value);
                static_cast<gz::cIGZWinText*>(raw)->Release();
            } else if (cIGZWin* w = window->GetChildWindowFromIDRecursive(id)) {
                w->SetCaption(value);   // the name's text button (window slot 0x128)
            }
        };
        caption(text, name);
        caption(text + 1, serviceText);
        cRZString money;
        g_host->MoneyString(static_cast<std::int64_t>(amount), money);
        caption(text + 2, money);
        ++drawn;
        text += 3;
        image += 2;
    }
    // the rows left over hide (their buildings are kept)
    for (std::uint32_t row = drawn; row < quota; ++row) {
        for (std::uint32_t id : {text, text + 1, text + 2, kIndivSliderFirst + row, kIndivSliderFirst + kIndivSecondOffset + row, image, image + 1})
            if (cIGZWin* w = window->GetChildWindowFromIDRecursive(id)) w->HideWindow();
        text += 3;
        image += 2;
    }
    cRZString money;
    g_host->MoneyString(total_, money);
    void* raw = nullptr;
    if (window->GetChildAsRecursive(text + 1, gz::kGZIID_cIGZWinText, &raw) && raw) {
        static_cast<gz::cIGZWinText*>(raw)->SetCaption(money);
        static_cast<gz::cIGZWinText*>(raw)->Release();
    }
    for (cISCPropertyHolder* holder : buildings)
        if (holder) holder->Release();
}


// DoIndividualBudgetWindow (i386 0x00269f00): a second dialog (purpose 0, this period) over the department's buildings, run
// modally; afterwards it is destroyed.
void cSC4SubBudgetWindow::DoIndividualBudgetWindow(std::uint32_t department) {
    if (child_ || !g_host) return;
    child_ = new cSC4SubBudgetWindow();
    child_->AddRef();
    cIGZWin* window = nullptr;
    if (child_->Create(&window, 0, period_, department) && window) {
        window->ShowWindow();
        window->SetFlag(0x800, true);
        window->PullToFront();
        window->SetFlag(0x800, false);
        window->SetFlag(0x100, true);
        gz::cIGZWinGen* gen = nullptr;
        if (window->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen)) && gen) {
            gen->SetWinGenFlag(0x1000, true);
            gen->SetWinGenFlag(0x2000, true);
            gen->SetWinGenFlag(0x4000, true);
            gen->Release();
        }
        if (cIGZWinMgr* manager = g_host->WindowManager()) manager->RunPopup(window);
        window->Release();
    }
    child_->Destroy();
    child_->Release();
    child_ = nullptr;
}

}  // namespace sc4
