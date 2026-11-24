#include "sc4/PickSimWinProc.h"

#include <string>
#include <utility>

#include "gz/WinBMP.h"
#include "gz/WinCombo.h"
#include "gz/WinGen.h"
#include "gz/WinInterfaces.h"
#include "gz/WinTextEdit.h"
#include "rz/LanguageManager.h"
#include "rz/cRZKeyboard.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutoHelpers.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SLBuffer.h"
#include "sc4/SLString.h"

namespace sc4 {
namespace {
IPickSimHost* g_host = nullptr;

constexpr std::uint32_t kDialogScriptGroup = 0x96a006b0, kDialogScriptInstance = 0x0a243d80, kDialogId = 0x6a243d9e;
constexpr std::uint32_t kImportResult = 0x6a243d9f, kImportResultClose = 0xea57da77;
constexpr std::uint32_t kNameEdit = 0x8a243f1f, kBioEdit = 0x8a243f6e, kSignCombo = 0xaa243f5e, kSexCaption = 0xea243f57;
constexpr std::uint32_t kMoveIn = 0xca243e0c, kCancel = 0x0a243dd1, kImport = 0x2a243df9;
constexpr std::uint32_t kSortName = 0x4a243df1, kSortSign = 0x4a243de9, kSortSex = 0x0a243dda;
constexpr std::uint32_t kPrevGroup = 0x2a243dfa, kNextGroup = 0xca243e04, kImportedCount1 = 0x8a9ff482, kImportedCount2 = 0x8a9ff484;
constexpr std::uint32_t kPageButton = 0x12345000, kPageCurrent = 0x12345100, kHeadImage = 0x12340000, kPickedImage = 0x12340015;
constexpr std::uint32_t kPortraitGroup = 0x46a006b0, kSignStringGroup = 0x6a231eaa, kSexStringGroup = 0xea231e96;
constexpr std::uint32_t kFemaleString = 0x8a579224, kMaleString = 0x8a57920a;
constexpr std::uint32_t kRowsPerPage = 21, kPagesPerGroup = 6;
constexpr std::uint32_t kFlagVisible = 1, kFlagEnabled = 2;
constexpr std::uint32_t kSignStrings[12] = {0xea822e90, 0xca822e93, 0xca822e97, 0xaa822e9b, 0xca822ea4, 0xaa822ea8,
                                            0xca822eac, 0xca822eaf, 0xca822eb3, 0x6a822eb6, 0x6a822eb9, 0xea822ebd};

template <class T> T* ChildAs(cIGZWin* parent, std::uint32_t id, std::uint32_t iid) {
    void* raw = nullptr;
    return parent && parent->GetChildAsRecursive(id, iid, &raw) && raw ? static_cast<T*>(raw) : nullptr;
}
void Show(cIGZWin* window, bool on) { if (window) window->SetFlag(kFlagVisible, on); }
}  // namespace

IPickSimHost* PickSimHost() { return g_host; }
void SetPickSimHost(IPickSimHost* host) { g_host = host; }

const std::uint32_t cSC4PickSimWinProc::kHeadButtons[21] = {
    0xaa243e23, 0xaa243e2d, 0x6a243e35, 0xea243e3d, 0x0a243e45, 0x6a243e4e, 0x2a243e56, 0x8a243e6c, 0x2a243f85, 0xea243eb9, 0x6a243ec1,
    0x8a243ec9, 0xca243ed1, 0x8a243edb, 0xaa243ee2, 0x2a243eec, 0xca243ef3, 0x6a243efe, 0x8a243f08, 0xaa243f10, 0x6a243f17};

cSC4PickSimWinProc::cSC4PickSimWinProc(std::uint32_t slot) : slot_(slot) {}

cSC4PickSimWinProc::~cSC4PickSimWinProc() {
    if (tick_) tick_->Release();
    if (dialog_) dialog_->Release();
}

bool cSC4PickSimWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc) *out = static_cast<gz::cIGZWinProc*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1) *out = static_cast<IPickSimWinProc*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4PickSimWinProc::Release() {
    if (refs_ > 1) return --refs_;
    delete this;
    return 0;
}

cIGZWin* cSC4PickSimWinProc::Window() const { return dialog_ ? dialog_->AsIGZWin() : nullptr; }

cIGZWin* cSC4PickSimWinProc::Find(std::uint32_t id) const {
    cIGZWin* window = Window();
    cIGZWin* parent = window ? window->GetParentWin() : nullptr;
    return parent ? parent->GetChildWindowFromIDRecursive(id) : nullptr;
}

void cSC4PickSimWinProc::SetTemplatesForTest(const std::vector<tMySimTemplate>& templates) {
    templates_ = templates;
    count_ = static_cast<std::uint32_t>(templates_.size());
    order_.resize(count_);
    for (std::uint32_t i = 0; i < count_; ++i) order_[i] = i;
}

// 0x00281d7e: the modal loop ends and the dialog goes.
void cSC4PickSimWinProc::EndDialog() {
    cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr;
    cIGZWin* window = Window();
    if (modal_) {
        if (manager) manager->EndDialog(0xf, window, 0);
        modal_ = false;
    }
    if (cIGZWinMgr* own = window ? window->GetWindowManager() : nullptr) own->DestroyWindow(window);
}

// 0x00281dfc: "Move in" is enabled while the name has a character above a space.
bool cSC4PickSimWinProc::ValidateText() {
    if (!dialog_) return true;
    cIGZWin* window = Window();
    auto* edit = ChildAs<gz::cIGZWinTextEdit>(window, kNameEdit, gz::kGZIID_cIGZWinTextEdit);
    if (!edit) return true;
    cIGZWin* moveIn = window->GetChildWindowFromIDRecursive(kMoveIn);
    cRZString name;
    edit->GetText(name);
    bool visible = false;
    for (std::uint32_t i = 0; i < name.Strlen() && !visible; ++i)
        visible = static_cast<unsigned char>(name.ToChar()[i]) > 0x20;
    if (moveIn) moveIn->SetFlag(kFlagEnabled, visible);
    edit->Release();
    return visible;
}

// 0x00281f1c: the six page buttons of the current group, numbered from the group's first page; the current page shows its
// pressed twin; the group arrows; page buttons past the list are hidden (by the list's size alone, as compiled).
void cSC4PickSimWinProc::SetScrollButtons() {
    for (std::uint32_t k = 0; k < kPagesPerGroup; ++k) Show(Find(kPageCurrent + k), false);
    for (std::uint32_t k = 0; k < kPagesPerGroup; ++k) Show(Find(kPageButton + k), true);
    for (std::uint32_t k = 0; k < kPagesPerGroup; ++k) {
        const std::string number = std::to_string(k + pageGroup_ + 1);
        if (cIGZWin* button = Find(kPageButton + k)) button->SetCaption(cRZString(number.c_str()));
        if (page_ == k + pageGroup_) {
            Show(Find(kPageCurrent + k), true);
            Show(Find(kPageButton + k), false);
            if (cIGZWin* current = Find(kPageCurrent + k)) current->SetCaption(cRZString(number.c_str()));
        }
    }
    Show(Find(kPrevGroup), pageGroup_ != 0);
    Show(Find(kNextGroup), !(count_ <= pageGroup_ * kRowsPerPage + 0x69));
    static const std::pair<std::uint32_t, std::uint32_t> kLimits[] = {{0x6a, 5}, {0x55, 4}, {0x40, 3}, {0x2b, 2}, {0x16, 1}};
    for (const auto& limit : kLimits) {
        if (count_ >= limit.first) continue;
        Show(Find(kPageButton + limit.second), false);
        Show(Find(kPageCurrent + limit.second), false);
    }
}

// 0x00282868: the dialog keeps re-checking the name every tick (a message posted to itself).
void cSC4PickSimWinProc::TickSelf() {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    if (!tick_) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kTickMessage);
        tick_ = message;
    }
    server->GeneralMessagePostToTarget(tick_, static_cast<cIGZMessageTarget2*>(this));
}

bool cSC4PickSimWinProc::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kTickMessage) {
        ValidateText();
        TickSelf();
    }
    return true;
}

// The portrait of a template: an imported sim's file in the MySim directory (colour type 8), else the game's image.
void cSC4PickSimWinProc::ShowImage(std::uint32_t bmpId, const tMySimTemplate& sim) {
    cIGZWin* window = Window();
    cIGZWin* parent = window ? window->GetParentWin() : nullptr;
    auto* bmp = ChildAs<gz::cIGZWinBMP>(parent, bmpId, gz::kGZIID_cIGZWinBMP);
    if (sim.bitmapId == 0) {
        cRZString path;
        if (g_host) g_host->MySimDirectory(path);
        path.AppendString(sim.bitmapFile);
        cSLBuffer image(path, 8, false);
        if (bmp) bmp->SetBMP(image.GetBuffer());
    } else {
        cSLBuffer image(0, kPortraitGroup, sim.bitmapId, 1, false);
        if (bmp) bmp->SetBMP(image.GetBuffer());
    }
    if (bmp) bmp->Release();
}

// 0x00282c3c: the 21 rows from `top` (the last full page at most; all of them when fewer).
void cSC4PickSimWinProc::SetupPickSims(std::uint32_t top) {
    std::uint32_t end = count_;
    if (count_ - kRowsPerPage < top) {   // unsigned, as compiled: never true while the list is shorter than a page
        top_ = count_ - kRowsPerPage;
        top = top_;
    }
    if (count_ < kRowsPerPage) {
        top_ = 0;
        top = 0;
    } else {
        end = top + kRowsPerPage;
    }
    std::uint32_t image = kHeadImage;
    for (std::uint32_t row = top; row < end; ++row, ++image) {
        if (row < order_.size() && order_[row] < templates_.size()) ShowImage(image, templates_[order_[row]]);
        SetScrollButtons();
    }
}

// 0x00282ed2: a bubble sort of the rows by name (the language's comparison, case-insensitive).
void cSC4PickSimWinProc::SortByName(bool ascending) {
    for (int n = static_cast<int>(count_); n >= 1;) {
        int i = 1;
        for (; i != n; ++i) {
            const int order = cRZLanguageUtility::CompareStringsStatic(templates_[order_[i]].name, templates_[order_[i - 1]].name, false);
            if (ascending ? order < 0 : order > 0) std::swap(order_[i], order_[i - 1]);
        }
        n = i - 1;
    }
    SetupPickSims(top_);
}

// 0x00282ff0: grouped by sign, 0..11 (or 11..0, each group from the end of the list).
void cSC4PickSimWinProc::SortBySign(bool ascending) {
    std::uint32_t at = 0;
    if (ascending) {
        for (std::uint32_t sign = 0; sign != 12; ++sign)
            for (std::uint32_t i = 0; i < count_; ++i)
                if (templates_[i].zodiac == sign) order_[at++] = i;
    } else {
        for (int sign = 11; sign != -1; --sign)
            for (int i = static_cast<int>(count_) - 1; i >= 0; --i)
                if (templates_[static_cast<std::uint32_t>(i)].zodiac == sign) order_[at++] = static_cast<std::uint32_t>(i);
    }
    SetupPickSims(top_);
}

// 0x002830ae: the sims of the other sex than `male` first, then the rest.
void cSC4PickSimWinProc::SortByGender(bool male) {
    std::uint32_t at = 0;
    for (std::uint32_t i = 0; i < count_; ++i)
        if (male != templates_[i].male) order_[at++] = i;
    for (std::uint32_t i = 0; i < count_; ++i)
        if (templates_[i].male == male) order_[at++] = i;
    SetupPickSims(top_);
}

// 0x0028313e: the typed name, the bio and the sign go to the MySim agent simulator with the picked portrait; the dialog ends.
void cSC4PickSimWinProc::MoveIn() {
    cRZString name, bio;
    cIGZWin* window = Window();
    cIGZWin* parent = window ? window->GetParentWin() : nullptr;
    if (auto* edit = ChildAs<gz::cIGZWinTextEdit>(parent, kNameEdit, gz::kGZIID_cIGZWinTextEdit)) {
        name = cRZString(edit->GetText());
        edit->Release();
    }
    if (auto* edit = ChildAs<gz::cIGZWinTextEdit>(parent, kBioEdit, gz::kGZIID_cIGZWinTextEdit)) {
        bio = cRZString(edit->GetText());
        edit->Release();
    }
    std::uint8_t sign = 0;
    if (auto* combo = ChildAs<gz::cIGZWinCombo>(parent, kSignCombo, gz::kGZIID_cIGZWinCombo)) {
        sign = static_cast<std::uint8_t>(combo->GetSelection());
        combo->Release();
    }
    if (g_host) g_host->MoveIn(name, male_, sign, bio, bitmapId_, bitmapFile_, static_cast<std::uint8_t>(slot_));
    EndDialog();
}

// 0x002833fa: the row's portrait, name, sex and sign become the picked sim.
void cSC4PickSimWinProc::PickHead(std::uint32_t head) {
    const std::uint32_t row = top_ + head;
    if (row >= order_.size() || order_[row] >= templates_.size()) return;
    const tMySimTemplate& sim = templates_[order_[row]];
    ShowImage(kPickedImage, sim);
    bitmapId_ = sim.bitmapId;
    if (bitmapId_ == 0) bitmapFile_ = sim.bitmapFile;
    cIGZWin* window = Window();
    cIGZWin* parent = window ? window->GetParentWin() : nullptr;
    if (auto* edit = ChildAs<gz::cIGZWinTextEdit>(parent, kNameEdit, gz::kGZIID_cIGZWinTextEdit)) {
        edit->SetText(sim.name, false);
        edit->Release();
    }
    if (cIGZWin* sex = parent ? parent->GetChildWindowFromIDRecursive(kSexCaption) : nullptr)
        sex->SetCaption(cSLString(kSexStringGroup, sim.male ? kMaleString : kFemaleString, true));
    male_ = sim.male;
    if (auto* combo = ChildAs<gz::cIGZWinCombo>(parent, kSignCombo, gz::kGZIID_cIGZWinCombo)) {
        combo->SetSelection(sim.zodiac, false);
        combo->Release();
    }
    picked_ = row;
}

bool cSC4PickSimWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (!window || window != Window()) return false;
    if (message.type != kGZMsgCommand) {
        if (message.type == 0x9b1552d8u) ValidateText();
        return false;
    }
    if (message.param1 != gz::kWinCommandSender) return false;
    const std::uint32_t id = static_cast<std::uint32_t>(message.param2);
    const bool control = cRZKeyboard::IsKeyDownNowStatic(0x11);
    if (id >= kPageButton && id < kPageButton + kPagesPerGroup) {
        const std::uint32_t k = id - kPageButton;
        top_ = pageGroup_ * kRowsPerPage + k * kRowsPerPage;
        page_ = pageGroup_ + k;
        SetupPickSims(top_);
    } else if (id == kSortSex) {
        SortByGender(!control);
    } else if (id == kCancel) {
        EndDialog();
        return true;
    } else if (id == kSortName) {
        SortByName(!control);
    } else if (id == kPrevGroup) {
        if (pageGroup_ != 0) --pageGroup_;
        SetScrollButtons();
    } else if (id == kSortSign) {
        SortBySign(!control);
    } else if (id == kImport) {
        cIGZWinMgr* manager = window->GetWindowManager();
        templates_.clear();
        if (pool_) pool_->GetMySimTemplates(templates_);
        std::uint32_t imported = 0;
        if (pool_ && pool_->Import(imported)) {
            templates_.clear();
            pool_->GetMySimTemplates(templates_);
            count_ = static_cast<std::uint32_t>(templates_.size());
            cIGZWin* parent = window->GetParentWin();
            for (std::uint32_t captionId : {kImportedCount1, kImportedCount2}) {
                cIGZWin* caption = parent ? parent->GetChildWindowFromIDRecursive(captionId) : nullptr;
                if (!caption) continue;
                cRZString text(caption->GetCaption());
                SC4InsertNumberIntoString(text, imported);
                caption->SetCaption(text);
            }
            cIGZWin* result = parent ? parent->GetChildWindowFromIDRecursive(kImportResult) : nullptr;
            if (result && manager) {
                Show(result, true);
                cIGZWin* focus = manager->GZGetFocus();
                if (g_host) g_host->RestoreWindowLocation(result);
                manager->RunPopup(result);
                if (g_host) g_host->SaveWindowLocation(result);
                if (manager->IsWindowValid(focus)) manager->GZSetFocus(focus);
            }
            order_.resize(count_);
            for (std::uint32_t i = 0; i < count_; ++i) order_[i] = i;
        }
        SetScrollButtons();
    } else if (id == kMoveIn) {
        MoveIn();
        return true;
    } else if (id == kImportResultClose) {
        cIGZWin* parent = window->GetParentWin();
        cIGZWin* result = parent ? parent->GetChildWindowFromIDRecursive(kImportResult) : nullptr;
        if (cIGZWinMgr* manager = window->GetWindowManager()) manager->EndDialog(0xf, result, 0);
        Show(result, false);
    } else if (id == kNextGroup) {
        ++pageGroup_;
        SetScrollButtons();
    }
    for (std::uint32_t k = 0; k < 21; ++k)
        if (id == kHeadButtons[k]) PickHead(k);
    return false;
}

// 0x00283db8: builds the dialog, fills the sign list and the sims, then runs it modally with the game paused, where the
// player left it last time; the focus goes back afterwards.
bool cSC4PickSimWinProc::Init() {
    cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr;
    if (!manager) return false;
    cIGZWin* focus = manager->GZGetFocus();
    if (focus) focus->AddRef();
    if (dialog_) {
        dialog_->Release();
        dialog_ = nullptr;
    }
    dialog_ = g_host->CreateDialog(cGZPersistResourceKey{0, kDialogScriptGroup, kDialogScriptInstance}, kDialogId);
    if (!dialog_) {
        if (focus) focus->Release();
        return false;
    }
    dialog_->SetWinProc(static_cast<gz::cIGZWinProc*>(this));
    cIGZWin* window = Window();
    window->AddRef();
    cIGZWin* parent = window->GetParentWin();
    if (parent) parent->AddRef();
    Show(Find(kImportResult), false);
    if (auto* edit = ChildAs<gz::cIGZWinTextEdit>(parent, kNameEdit, gz::kGZIID_cIGZWinTextEdit)) {
        edit->SetMaximumTextToAllow(0x80);
        edit->Release();
    }
    if (auto* edit = ChildAs<gz::cIGZWinTextEdit>(parent, kBioEdit, gz::kGZIID_cIGZWinTextEdit)) {
        edit->SetMaximumTextToAllow(0x800);
        edit->Release();
    }
    if (cIGZWin* close = parent ? parent->GetChildWindowFromIDRecursive(kImportResultClose) : nullptr)
        close->SetNotificationTarget(window);
    if (auto* combo = ChildAs<gz::cIGZWinCombo>(parent, kSignCombo, gz::kGZIID_cIGZWinCombo)) {
        combo->RemoveAllStrings();
        for (std::uint32_t instance : kSignStrings) combo->InsertString(cSLString(kSignStringGroup, instance, true), -2);
        combo->Release();
    }
    pool_ = std::make_unique<cSC4MySimPool>();
    pool_->GetMySimTemplates(templates_);
    count_ = static_cast<std::uint32_t>(templates_.size());
    order_.resize(count_);
    for (std::uint32_t i = 0; i < count_; ++i) order_[i] = i;
    if (cIGZWin* import = window->GetChildWindowFromIDRecursive(kImport)) import->SetFlag(kFlagEnabled, pool_->FindTheSimsInstallation());
    SetupPickSims(top_);
    PickHead(0);
    SetScrollButtons();
    TickSelf();
    modal_ = true;
    {
        cSC4AutoGamePause pause(false, true, true, true);
        g_host->RestoreWindowLocation(window);
        manager->RunPopup(window);
        g_host->SaveWindowLocation(window);
    }
    if (manager->IsWindowValid(focus)) manager->GZSetFocus(focus);
    if (parent) parent->Release();
    window->Release();
    if (focus) focus->Release();
    return true;
}

// 0x00282b5e: ends the dialog if it still runs, destroys it and lets go of the templates and the pool.
bool cSC4PickSimWinProc::Shutdown() {
    AddRef();
    cIGZWin* window = Window();
    if (modal_ && g_host && g_host->WindowManager()) {
        g_host->WindowManager()->EndDialog(0xf, window, 0);
        modal_ = false;
    }
    if (cIGZWinMgr* own = window ? window->GetWindowManager() : nullptr) own->DestroyWindow(window);
    if (dialog_) {
        dialog_->Release();
        dialog_ = nullptr;
    }
    templates_.clear();
    pool_.reset();
    order_.clear();
    Release();
    return true;
}

}  // namespace sc4
