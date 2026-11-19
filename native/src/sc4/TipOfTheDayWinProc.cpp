#include "sc4/TipOfTheDayWinProc.h"

#include <cstdio>
#include <cstring>

#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinHTML.h"
#include "gz/WinInterfaces.h"

namespace sc4 {
namespace {

ITipOfTheDayHost* g_host = nullptr;

constexpr std::uint32_t kUIScriptGroup = 0x96a006b0;
constexpr std::uint32_t kWindowGenIid = 0x5386d516;
constexpr std::uint32_t kCommandButton = 0x287259f6;
constexpr std::uint32_t kMsgInitDialog = 0xa2bf8ad5, kMsgDialogShown = 0xa2bf8ad6;
constexpr std::uint32_t kWinFlagModalDialog = 0x1000, kWinFlagZOrderMovable = 0x800;
constexpr std::uint32_t kCloseButtons[] = {0x42b7c353, 0x42b7c356, 0xaa3be170};

// the run's statics (0x00b4dfd0..0x00b4dfd3)
bool g_shown = false, g_found = false, g_presenceMode = false, g_scanned = false;

template <class T>
void Drop(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The collector (0x0078cc70) and its callback (0x0078cb40): in presence mode only "is there one" matters (the walk still
// visits every key but loads no more exemplars once one is found).
void Collect(std::vector<cGZPersistResourceKey>* tips) {
    if (!g_host) return;
    if (tips) tips->clear();
    g_host->ForEachExemplarKey([tips](const cGZPersistResourceKey& key) {
        if (g_presenceMode && g_found) return;
        std::uint32_t type = 0;
        if (!g_host->ExemplarType(key, type) || type != kTipExemplarType) return;
        if (g_presenceMode) g_found = true;
        else if (tips) tips->push_back(key);
    });
}

}  // namespace

ITipOfTheDayHost* TipOfTheDayHost() { return g_host; }
void SetTipOfTheDayHost(ITipOfTheDayHost* host) { g_host = host; }

cSC4TipOfTheDayWinProc::~cSC4TipOfTheDayWinProc() {
    Drop(html_);
    Drop(window_);
}

bool cSC4TipOfTheDayWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc || iid == 1) {
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4TipOfTheDayWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

cIGZWin* cSC4TipOfTheDayWinProc::Window() const { return window_ ? window_->AsIGZWin() : nullptr; }

bool cSC4TipOfTheDayWinProc::HasBeenShownAlready() { return g_shown; }

bool cSC4TipOfTheDayWinProc::EntriesPresent() {
    if (g_found || g_scanned) return g_found;
    g_scanned = true;
    g_presenceMode = true;
    Collect(nullptr);
    g_presenceMode = false;
    return g_found;
}

void cSC4TipOfTheDayWinProc::ResetForTest() { g_shown = g_found = g_presenceMode = g_scanned = false; }

bool cSC4TipOfTheDayWinProc::DoModelessWin(cSC4TipOfTheDayWinProc** out) {
    cIGZWin* parent = g_host ? g_host->DialogParent() : nullptr;
    if (!parent || parent->GetChildWindowFromIDRecursive(kTipWindowId)) return false;
    gz::cIGZWinGen* gen = nullptr;
    const cGZPersistResourceKey key{0, kUIScriptGroup, kTipScript};
    if (!g_host->CreateWindowFromScript(key, parent, kTipWindowId, kWindowGenIid, reinterpret_cast<void**>(&gen)) || !gen)
        return false;
    auto* proc = new cSC4TipOfTheDayWinProc();
    proc->AddRef();
    proc->window_ = gen;   // the script's reference becomes the proc's
    gen->AsIGZWin()->PullToFront();
    gen->AsIGZWin()->SetFlag(kWinFlagZOrderMovable, false);
    gen->SetWinProc(static_cast<gz::cIGZWinProc*>(proc));
    if (out) *out = proc;
    else proc->Release();   // the window's proc reference keeps it
    return true;
}

void cSC4TipOfTheDayWinProc::LoadAllExemplarData() { Collect(&tips_); }

void cSC4TipOfTheDayWinProc::InitDialog() {
    if (!window_) return;
    g_shown = true;
    cIGZWin* dialog = window_->AsIGZWin();
    if (cIGZWin* placeholder = dialog->GetChildWindowFromID(kTipPlaceholder)) {
        Drop(html_);
        html_ = g_host ? g_host->CreateHtmlControl() : nullptr;
        if (html_) {
            html_->AsIGZWin()->SetArea(placeholder->GetArea());
            html_->ClearFlag(0x40);
            html_->ClearFlag(4);
            html_->ClearFlag(8);   // no vertical scroll bar
            dialog->ChildAdd(html_->AsIGZWin());
        }
    }
    LoadAllExemplarData();
    if (std::uint8_t* preferences = g_host ? g_host->Preferences() : nullptr) {
        std::uint32_t last = 0;
        std::memcpy(&last, preferences + kTipPrefLastTip, 4);
        for (std::uint32_t i = 0; i < tips_.size(); ++i) {
            if (tips_[i].instance == last) {
                index_ = i;
                break;
            }
        }
    }
    GoToNextTip(true);   // the tip after the last one shown (the second one when none matched)
}

void cSC4TipOfTheDayWinProc::GoToNextTip(bool forward) {
    if (tips_.empty()) return;
    if (forward) index_ = index_ + 1 >= tips_.size() ? 0 : index_ + 1;
    else index_ = index_ == 0 ? static_cast<std::uint32_t>(tips_.size() - 1) : index_ - 1;
    GoToTip(tips_[index_]);
}

void cSC4TipOfTheDayWinProc::GoToTip(const cGZPersistResourceKey& key) {
    if (!g_host || !html_) return;
    cRZString text;
    if (!g_host->TipText(key, text)) return;
    char url[64];
    std::snprintf(url, sizeof url, "sc4://0x%08x_0x%08x_0x%08x.html", key.type, key.group, key.instance);
    html_->SetDocument(std::string(text.Data(), text.Strlen()), cRZString(url));
}

void cSC4TipOfTheDayWinProc::EndDialog() {
    if (!window_) return;
    AddRef();
    cIGZWin* dialog = window_->AsIGZWin();
    if (std::uint8_t* preferences = g_host ? g_host->Preferences() : nullptr) {
        gz::cIGZWinBtn* box = nullptr;
        if (cIGZWin* check = dialog->GetChildWindowFromIDRecursive(kTipCheckBox))
            check->QueryInterface(gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&box));
        if (box) {
            preferences[kTipPrefShowTips] = box->IsChecked();
            box->Release();
        }
        if (!tips_.empty()) std::memcpy(preferences + kTipPrefLastTip, &tips_[index_].instance, 4);
    }
    window_->SetWinProc(nullptr);
    if (dialog->GetFlag(kWinFlagModalDialog)) {
        if (cIGZWinMgr* manager = dialog->GetWindowManager()) manager->EndDialog(0x0f, dialog, 1);
    } else if (cIGZWin* parent = dialog->GetParentWin()) {
        parent->ChildDelete(dialog);
    }
    Drop(window_);
    Drop(html_);
    Release();
}

bool cSC4TipOfTheDayWinProc::DoCommand(cGZMessage& message) {
    if (static_cast<std::uint32_t>(message.param1) != kCommandButton) return false;
    const auto id = static_cast<std::uint32_t>(message.param2);
    for (std::uint32_t close : kCloseButtons) {
        if (id == close) {
            EndDialog();
            return true;
        }
    }
    if (id == kTipNext) {
        GoToNextTip(true);
        return true;
    }
    if (id == kTipPrevious) {
        GoToNextTip(false);
        return true;
    }
    return false;
}

bool cSC4TipOfTheDayWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (!window_ || window != Window()) return false;
    if (message.type == kGZMsgCommand) return DoCommand(message);
    if (message.type == kMsgInitDialog) {
        InitDialog();
        return true;
    }
    return message.type == kMsgDialogShown;
}

}  // namespace sc4
