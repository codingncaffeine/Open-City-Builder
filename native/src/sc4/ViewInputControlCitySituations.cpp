#include "sc4/ViewInputControlCitySituations.h"


namespace sc4 {

namespace {
ICitySituationsToolHost* g_host = nullptr;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2Id = 0x452294aa;
constexpr std::uint32_t kMessageViewChanged = 0xa6b79602;
constexpr std::uint32_t kMessageViewChangedB = 0xa6b79612;
constexpr std::uint32_t kMessageIndicatorsReset = 0x06cdb65b;
constexpr std::uint32_t kCitySituationsToolCursor = 0x43401c71;
}  // namespace
ICitySituationsToolHost* CitySituationsToolHost() { return g_host; }
void SetCitySituationsToolHost(ICitySituationsToolHost* host) { g_host = host; }

cSC4ViewInputControlCitySituations::cSC4ViewInputControlCitySituations() {
    id_ = kCitySituationsToolId;
    cursorId_ = kCitySituationsToolCursor;
}

bool cSC4ViewInputControlCitySituations::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZMessageTarget2Id) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cSC4ViewInputControl::QueryInterface(iid, out);
}

std::uint32_t cSC4ViewInputControlCitySituations::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

bool cSC4ViewInputControlCitySituations::Init() {
    if (initialized_) return true;
    cSC4ViewInputControl::Init();
    if (g_host) {
        managers_ = g_host->HasManagers();
        g_host->Listen(static_cast<cIGZMessageTarget2*>(this), kMessageViewChanged, true);
    }
    return true;
}

bool cSC4ViewInputControlCitySituations::Shutdown() {
    if (!initialized_) return true;
    if (g_host) g_host->Listen(static_cast<cIGZMessageTarget2*>(this), kMessageViewChanged, false);
    managers_ = false;
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4ViewInputControlCitySituations::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message ? message->GetType() : 0;
    if (type == kMessageViewChanged || type == kMessageViewChangedB || type == kMessageIndicatorsReset) {
        if (hovered_ != 0 && g_host && managers_ && g_host->HasDispatchView()) {
            g_host->LockIndicator(hovered_, false);
            g_host->SetActiveIndicator(0, 1);
        }
        hovered_ = 0;
        pressed_ = 0;
    }
    return true;
}

bool cSC4ViewInputControlCitySituations::OnMouseDownL(int x, int y, std::uint32_t) {
    if (!g_host || !g_host->InSurfaceView()) return false;
    pressed_ = 0;
    if (!managers_ || !g_host->HasDispatchView()) return false;
    const std::uint32_t handle = g_host->FindIndicatorHandle(x, y);
    if (handle == 0) return false;
    g_host->SetActiveIndicator(handle, 2);
    pressed_ = handle;
    return true;
}

bool cSC4ViewInputControlCitySituations::OnMouseUpL(int x, int y, std::uint32_t) {
    const bool view = g_host && g_host->InSurfaceView() && managers_ && g_host->HasDispatchView();
    const std::uint32_t handle = view ? g_host->FindIndicatorHandle(x, y) : 0;
    if (handle != 0) g_host->SetActiveIndicator(handle, 1);
    cISC4Occupant* occupant = (handle != 0 && handle == pressed_) ? g_host->ActiveIndicatorOccupant() : nullptr;
    if (!occupant) {
        pressed_ = 0;
        return false;
    }
    g_host->InitiateSituation(occupant);
    return true;
}

bool cSC4ViewInputControlCitySituations::OnMouseMove(int x, int y, std::uint32_t) {
    if (g_host && managers_ && g_host->HasDispatchView()) {
        const std::uint32_t handle = g_host->FindIndicatorHandle(x, y);
        if (hovered_ == 0) {
            g_host->LockIndicator(handle, true);
            hovered_ = handle;
            g_host->SetActiveIndicator(handle, 3);
        } else if (handle != hovered_) {
            g_host->LockIndicator(hovered_, false);
            hovered_ = 0;
        }
    }
    return false;
}

}  // namespace sc4
