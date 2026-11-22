// cSC4MySimBalloonWinProc (i386 0x0047bed8..0x0047d0ac, object size 0x80): the speech balloon over a MySim. It owns a
// generic window (cGZWinGen, a child of the city view) and runs a small state machine on a self-posted tick message:
// hidden (4) -> fading in (1) -> shown (3) -> fading out (2) -> hidden, with 5 = disabled while the sim is not in the
// city. Each new balloon takes the sim's next event balloon advice, else its next regular one; the advice's icon
// buffers are blitted into the window's private buffer, the first one at the bottom center and the others centered
// on it. The balloon follows the sim's screen position and starts the advice's sound with the advice's chance.
//
// Original layout: +0x10 state, +0x14 current advice, +0x18 the sim, +0x1c the window, +0x20 icon buffers,
// +0x2c icons need painting, +0x30 cRZTimer (milliseconds), +0x44 fade, +0x48 shown, +0x4c hidden durations,
// +0x50 initialized, +0x51 a balloon sound plays.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZTimer.h"
#include "sc4/MySim.h"

namespace gz { class cIGZWinGen; }

namespace sc4 {

constexpr std::uint32_t kMessageMySimBalloonTick = 0xcb7cb509;    // the self-posted tick
constexpr std::uint32_t kMySimBalloonWindowId = 0x4c30e4fa;
constexpr std::uint32_t kWinProcMsgPaint = 0xa2bf8ace;            // the window asks its proc to paint

// What the balloon reaches through the SL:: globals of the original.
class IMySimBalloonHost {
public:
    virtual ~IMySimBalloonHost() = default;
    virtual gz::cIGZWinGen* CreateWinGen() = 0;                      // GZCOM 0x4386d516 / 0x5386d516 (one reference)
    virtual cIGZWin* View3DWin() = 0;                                // SL::spView3DWin (borrowed)
    virtual cIGZMessageServer2* Messages() = 0;                      // SL::spMS2
    virtual double RandomPercent() = 0;                              // spCity 0x144 -> 0x24 RandomDoubleRangeUniform(0, 100)
    // The sound system (cRZSysServPtr<cIGZSndSys>); its event 1 starts the balloon sound for the sim, 0xca45190f
    // places it, 2 stops it.
    virtual bool SoundSystemPresent() = 0;
    virtual void StartBalloonSound(std::uint32_t soundId, cISC4MySim* sim) = 0;
    virtual void PlaceBalloonSound(cISC4MySim* sim, int x, int y, int z) = 0;
    virtual void StopBalloonSound(cISC4MySim* sim, bool immediate) = 0;
};
IMySimBalloonHost* MySimBalloonHost();
void SetMySimBalloonHost(IMySimBalloonHost* host);

class cSC4MySimBalloonWinProc final : public gz::cIGZWinProc, public cIGZMessageTarget2, public IMySimBalloonWinProc {
public:
    explicit cSC4MySimBalloonWinProc(cISC4MySim* sim);                // @ 0x0047ceb8
    ~cSC4MySimBalloonWinProc();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override;

    // IMySimBalloonWinProc
    bool Init() override;                                              // @ 0x0047c402
    bool Shutdown() override;                                          // @ 0x0047bff0
    void SetHiddenDuration(int value) override { hidden_ = value; }    // +0x4c
    void SetShownDuration(int value) override { shown_ = value; }      // +0x48
    void SetFadeDuration(int value) override { fade_ = value; }        // +0x44

    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x0047c80c
    bool DoMessage(cIGZMessage2* message) override;                          // @ 0x0047cc1a

    int State() const { return state_; }
    cISC4MySimBalloonAdvice* CurrentAdvice() const { return advice_.get(); }
    std::size_t IconCount() const { return icons_.size(); }
    gz::cIGZWinGen* Window() const { return window_; }

private:
    void tick_self(cIGZMessage2* message);                             // @ 0x0047bf56
    bool check_enabled();                                              // @ 0x0047c064
    void stop_balloon_sound(bool immediate);                           // @ 0x0047c102
    void start_balloon_sound();                                        // @ 0x0047c188
    void on_paint();                                                   // @ 0x0047c608
    bool set_new_balloon();                                            // @ 0x0047c93c
    cIGZWin* Win() const;

    std::uint32_t refs_ = 0;
    int state_ = 5;                                                    // +0x10
    cRZAutoRefCount<cISC4MySimBalloonAdvice> advice_;                  // +0x14
    cISC4MySim* sim_ = nullptr;                                        // +0x18 (counted)
    gz::cIGZWinGen* window_ = nullptr;                                 // +0x1c (counted)
    std::vector<cRZAutoRefCount<cIGZBuffer>> icons_;                   // +0x20
    bool dirty_ = false;                                               // +0x2c
    cRZTimer timer_{cRZTimer::kMilliseconds};                          // +0x30
    int fade_ = 300;                                                   // +0x44
    int shown_ = 0x9c4;                                                // +0x48
    int hidden_ = 300;                                                 // +0x4c
    bool initialized_ = false;                                         // +0x50
    bool soundPlaying_ = false;                                        // +0x51
};

}  // namespace sc4
