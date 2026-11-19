// cSC4ToolTipManager (Mac i386 0x0051ce5c..0x00524e40, iid 0x22c010cd): the application's tool tip service. It listens on the
// message server (RZ::MS()) for the tip messages the buttons send (param1: the gz::BtnTipRequest; 0x22c010d0 show, 0x22c010d1 update, 0x22c010d2 hide,
// 0x22c010d8 hide all) and shows the tip in a callout box under the main window; its timer (750 ms) posts 0xca56dd76, on which
// the box follows the pointer and comes to the front.
//
// - the tip text is split at the first "|": with the Expanded Tool Tips preference (SL::spPreferences +0xf0a) on and both halves
//   present, the box shows a title and a body on the large skin (0x14416190, margins 8,8, avoidance +-28); otherwise one line
//   (the part before "|" when there is one, else the whole text) on the small skin (0x14416192, margins 5,3, avoidance -4..20);
// - the box appears one timer period after the request, then follows the pointer every period; a text with "ESRB" in its first
//   part shows at once;
// - 0x22c010d5 / 0x22c010d6 are registered but not handled.
#pragma once
#include <cstdint>

#include "gz/cGZMessageServer.h"
#include "rz/cRZCallbackTimer.h"

class cIGZBuffer;
class cIGZFont;
namespace sc4 { class cISC4WinCalloutBox; }

constexpr std::uint32_t kGZIID_cIGZWinToolTipMgr = 0x22c010cd;
constexpr std::uint32_t kMsgToolTipShow = 0x22c010d0, kMsgToolTipUpdate = 0x22c010d1, kMsgToolTipHide = 0x22c010d2,
                        kMsgToolTipHideAll = 0x22c010d8, kMsgToolTipTimer = 0xca56dd76;

class cIGZWinToolTipMgr : public cIGZMessageTarget {};

class cSC4ToolTipManager final : public cIGZWinToolTipMgr {
public:
    cSC4ToolTipManager() = default;                                // @ 0x00524cfa
    ~cSC4ToolTipManager();                                         // @ 0x00524d92

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x0051ce5c
    std::uint32_t AddRef() override { return ++refCount_; }        // @ 0x0051ce94
    std::uint32_t Release() override;                              // @ 0x00524e40

    // The font and the two flags (the app passes RZ::WCM()'s system font 0xa2cae9df, 3, 1) are not read.
    bool Init(cIGZFont* font, signed char a, signed char b);       // @ 0x0051d75e
    bool Shutdown();                                               // @ 0x0051d2d0
    bool DoMessage(cGZMessage& message) override;                  // @ 0x0051f898

    static void static_tip_timer_callback(void*);                  // @ 0x0051cea6

private:
    std::uint32_t refCount_ = 0;              // +4
    bool initialized_ = false;                // +8
    bool showing_ = false;                    // +9
    sc4::cISC4WinCalloutBox* box_ = nullptr;  // +0xc (held)
    cIGZBuffer* expandedImage_ = nullptr;     // +0x10 (held)
    cIGZBuffer* simpleImage_ = nullptr;       // +0x14 (held)
    cRZCallbackTimer timer_;                  // +0x18
};
