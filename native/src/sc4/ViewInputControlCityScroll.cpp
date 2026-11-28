#include "sc4/ViewInputControlCityScroll.h"
#include "platform/Unwired.h"

#include <cmath>
#include <cstdlib>

namespace sc4 {
namespace {
ICityScrollHost* g_host = nullptr;
constexpr float kPi = 3.1415927f;
constexpr float kVerticalAngle[2] = {kPi * 0.5f, kPi * 1.5f};   // DAT_100a1c500, indexed by dy < 0
}  // namespace

ICityScrollHost* CityScrollHost() { return g_host; }
void SetCityScrollHost(ICityScrollHost* host) { g_host = host; }

bool cSC4ViewInputControlCityScroll::Init() {
    if (!initialized_) {
        id_ = 0x48e2a1f8;
        cursorId_ = 0xc2a676ac;
        moved_ = false;
        ReadTunableValues();
        cSC4ViewInputControl::Init();
        selfScrolling_ = false;   // vslot 0xc8; the native graphics system never scrolls by itself
        if (!selfScrolling_) SetCapture();
    }
    return true;
}

void cSC4ViewInputControlCityScroll::ReadTunableValues() {
    if (g_host) g_host->ReadTunables(deadZone_, fullSpeed_);
}

bool cSC4ViewInputControlCityScroll::Shutdown() {
    if (initialized_) {
        if (g_host) g_host->RemovePressPoint();
        if (!selfScrolling_) ReleaseCapture();
        if (g_host) g_host->ScrollStop();
        pressImageLoaded_ = false;
        cSC4ViewInputControl::Shutdown();
    }
    return true;
}

void cSC4ViewInputControlCityScroll::EndInput() {
    if (g_host) {
        g_host->ScrollStop();
        g_host->RemovePressPoint();
    }
    cSC4ViewInputControl::EndInput();
}

bool cSC4ViewInputControlCityScroll::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    if (!g_host || !g_host->RightButtonDown()) return OnMouseUpR(x, y, modifiers);   // the button went up unseen
    const int dx = x - pressX_, dy = y - pressY_;
    const int halfX = dx / 2, halfY = dy / 2;
    const std::uint32_t absX = static_cast<std::uint32_t>(std::abs(halfX)), absY = static_cast<std::uint32_t>(std::abs(halfY));
    if (fullSpeed_ < absX) velocityX_ = dx < 2 ? -static_cast<float>(fullSpeed_) : static_cast<float>(fullSpeed_);
    if (fullSpeed_ < absY) velocityY_ = dy < 2 ? -static_cast<float>(fullSpeed_) : static_cast<float>(fullSpeed_);
    if (absX <= fullSpeed_ && deadZone_ < absX) velocityX_ = static_cast<float>(halfX);
    if (absY <= fullSpeed_ && deadZone_ < absY) velocityY_ = static_cast<float>(halfY);
    if (absX <= fullSpeed_ && absX <= deadZone_) velocityX_ = 0;
    if (absY <= fullSpeed_ && absY <= deadZone_) velocityY_ = 0;
    const bool scrolling = deadZone_ < absX || deadZone_ < absY;
    // i386 0x0020687a: the cursor points the way the view scrolls (eight cursors), the centre one inside the dead zone
    constexpr std::uint32_t kCentre = 0xc2a676ac;
    std::uint32_t cursor = kCentre;
    const int dz = static_cast<int>(deadZone_);
    if (scrolling) {
        if (halfY < dz) {
            if (-dz < halfY) cursor = halfX < dz ? (-dz < halfX ? kCentre : 0x62a67694u) : 0xc2a676a4u;   // left / right
            else cursor = halfX < dz ? (-dz < halfX ? 0x62a67606u : 0x02a67691u) : 0x82a676a8u;           // up, up-left, up-right
        } else {
            cursor = halfX < dz ? (-dz < halfX ? 0x62a6769bu : 0xa2a67698u) : 0x22a676a0u;                // down, down-left, down-right
        }
    } else if (CursorId() != kCentre) {
        SetCursor(kCentre);
    }
    if (!moved_ && cursor != kCentre) moved_ = true;
    if (cursor != CursorId()) SetCursor(cursor);
    float speed = std::sqrt(static_cast<float>(halfY * halfY) + static_cast<float>(halfX) * static_cast<float>(halfX)) /
                  static_cast<float>(fullSpeed_ - deadZone_);
    if (speed > 1.0f) speed = 1.0f;
    float angle = 0.0f;
    if (scrolling) {
        const float slope = halfX != 0 ? static_cast<float>(halfY) / static_cast<float>(halfX) : 0.0f;
        if (dx >= 2) {
            angle = (dy > -2 && dy < 2) ? 0.0f : std::atan(slope);
        } else if (dx < -1) {
            angle = (dy > -2 && dy < 2) ? kPi : std::atan(slope) + kPi;
        } else {
            angle = kVerticalAngle[dy < -1 ? 1 : 0];
        }
    }
    g_host->SetScrolling(scrolling, angle, speed);
    return false;
}

}  // namespace sc4
