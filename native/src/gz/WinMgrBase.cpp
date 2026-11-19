#include "gz/WinMgrBase.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gz/Cursor.h"
#include "gz/WinMsgBox.h"

namespace gz {

namespace {
// Writes `value` into the low 32 bits of a message word (the original passes the word's address to a cIGZWin
// coordinate converter, which treats it as an int).
void SetLow32(std::uint64_t& word, int value) { word = (word & ~std::uint64_t(0xffffffffu)) | std::uint32_t(value); }
int Low32(std::uint64_t word) { return int(std::uint32_t(word)); }

// Converts the screen position held in a mouse message into the window's coordinates, in place.
void ScreenToWindowInMessage(cIGZWin* window, cGZMessage& message) {
    int x = Low32(message.param1), y = Low32(message.param2);
    window->ScreenToWindowCoordinates(x, y);       // cIGZWin vslot 0x1d8
    SetLow32(message.param1, x);
    SetLow32(message.param2, y);
}

// ProcessPostedMessages()::s_size — a function-local static shared by every manager (kept).
int g_postedBudget = 0;
// DestroyCondemnedWindows()::nReentrancyCount — also a function-local static.
int g_condemnedReentrancy = 0;
}  // namespace

cGZWinMgrBase::cGZWinMgrBase(IWinMgrBaseHost* host)
    : cRZSystemService(kGZSID_cGZWinMgrBase, kGZWinMgrServicePriority, kGZWinMgrTickPriority), host_(host) {
    validWindows_.max_load_factor(1.0f);   // +0xa8 = 0x3f800000
}

cGZWinMgrBase::~cGZWinMgrBase() {
    // The original clears the lists, releases the five window slots and the critical section by hand. The slots must go first:
    // a window's destructor calls back into CleanUpWindowReferences, which reads validWindows_ - and that member is declared
    // after the slots, so the implicit member destruction would tear the set down before the main window is released.
    keyFilters_.clear();
    capture_ = nullptr;
    focus_ = nullptr;
    modal_ = nullptr;
    mouse_ = nullptr;
    mainWindow_ = nullptr;
}

bool cGZWinMgrBase::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZWinMgr) {   // 0x5a4: answers itself (the thunks hand out the matching sub-object)
        *out = static_cast<cIGZWinMgr*>(this);
        AddRef();
        return true;
    }
    return cRZSystemService::QueryInterface(iid, out);
}

bool cGZWinMgrBase::Init() {
    if (!initialised_) {
        initialised_ = true;
        if (!mainWindow_) {
            cGZWin* desktop = new cGZWin(static_cast<cIGZWinMgr*>(this), &keyboard_);
            AssignRef<cIGZWin>(mainWindow_, desktop);   // AddRef the new one before the old one goes
            mainWindow_->Init();                        // vslot 0x20
        }
        if (!framework_ && host_) framework_ = host_->Framework();
        if (framework_) framework_->AddToTick(static_cast<cIGZSystemService*>(this));   // framework vslot 0x48
        if (!cursors_) {
            void* found = nullptr;
            if (host_ && host_->GetCOMObject(kGZCLSID_CursorManager, kGZIID_CursorManager, &found)) {
                cursors_ = static_cast<cIGZCursorManager*>(found);   // GetCOMObject hands out a counted reference
                static_cast<cIGZCursorManager*>(found)->Release();
            } else {
                cursors_ = new cRZCursorManager();   // the fallback: our own reference
            }
            cursors_->Init();                        // vslot 0x18
        }
    }
    return true;
}

bool cGZWinMgrBase::Shutdown() {
    if (initialised_) {
        initialised_ = false;
        // Keep every slot alive while the others are torn down (the original AddRefs them, releases the slots, then
        // drops the extra references at the end).
        cIGZWin* guards[5] = {capture_.get(), focus_.get(), modal_.get(), mouse_.get(), mainWindow_.get()};
        for (cIGZWin* g : guards) if (g) g->AddRef();
        capture_ = nullptr;
        modal_ = nullptr;
        mouse_ = nullptr;
        if (mainWindow_) {
            mainWindow_->Shutdown();    // vslot 0x28
            mainWindow_ = nullptr;
        }
        if (cursors_) {
            cursors_->Shutdown();       // vslot 0x20
            cursors_ = nullptr;
        }
        focus_ = nullptr;
        if (framework_) framework_->RemoveFromTick(static_cast<cIGZSystemService*>(this));   // framework vslot 0x50
        for (int i = 4; i >= 0; --i) if (guards[i]) guards[i]->Release();
    }
    return true;
}

bool cGZWinMgrBase::OnTick(std::uint32_t) {
    ProcessPostedMessages();   // vslot 0x80
    return true;
}

bool cGZWinMgrBase::GetMainWinAs(std::uint32_t iid, void** out) const {
    return mainWindow_ ? mainWindow_->QueryInterface(iid, out) : false;
}

void cGZWinMgrBase::GZWinMsgPost_Mouse(std::uint32_t type, int x, int y, std::uint32_t flags) {
    // x and y are packed as two 16-bit halves and sign-extended from 32 bits; the flags go in both outer words.
    const std::int32_t packed = std::int32_t((std::uint32_t(x) & 0xffffu) | (std::uint32_t(y) << 16));
    PostWinMessage(type, flags, std::uint64_t(std::int64_t(packed)), flags);
}

void cGZWinMgrBase::GZWinMsgPost_Keyboard(std::uint32_t type, std::uint32_t key, std::uint32_t modifiers) {
    // The binary forwards whatever register holds the fourth word; zero here.
    PostWinMessage(type, key, modifiers, 0);
}

void cGZWinMgrBase::EndModal(cIGZWin* window, std::uint64_t result) {
    PostWinMessage(kWinMgrMsgEndModal, std::uint64_t(reinterpret_cast<std::uintptr_t>(window)), result, result);
}

void cGZWinMgrBase::AddDeferredEndModal(cIGZWin* window, std::uint64_t result) { deferredEndModal_[window] = result; }

// ---- plotting ----------------------------------------------------------------------------------------------------------
void cGZWinMgrBase::Plot() {
    if (plotEnabled_) {
        plotList_.clear();
        plotting_ = true;
        if (AppendToPlotStrategyEx(mainWindow_.get())) ExecutePlotStrategy();
        plotting_ = false;
    }
}

bool cGZWinMgrBase::AppendToPlotStrategyEx(cIGZWin* window) {
    if (!window || !plotting_) return false;
    plotList_.push_back(window);
    return true;
}

bool cGZWinMgrBase::ExecutePlotStrategy() {
    if (!plotting_) return false;
    while (!plotList_.empty()) {
        cIGZWin* window = plotList_.front();
        plotList_.pop_front();
        window->Plot();   // vslot 0x2c8
    }
    return true;
}

// ---- messages ----------------------------------------------------------------------------------------------------------
bool cGZWinMgrBase::SendMessageToTarget(cIGZWin* target, cGZMessage& message) {
    if (target && IsWindowValid(target)) return target->DoMessage(message);   // vslot 0xc0 then 0x18
    return false;
}

bool cGZWinMgrBase::SendMessageToTarget(cIGZWin* target, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) {
    cGZMessage message;
    message.type = type;
    message.param1 = p1;
    message.param2 = p2;
    message.param3 = p3;
    // Unlike the overload above this one does not test for a null target before IsWindowValid (which rejects null anyway).
    return IsWindowValid(target) ? target->DoMessage(message) : false;
}

bool cGZWinMgrBase::ProcessMouseMessage(cGZMessage& message) {
    const std::uint32_t type = message.type;
    mouseX_ = Low32(message.param1);
    mouseY_ = Low32(message.param2);
    mouseFlags_ = int(message.param3 >> 16);

    if (cIGZWin* captured = capture_.get()) {
        captured->AddRef();
        bool handled;
        if (type == kGZMsgMouseMove) {
            ScreenToWindowInMessage(captured, message);
            SetCurrentMouseWin(captured);
            handled = captured->DoMessage(message);
        } else if (!captured->IsEnabled()) {   // vslot 0x240
            // A disabled capture window: the mouse goes to whatever is under the pointer instead; nothing is handled.
            if (cIGZWin* main = mainWindow_.get())
                SetCurrentMouseWin(main->GetChildWindowFromPoint(Low32(message.param1), Low32(message.param2)));   // vslot 0x138
            handled = false;
        } else {
            ScreenToWindowInMessage(captured, message);
            if (type - kGZMsgMouseDownL < 2) {   // left or right button down
                captured->PullToFront();         // vslot 0xb8
                GZSetFocus(captured);
            }
            SetCurrentMouseWin(captured);
            handled = captured->DoMessage(message);
        }
        captured->Release();
        return handled;
    }

    cIGZWin* target = nullptr;
    if (cIGZWin* modal = modal_.get()) {
        const bool inside = modal->IsPointInWindowScreenCoordinates(Low32(message.param1), Low32(message.param2));   // vslot 0x1f0
        bool take;
        if (type == kGZMsgMouseMove) take = inside && modal->IsVisible();                        // vslot 0x238
        else take = inside && modal->IsVisible() && modal->IsEnabled();                           // + vslot 0x240
        if (take) target = modal_.get();
    } else {
        target = mainWindow_.get();
        if (!target) return false;
    }
    if (target) return target->ProcessCursorMessage(message);   // vslot 0x298
    // A modal window is up and the pointer is outside it (or it is hidden): optionally keep the hover target current.
    if (moveFallbackToMain && mainWindow_) {
        SetCurrentMouseWin(mainWindow_->GetChildWindowFromPoint(Low32(message.param1), Low32(message.param2)));
    }
    return false;
}

bool cGZWinMgrBase::ProcessKeyboardMessage(cGZMessage& message) {
    if (message.type == kGZMsgKeyUp) keyboard_.ProcessKeyUp(std::uint32_t(message.param1));
    else if (message.type == kGZMsgKeyDown) keyboard_.ProcessKeyDown(std::uint32_t(message.param1));

    // The focus window gets the key when it is enabled, else the main window.
    cIGZWin* target;
    if (focus_ && focus_->IsEnabled()) target = focus_.get();
    else if (mainWindow_ && mainWindow_->IsEnabled()) target = mainWindow_.get();
    else return false;
    if (!target) return false;

    static const bool keyDebug = std::getenv("SC4_KEYDEBUG") != nullptr;   // dev: where each key goes
    if (keyDebug && message.type != kGZMsgCharacter)
        std::printf("keys: %s %02x mod %x -> window %08x (focus %08x, modal %08x)\n", message.type == kGZMsgKeyDown ? "down" : "up",
                    unsigned(message.param1), unsigned(message.param2), target->GetID(), focus_ ? focus_->GetID() : 0u,
                    modal_ ? modal_->GetID() : 0u);
    if (keyDebug && message.type == kGZMsgKeyDown) {
        std::printf("keys:   chain");
        for (cIGZWin* w = target; w; w = w->GetParentWin()) std::printf(" %08x%s", w->GetID(), w->IsVisible() ? "" : "(hidden)");
        std::printf("\n");
    }
    cGZMessage preview;
    preview.type = kWinMgrMsgKeyPreview;
    preview.param1 = std::uint64_t(reinterpret_cast<std::uintptr_t>(target));
    preview.param2 = message.type;
    preview.param3 = std::uint64_t(reinterpret_cast<std::uintptr_t>(&message));
    if (RunKeyFilters(preview)) return true;                       // vslot 0x210
    for (cIGZWin* w = target; w; w = w->GetParentWin())            // vslot 0x58
        if (w->DoMessage(preview)) return true;
    if (target->DoMessage(message)) return true;                   // the key itself
    // Nobody wanted it: offer the "unhandled" form from the target upwards, stopping at the modal window.
    preview.type = kWinMgrMsgKeyUnhandled;
    cIGZWin* w = target;
    do {
        if (w->DoMessage(preview)) return true;
    } while (w != modal_.get() && (w = w->GetParentWin()) != nullptr);
    return RunKeyFilters(preview);
}

void cGZWinMgrBase::PostMessageToTarget(cIGZWin* target, const cGZMessage& message) {
    postLock_.Lock();
    posted_.push_back({target, message});
    postLock_.Unlock();
}

void cGZWinMgrBase::PostMessageToTarget(cIGZWin* target, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) {
    cGZMessage message;
    message.type = type;
    message.param1 = p1;
    message.param2 = p2;
    message.param3 = p3;
    PostMessageToTarget(target, message);
}

void cGZWinMgrBase::ProcessPostedMessages() {
    cGZMessage message;
    if (g_postedBudget < 1) {
        // First entry: the budget is the number of messages queued right now.
        postLock_.Lock();
        g_postedBudget = int(posted_.size());
        postLock_.Unlock();
        const bool none = g_postedBudget < 1;
        --g_postedBudget;
        if (none) return;
    } else {
        --g_postedBudget;   // a nested call keeps draining the outer call's budget
    }
    bool more;
    do {
        cIGZWin* target;
        postLock_.Lock();
        if (posted_.empty()) {   // not guarded in the original (it would unlink the list head); a nested drain can get here
            postLock_.Unlock();
            g_postedBudget = -1;
            return;
        }
        MessageTargetInfo front = posted_.front();
        posted_.pop_front();
        postLock_.Unlock();
        target = front.target;
        message = front.message;
        if (!IsWindowValid(target)) {
            // A message for no window (null target) is a broadcast to every valid window; a stale target is dropped.
            if (!target) {
                IncrementBusyCount();
                std::vector<cIGZWin*> snapshot(validWindows_.begin(), validWindows_.end());   // the original walks the live set
                for (cIGZWin* w : snapshot) w->DoMessage(message);
                DecrementBusyCount();
            }
        } else {
            target->DoMessage(message);
        }
        more = 0 < g_postedBudget;
        --g_postedBudget;
    } while (more);
}

// ---- key filters --------------------------------------------------------------------------------------------------------
void cGZWinMgrBase::GZRegisterKeyFilter(cIGZWinMessageFilter* filter) {
    for (const auto& f : keyFilters_) if (f.get() == filter) return;
    keyFilters_.push_back(cRZAutoRefCount<cIGZWinMessageFilter>(filter));
}

void cGZWinMgrBase::GZUnregisterKeyFilter(cIGZWinMessageFilter* filter) {
    for (auto it = keyFilters_.begin(); it != keyFilters_.end(); ++it) {
        if (it->get() == filter) {
            keyFilters_.erase(it);
            return;
        }
    }
}

bool cGZWinMgrBase::RunKeyFilters(cGZMessage& message) {
    for (auto& filter : keyFilters_)
        if (filter->DoMessage(nullptr, message)) return true;   // filter vslot 0x18
    return false;
}

// ---- capture / cursor / focus ----------------------------------------------------------------------------------------
bool cGZWinMgrBase::GZSetCaptureEx(cIGZWin* window) {
    cIGZWin* old = capture_.get();
    if (old != window && (old == nullptr || allowCaptureSteal)) {
        if (window) window->AddRef();
        if (old) old->AddRef();
        capture_ = window;
        // Both the loser and the new owner hear about the change: p1 = the old capture, p2 = the new one.
        if (old) SendMessageToTarget(old, kWinMgrMsgCaptureChanged, std::uint64_t(reinterpret_cast<std::uintptr_t>(old)), std::uint64_t(reinterpret_cast<std::uintptr_t>(window)), 0);
        if (capture_) SendMessageToTarget(capture_.get(), kWinMgrMsgCaptureChanged, std::uint64_t(reinterpret_cast<std::uintptr_t>(old)), std::uint64_t(reinterpret_cast<std::uintptr_t>(window)), 0);
        if (old) old->Release();
        if (window) window->Release();
    }
    return true;
}

bool cGZWinMgrBase::GZReleaseCaptureEx(cIGZWin* window) {
    const bool held = capture_.get() == window;   // true for a null window when nothing is captured (kept)
    if (held) {
        GZSetCapture(nullptr);   // vslot 0xe8
        // Re-post the last pointer position so the window under the cursor reacts immediately.
        GZWinMsgPost_Mouse(kGZMsgMouseMove, mouseX_, mouseY_, std::uint32_t(mouseFlags_));   // vslot 0x50
    }
    return held;
}

void cGZWinMgrBase::GetCursorRelativePosition(cIGZWin* window, int& x, int& y) {
    GetCursorScreenPosition(x, y);               // vslot 0x108
    window->ScreenToWindowCoordinates(x, y);     // cIGZWin vslot 0x1d8
}

int cGZWinMgrBase::GZShowCursor(bool show) { return host_ ? host_->GraphicSystemShowCursor(show) : 0; }

void cGZWinMgrBase::GetCursorScreenPosition(int& x, int& y) {
    x = mouseX_;
    y = mouseY_;
}

void cGZWinMgrBase::SetCursorPosition(int, int) {}

bool cGZWinMgrBase::SetCurrentMouseWinEx(cIGZWin* window) {
    if (mouse_.get() == window) return false;
    if (window) window->AddRef();
    cGZMessage message;   // zeroed: type 0, no payload
    if (!IsWindowValid(mouse_.get()) && mouse_) mouse_ = nullptr;   // a dead window under the mouse is forgotten without a message
    cIGZWin* next = IsWindowValid(window) ? window : nullptr;
    if (mouse_) {
        message.type = kWinMgrMsgMouseExit;
        message.param1 = std::uint64_t(reinterpret_cast<std::uintptr_t>(next));
        mouse_->DoMessage(message);
    }
    if (next) {
        message.type = kWinMgrMsgMouseEnter;
        message.param1 = std::uint64_t(reinterpret_cast<std::uintptr_t>(mouse_.get()));
        next->DoMessage(message);
    }
    if (mouse_.get() != next) mouse_ = next;
    if (window) window->Release();
    return true;
}

cIGZWin* cGZWinMgrBase::GZSetFocusEx(cIGZWin* window) {
    if (focus_.get() == window || !IsWindowValid(window)) return focus_.get();
    bool apply = true;
    if (window) {
        window->AddRef();
        apply = window->GetFlag(kWinFlagCanFocus);   // vslot 0x218
    }
    if (apply) {
        cIGZWin* old = focus_.get();
        if (old) {
            old->AddRef();
            SendMessageToTarget(old, kWinMgrMsgSetFocusLost, std::uint64_t(reinterpret_cast<std::uintptr_t>(window)), 0, 0);
        }
        if (focus_.get() != window) focus_ = window;
        if (focus_) SendMessageToTarget(focus_.get(), kWinMgrMsgSetFocusGained, std::uint64_t(reinterpret_cast<std::uintptr_t>(old)), 0, 0);
        if (old) old->Release();
        if (!window) return focus_.get();   // (unreachable: a null window is never valid) the original skips the release
    }
    window->Release();
    return focus_.get();
}

// ---- modal ----------------------------------------------------------------------------------------------------------------
std::uint64_t cGZWinMgrBase::DoModalWin(cIGZWin* window) {
    std::uint64_t result = kModalNoResult;
    if (!IsWindowValid(window)) return result;
    cIGZWin* main = GetMainWindow();
    cIGZWin* parent = window->GetParentWin();   // vslot 0x58
    if (!((parent && IsWindowValid(parent)) || (main && IsWindowValid(main)))) return result;
    cIGZMessageServer* ms = host_ ? host_->MessageServer() : nullptr;
    if (!ms) return result;

    cIGZWin* oldFocus = GZGetFocus();
    if (oldFocus) oldFocus->AddRef();
    cIGZWin* oldModal = modal_.get();
    if (oldModal) oldModal->AddRef();
    const bool parentNotValid = parent ? !IsWindowValid(parent) : true;

    window->AddRef();
    cGZMessage begin;
    begin.type = kWinMgrModalBegin;
    begin.param1 = window->GetID();
    begin.param2 = std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
    begin.param3 = modalNest_;
    ms->MessageSend(begin);   // message server vslot 0x18

    if (parent != nullptr || main->ChildAdd(window)) {   // a window without a parent is hung under the main window first
        GZSetFocus(window);
        if (releaseCaptureOnModal && GZGetCapture() != window) GZSetCapture(nullptr);
        if (showModalWindow) window->ShowWindow();    // vslot 0x228
        if (pullModalToFront) window->PullToFront();  // vslot 0xb8
        if (modal_.get() != window) modal_ = window;
        window->SetFlag(kWinFlagModal, true);         // vslot 0x220
        ++modalNest_;
        result = RunModalLoop(window, framework_, 0x80000000u, 0x80000001u);   // vslot 0x220
        --modalNest_;
        window->SetFlag(kWinFlagModal, false);
        if (modal_.get() != oldModal) modal_ = oldModal;
        // A deferred EndModal is keyed by the window that is modal again (the outer one), not the one that just ended.
        if (cIGZWin* outer = modal_.get()) {
            auto it = deferredEndModal_.lower_bound(outer);
            if (it != deferredEndModal_.end() && !(outer < it->first)) {
                EndModal(outer, it->second);   // vslot 0x1d8
                deferredEndModal_.erase(it);
            }
        }
        if (alwaysRemoveModal || (removeOrphanModal && parentNotValid)) {
            if (cIGZWin* owner = window->GetParentWin()) owner->ChildRemove(window);   // vslot 0x58 then 0x78 (null not tested in the original)
        }
        // Put the focus back where it was when that window survived, else on the outer modal window.
        if (!IsWindowValid(oldFocus) || window->IsWinInChildChain(oldFocus)) {   // vslot 0xb0
            if (!IsWindowValid(modal_.get()) || window->IsWinInChildChain(modal_.get())) {
                if (IsWindowValid(focus_.get()) && window->IsWinInChildChain(focus_.get())) GZSetFocus(nullptr);
            } else {
                GZSetFocus(modal_.get());
            }
        } else {
            GZSetFocus(oldFocus);
        }
        if (framework_) framework_->OnIdle();   // framework vslot 0x80
    }

    cGZMessage end;
    end.type = kWinMgrModalEnd;
    end.param1 = window->GetID();
    end.param2 = std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
    end.param3 = modalNest_;
    ms->MessageSend(end);
    window->Release();
    if (oldModal) oldModal->Release();
    if (oldFocus) oldFocus->Release();
    return result;
}

// The first half of GZMsgBox (0x100623722): COM class lookup (class 0x593, iid 0x57c) with the stock cGZWinMsgBox as fallback.
cIGZWin* cGZWinMgrBase::CreateMsgBoxWindow(const cIGZString& text, const cIGZString& title, std::uint32_t style) {
    void* found = nullptr;
    cIGZWinMsgBox* box;
    if (host_ && host_->GetCOMObject(kGZCLSID_WinMsgBox, kGZIID_WinMsgBox, &found)) {
        box = static_cast<cIGZWinMsgBox*>(found);   // a counted reference
    } else {
        auto* stock = new cGZWinMsgBox(static_cast<cIGZWinMgr*>(this));
        box = stock;
        box->AddRef();
    }
    // vslot 0x30 of the dialog interface is CreateDlg(message, title, style). A replacement dialog from COM that is not
    // a cGZWinMsgBox cannot be driven through this recovered interface (it only exposes AsIGZWin).
    auto* dlg = dynamic_cast<cGZWinMsgBox*>(box);
    cIGZWin* window = nullptr;
    if (dlg && dlg->CreateDlg(text, title, style)) {
        window = box->AsIGZWin();   // vslot 0x28
        if (window) window->AddRef();
    }
    box->Release();
    return window;
}

int cGZWinMgrBase::GZMsgBox(const cIGZString& text, const cIGZString& title, std::uint32_t style, bool, std::uint32_t) {
    int answer = -1;
    if (cIGZWin* box = CreateMsgBoxWindow(text, title, style)) {   // returns the dialog window with a reference
        answer = int(DoModalWin(box));   // vslot 0x148
        box->Shutdown();                 // vslot 0x28
        box->Release();
    }
    return answer;
}

// ---- valid-window list ------------------------------------------------------------------------------------------------
bool cGZWinMgrBase::TryAddWindowToValidList(cIGZWin* window) {
    if (validWindows_.count(window)) return false;
    if (window->GetInstanceID() == 0) window->SetInstanceID(nextInstanceId_++);   // vslots 0x208 / 0x210
    validWindows_.insert(window);
    return true;
}

bool cGZWinMgrBase::TryRemoveWindowFromValidList(cIGZWin* window) { return validWindows_.erase(window) != 0; }

void cGZWinMgrBase::CleanUpWindowReferences(cIGZWin* window) {
    if (IsWindowValid(capture_.get())) {
        cIGZWin* c = capture_.get();
        if (c == window || c->IsWinInParentChain(window)) GZReleaseCapture(c);   // vslot 0xa8
    }
    if (!IsWindowValid(focus_.get())) {
        GZSetFocus(mainWindow_.get());
    } else {
        cIGZWin* f = focus_.get();
        if (f == window || f->IsWinInParentChain(window)) {
            // Focus moves to the dying window's notification target, else the desktop.
            cIGZWin* next = window->GetNotificationTarget();   // vslot 0x2b8 (asked twice in the original)
            GZSetFocus(next ? window->GetNotificationTarget() : mainWindow_.get());
        }
    }
    if (!IsWindowValid(mouse_.get())) {
        mouse_ = nullptr;
    } else {
        cIGZWin* m = mouse_.get();
        if (m == window || m->IsWinInParentChain(window)) mouse_ = nullptr;
    }
}

bool cGZWinMgrBase::IsWindowValid(cIGZWin* window) { return window && validWindows_.count(window) != 0; }

cIGZWin* cGZWinMgrBase::GetWindowFromPoint(int x, int y) {
    return mainWindow_ ? mainWindow_->GetChildWindowFromPoint(x, y) : nullptr;   // vslot 0x138
}

void cGZWinMgrBase::InvalidateAllWindows() {
    std::vector<cIGZWin*> snapshot(validWindows_.begin(), validWindows_.end());
    for (cIGZWin* w : snapshot) w->InvalidateSelf();   // vslot 0x2d8
}

// ---- destruction --------------------------------------------------------------------------------------------------------
bool cGZWinMgrBase::DestroyWindowAbsolute(cIGZWin* window) {
    const bool valid = IsWindowValid(window);
    CleanUpWindowReferences(window);                                     // vslot 0xb0
    cIGZWin* parent = valid ? window->GetParentWin() : nullptr;
    if (IsWindowValid(parent)) return parent->ChildDeleteAbsolute(window);   // vslot 0x88
    if (valid) {
        window->AddRef();
        window->Shutdown();
        window->Release();
    }
    return valid;
}

int cGZWinMgrBase::DecrementBusyCount() {
    if (busyCount_ < 1) {
        busyCount_ = 0;
        return 0;
    }
    if (--busyCount_ == 0) {
        DestroyCondemnedWindows();   // vslot 0x1f0
        return busyCount_;
    }
    return busyCount_;
}

bool cGZWinMgrBase::DestroyWindow(cIGZWin* window) {
    if (busyCount_ > 0) {
        // The manager is busy (inside a message walk): condemn the window instead, once.
        if (!IsWindowOrParentInCondemnedList(window)) condemned_.push_back(window);
        return true;
    }
    return DestroyWindowAbsolute(window);   // vslot 0x1e8
}

bool cGZWinMgrBase::DestroyCondemnedWindows() {
    if (g_condemnedReentrancy >= 1) return false;
    ++g_condemnedReentrancy;
    if (busyCount_ < 1) {
        for (auto it = condemned_.begin(); it != condemned_.end(); ++it) DestroyWindowAbsolute(*it);
        condemned_.clear();
    }
    --g_condemnedReentrancy;
    return true;
}

bool cGZWinMgrBase::IsWindowOrParentInCondemnedList(cIGZWin* window) {
    for (cIGZWin* condemned : condemned_)
        for (cIGZWin* w = window; w; w = w->GetParentWin())
            if (w == condemned) return true;
    return false;
}

}  // namespace gz
