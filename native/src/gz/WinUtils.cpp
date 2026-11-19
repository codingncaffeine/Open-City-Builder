#include "gz/WinUtils.h"

// The GetChildAs... helpers hand back a counted child: each is released here, as in the original (Mac i386 0x00474a92..).
namespace nsGZWinUtils {

bool FindSiblingAs(cIGZWin* window, std::uint32_t id, std::uint32_t iid, void** out) {
    return window->GetParentWin()->GetChildAs(id, iid, out);
}

bool SetWindowVisible(cIGZWin* window, std::uint32_t id, bool visible, bool invalidate) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    if (!child) return false;
    if (child->GetFlag(kWinVisible) != visible) {
        child->SetFlag(kWinVisible, visible);
        if (invalidate) child->InvalidateSelfAndParents();
    }
    return true;
}

bool GetWindowVisible(cIGZWin* window, std::uint32_t id) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    return child && child->GetFlag(kWinVisible);
}

bool SetWindowEnabled(cIGZWin* window, std::uint32_t id, bool enabled) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    if (child) child->SetFlag(kWinEnabled, enabled);
    return child != nullptr;
}

bool SetWindowCaption(cIGZWin* window, std::uint32_t id, const cIGZString& caption) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    if (child) child->SetCaption(caption);
    return child != nullptr;
}

bool SetWindowNotificationTarget(cIGZWin* window, std::uint32_t id, cIGZWin* target) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    if (child) child->SetNotificationTarget(target);
    return child != nullptr;
}

bool sSetNotification(cIGZWin*, std::uint32_t, void* child, void* target) {
    auto* window = static_cast<cIGZWin*>(child);
    window->SetNotificationTarget(static_cast<cIGZWin*>(target));
    window->EnumChildren(kGZIID_cIGZWin, sSetNotification, target);
    return true;
}

void SetWindowNotificationTargetRecursive(cIGZWin* window, cIGZWin* target) {
    window->EnumChildren(kGZIID_cIGZWin, sSetNotification, target);
}

bool SetWindowNotificationTargetRecursive(cIGZWin* window, std::uint32_t id, cIGZWin* target) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    if (child) child->EnumChildren(kGZIID_cIGZWin, sSetNotification, target);
    return child != nullptr;
}

bool SetButtonToggleState(cIGZWin* window, std::uint32_t id, bool on) {
    gz::cIGZWinBtn* button = nullptr;
    if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&button))) return false;
    if (on) button->ToggleOn();
    else button->ToggleOff();
    button->Release();
    return true;
}

bool GetButtonToggleState(cIGZWin* window, std::uint32_t id) {
    gz::cIGZWinBtn* button = nullptr;
    if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&button))) return false;
    const bool on = button->IsOn();
    button->Release();
    return on;
}

void ExcludeButtonByRange(cIGZWin* window, std::uint32_t firstId, std::uint32_t count, std::uint32_t onId) {
    for (std::uint32_t id = firstId; id < firstId + count; ++id) SetButtonToggleState(window, id, onId == id);
}

bool SetEditText(cIGZWin* window, std::uint32_t id, const cIGZString& text) {
    gz::cIGZWinTextEdit* edit = nullptr;
    if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinTextEdit, reinterpret_cast<void**>(&edit))) return false;
    edit->SetText(text, false);
    edit->Release();
    return true;
}

bool SetSpinnerValue(cIGZWin* window, std::uint32_t id, int value) {
    gz::cIGZWinSpinner* spinner = nullptr;
    if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) return false;
    spinner->SetValue(value);
    spinner->Release();
    return true;
}

}  // namespace nsGZWinUtils
