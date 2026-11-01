#include "sc4/DialogWinProcs.h"

#include "gz/GZCOMDirector.h"
#include "rz/cRZString.h"

namespace sc4 {

namespace {
ICreditsView* g_creditsView = nullptr;
constexpr std::uint32_t kCreditsInstance = 0xca551016, kCreditsWindowId = 0x0a592004, kCreditsCloseButton = 0xea5920fd;
constexpr std::uint32_t kSenderButton = 0x287259f6;
constexpr std::uint32_t kEndDialogCode = 0xf;

cIGZUIScriptService* UIScript() {
    cIGZFrameWork* framework = RZGetFramework();
    cIGZUIScriptService* service = nullptr;
    if (framework) framework->GetSystemService(kServiceUIScript, kIIDUIScript, reinterpret_cast<void**>(&service));
    return service;
}
}  // namespace

// ---- the script window factory ----------------------------------------------------------------------------

cIGZWin* SC4CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) {
    cIGZUIScriptService* service = UIScript();
    cIGZWin* window = service ? service->CreateWindowFromScript(key, parent, id) : nullptr;
    if (service) service->Release();
    return window;
}

bool SC4CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) {
    cIGZUIScriptService* service = UIScript();
    const bool ok = service && service->CreateWindowFromScriptAs(key, parent, id, iid, out);
    if (service) service->Release();
    return ok;
}

bool SC4CreateWindowFromScript(std::uint32_t instance, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) {
    return SC4CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroup, instance}, parent, id, iid, out);
}

DialogServices& GetDialogServices() {
    static DialogServices services;
    return services;
}

// ---- cSC4DialogWinProcBase ----------------------------------------------------------------------------

bool cSC4DialogWinProcBase::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != gz::kGZIID_cIGZWinProc && iid != 1) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4DialogWinProcBase::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

std::uint32_t cSC4DialogWinProcBase::Display(cIGZWin* parent) {
    AddRef();
    std::uint32_t result = 0;
    const cGZPersistResourceKey key{0, kUIScriptGroup, resourceInstance_};
    cIGZWin* window = SC4CreateWindowFromScript(key, parent, windowId_);
    if (window) {
        gz::cIGZWinGen* gen = nullptr;
        if (window->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen))) {
            cIGZWinMgr* manager = window->GetWindowManager();
            if (GetDialogServices().busyCursor) GetDialogServices().busyCursor(true);
            gen->SetWinProc(this);
            if (GetDialogServices().busyCursor) GetDialogServices().busyCursor(false);
            cSLWinLocationSaver* saver = GetDialogServices().locationSaver ? GetDialogServices().locationSaver() : nullptr;
            if (saver) saver->RestoreWindowLocation(window, true);
            manager->GZSetFocus(manager->GetMainWindow());
            result = static_cast<std::uint32_t>(manager->RunPopup(window));       // the modal loop
            if (saver) saver->SaveWindowLocation(window);
        }
        window->GetWindowManager()->DestroyWindow(window);
        window->Release();
        if (gen) gen->Release();
    }
    Release();
    return result;
}

bool cSC4DialogWinProcBase::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    switch (message.type) {
        case gz::kWinProcInit:
            window_ = window;
            GZOnAttach();
            return true;
        case gz::kWinProcShutdown:
            window_ = nullptr;
            GZOnDetach();
            return true;
        case kGZMsgCommand:
            if (message.param1 == kSenderButton) GZOnButtonActivate(static_cast<std::uint32_t>(message.param2));
            return false;
        // message 0x11: the window got the focus (this case was named KillFocus while the enum names were swapped)
        case kGZMsgSetFocus:
            if (!window->GetFlag(0x1000) && defaultFocusId_ != 0) {
                if (cIGZWin* control = window->GetChildWindowFromIDRecursive(defaultFocusId_)) {
                    control->GetWindowManager()->GZSetFocus(control);
                    return true;
                }
            }
            return false;
        default:
            return false;
    }
}

bool cSC4DialogWinProcBase::GZOnButtonActivate(std::uint32_t id) {
    // 0x8a5b9959 is the OK button and 0xca5b9966 Cancel (Mac i386 0x002eb4a8: vslot 0x30 / 0x2c; the dialog scripts
    // caption them "OK" / "Load Region" and "Cancel").
    if (id == 0x8a5b9959) GZOnOK();
    else if (id == 0xca5b9966) GZOnCancel();
    else return false;
    return true;
}

void cSC4DialogWinProcBase::EndDialog(std::uint32_t result) { window_->GetWindowManager()->EndDialog(kEndDialogCode, window_, result); }
void cSC4DialogWinProcBase::GZOnCancel() { window_->GetWindowManager()->EndDialog(kEndDialogCode, window_, 0); }
void cSC4DialogWinProcBase::GZOnOK() { window_->GetWindowManager()->EndDialog(kEndDialogCode, window_, 1); }

gz::cIGZWinBtn* cSC4DialogWinProcBase::GetBtnControl(std::uint32_t id) {
    cIGZWin* control = window_->GetChildWindowFromIDRecursive(id);
    gz::cIGZWinBtn* button = nullptr;
    if (control && control->QueryInterface(gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&button)) && button) button->Release();   // a borrowed pointer
    return button;
}

void cSC4DialogWinProcBase::TranslateTokensInCaption(std::uint32_t id) {
    cIGZWin* control = window_->GetChildWindowFromIDRecursive(id);
    if (id == 0 || !control) return;
    cRZString in(control->GetCaption().Data()), out;
    if (GetDialogServices().detokenize) GetDialogServices().detokenize(in, out);
    control->SetCaption(out);
}

// ---- cSC4CreditsWinProc -------------------------------------------------------------------------------

void SetCreditsView(ICreditsView* view) { g_creditsView = view; }

cSC4CreditsWinProc::~cSC4CreditsWinProc() {
    if (htmlView_) htmlView_->Release();
    if (dialog_) dialog_->Release();
}

bool cSC4CreditsWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != gz::kGZIID_cIGZWinProc) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4CreditsWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4CreditsWinProc::ShowDialog(eCreditsWindowType type) {
    if (type != kStandard) return false;
    if (dialog_) {
        gz::cIGZWinGen* old = dialog_;
        dialog_ = nullptr;
        old->Release();
    }
    const cGZPersistResourceKey key{0, kUIScriptGroup, kCreditsInstance};
    if (!SC4CreateWindowFromScript(key, nullptr, kCreditsWindowId, gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&dialog_))) return false;
    InitDialog();
    dialog_->SetWinProc(this);
    return true;
}

// Modal: the game is paused (animation and view, not graphics or simulator) while the dialog shows.
bool cSC4CreditsWinProc::DoModalWin() {
    cIGZWin* window = dialog_->AsIGZWin();
    if (window) window->AddRef();
    cSLWinLocationSaver* saver = GetDialogServices().locationSaver ? GetDialogServices().locationSaver() : nullptr;
    if (saver) saver->RestoreWindowLocation(window, true);
    window->GetWindowManager()->RunPopup(window);
    if (saver) saver->SaveWindowLocation(window);
    if (window) window->Release();
    return true;
}

void cSC4CreditsWinProc::InitDialog() {
    if (!dialog_ || htmlView_ || !g_creditsView) return;
    g_creditsView->Build(dialog_->AsIGZWin(), htmlView_);
}

bool cSC4CreditsWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (dialog_->AsIGZWin() != window) return false;
    if (window->IsVisible() && message.type == kGZMsgCommand) {
        if (message.param1 == kSenderButton && static_cast<std::int32_t>(message.param2) == -0x15a6df83) EndDialog();
        return true;
    }
    return false;
}

void cSC4CreditsWinProc::EndDialog() {
    AddRef();
    if (dialog_) {
        cIGZWin* window = dialog_->AsIGZWin();
        cIGZWinMgr* manager = window->GetWindowManager();
        if (manager) {
            if (window->GetFlag(0x1000)) manager->EndDialog(kEndDialogCode, window, 1);
            manager->DestroyWindow(window);
        }
        gz::cIGZWinGen* dialog = dialog_;
        dialog_ = nullptr;
        dialog->Release();
    }
    if (htmlView_) {
        cIGZUnknown* view = htmlView_;
        htmlView_ = nullptr;
        view->Release();
    }
    Release();
}

}  // namespace sc4
