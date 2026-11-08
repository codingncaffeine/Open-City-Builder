// cSC4NewsTickerWinProc — the news ticker's window procedure (18 functions at 0x10031b896..0x10031e0ec plus the
// compiler thunks; one of the five procedures cSC4View3DWin::AddGUI attaches). It owns
//
//  * the "news" advice list (the panel below the toolbar, iid 0xca1492a2) at +0x50, refilled by
//    updateBodyTextCtrl whenever the advisor system reports a new / changed advice,
//  * the "flipper" line at +0x58, a one-line scroller that slides each queued advice up by one pixel every 40 ms
//    (the timer posts 0xa344006 to itself; the 3-entry ring at +0x88 holds the lines on screen),
//  * the three toolbar buttons 0xca2aedcc / 0xca2aedcd / 0x4a231510 (show / hide / stop) and the ticker control
//    0xaa231508 that owns both lists,
//  * the cRZCallbackTimer at +0x18 (3000 ms between flipper messages).
//
// The window procedure is attached to the script window id 0xca2aedc0 (cIGZWinGen, created by AddGUI) and it
// also implements cIGZMessageTarget2: it listens for 0x8a09fa64 (advice added), 0x8a09fa65 (advice updated),
// 0x8a09fa67 (advisor removed), 0xca09fa6c (advice removed) while its window is attached.
//
// Layout (0xd0 bytes): +0x10 refcount, +0x14 initialised, +0x15 notifications registered, +0x18 the callback
// timer, +0x50 the advice list, +0x58 the flipper list, +0x60 std::list<cRZAutoRefCount<cISC4Advice>> (the
// ranked advice list), +0x78 the gen window, +0x80 flag (1), +0x81 ticker shown, +0x88 the 3-entry ring of
// advices on the flipper, +0xa0 byte (never touched again), +0xa4 flipper scroll (pixels), +0xac the flipper
// area (left, top, right, bottom), +0xbc how many ring entries are set, +0xc0 "a line already scrolled" flag,
// +0xc4 the flipper list needs a rebuild, +0xc8 the font's line height.
//
// Both lists are cSC4WinAdviceList windows the ticker's window script creates, reached through cIGZWinAdviceList.
#pragma once
#include <cstdint>
#include <list>

#include "gz/WinGen.h"
#include "gz/cIGZMessage2.h"
#include "gz/WinInterfaces.h"
#include "gz/WinProc.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZCallbackTimer.h"
#include "sc4/Advice.h"
#include "sc4/WinAdviceList.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4WinAdviceList = 0xca1492a2;   // the cSC4WinAdviceList view
constexpr std::uint32_t kGZIID_cIGZWinGen = 0x5386d516;
constexpr std::uint32_t kNewsTickerIIDMessageTarget2 = 0x452294aa;   // cIGZMessageTarget2

// The windows the ticker works on (all children of the ticker script window's parent).
constexpr std::uint32_t kNewsTickerWindowId = 0xca2aedc0;      // the cIGZWinGen AddGUI hands to Init
constexpr std::uint32_t kNewsTickerButtonToggle = 0xca2aedcc;  // show / hide the ticker
constexpr std::uint32_t kNewsTickerButtonToggleAlt = 0xca2aedcd;
constexpr std::uint32_t kNewsTickerButtonStop = 0x4a231510;    // hide and stop following the ticker
constexpr std::uint32_t kNewsTickerControlHost = 0xaa231508;   // the control that owns the two advice lists
constexpr std::uint32_t kNewsTickerAdviceListBody = 0x6a231531;
constexpr std::uint32_t kNewsTickerAdviceListFlipper = 0xaa12f33c;
constexpr std::uint32_t kNewsTickerResizeLabel = 0xca2aeec0;   // shortened to two line heights
constexpr std::uint32_t kNewsTickerImageBody = 0x14416264;     // the advice list's background image
constexpr std::uint32_t kNewsTickerImageFlipper = 0x14416265;
constexpr std::uint32_t kNewsTickerFontId = 0xaa0f4ab4;        // RZ::WCM()->SystemFont

// Messages.
constexpr std::uint32_t kNewsTickerAdviceAdded = 0x8a09fa64;
constexpr std::uint32_t kNewsTickerAdviceChanged = 0x8a09fa65;
constexpr std::uint32_t kNewsTickerAdvisorRemoved = 0x8a09fa67;
constexpr std::uint32_t kNewsTickerAdviceRemoved = 0xca09fa6c;
constexpr std::uint32_t kNewsTickerFlipperTick = 0xa344006;   // posted by NewsTickCallback
constexpr std::uint32_t kNewsTickerAdviceNewsOnly = 0x2000;    // cISC4Advice flag: not shown in the ticker

constexpr std::uint32_t kNewsTickerLineDelayMs = 3000;   // between two flipper lines
constexpr std::uint32_t kNewsTickerScrollMs = 0x28;      // per scrolled pixel

// cSC4WinAdviceList as the ticker uses it: the recovered list's own interface (iid 0xca1492a2, the same ten slots).
using IC4WinAdviceList = cIGZWinAdviceList;

class cSC4NewsTickerWinProc : public cIGZMessageTarget2, public gz::cIGZWinProc {
public:
    cSC4NewsTickerWinProc();
    ~cSC4NewsTickerWinProc();

    // cIGZMessageTarget2 / cIGZWinProc (both share the one count at +0x10)
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;

    // The original's class virtuals (primary vtable slots 7..13).
    virtual void ShowTicker();                          // @ 0x10031c644
    virtual void HideTicker();                          // @ 0x10031c760
    virtual void RemoveMessage(cISC4Advice* advice);    // @ 0x10031c7e0
    virtual void AddMessage(cISC4Advice* advice);       // @ 0x10031c808
    virtual void updateBodyTextCtrl();                  // @ 0x10031c9a0
    virtual void DoFlipperMessage();                    // @ 0x10031db66
    virtual void RemoveInactive();                      // @ 0x10031db20

    bool Init(gz::cIGZWinGen* window);                      // @ 0x10031bc44
    bool Shutdown();                                    // @ 0x10031c016
    void RegisterMessageNotification(bool on);          // @ 0x10031c0f0

    // The callback timer's function (0x10031bf54): posts kNewsTickerFlipperTick to `context`.
    static void NewsTickCallback(void* context);

    // Hooks for the harness.
    bool Initialised() const { return initialised_; }
    bool NotificationsRegistered() const { return notificationsRegistered_; }
    bool TickerVisible() const { return tickerVisible_; }
    gz::cIGZWinGen* Window() const { return window_; }
    IC4WinAdviceList* AdviceList() const { return adviceList_; }
    IC4WinAdviceList* FlipperList() const { return flipperList_; }
    const std::list<cRZAutoRefCount<cISC4Advice>>& Messages() const { return messages_; }
    std::uint32_t FlipperScroll() const { return flipperScroll_; }
    std::uint32_t RingCount() const { return ringCount_; }
    int LineHeight() const { return lineHeight_; }
    const cRZRect& FlipperArea() const { return flipperArea_; }

    // The news-flipper line itself (0x10031d224); public for the harness, not a virtual.
    void DisplayNewsFlipperLine(cISC4Advice* advice);

private:
    std::uint32_t refCount_ = 0;                       // +0x10
    bool initialised_ = false;                         // +0x14
    bool notificationsRegistered_ = false;             // +0x15
    cRZCallbackTimer timer_;                           // +0x18 (0x38 bytes)
    cRZAutoRefCount<IC4WinAdviceList> adviceList_;     // +0x50
    cRZAutoRefCount<IC4WinAdviceList> flipperList_;    // +0x58
    std::list<cRZAutoRefCount<cISC4Advice>> messages_; // +0x60
    cRZAutoRefCount<gz::cIGZWinGen> window_;               // +0x78
    [[maybe_unused]] std::uint16_t flags_ = 1;         // +0x80 (the second byte is +0x81)
    bool tickerVisible_ = false;                       // +0x81
    cRZAutoRefCount<cISC4Advice> ring_[3];             // +0x88 / +0x90 / +0x98
    [[maybe_unused]] bool fieldA0_ = false;            // +0xa0 (set to 0 by the ctor, never read)
    std::uint32_t flipperScroll_ = 0;                  // +0xa4 pixels the flipper area has moved up
    [[maybe_unused]] std::uint32_t fieldA8_ = 0;       // +0xa8 (never read by recovered code)
    cRZRect flipperArea_{};                            // +0xac
    std::uint32_t ringCount_ = 0;                      // +0xbc ring entries set
    std::uint32_t flipperScrolled_ = 0;                // +0xc0 a line has been scrolled already
    std::uint32_t rebuildPending_ = 0;                 // +0xc4 rebuild the flipper list
    int lineHeight_ = 0;                               // +0xc8
};

}  // namespace sc4
