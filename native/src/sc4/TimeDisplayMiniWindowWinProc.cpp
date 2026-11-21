#include "sc4/TimeDisplayMiniWindowWinProc.h"

#include "rz/cRZDate.h"
#include "rz/cRZString.h"
#include "sc4/DialogWinProcs.h"
#include "sc4/SmallWinProcs.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kInstance = 0xaa5e60d1, kWindowId = 0xca5e6261, kLabelId = 0x4a5e623e;
constexpr std::uint32_t kMessageShutdown = 0xa2bf8ad1, kCloseButton = 0x42b7c356, kSenderButton = 0x287259f6;
constexpr std::uint32_t kEndDialogCode = 0xf, kKeepFocusFlag = 0x1000, kFlag800 = 0x800;
constexpr std::size_t kPrefClockShown = 0x4ee;
constexpr std::uint32_t kSecondsPerDay = 0x15180;
TimeDisplayServices g_services;

// julian day of the epoch the counter is relative to (the original's DAT_100b73888: January 1st, 1970)
std::uint32_t EpochDay() { return cRZDate(1, 1, 1970).JulianDay(); }

std::uint32_t SecondsOfToday(std::uint32_t julianDay, const std::tm& local) {
    if (julianDay == 0) return 0;
    return static_cast<std::uint32_t>(local.tm_min * 60 + local.tm_sec + local.tm_hour * 3600) + (julianDay - EpochDay()) * kSecondsPerDay;
}
}  // namespace

TimeDisplayServices& GetTimeDisplayServices() { return g_services; }

cSC4TimeDisplayMiniWindowWinProc::cSC4TimeDisplayMiniWindowWinProc() {
    const std::time_t now = g_services.now ? g_services.now() : std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const cRZDate today(static_cast<std::uint32_t>(local.tm_mon + 1), static_cast<std::uint32_t>(local.tm_mday), static_cast<std::uint32_t>(local.tm_year + 1900));
    seconds_ = today.JulianDay() == 0 ? 0 : (today.JulianDay() - EpochDay()) * kSecondsPerDay;      // days only; the tick adds the time
    timer_.Start();
}

cSC4TimeDisplayMiniWindowWinProc::~cSC4TimeDisplayMiniWindowWinProc() {
    if (label_) label_->Release();
    if (dialog_) dialog_->Release();
}

bool cSC4TimeDisplayMiniWindowWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != 0x22e85d8e) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4TimeDisplayMiniWindowWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4TimeDisplayMiniWindowWinProc::Init() {
    if (!initialized_) initialized_ = true;
    return true;
}

bool cSC4TimeDisplayMiniWindowWinProc::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    cSLWinLocationSaver* saver = g_services.locationSaver ? g_services.locationSaver() : nullptr;
    if (saver && dialog_) saver->SaveWindowLocation(dialog_->AsIGZWin());
    if (std::uint8_t* preferences = g_services.preferences ? g_services.preferences() : nullptr) preferences[kPrefClockShown] = 0;
    gz::cIGZWinGen* dialog = dialog_;
    if (dialog) {
        dialog->AddRef();
        dialog_ = nullptr;
        dialog->Release();
    }
    if (label_) {
        gz::cIGZWinText* label = label_;
        label_ = nullptr;
        label->Release();
    }
    if (dialog) {
        dialog->SetWinProc(nullptr);
        dialog->Release();
    }
    return true;
}

bool cSC4TimeDisplayMiniWindowWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    const std::uint32_t type = message.type;
    if (type == kMessageShutdown) {
        Shutdown();
        return false;
    }
    if (type != kMessageSignpostTick) {
        if (type != kGZMsgCommand) return false;
        if (message.param1 == kSenderButton && message.param2 == kCloseButton) {
            EndDialog();
            return true;
        }
    }
    if (timer_.GetElapsedTimeU32() > 1) {
        timer_.Restart();
        UpdateTimeDisplay();
    }
    window->PullToFront();
    return false;
}

void cSC4TimeDisplayMiniWindowWinProc::EndDialog() {
    if (!dialog_) return;
    AddRef();
    cIGZWin* window = dialog_->AsIGZWin();
    cIGZWinMgr* manager = window->GetWindowManager();
    if (!window->GetFlag(kKeepFocusFlag)) window->GetParentWin()->ChildDelete(window);
    else manager->EndDialog(kEndDialogCode, window, 1);
    Shutdown();
    Release();
}

// Re-reads the local clock; when the second count moved the label shows the formatted hour and minute.
void cSC4TimeDisplayMiniWindowWinProc::UpdateTimeDisplay() {
    const std::time_t now = g_services.now ? g_services.now() : std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const cRZDate today(static_cast<std::uint32_t>(local.tm_mon + 1), static_cast<std::uint32_t>(local.tm_mday), static_cast<std::uint32_t>(local.tm_year + 1900));
    const std::uint32_t seconds = SecondsOfToday(today.JulianDay(), local);
    if (seconds_ == seconds) return;
    seconds_ = seconds;
    if (!label_ || !g_services.formatTime) return;
    const cRZString text(g_services.formatTime(seconds_ % kSecondsPerDay / 3600, seconds_ % 3600 / 60).c_str());
    label_->SetCaption(text);
}

void cSC4TimeDisplayMiniWindowWinProc::Attach(gz::cIGZWinGen* dialog) {
    if (dialog) dialog->AddRef();
    if (dialog_) dialog_->Release();
    dialog_ = dialog;
    if (label_) { label_->Release(); label_ = nullptr; }
    // win vslot 0x94 (i386) = GetChildAsRecursive: the label sits inside the dialog's frame, not directly under it
    if (dialog_) dialog_->AsIGZWin()->GetChildAsRecursive(kLabelId, gz::kGZIID_cIGZWinText, reinterpret_cast<void**>(&label_));
}

// Opens the clock under the application window, or raises it when it is already there.
bool cSC4TimeDisplayMiniWindowWinProc::DoModelessWin() {
    cIGZWin* app = g_services.appWindow ? g_services.appWindow() : nullptr;
    if (!app) return false;
    cIGZWin* existing = app->GetChildWindowFromIDRecursive(kWindowId);
    if (existing) {
        existing->ShowWindow();
        existing->SetFlag(kFlag800, true);
        existing->PullToFront();
        existing->SetFlag(kFlag800, false);
        return false;
    }
    gz::cIGZWinGen* dialog = nullptr;
    const cGZPersistResourceKey key{0, kUIScriptGroup, kInstance};
    if (!SC4CreateWindowFromScript(key, app, kWindowId, gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&dialog))) return false;
    auto* proc = new cSC4TimeDisplayMiniWindowWinProc;
    proc->AddRef();
    proc->Attach(dialog);
    cSLWinLocationSaver* saver = g_services.locationSaver ? g_services.locationSaver() : nullptr;
    cIGZWin* window = dialog->AsIGZWin();
    if (saver) saver->RestoreWindowLocation(window, true);
    window->SetFlag(kFlag800, true);
    window->PullToFront();
    dialog->SetWinProc(proc);
    proc->UpdateTimeDisplay();
    proc->Release();
    dialog->Release();
    return true;
}

}  // namespace sc4
