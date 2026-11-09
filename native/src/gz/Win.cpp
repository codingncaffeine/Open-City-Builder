#include "gz/Win.h"

#include <algorithm>
#include <vector>

#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "rz/cRZVariant.h"

static IWinGraphics* g_graphics = nullptr;
static WinClockFn g_clock = nullptr;
IWinGraphics* WinGraphics() { return g_graphics; }
void SetWinGraphics(IWinGraphics* graphics) { g_graphics = graphics; }
void SetWinClock(WinClockFn fn) { g_clock = fn; }

namespace {
constexpr std::uint32_t kFadeAlphaKey = 0xe6e998fb, kFadeStartKey = 0xe6e998fc, kFadePeriodKey = 0xe6e998fd;
}

cGZWin::cGZWin(cIGZWinMgr* mgr, cIGZKeyboard* keyboard)
    : mgr_(mgr), keyboard_(keyboard), children_(new ChildList) {
    if (mgr_) mgr_->AddWindowToValidList(this);
}

cGZWin::~cGZWin() {
    ChildDeleteAll();
    Shutdown();
    mgr_->CleanUpWindowReferences(this);
    mgr_->RemoveWindowFromValidList(this);
    delete children_;
    for (cIGZWinMessageFilter* f : filters_) f->Release();
    for (cIGZWinMessageFilter* f : pendingRemove_) f->Release();
}

bool cGZWin::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cGZWinAlt || iid == 1 || iid == kGZIID_cIGZWin) {
        *out = this;
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cGZWin::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cGZWin::Init() {
    if (!GetFlag(kWinInitialized)) {
        SetFlag(kWinInitialized, true);
        if ((flags_ & 0x10001) == 0x10001) PrivateBuffer(true);
        SetBufferToDrawToRecursive();
    }
    return true;
}

bool cGZWin::Shutdown() {
    if (GetFlag(kWinInitialized)) {
        SetFlag(kWinInitialized, false);
        if (accelerator_) {
            accelerator_->Release();
            accelerator_ = nullptr;
        }
        for (cIGZWinMessageFilter* f : filters_) f->Release();
        filters_.clear();
        for (cIGZWinMessageFilter* f : pendingRemove_) f->Release();
        pendingRemove_.clear();
        HideWindow();
        ChildDeleteAll();
        PrivateBuffer(false);
        for (auto& entry : params_)
            if (entry.second) entry.second->Release();
        params_.clear();
    }
    return true;
}

// ---- child list (readers snapshot, writers copy) -------------------------------------------------------------------

void cGZWin::UnlockChildListRead(ChildList* list) const {
    if (--list->locks != 0 || children_ == list) return;
    for (cIGZWin* child : list->items) child->Release();
    delete list;
}

cGZWin::ChildList* cGZWin::LockChildListWrite() {
    if (children_->locks != 0) {
        auto* copy = new ChildList;
        copy->items = children_->items;
        for (cIGZWin* child : copy->items) child->AddRef();
        children_ = copy;
    }
    return children_;
}

std::uint32_t cGZWin::GetChildCount() const {
    ChildList* list = LockChildListRead();
    const std::uint32_t count = static_cast<std::uint32_t>(list->items.size());
    UnlockChildListRead(list);
    return count;
}

bool cGZWin::ChildExists(cIGZWin* child) {
    if (!mgr_->IsWindowValid(child)) return false;
    ChildList* list = LockChildListRead();
    const bool found = std::find(list->items.begin(), list->items.end(), child) != list->items.end();
    UnlockChildListRead(list);
    return found;
}

bool cGZWin::ChildExists(std::uint32_t id) {
    ChildList* list = LockChildListRead();
    bool found = false;
    for (cIGZWin* child : list->items)
        if ((found = child->GetID() == id)) break;
    UnlockChildListRead(list);
    return found;
}

bool cGZWin::SetParentWin(cIGZWin* parent) {
    cIGZWin* old = parent_;
    if (old == parent) return true;
    if (parent == nullptr || old == nullptr) {
        if (notify_ == nullptr) notify_ = parent;
    } else {
        if (!old->ChildRemove(this)) return false;
        if (notify_ == old) notify_ = nullptr;
        if (notify_ == nullptr) notify_ = parent;
    }
    parent_ = parent;
    return true;
}

bool cGZWin::ChildAddAbsolute(cIGZWin* child) {
    if (!mgr_->IsWindowValid(child)) return false;
    {
        ChildList* list = LockChildListRead();
        const bool present = std::find(list->items.begin(), list->items.end(), child) != list->items.end();
        UnlockChildListRead(list);
        if (present) return false;
    }
    if (!child->SetParentWin(this)) return false;
    if (!child->Init()) {
        child->Shutdown();
        child->SetParentWin(nullptr);
        return false;
    }
    ChildList* list = LockChildListWrite();
    list->items.push_front(child);
    child->AddRef();
    child->CalcAbsoluteArea();
    child->SetBufferToDrawToRecursive();
    return true;
}

bool cGZWin::ChildRemoveAbsolute(cIGZWin* child) {
    const bool valid = mgr_->IsWindowValid(child);
    ChildList* list = LockChildListWrite();
    auto it = std::find(list->items.begin(), list->items.end(), child);
    if (it == list->items.end()) return false;
    if (valid) {
        mgr_->CleanUpWindowReferences(child);
        child->SetParentWin(nullptr);
        child->Release();
    }
    list->items.erase(it);
    return true;
}

bool cGZWin::ChildDeleteAll() {
    bool failed = false;
    ChildList* list = LockChildListRead();
    while (!list->items.empty()) {
        cIGZWin* child = list->items.front();
        UnlockChildListRead(list);
        if (!child) return !failed;
        failed |= !mgr_->DestroyWindow(child);
        list = LockChildListRead();
    }
    UnlockChildListRead(list);
    return !failed;
}

bool cGZWin::ChildDeleteAbsolute(cIGZWin* child) {
    bool failed = true;
    if (mgr_->IsWindowValid(child)) {
        child->AddRef();
        failed = !child->Shutdown();
    }
    const bool removed = ChildRemoveAbsolute(child);
    mgr_->DestroyWindow(child);
    child->Release();
    return removed && !failed;
}

bool cGZWin::IsWinInParentChain(cIGZWin* window) {
    for (cIGZWin* p = GetParentWin(); p; p = p->GetParentWin())
        if (p == window) return true;
    return false;
}

bool cGZWin::IsWinInChildChain(cIGZWin* window) {
    ChildList* list = LockChildListRead();
    bool found = false;
    for (cIGZWin* child : list->items) {
        if (child == window || child->IsWinInChildChain(window)) {
            found = true;
            break;
        }
    }
    UnlockChildListRead(list);
    return found;
}

bool cGZWin::ChildToFront(cIGZWin* child) {
    if (!mgr_->IsWindowValid(child)) return false;
    ChildList* list = LockChildListRead();
    auto it = std::find(list->items.begin(), list->items.end(), child);
    const bool found = it != list->items.end();
    const bool alreadyFront = found && it == list->items.begin();
    UnlockChildListRead(list);
    if (!found) return false;
    if (GetParentWin()) GetParentWin()->ChildToFront(this);
    const bool movable = child->GetFlag(kWinFlag800);
    if (alreadyFront) return movable;
    if (!movable) return false;
    ChildList* w = LockChildListWrite();
    w->items.remove(child);
    w->items.push_front(child);
    return true;
}

bool cGZWin::ChildToBack(cIGZWin* child) {
    if (!mgr_->IsWindowValid(child) || !ChildExists(child) || !child->GetFlag(kWinFlag800)) return false;
    ChildList* w = LockChildListWrite();
    w->items.remove(child);
    w->items.push_back(child);
    return true;
}

bool cGZWin::ChildStepFront(cIGZWin* child) {
    if (!mgr_->IsWindowValid(child)) return false;
    ChildList* w = LockChildListWrite();
    auto it = std::find(w->items.begin(), w->items.end(), child);
    if (it == w->items.end()) return false;
    if (it != w->items.begin()) w->items.splice(std::prev(it), w->items, it);
    return true;
}

bool cGZWin::ChildStepBack(cIGZWin* child) {
    if (!mgr_->IsWindowValid(child)) return false;
    ChildList* w = LockChildListWrite();
    auto it = std::find(w->items.begin(), w->items.end(), child);
    if (it == w->items.end()) return false;
    auto next = std::next(it);
    if (next != w->items.end()) w->items.splice(std::next(next), w->items, it);
    return true;
}

bool cGZWin::MoveRelativeTo(cIGZWin* other, bool before) {
    return parent_ ? parent_->ChildMoveRelative(this, other, before) : false;
}

bool cGZWin::ChildMoveRelative(cIGZWin* child, cIGZWin* other, bool before) {
    if (!ChildExists(child) || !ChildExists(other)) return false;
    ChildList* w = LockChildListWrite();
    auto itChild = std::find(w->items.begin(), w->items.end(), child);
    auto itOther = std::find(w->items.begin(), w->items.end(), other);
    if (!before && itOther != w->items.end()) ++itOther;
    if (itChild != itOther) w->items.splice(itOther, w->items, itChild);
    return true;
}

bool cGZWin::EnumChildren(std::uint32_t iid, EnumChildFn fn, void* context) {
    if (!fn) return false;
    ChildList* list = LockChildListRead();
    if (list->items.empty()) {
        UnlockChildListRead(list);
        return false;
    }
    int failures = 0;
    for (cIGZWin* child : list->items) {
        if (iid == kGZIID_cIGZWin) {
            failures += !fn(this, child->GetID(), child, context);
        } else {
            void* typed = nullptr;
            if (child->QueryInterface(iid, &typed)) {
                failures += !fn(this, child->GetID(), typed, context);
                child->Release();
            }
        }
    }
    UnlockChildListRead(list);
    return failures == 0;
}

// Insertion sort that restarts after every move (the original's O(n^2) scheme); windows with flag 0x800 stay
// put unless `includeFixed` is set.
bool cGZWin::SortChildren(SortFn fn, void* context, bool includeFixed) {
    SortFn less = fn ? fn : lessfunc_id;
    ChildList* w = LockChildListWrite();
    if (w->items.size() < 2) return false;
    auto prev = w->items.begin();
    auto cur = std::next(prev);
    while (cur != w->items.end()) {
        cIGZWin* win = *cur;
        bool restarted = false;
        while ((includeFixed || !win->GetFlag(kWinFlag800)) && less(win, *prev, context)) {
            w->items.erase(cur);
            auto where = w->items.begin();
            while (where != w->items.end() && !less(win, *where, context)) ++where;
            w->items.insert(where, win);
            prev = w->items.begin();
            cur = std::next(prev);
            restarted = true;
            if (cur == w->items.end()) return true;
            win = *cur;
        }
        (void)restarted;
        prev = cur;
        ++cur;
    }
    return true;
}

cIGZWin* cGZWin::GetChildWindowFromID(std::uint32_t id) {
    ChildList* list = LockChildListRead();
    cIGZWin* result = nullptr;
    for (cIGZWin* child : list->items)
        if (child->GetID() == id) { result = child; break; }
    UnlockChildListRead(list);
    return result;
}

cIGZWin* cGZWin::GetChildWindowFromIDRecursive(std::uint32_t id) {
    ChildList* list = LockChildListRead();
    cIGZWin* result = nullptr;
    for (cIGZWin* child : list->items)
        if ((result = child->GetChildWindowFromIDRecursive(id))) break;
    UnlockChildListRead(list);
    if (result) return result;
    return GetID() == id ? this : nullptr;
}

bool cGZWin::GetChildAs(std::uint32_t id, std::uint32_t iid, void** out) {
    cIGZWin* child = GetChildWindowFromID(id);
    return child ? child->QueryInterface(iid, out) : false;
}

bool cGZWin::GetChildAsRecursive(std::uint32_t id, std::uint32_t iid, void** out) {
    cIGZWin* child = GetChildWindowFromIDRecursive(id);
    return child ? child->QueryInterface(iid, out) : false;
}

cIGZWin* cGZWin::GetChildWindowFromPoint(int x, int y) {
    ChildList* list = LockChildListRead();
    cIGZWin* result = nullptr;
    for (cIGZWin* child : list->items)
        if (child->GetFlag(kWinVisible) && (result = child->GetChildWindowFromPoint(x, y))) break;
    UnlockChildListRead(list);
    if (result) return result;
    return IsPointInWindowScreenCoordinates(x, y) ? this : nullptr;
}

cIGZWin* cGZWin::GetChildWindowFromCursorPoint(int x, int y) {
    ChildList* list = LockChildListRead();
    cIGZWin* result = nullptr;
    for (cIGZWin* child : list->items)
        if (child->GetFlag(kWinVisible) && !child->GetFlag(kWinIgnoreCursor) &&
            (result = child->GetChildWindowFromCursorPoint(x, y)))
            break;
    UnlockChildListRead(list);
    if (result) return result;
    return IsPointInWindowScreenCoordinates(x, y) ? this : nullptr;
}

cIGZWin* cGZWin::IsChildKeyEquivalent(std::uint32_t key, std::uint32_t modifiers) {
    ChildList* list = LockChildListRead();
    cIGZWin* result = nullptr;
    for (cIGZWin* child : list->items)
        if (child->CheckKeyEquivalent(key, modifiers)) { result = child; break; }
    UnlockChildListRead(list);
    return result;
}

// ---- geometry ----------------------------------------------------------------------------------------------

void cGZWin::SetArea(int l, int t, int r, int b) {
    const bool sameSize = (r - l == area_.right - area_.left) && (b - t == area_.bottom - area_.top);
    area_ = {l, t, r, b};
    if (!sameSize && (flags_ & kWinPrivateBuffer)) PrivateBuffer(true);
    CalcAbsoluteArea();
    SetAreaToDrawToRecursive();
}

void cGZWin::CalcAbsoluteArea() {
    cRZRect abs = GetArea();
    for (cIGZWin* p = GetParentWin(); p; p = p->GetParentWin()) {
        const int l = p->GetL(), t = p->GetT();
        abs.left += l; abs.top += t; abs.right += l; abs.bottom += t;
    }
    absoluteArea_ = abs;
    ChildList* list = LockChildListRead();
    for (cIGZWin* child : list->items) child->CalcAbsoluteArea();
    UnlockChildListRead(list);
}

void cGZWin::CenterWindowInRect(const cRZRect& rect) {
    const int w = GetW(), h = GetH();
    const int x = ((rect.right - rect.left) - w) / 2 + rect.left;
    const int y = ((rect.bottom - rect.top) - h) / 2 + rect.top;
    SetArea(x, y, w + x, h + y);
}

bool cGZWin::FitRectToWindow(cRZRect& rect, int margin) {
    const int w = GetW(), h = GetH();
    if (rect.left < 0) rect.left = margin;
    if (rect.top < 0) rect.top = margin;
    if (w < rect.right) {
        const int d = (w - rect.right) - margin;
        rect.left += d;
        rect.right += d;
    }
    if (h < rect.bottom) {
        const int d = (h - rect.bottom) - margin;
        rect.top += d;
        rect.bottom += d;
    }
    if (w < (rect.right + margin) - rect.left) return false;
    return (rect.bottom + margin) - rect.top <= h;
}

void cGZWin::ScreenToWindowCoordinates(int& x, int& y) const {
    int ox = 0, oy = 0;
    WindowToScreenCoordinates(ox, oy);
    x -= ox;
    y -= oy;
}

bool cGZWin::WindowToWindowCoordinates(cIGZWin* other, int& x, int& y) const {
    if (!other) return false;
    WindowToScreenCoordinates(x, y);
    other->ScreenToWindowCoordinates(x, y);
    return true;
}

bool cGZWin::IsPointInWindowWindowCoordinates(int x, int y) const {
    bool inside = !((x | y) < 0) && y < area_.bottom - area_.top && x < area_.right - area_.left;
    if (inside && GetFlag(kWinHitTestTransparency)) inside = !is_point_transparent(x, y);
    return inside;
}

bool cGZWin::IsPointInWindowParentCoordinates(int x, int y) const {
    return !(x < area_.left || y < area_.top || area_.right <= x) && y < area_.bottom;
}

bool cGZWin::IsPointInWindowScreenCoordinates(int x, int y) const {
    bool inside = !(x < absoluteArea_.left || y < absoluteArea_.top || absoluteArea_.right <= x) && y < absoluteArea_.bottom;
    if (inside && GetFlag(kWinHitTestTransparency)) {
        int wx = x, wy = y;
        ScreenToWindowCoordinates(wx, wy);
        inside = !is_point_transparent(wx, wy);
    }
    return inside;
}

// ---- flags, visibility, focus ----------------------------------------------------------------------------------

std::uint32_t cGZWin::SetFlag(std::uint32_t flag, bool on) {
    flags_ = on ? (flags_ | flag) : (flags_ & ~flag);
    auto refocusToNotifyTarget = [this] {
        if (mgr_->GZGetFocus() == this && mgr_->IsWindowValid(notify_)) mgr_->GZSetFocus(notify_);
    };
    if (flag < 0x10000) {
        if (flag == kWinVisible) {
            if (!GetFlag(kWinPrivateBuffer)) child_private_buffers(on);
            else PrivateBuffer(on);
            if (!on) refocusToNotifyTarget();
            if (parent_) parent_->InvalidateSelfAndParents();
            flags_ &= ~kWinFlag400000;
        } else if (flag == kWinEnabled) {
            if (!on) {
                if (mgr_->GZGetCapture() == this) mgr_->GZReleaseCapture(this);
                refocusToNotifyTarget();
            }
        } else if (flag == kWinFade) {
            set_fade_state(on ? 0 : 0xff, on ? fadeInMs_ : 0);
        }
    } else if (flag == kWinPrivateBuffer) {
        PrivateBuffer(on && IsVisible());
    } else if (flag == kWinPrivateBufferTransparent) {
        if (GetFlag(kWinPrivateBuffer)) InvalidateSelfAndParents();
    } else if (flag == kWinDesktop) {
        SetBufferToDrawToRecursive();
        SetAreaToDrawToRecursive();
    }
    return flags_;
}

bool cGZWin::child_private_buffers(bool on) {
    ChildList* list = LockChildListRead();
    bool failed = false;
    for (cIGZWin* child : list->items) {
        bool moveAfter = false;
        if (!child->GetFlag(kWinPrivateBuffer)) {
            static_cast<cGZWin*>(child)->child_private_buffers(on);
            moveAfter = on;
        } else if (on) {
            if (!child->IsVisible()) static_cast<cGZWin*>(child)->child_private_buffers(false);
            else failed |= !child->PrivateBuffer(true);
            moveAfter = true;
        } else {
            failed |= !child->PrivateBuffer(false);
        }
        if (moveAfter) child->GZWinMoveTo(child->GetL(), child->GetT());
    }
    UnlockChildListRead(list);
    return !failed;
}

void cGZWin::ShowWindow() {
    if (!GetFlag(kWinVisible)) {
        SetFlag(kWinVisible, true);
        if (GetFlag(kWinFade)) set_fade_state(0, fadeInMs_);
    }
}

void cGZWin::HideWindow() {
    if (GetFlag(kWinVisible)) {
        SetFlag(kWinVisible, false);
        if (GetFlag(kWinFade)) set_fade_state(0xff, 0);
    }
}

bool cGZWin::SetFocusTo(cIGZWin* window) {
    const bool valid = mgr_->IsWindowValid(window);
    if (valid) mgr_->GZSetFocus(window);
    return valid;
}

bool cGZWin::GZOnSetFocus(cIGZWin*) {
    if (!GetFlag(kWinVisible) && mgr_->IsWindowValid(notify_)) mgr_->GZSetFocus(notify_);
    return false;
}

void cGZWin::InvalidateAllParents() {
    for (cIGZWin* p = GetParentWin(); p; p = p->GetParentWin()) {
        p->InvalidateSelf();
        if (p->GetFlag(kWinDesktop)) break;
    }
}

void cGZWin::InvalidateSelfAndParents() {
    InvalidateSelf();
    if (!GetFlag(kWinDesktop)) InvalidateAllParents();
}

// ---- messages ----------------------------------------------------------------------------------------------------

bool cGZWin::DoMessage(cGZMessage& m) {
    const bool wasDispatching = dispatching_;
    dispatching_ = true;
    for (cIGZWinMessageFilter* filter : filters_) {
        if (filter->DoMessage(this, m)) {
            dispatching_ = wasDispatching;
            return true;
        }
    }
    dispatching_ = wasDispatching;
    const int a = static_cast<int>(m.param1), b = static_cast<int>(m.param2);
    const short sa = static_cast<short>(m.param1), sb = static_cast<short>(m.param2);
    const std::uint16_t flags16 = static_cast<std::uint16_t>(m.param3 >> 16);
    switch (m.type) {
        case kGZMsgDeleteChild: return ChildDelete(reinterpret_cast<cIGZWin*>(m.param1));
        case kGZMsgCommand: return GZOnCommand(static_cast<std::uint32_t>(a), m.param2);
        case kGZMsgCharacter: return GZOnCharacter(static_cast<char>(m.param1));
        case kGZMsgKeyDown: {
            const bool handled = GZOnKeyDown(a, b);
            if (accelerator_ && !handled) return AccelerateKeyboardMsg(m);
            return handled;
        }
        case kGZMsgKeyUp: {
            const bool handled = GZOnKeyUp(a, b);
            if (accelerator_ && !handled) return AccelerateKeyboardMsg(m);
            return handled;
        }
        case kGZMsgMouseDownL: return GZOnMouseDownL(sa, sb, flags16);
        case kGZMsgMouseDownR: return GZOnMouseDownR(sa, sb, flags16);
        case kGZMsgMouseUpL: return GZOnMouseUpL(sa, sb, flags16);
        case kGZMsgMouseUpR: return GZOnMouseUpR(sa, sb, flags16);
        case kGZMsgMouseMove: return GZOnMouseMove(sa, sb, flags16);
        case kGZMsgMouseWheel:
            return GZOnMouseWheel(a, b, static_cast<std::uint32_t>(m.param3 & 0xffffffff) >> 16,
                                  static_cast<short>(m.param3 & 0xffffffff));
        case kGZMsgKillFocus: return GZOnKillFocus(reinterpret_cast<cIGZWin*>(m.param1));
        case kGZMsgSetFocus: return GZOnSetFocus(reinterpret_cast<cIGZWin*>(m.param1));
        case kGZMsgCaptureChanged: return GZOnCaptureChanged(reinterpret_cast<cIGZWin*>(m.param1), reinterpret_cast<cIGZWin*>(m.param2));
        case kGZMsgMouseEnter: return GZOnMouseEnter(reinterpret_cast<cIGZWin*>(m.param1));
        case kGZMsgMouseExit: return GZOnMouseExit(reinterpret_cast<cIGZWin*>(m.param1));
        default: return false;
    }
}

bool cGZWin::SendMsg(cIGZWin* target, cGZMessage& message) {
    return mgr_->IsWindowValid(target) ? target->DoMessage(message) : false;
}

void cGZWin::SendMsg(cIGZWin* target, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) {
    cGZMessage m;
    m.type = type; m.param1 = p1; m.param2 = p2; m.param3 = p3;
    SendMsg(target, m);
}

bool cGZWin::PostMsg(cIGZWin* target, cGZMessage& message) {
    mgr_->PostMessageToTarget(target, message);
    return true;
}

void cGZWin::PostMsg(cIGZWin* target, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) {
    cGZMessage m;
    m.type = type; m.param1 = p1; m.param2 = p2; m.param3 = p3;
    PostMsg(target, m);
}

bool cGZWin::AddMessageFilter(cIGZWinMessageFilter* filter) {
    if (filter) filter->AddRef();
    filters_.push_back(filter);
    return true;
}

bool cGZWin::RemoveMessageFilter(cIGZWinMessageFilter* filter) {
    if (filter) filter->AddRef();
    bool result = false;
    auto it = std::find(filters_.begin(), filters_.end(), filter);
    if (it != filters_.end()) {
        result = true;
        if (!dispatching_) {
            (*it)->Release();
            filters_.erase(it);
        } else {
            pendingRemove_.push_back(*it);
        }
    }
    if (filter) filter->Release();
    return result;
}

void cGZWin::SetKeyboardAccelerator(cIGZWinKeyAccelerator* accelerator) {
    if (accelerator_ == accelerator) return;
    if (accelerator_) accelerator_->Release();
    accelerator_ = accelerator;
    if (accelerator) accelerator->AddRef();
}

// ---- cursor ------------------------------------------------------------------------------------------------------

void cGZWin::UpdateCursor() {
    if (!cursor_) {
        cursor_ = mgr_->GetCursorManager()->LoadCursor(1);
        if (!cursor_) return;
    }
    mgr_->GetCursorManager()->SetCursor(cursor_);
}

bool cGZWin::ProcessCursorMessage(cGZMessage& m) {
    const std::uint32_t type = m.type;
    ChildList* list = LockChildListRead();
    for (cIGZWin* child : list->items) {
        if (child->IsVisible() && !child->GetFlag(kWinIgnoreCursor) &&
            child->IsPointInWindowScreenCoordinates(static_cast<int>(m.param1), static_cast<int>(m.param2)) &&
            (type == kGZMsgMouseMove || child->IsEnabled())) {
            UnlockChildListRead(list);
            return child->ProcessCursorMessage(m);
        }
    }
    UnlockChildListRead(list);
    if (GetFlag(kWinIgnoreCursor) || !IsVisible()) return false;
    int x = static_cast<int>(m.param1), y = static_cast<int>(m.param2);
    if (!IsEnabled()) {
        if (type != kGZMsgMouseMove) return false;
        ScreenToWindowCoordinates(x, y);
    } else {
        if (type - 7u < 3u) {
            PullToFront();
            mgr_->GZSetFocus(this);
        }
        ScreenToWindowCoordinates(x, y);
        m.param1 = static_cast<std::uint32_t>(x);
        m.param2 = static_cast<std::uint32_t>(y);
        if (type == kGZMsgMouseMove) UpdateCursor();
        mgr_->SetCurrentMouseWin(this);
        return DoMessage(m);
    }
    m.param1 = static_cast<std::uint32_t>(x);
    m.param2 = static_cast<std::uint32_t>(y);
    mgr_->SetCurrentMouseWin(this);
    return DoMessage(m);
}

// ---- parameters ----------------------------------------------------------------------------------------------------

bool cGZWin::SetParam(std::uint32_t key, cIGZVariant* value) {
    auto it = params_.find(key);
    if (it != params_.end()) {
        if (it->second) it->second->Release();
        params_.erase(it);
    }
    if (value) {
        value->AddRef();
        params_.emplace(key, value);
    }
    return true;
}

bool cGZWin::GetParam(std::uint32_t key, cIGZVariant** out) const {
    auto it = params_.find(key);
    if (it == params_.end()) return false;
    *out = it->second;
    it->second->AddRef();
    return true;
}

bool cGZWin::EnumParams(EnumParamFn fn, void* context) {
    for (auto& entry : params_)
        if (!fn(this, entry.first, entry.second, context)) return false;
    return true;
}

// ---- colours ----------------------------------------------------------------------------------------------------------

void cGZWin::GetFillColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
    r = static_cast<std::uint8_t>(fillColor_ >> 16);
    g = static_cast<std::uint8_t>(fillColor_ >> 8);
    b = static_cast<std::uint8_t>(fillColor_);
}

std::uint32_t cGZWin::GetFillColor() const {
    return WinGraphics()->MakeNativeColor((fillColor_ >> 16) & 0xff, (fillColor_ >> 8) & 0xff, fillColor_ & 0xff);
}

void cGZWin::SetFillColor(std::uint32_t nativeColor) {
    std::uint8_t r, g, b;
    WinGraphics()->NativeToRGB(nativeColor, r, g, b);
    SetFillColor(r, g, b);
}

std::uint32_t cGZWin::MakeFillColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (drawBuffer_) return drawBuffer_->MakeNativeColor(r, g, b);
    return WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0;
}

std::uint32_t cGZWin::ConvertPackedRGBToNative(std::uint32_t rgb) const {
    const std::uint8_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    if (drawBuffer_) return drawBuffer_->MakeNativeColor(r, g, b);
    return WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0;
}

std::uint32_t cGZWin::ConvertNativeToPackedRGB(std::uint32_t native) const {
    std::uint8_t r = 0, g = 0, b = 0;
    if (drawBuffer_) drawBuffer_->NativeToRGB(native, r, g, b);
    else if (WinGraphics()) WinGraphics()->NativeToRGB(native, r, g, b);
    return b | (std::uint32_t(g) << 8) | (std::uint32_t(r) << 16) | 0xff000000u;
}

// ---- fade effect ------------------------------------------------------------------------------------------------------------

void cGZWin::set_fade_state(std::uint8_t alpha, int periodMs) {
    if ((flags_ & 0x10020) != 0x10020) return;
    auto store = [this](std::uint32_t key, auto setter) {
        auto* v = new cRZVariant();
        v->AddRef();
        setter(v);
        SetParam(key, v);
        v->Release();
    };
    store(kFadeAlphaKey, [&](cRZVariant* v) { v->SetValUint8(alpha); });
    store(kFadeStartKey, [&](cRZVariant* v) { v->SetValUint32(g_clock ? g_clock() : 0); });
    store(kFadePeriodKey, [&](cRZVariant* v) { v->SetValSint32(periodMs); });
    if (privateBuffer_) privateBuffer_->SetFadeAlpha(alpha);
}

bool cGZWin::fade_in_progress() {
    if ((flags_ & 0x10020) != 0x10020) return false;
    cIGZVariant* v = nullptr;
    if (!GetParam(kFadePeriodKey, &v)) return false;
    const bool active = (v->AsSint32() & 0xff) != 0;   // the original tests only the low byte
    v->Release();
    return active;
}

void cGZWin::recalc_blt_area() {
    bltArea_ = GetArea();
    for (cIGZWin* p = GetParentWin(); p && !p->GetPrivateBuffer(); p = p->GetParentWin()) {
        const int l = p->GetL(), t = p->GetT();
        bltArea_.left += l; bltArea_.top += t; bltArea_.right += l; bltArea_.bottom += t;
    }
}

void cGZWin::SetBufferToDrawTo() {
    drawBuffer_ = static_cast<IWinBuffer*>(GetPrivateBuffer());
    usesOwnBuffer_ = true;
    if (!drawBuffer_) {
        for (cIGZWin* p = GetParentWin(); p; p = p->GetParentWin()) {
            if (void* b = p->GetPrivateBuffer()) {
                drawBuffer_ = static_cast<IWinBuffer*>(b);
                break;
            }
            if (GetFlag(kWinDesktop)) break;
        }
        if (!drawBuffer_ && WinGraphics()) {
            drawBuffer_ = WinGraphics()->PrimaryBuffer();
            usesOwnBuffer_ = false;
        }
    }
    drawContext_ = drawBuffer_ ? drawBuffer_->GetDrawContext() : nullptr;
    SetAreaToDrawToRecursive();
}

void cGZWin::SetBufferToDrawToRecursive() {
    SetBufferToDrawTo();
    ChildList* list = LockChildListRead();
    for (cIGZWin* child : list->items) child->SetBufferToDrawToRecursive();
    UnlockChildListRead(list);
}

void cGZWin::SetAreaToDrawToRecursive() {
    SetAreaToDrawTo();
    ChildList* list = LockChildListRead();
    for (cIGZWin* child : list->items) child->SetAreaToDrawToRecursive();
    UnlockChildListRead(list);
}

void cGZWin::SetAreaToDrawTo() {
    if (!privateBuffer_) {
        cRZRect local = area_;
        drawArea_ = area_;
        for (cIGZWin* p = GetParentWin(); p; p = p->GetParentWin()) {
            if (p->GetPrivateBuffer()) break;
            if (GetFlag(kWinDesktop)) {
                local = absoluteArea_;
                break;
            }
            const int l = p->GetL(), t = p->GetT();
            local.left += l; local.top += t; local.right += l; local.bottom += t;
        }
        drawArea_ = local;
        if (WinGraphics() && WinGraphics()->DrawSystemActive()) {
            // The rectangle moves to (0, 0) keeping its size (cRZRect::MoveTo, PC 0x0082ae40: dx/dy applied to all four
            // edges). The earlier "bottom keeps its value" reading stretched every 9-slice button and text box by its y.
            const int left = drawArea_.left, top = drawArea_.top;
            drawArea_.left = 0;
            drawArea_.top = 0;
            drawArea_.right -= left;
            drawArea_.bottom -= top;
        }
    } else {
        drawArea_ = {0, 0, area_.right - area_.left, area_.bottom - area_.top};
        recalc_blt_area();
    }
    accumulatedArea_ = drawArea_;
    cIGZWin* p = this;
    while (!p->GetPrivateBuffer()) {
        if (p->GetFlag(kWinDesktop)) {
            const cRZRect& a = p->GetAreaAbsolute();
            accumulatedArea_.left += a.left; accumulatedArea_.top += a.top;
            accumulatedArea_.right += a.left; accumulatedArea_.bottom += a.top;
            break;
        }
        const cRZRect& a = p->GetArea();
        accumulatedArea_.left += a.left; accumulatedArea_.top += a.top;
        accumulatedArea_.right += a.left; accumulatedArea_.bottom += a.top;
        p = p->GetParentWin();
        if (!p) break;
    }
}

namespace {
constexpr std::uint32_t kFadeTmpKey = 0xe6e998fe;
constexpr std::uint32_t kLockFill = 0x8001, kLockPresent = 0x8010, kLockHitTest = 0x800;

bool AllAncestorsVisible(cIGZWin* window) {
    for (cIGZWin* p = window->GetParentWin(); p; p = p->GetParentWin())
        if (!p->IsVisible()) return false;
    return true;
}
}  // namespace

// Turns the window's private (off-screen) buffer on or off. Enabling needs every ancestor visible; the buffer is
// (re)created when it is missing or the wrong size. Disabling releases it - unless the window is fading out, in
// which case a cGZFadeTmpWin adopts the buffer and finishes the fade (the "fade" flag 0x20 with the pending
// flag 0x400000 set by PlotComposite).
bool cGZWin::PrivateBuffer(bool enable) {
    bool ok = true;
    bool disable = !enable;
    if (enable) {
        if (!AllAncestorsVisible(this)) {
            disable = true;      // an invisible ancestor: take the "turn off" path, but still report success
        } else {
            flags_ |= kWinPrivateBuffer;
            const int w = area_.right - area_.left, h = area_.bottom - area_.top;
            bool sized = false;
            if (privateBuffer_) {
                const cRZRect* b = privateBuffer_->GetBounds();
                sized = (b->right - b->left == w) && (b->bottom - b->top == h);
            }
            if (!sized) {
                if (!create_private_buffer(w, h)) {
                    ok = false;
                } else {
                    if (GetFlag(kWinPrivateBufferTransparent)) set_private_buffer_transparency(true);
                    SetArea(area_.left, area_.top, area_.right, area_.bottom);
                }
            }
        }
    }
    if (disable && privateBuffer_) {
        if (privateBuffer_->IsReady()) {
            bool shutdown = true;
            if (GetFlag(kWinFade) && GetFlag(kWinFlag400000)) {
                cIGZVariant* v = nullptr;
                const bool isFadeTmp = GetParam(kFadeTmpKey, &v);
                if (isFadeTmp && v) v->Release();
                SetFlag(kWinFlag400000, false);
                if (AllAncestorsVisible(this) && !isFadeTmp) {
                    auto* tmp = new cGZFadeTmpWin(mgr_);
                    tmp->SetFlag(kWinFlag4, true);
                    tmp->StealBuffer(this);
                    shutdown = false;      // the temporary window owns the buffer now
                }
            }
            if (shutdown) privateBuffer_->Shutdown();
        }
        privateBuffer_->Release();
        privateBuffer_ = nullptr;
    }
    SetBufferToDrawToRecursive();
    child_private_buffers(GetFlag(kWinVisible));
    InvalidateSelfAndParents();
    return ok;
}

// (Re)creates the private buffer at the given size; a non-positive size in both directions just drops it. With a
// draw system the buffer comes from the service (an existing one is shut down and released first); without one
// the graphics system's buffer is initialised in place.
bool cGZWin::create_private_buffer(int w, int h) {
    if (w < 1 && h < 1) {
        if (privateBuffer_) {
            privateBuffer_->Release();
            privateBuffer_ = nullptr;
        }
        return false;
    }
    IWinGraphics* gfx = WinGraphics();
    if (gfx && gfx->DrawSystemActive()) {
        if (privateBuffer_) {
            privateBuffer_->Shutdown();
            privateBuffer_->Release();
            privateBuffer_ = nullptr;
        }
        privateBuffer_ = gfx->DrawSystemCreateBuffer(w, h);
        return privateBuffer_ != nullptr;
    }
    if (privateBuffer_) {
        privateBuffer_->Shutdown();
    } else {
        if (gfx) privateBuffer_ = gfx->GraphicsCreateBuffer();
        if (!privateBuffer_) return false;
    }
    if (!privateBuffer_->Init(w, h)) {
        privateBuffer_->Release();
        privateBuffer_ = nullptr;
        return false;
    }
    return true;
}

bool cGZWin::is_point_transparent(int x, int y) const {
    if (privateBuffer_ && privateBuffer_->GetTransparency() && privateBuffer_->Lock(kLockHitTest)) {
        const bool transparent = privateBuffer_->IsPixelTransparent(x, y);
        privateBuffer_->Unlock(kLockHitTest);
        return transparent;
    }
    return false;    // the original returns whatever was left in the register; callers only test it as a bool
}

// Keys the private buffer on the window class manager's transparent colour, or removes the key.
bool cGZWin::set_private_buffer_transparency(bool on) {
    if (!privateBuffer_) return false;
    if (on) {
        std::uint32_t color = 0;
        if (!WinGraphics() || !WinGraphics()->GetPrivateBufferKeyColor(color)) return false;
        privateBuffer_->SetTransparency(color);
    } else {
        privateBuffer_->RemoveTransparency();
    }
    return true;
}

bool cGZWin::fill_private_buffer_with_transparent_color() {
    if (privateBuffer_ && privateBuffer_->Lock(kLockFill)) {
        std::uint32_t color = 0;
        if (privateBuffer_->GetTransparentColor(color)) privateBuffer_->Fill(color, drawArea_);
        privateBuffer_->Unlock(kLockFill);
        return true;
    }
    return false;
}

// Moves the fade alpha along the clock: alpha = start alpha + elapsed * 255 / period (the period is negative
// for a fade-out). Leaving [0,256) ends the fade (period reset to 0) and clamps. The result is pushed into the
// private buffer's shade colour (white with that alpha).
void cGZWin::fade_advance() {
    if ((flags_ & 0x10020) != 0x10020) return;
    cIGZVariant* v = nullptr;
    if (!GetParam(kFadePeriodKey, &v)) return;
    const int period = v->AsSint32();
    v->Release();
    if (period == 0) return;
    if (!GetParam(kFadeAlphaKey, &v)) return;
    const std::uint8_t startAlpha = v->AsUint8();
    v->Release();
    if (!GetParam(kFadeStartKey, &v)) return;
    const int startTime = v->AsSint32();
    v->Release();
    const int now = static_cast<int>(g_clock ? g_clock() : 0);
    const double alpha = (static_cast<double>(static_cast<std::uint32_t>(now - startTime)) * 255.0) / static_cast<double>(period)
                         + static_cast<double>(startAlpha);
    std::uint32_t a;
    if (alpha < 0.0 || alpha >= 256.0) {
        if (GetParam(kFadePeriodKey, &v)) {
            v->SetValSint32(0);
            v->Release();
        }
        a = alpha < 0.0 ? 0 : 0xff;
    } else {
        a = static_cast<std::uint32_t>(alpha);
    }
    if ((flags_ & 0x10020) == 0x10020 && privateBuffer_) privateBuffer_->SetFadeAlpha(static_cast<std::uint8_t>(a));
}

void* cGZWin::BeginPaintFrame() {
    auto* ctx = static_cast<gz::cIGZDrawContext*>(drawContext_);
    if (!ctx) return nullptr;
    ctx->AddRef();
    ctx->PushTransform();
    ctx->OffsetAbsolute(cRZPoint{accumulatedArea_.left, accumulatedArea_.top});
    ctx->ClipAbsolute(accumulatedArea_);
    ctx->ClipReset();                      // as recovered (vslot 0x20): resets the clip right after ClipAbsolute
    (void)ctx->GetTransformStackDepth();   // vslot 0x78, result unused
    ctx->PushState();
    ctx->ResetToDefaultState();
    const bool blend = GetFlag(kWinFlag4) && !GetFlag(kWinPrivateBuffer);
    ctx->SetAlphaBlend(blend);
    ctx->SetBltModulate(blend);
    if (!GetFlag(kWinUseShadeColor) || GetFlag(kWinPrivateBuffer)) ctx->SetColor(0xffffffffu);
    else ctx->SetColor(shadeColor_);
    ctx->Resume();
    return ctx;
}

void cGZWin::EndPaintFrame(void* context) {
    auto* ctx = static_cast<gz::cIGZDrawContext*>(context);
    if (!ctx) return;
    ctx->Suspend();
    ctx->PopState();
    ctx->PopTransform();
    ctx->Release();
}

// Draws the window's own contents (PlotComposite 0x10016abd0): the paint message goes through the filters first
// (a consuming filter suppresses the paint); then the window paints via GZPaint inside its draw context with
// its children painted back to front. Children that would be hidden behind a "delayed plot" desktop-level child
// are deferred to the manager's plot strategy instead so the z order survives.
bool cGZWin::PlotComposite() {
    if ((flags_ & kWinFade) == 0) {
        if ((flags_ & kWinVisible) == 0) return false;
    } else {
        if ((flags_ & kWinVisible) == 0 && !fade_in_progress()) return false;
        fade_advance();
    }
    cGZMessage paint;
    paint.type = kGZMsgPaint;
    paint.param1 = paint.param2 = paint.param3 = 0;
    const bool wasDispatching = dispatching_;
    dispatching_ = true;
    bool consumed = false;
    for (cIGZWinMessageFilter* filter : filters_) {
        if (filter->DoMessage(this, paint)) { consumed = true; break; }
    }
    dispatching_ = wasDispatching;
    if (!wasDispatching) {
        // filters removed while dispatching are dropped now
        for (cIGZWinMessageFilter* pending : pendingRemove_) {
            pending->AddRef();
            auto it = std::find(filters_.begin(), filters_.end(), pending);
            if (it != filters_.end()) {
                (*it)->Release();
                filters_.erase(it);
            }
            pending->Release();
        }
        for (cIGZWinMessageFilter* pending : pendingRemove_) pending->Release();
        pendingRemove_.clear();
    }
    if (consumed) return true;

    if (!invalid_) {
        EnumChildren(0x22ba0121, add_delayedplot_windows, mgr_);
        return true;
    }
    if (privateBuffer_) {
        if (parent_) invalid_ = false;
        if ((flags_ & 0x60000) == 0x60000) fill_private_buffer_with_transparent_color();
    }
    void* ctx = BeginPaintFrame();
    GZPaint();
    if (privateBuffer_) flags_ |= kWinFlag400000;
    EndPaintFrame(ctx);

    ChildList* list = LockChildListRead();
    if (!GetFlag(kWinDesktop)) {
        std::vector<cRZRect> deferred;      // absolute areas of children whose plot was handed to the manager
        for (auto it = list->items.rbegin(); it != list->items.rend(); ++it) {
            cIGZWin* child = *it;
            if (!child->IsVisible()) continue;
            if (child->GetFlag(kWinDesktop)) {
                child->SetFlag(kWinPlotted, true);
                mgr_->AppendToPlotStrategy(child);
                deferred.push_back(child->GetAreaAbsolute());
                continue;
            }
            if (!deferred.empty()) {
                const cRZRect r = child->GetAreaAbsolute();
                bool overlaps = false;
                for (const cRZRect& d : deferred) {
                    if (d.left < r.right && r.left < d.right && d.top < r.bottom && r.top < d.bottom) { overlaps = true; break; }
                }
                if (overlaps) {
                    deferred.push_back(r);
                    child->SetFlag(kWinPlotted, true);
                    mgr_->AppendToPlotStrategy(child);
                    continue;
                }
            }
            child->Plot();
        }
    } else {
        // (the original walks the list back to front here too, and appends without a visibility test)
        for (auto it = list->items.rbegin(); it != list->items.rend(); ++it) {
            (*it)->SetFlag(kWinPlotted, true);
            mgr_->AppendToPlotStrategy(*it);
        }
    }
    UnlockChildListRead(list);
    return true;
}

// Copies the private buffer into the buffer the window draws to (PlotPresent 0x10016b49a). Through a draw
// context when the target supports one: blt with the window's shade as modulation colour (the private buffer's
// own fade shade when it has one), otherwise a plain locked blit.
bool cGZWin::PlotPresent() {
    if (!privateBuffer_) return true;
    cRZRect dest = bltArea_;
    IWinBuffer* target = nullptr;
    if (!GetFlag(kWinPlotted)) {
        if (parent_) target = static_cast<IWinBuffer*>(parent_->GetBufferToDrawTo());
    } else if (parent_) {
        target = WinGraphics() ? WinGraphics()->PrimaryBuffer() : nullptr;
        dest = absoluteArea_;
        SetFlag(kWinPlotted, false);
        if (!target) return true;
    }
    if (!target) target = WinGraphics() ? WinGraphics()->PrimaryBuffer() : nullptr;
    if (!target) return true;

    if (cIGZBufferDrawTarget* drawTarget = target->QueryDrawTarget()) {
        gz::cIGZDrawContext* ctx = drawTarget->LockContext();
        ctx->ClipReset();
        ctx->OffsetAbsolute(cRZPoint{0, 0});
        ctx->PushState();
        ctx->SetAlphaBlend(GetFlag(kWinFlag4));
        ctx->SetBltModulate(GetFlag(kWinFlag4));
        ctx->SetColor(GetFlag(kWinUseShadeColor) ? shadeColor_ : 0xffffffffu);
        std::uint8_t r, g, b, a;
        if (privateBuffer_->GetShadeColor(r, g, b, a)) {
            if (GetFlag(kWinUseShadeColor))
                ctx->SetColor((shadeColor_ & 0xffffffu) | (std::uint32_t(a) << 24));
            else
                ctx->SetColor(b | (std::uint32_t(g) << 8) | (std::uint32_t(r) << 16) | (std::uint32_t(a) << 24));
        }
        ctx->Blt(privateBuffer_->AsDrawSource(), drawArea_, dest);
        ctx->PopState();
        drawTarget->UnlockContext();
    } else if (target->Lock(kLockPresent)) {
        target->Blt(privateBuffer_, drawArea_, dest);
        target->Unlock(kLockPresent);
    }
    return true;
}

// ---- cGZFadeTmpWin -----------------------------------------------------------------------------------------------

cGZFadeTmpWin::cGZFadeTmpWin(cIGZWinMgr* mgr) : cGZWin(mgr) {
    auto* v = new cRZVariant();
    v->AddRef();
    SetParam(kFadeTmpKey, v);       // marks this window as a fade temporary (PrivateBuffer checks for it)
    v->Release();
}

// Takes over `source`'s private buffer: becomes a sibling of it (or a child of the main window when it has no
// parent), at the same place, and fades the buffer out over the source's fade-out period.
void cGZFadeTmpWin::StealBuffer(cIGZWin* source) {
    cIGZWin* parent = source->GetParentWin();
    if (!parent) {
        GetMainWindow()->ChildAdd(this);
        SetArea(source->GetAreaAbsolute());
    } else {
        parent->ChildAdd(this);
        SetArea(source->GetArea());
        MoveRelativeTo(source, false);
    }
    flags_ = (flags_ & 0xffd8ffdeu) | 0x230021u;
    privateBuffer_ = static_cast<IWinBuffer*>(source->GetPrivateBuffer());
    if (privateBuffer_) privateBuffer_->AddRef();
    SetBufferToDrawTo();
    int fadeIn = 0, fadeOut = 0;
    source->GetFadeEffectPeriod(fadeIn, fadeOut);
    set_fade_state(0xff, -fadeOut);
}

// While the fade runs it plots like a normal window and then invalidates; when it is over the window asks the
// manager to destroy it.
bool cGZFadeTmpWin::Plot() {
    if (!fade_in_progress()) {
        mgr_->DestroyWindow(this);
        return true;
    }
    const bool result = cGZWin::Plot();
    InvalidateSelfAndParents();
    return result;
}

bool cGZWin::Plot() {
    bool ok = PlotComposite() ? PlotPresent() : false;
    if (!usesOwnBuffer_) mgr_->Plot();
    return ok;
}

bool cGZWin::add_delayedplot_windows(cIGZWin*, std::uint32_t, void* child, void* manager) {
    auto* win = static_cast<cIGZWin*>(child);
    if (win->IsVisible()) {
        if (!win->GetFlag(kWinDesktop)) {
            win->EnumChildren(kGZIID_cIGZWin, add_delayedplot_windows, manager);
        } else {
            win->SetFlag(kWinPlotted, true);
            static_cast<cIGZWinMgr*>(manager)->AppendToPlotStrategy(win);
        }
    }
    return true;
}
