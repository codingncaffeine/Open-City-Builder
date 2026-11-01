// cSC4WinRCI — one bar of the city toolbar's RCI demand meter (14 functions at 0x100464bfe..0x1004653ae, object 0x1b8 bytes).
// The bar follows up to 16 demand channels (the ids set by SetDemandType): it sums their active demand minimum, maximum
// and value, and paints a bar from the middle of the window towards one end, scaled on a logarithmic curve.
//
// Object layout (the original's view order; the port puts the cGZWin first, like cSC4WinAuraBar):
//   +0x000 view cISC4WinRCI (iid 0x89e1574c, also answered for iid 1)   +0x008 cGZWin (iid 0x22ba0121)
//   +0x158 view cIGZMessageTarget2 (iid 0x452294aa)                      +0x160 int demand ids[16]
//   +0x1a0 int count   +0x1a4 int minimum sum   +0x1a8 int maximum sum   +0x1ac int value sum   +0x1b0 u32 colour
//
// Quirks kept:
//  * The three sums are truncated to int after every channel is added (float(sum) + channel value, then int()).
//  * RethinkBar reports "changed" for a missing demand simulator as false and leaves the sums alone; with one it zeroes
//    the sums first, and a channel whose id is 0 or negative, or whose demand the simulator does not know, adds nothing.
//  * GZPaint chooses the bar direction by the window shape: wider than tall draws a horizontal bar, otherwise a
//    vertical one. Positive value draws on the positive side of the middle, negative value on the other; a value of 0
//    draws nothing. The length is `ratio * middle * 0.5 + 0.5` with ratio = value/limit + log(value)/log(limit) (the
//    limit is the maximum sum for a positive value and the minimum sum for a negative one, both as magnitudes).
//    The positive horizontal bar's rectangle has its left edge past its right edge, as the original builds it.
//  * Init ignores the result of cGZWin::Init and always answers true; it listens for the demand-changed message
//    0x426840a0 only when the message server exists. DoMessage always answers false.
//  * SetDemandType copies `count` ids with no bound (a count over 16 runs into the fields behind the array); the port
//    keeps at most 16 (see WINRCI-1).
#pragma once
#include <cstdint>

#include "gz/Win.h"
#include "gz/cIGZMessage2.h"
#include "sc4/CommonIIDs.h"

namespace sc4 {

class cISC4DemandSimulator;

constexpr std::uint32_t kGZIID_cISC4WinRCI = 0x89e1574c;
constexpr std::uint32_t kWinRCIDemandChangedMessage = 0x426840a0;
constexpr int kWinRCIMaxChannels = 16;
constexpr std::uint32_t kWinRCIDemandKeyOffset = 0x20000;   // GetDemand(id, 0x20000)

class cISC4WinRCI : public cIGZUnknown {
public:
    virtual cIGZWin* AsIGZWin() = 0;                                      // [3] 0x18
    virtual void SetColor(std::uint32_t rgb) = 0;                        // [4] 0x20
    virtual void SetDemandType(const std::uint32_t* ids, int count) = 0;  // [5] 0x28
};

// SL::spDemandSim and SL::spMS2, the two services the bar reaches.
class IWinRCIHost {
public:
    virtual ~IWinRCIHost() = default;
    virtual cISC4DemandSimulator* DemandSimulator() = 0;   // SL::spDemandSim
    virtual cIGZMessageServer2* MessageServer() = 0;       // SL::spMS2 (+0x28 AddNotification, +0x30 RemoveNotification)
};
void SetWinRCIHost(IWinRCIHost* host);

class cSC4WinRCI : public cGZWin, public cISC4WinRCI, public cIGZMessageTarget2 {
public:
    explicit cSC4WinRCI(cIGZWinMgr* mgr);                                 // @ 0x100464bfe
    ~cSC4WinRCI() override;                                               // @ 0x100464c68 / 0x100464cde

    bool QueryInterface(std::uint32_t iid, void** out) override;         // @ 0x100464d4c
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }         // @ 0x100464db6
    std::uint32_t Release() override { return cGZWin::Release(); }       // @ 0x100464de0
    cIGZWin* AsIGZWin() override { return this; }                         // @ 0x100464e0a
    bool Init() override;                                                 // @ 0x100464e14
    bool Shutdown() override;                                             // @ 0x100464e78
    void SetColor(std::uint32_t rgb) override { color_ = rgb; }          // @ 0x100464ed8
    void SetDemandType(const std::uint32_t* ids, int count) override;    // @ 0x100464ee4
    bool DoMessage(cIGZMessage2* message) override;                       // @ 0x100465056
    bool GZPaint() override;                                              // @ 0x1004650d6

    bool RethinkBar();                                                    // @ 0x100464f14: true when a sum changed

    int Count() const { return count_; }
    int MinimumSum() const { return minimumSum_; }
    int MaximumSum() const { return maximumSum_; }
    int ValueSum() const { return valueSum_; }

private:
    std::int32_t demandIds_[kWinRCIMaxChannels] = {};   // +0x160
    std::int32_t count_ = 0;                              // +0x1a0
    std::int32_t minimumSum_ = 0;                         // +0x1a4
    std::int32_t maximumSum_ = 0;                         // +0x1a8
    std::int32_t valueSum_ = 0;                           // +0x1ac
    std::uint32_t color_ = 0;                             // +0x1b0
};

}  // namespace sc4
