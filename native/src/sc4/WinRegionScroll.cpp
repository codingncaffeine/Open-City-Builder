#include "sc4/WinRegionScroll.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>

namespace sc4 {

namespace {
IRegionScrollHost* g_host = nullptr;
// The 9 arrow cursors, indexed [row * 3 + column] where each axis is 0 left/up, 1 centre, 2 right/down (table @ 0x100a1b670).
constexpr std::uint32_t kCursorMap[9] = {0x02a67691, 0x62a67606, 0x82a676a8, 0x62a67694, 0xc2a676ac,
                                         0xc2a676a4, 0xa2a67698, 0x62a6769b, 0x22a676a0};
int Sign(float v) { return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0); }
}  // namespace

IRegionScrollHost* RegionScrollHost() { return g_host; }
void SetRegionScrollHost(IRegionScrollHost* host) { g_host = host; }

cSC4WinRegionScroll::cSC4WinRegionScroll(cIGZWinMgr* mgr) : cGZWin(mgr) {}

cSC4WinRegionScroll::~cSC4WinRegionScroll() {
    cSC4WinRegionScroll::Shutdown();   // the destructor calls the class's own Shutdown (non-virtually)
}

bool cSC4WinRegionScroll::Init() {
    if (!get_init_flag()) {
        cGZWin::Init();
        cursorId_ = kRegionScrollCenterCursor;
        SetFlag(0x10000, false);
        SetFlag(0x8000, false);
    }
    return true;
}

bool cSC4WinRegionScroll::Shutdown() {
    if (get_init_flag()) {
        if (originImage_) {
            originImage_->Release();
            originImage_ = nullptr;
        }
        cGZWin::Shutdown();
    }
    return true;
}

namespace { bool ScrollDebug() { static const bool on = std::getenv("SC4_SCROLLDEBUG") != nullptr; return on; } }

bool cSC4WinRegionScroll::GZOnMouseDownR(int x, int y, std::uint32_t) {
    if (ScrollDebug()) std::fprintf(stderr, "region scroll: down %d,%d (%p)\n", x, y, static_cast<void*>(this));
    if (cIGZWin* marker = GetChildWindowFromID(kRegionScrollArrowWindowId)) ChildDelete(marker);
    SetFlag(0x200000, false);
    mgr_->GZSetCapture(this);
    SetMouseScroll(x, y);
    return true;
}

void cSC4WinRegionScroll::SetMouseScroll(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    mouseScrolling_ = true;
    if (!originImage_) {
        if (g_host) originImage_ = g_host->LoadScrollOriginImage();
        if (originImage_) {
            originImage_->SetTransparency(originImage_->MakeNativeColor(0xff, 0, 0xff));
        }
        if (!originImage_) return;
    }
    const cRZRect* b = originImage_->GetBounds();
    const int left = b->left, top = b->top, right = b->right, bottom = b->bottom;
    cIGZWin* marker = g_host ? g_host->CreateImageWindow(originImage_) : nullptr;
    if (!marker) return;
    // The marker is centred on the pointer.
    const int dx = -((right - left) / 2), dy = -((bottom - top) / 2);
    marker->SetArea(dx + left + mouseX_, dy + top + mouseY_, right + dx + mouseX_, bottom + dy + mouseY_);
    marker->SetFlag(2, false);          // disabled
    marker->SetFlag(0x200000, true);    // ignores the cursor
    marker->SetFlag(4, true);
    marker->ShowWindow();
    if (ChildAdd(marker)) {
        ChildToFront(marker);
    } else {
        GetWindowManager()->DestroyWindow(marker);   // mgr vslot 0xb8
    }
    marker->Release();
}

bool cSC4WinRegionScroll::GZOnMouseUpR(int x, int y, std::uint32_t) {
    if (ScrollDebug()) std::fprintf(stderr, "region scroll: up %d,%d (%p)\n", x, y, static_cast<void*>(this));
    if (cIGZWin* marker = GetChildWindowFromID(kRegionScrollArrowWindowId)) ChildDelete(marker);
    mouseScrolling_ = false;
    mouseVector_[0] = mouseVector_[1] = 0.0f;
    mgr_->GZReleaseCapture(this);
    SetFlag(0x200000, true);
    return true;
}

void cSC4WinRegionScroll::SetCursorByID(std::uint32_t id) {
    cIGZCursorManager* cursors = mgr_->GetCursorManager();   // RZ::WM() vslot 0x100 in the original
    if (!cursors) return;
    if (cIGZCursor* cursor = cursors->LoadCursor(id)) {
        cursorId_ = id;
        cGZWin::SetCursor(cursor, true);
    }
}

bool cSC4WinRegionScroll::GZOnMouseMove(int x, int y, std::uint32_t) {
    if (ScrollDebug()) std::fprintf(stderr, "region scroll: move %d,%d (scrolling %d, from %d,%d)\n", x, y, int(mouseScrolling_), mouseX_, mouseY_);
    const float dx = static_cast<float>(x - mouseX_);
    const float dy = static_cast<float>(y - mouseY_);
    const float dist2 = dy * dy + dx * dx;
    std::uint32_t cursor;
    if (static_cast<float>(deadZone_ * deadZone_) <= dist2) {
        const float dist = std::sqrt(dist2);
        const float capped = std::min(static_cast<float>(maxDistance_), dist);
        const float speed = std::max(1.0f, (capped - static_cast<float>(deadZone_)) / speedDivisor_);
        mouseVector_[0] = (speed / dist) * dx;
        mouseVector_[1] = (speed / dist) * dy;
        const float dz = static_cast<float>(static_cast<int>(deadZone_)), ndz = static_cast<float>(-static_cast<int>(deadZone_));
        const int col = (dz <= dx ? 1 : 0) + (ndz <= dx ? 1 : 0);
        const int row = (dz <= dy ? 1 : 0) + (ndz <= dy ? 1 : 0);
        cursor = kCursorMap[row * 3 + col];
    } else {
        mouseVector_[0] = mouseVector_[1] = 0.0f;
        cursor = kRegionScrollCenterCursor;
        SetCursorByID(cursor);   // quirk kept: set twice in the dead zone
    }
    SetCursorByID(cursor);
    return false;
}

bool cSC4WinRegionScroll::GZPaint() {
    if (!timer_.IsTimerRunning()) {
        timer_.Start();
        return true;
    }
    const std::int32_t elapsed = static_cast<std::int32_t>(timer_.GetElapsedTimeU32());
    timer_.Restart();
    float fx = edgeVector_[0] + mouseVector_[0];
    float fy = edgeVector_[1] + mouseVector_[1];
    if (g_host && g_host->EdgeScrollEnabled() && g_host->CursorInGameWindow()) {
        int mx, my;
        mgr_->GetCursorPos(this, mx, my);   // mgr vslot 0x110
        if (GetL() + 1 < mx) {
            if (GetR() - 1 <= mx) fx += edgeSpeed_;
        } else {
            fx -= edgeSpeed_;
        }
        if (GetT() + 1 < my) {
            if (GetB() - 1 <= my) fy += edgeSpeed_;
        } else {
            fy -= edgeSpeed_;
        }
    }
    const float seconds = static_cast<float>(elapsed) / 1000.0f;
    float outX = fx, outY = fy;
    if (fx != 0.0f || fy != 0.0f) {
        velocity_[0] = fx;
        velocity_[1] = fy;
        decay_[0] = fx * decayFactor_;
        decay_[1] = fy * decayFactor_;
    } else {
        // Coast: the pre-decay velocity is what gets sent this frame (quirk kept).
        const float ox = velocity_[0], oy = velocity_[1];
        velocity_[0] = ox - decay_[0] * seconds;
        velocity_[1] = oy - seconds * decay_[1];
        if (!(Sign(velocity_[0]) == Sign(ox) && Sign(velocity_[1]) == Sign(oy))) {
            decay_[0] = decay_[1] = 0.0f;
            velocity_[0] = velocity_[1] = 0.0f;
        }
        outX = ox;
        outY = oy;
    }
    if (outX != 0.0f || outY != 0.0f) {
        const float sx = outX * seconds, sy = seconds * outY;
        if (!scrolling_) {
            scrolling_ = true;
            PostMsg(notify_, kRegionScrollMsgStart, 0, 0, 0);
        }
        PostMsg(notify_, kRegionScrollMsgDelta, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<int>(sx * 256.0f))),
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<int>(sy * 256.0f))), 0);
    } else if (scrolling_) {
        PostMsg(notify_, kRegionScrollMsgStop, 0, 0, 0);
        scrolling_ = false;
    }
    return true;
}

}  // namespace sc4
