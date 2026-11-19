#include "sc4/WinLocationSaver.h"

#include <cstring>

#include "sc4/ServiceLocator.h"

bool cSLWinLocationSaver::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSLWinLocationSaver || iid == 1 || iid == kGZIID_cSLWinLocationSaver) {
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSLWinLocationSaver::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSLWinLocationSaver::Init() {
    if (!initialized_) {
        mainWindow_ = SL::spSC4->GetMainWindow();
        // the app's preferences (spSC4 vslot 0x98): SL::spPreferences is not published yet (cSC4App::Init calls this before
        // SL::OnAppInit1)
        const std::uint8_t* entries = SL::spSC4->GetPreferences() + kPreferencesOffset;
        for (int i = 0; i < kSavedWindowCount; ++i) {
            std::uint32_t triple[3];
            std::memcpy(triple, entries + i * 12, sizeof triple);
            if (triple[0] == 0) break;
            SaveWindowLocation(triple[0], static_cast<int>(triple[1]), static_cast<int>(triple[2]));
        }
        initialized_ = true;
    }
    return true;
}

bool cSLWinLocationSaver::Shutdown() {
    if (initialized_) {
        mainWindow_ = nullptr;
        if (SL::spPreferences) {
            auto it = locations_.begin();
            for (int i = 0; i < kSavedWindowCount; ++i) {
                std::uint32_t triple[3] = {0, 0, 0};
                if (it != locations_.end()) {
                    triple[0] = it->first;
                    triple[1] = static_cast<std::uint32_t>(it->second.x);
                    triple[2] = static_cast<std::uint32_t>(it->second.y);
                    ++it;
                }
                std::memcpy(SL::spPreferences + kPreferencesOffset + i * 12, triple, sizeof triple);
            }
        }
        initialized_ = false;
    }
    return true;
}

bool cSLWinLocationSaver::SaveWindowLocation(cIGZWin* window) {
    if (!window) return false;
    return SaveWindowLocation(window->GetID(), window->GetL(), window->GetT());
}

bool cSLWinLocationSaver::SaveWindowLocation(std::uint32_t id, int x, int y) {
    if (id != 0) locations_[id] = {x, y};
    return id != 0;
}

bool cSLWinLocationSaver::GetWindowRestoreLocation(cIGZWin* window, int& x, int& y, bool unused) {
    if (!window) return false;
    return GetWindowRestoreLocation(window->GetID(), x, y, unused);
}

bool cSLWinLocationSaver::GetWindowRestoreLocation(std::uint32_t id, int& x, int& y, bool) {
    if (id == 0) return false;
    auto it = locations_.find(id);
    if (it == locations_.end()) return false;
    x = it->second.x;
    y = it->second.y;
    return true;
}

// With `keepOnScreen` the saved place is dropped for (8, 8) when less than 8 pixels of the window would stay inside the
// parent.
bool cSLWinLocationSaver::RestoreWindowLocation(cIGZWin* window, bool keepOnScreen) {
    if (!window) return false;
    int x = 0, y = 0;
    if (!GetWindowRestoreLocation(window->GetID(), x, y, keepOnScreen)) return false;
    if (keepOnScreen) {
        cIGZWin* parent = window->GetParentWin();
        if (!parent) return true;                 // (the window is not moved when it has no parent)
        int parentWidth = parent->GetW(), parentHeight = parent->GetH();
        int width = window->GetW(), height = window->GetH();
        if (parentWidth - 8 < x || width + x < 8 || parentHeight - 8 < y || height + y < 8) {
            x = 8;
            y = 8;
        }
    }
    window->GZWinMoveTo(x, y);
    return true;
}

bool cSLWinLocationSaver::RestoreWindowLocation(std::uint32_t id, bool keepOnScreen) {
    if (id == 0) return false;
    cIGZWin* window = mainWindow_ ? mainWindow_->GetChildWindowFromIDRecursive(id) : nullptr;
    return window ? RestoreWindowLocation(window, keepOnScreen) : false;
}
