// cSC4ViewInputControlCityScroll (0x1002c13a2..0x1002c1b8a, 0x60 bytes): the tool the 3D view pushes while the right
// mouse button is held - the offset of the mouse from where the button went down scrolls the city in that direction,
// faster the further it is, with a dead zone around the press point.
//
// Layout (from +0x3c): +0x3c / +0x40 the press point, +0x44 / +0x48 the clamped half offsets, +0x4c "has moved out of
// the dead zone", +0x4d "the graphics system scrolls by itself" (no capture then), +0x50 the press-point overlay window,
// +0x58 dead zone (12) and +0x5c full-speed radius (80) in half pixels (tuning exemplar 0x690f693f / 0xe90f6c84,
// properties 0xe90f5de4 and 0xa90f5dec). Id 0x48e2a1f8, cursor 0xc2a676ac.
//
// Seams (ICityScrollHost): GetAsyncKeyState(VK_RBUTTON), the tuning exemplar and the view's SetScrolling / ScrollStop (view vslots
// 0x108 / 0x110). Not ported: the eight direction cursors OnMouseMove switches between and the press-point overlay window
// SetMouseScroll creates (and Shutdown / EndInput remove) - both are cursor art only.
// The angle for a purely vertical offset is DAT_100a1c500[dy < 0], taken as {pi / 2, 3 pi / 2} (screen y grows down).
#pragma once
#include "sc4/ViewInputControl.h"
#include "platform/Unwired.h"

namespace sc4 {

class ICityScrollHost {
public:
    virtual ~ICityScrollHost() = default;
    virtual bool RightButtonDown() = 0;                                    // GetAsyncKeyState(2) != 0
    virtual bool ReadTunables(std::uint32_t& deadZone, std::uint32_t& fullSpeed) = 0;   // false: keep the defaults
    virtual void SetScrolling(bool scrolling, float angle, float speed) = 0;   // the view's vslot 0x108
    virtual void ScrollStop() = 0;                                              // the view's vslot 0x110
};
ICityScrollHost* CityScrollHost();
void SetCityScrollHost(ICityScrollHost* host);

class cSC4ViewInputControlCityScroll final : public cSC4ViewInputControl {
public:
    cSC4ViewInputControlCityScroll() = default;   // @ 0x1002c13a2

    bool Init() override;                                                   // @ 0x1002c13e4
    bool Shutdown() override;                                               // @ 0x1002c14c8
    void EndInput() override;                                               // @ 0x1002c1556
    bool OnMouseUpR(int, int, std::uint32_t) override { EndInput(); return true; }   // @ 0x1002c15b6
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;       // @ 0x1002c15c8
    bool IsSelfScrollingView() override { return true; }                    // @ 0x1002c1b6c
    void SetMouseScroll(int x, int y) {                                     // @ 0x1002c1930 (overlay not ported)
        pressX_ = x;
        pressY_ = y;
        SC4_UNWIRED("cSC4ViewInputControlCityScroll::SetMouseScroll: the press-point overlay window is not created");
    }

private:
    void ReadTunableValues();                                               // @ 0x1002c143a

    int pressX_ = 0, pressY_ = 0;            // +0x3c / +0x40
    float velocityX_ = 0, velocityY_ = 0;    // +0x44 / +0x48
    bool moved_ = false;                     // +0x4c
    bool selfScrolling_ = false;             // +0x4d
    std::uint32_t deadZone_ = 12;            // +0x58
    std::uint32_t fullSpeed_ = 80;           // +0x5c
};

}  // namespace sc4
