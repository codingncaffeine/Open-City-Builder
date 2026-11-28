// cSC4WinRegionScroll (12 functions, 0x100234e56..0x1002354de + 0x1002353c4): the transparent window over the region view that
// scrolls it. Two sources feed one scroll vector every paint:
//  * holding the right mouse button shows a scroll-origin image where the button went down; the pointer's distance from that point
//    (dead zone 10, capped at 32, divided by 3, at least 1) times the direction becomes the speed (cursor picks one of 9 arrows);
//  * with the edge-scroll preference on, a pointer at the window's border adds +/-300 (the +0x17c speed) along that axis.
// While the vector is non-zero it posts 0xaaa1cdf2 (dx*256, dy*256) to the notification target (0x8a4bac53 first, 0x8a4bac5b when
// it stops); when input ends the last velocity coasts down at 5x its last value per second.
#pragma once
#include "gz/Win.h"
#include "rz/cRZTimer.h"

namespace sc4 {

constexpr std::uint32_t kRegionScrollArrowWindowId = 0x48e945b4;
constexpr std::uint32_t kRegionScrollMsgStart = 0x8a4bac53, kRegionScrollMsgStop = 0x8a4bac5b, kRegionScrollMsgDelta = 0xaaa1cdf2;
constexpr std::uint32_t kRegionScrollCenterCursor = 0xc2a676ac;

// What the scroll window reaches for outside itself.
class IRegionScrollHost {
public:
    virtual ~IRegionScrollHost() = default;
    virtual bool EdgeScrollEnabled() = 0;       // SL::spPreferences +0xf09
    virtual bool CursorInGameWindow() = 0;      // graphic system service (0xc416025c / 0x73283c) vslot 0x60
    // cSLBuffer(0x856ddbac, 0x46a006b0, 0x094ac89a, 1, 0): the origin marker image; the caller owns one reference. May be null.
    virtual IWinBuffer* LoadScrollOriginImage() = 0;
    // RZ::WCM() vslot 0x78 (0x48e945b4, image, 1): a bitmap window showing the image; the caller owns one reference. May be null.
    virtual cIGZWin* CreateImageWindow(IWinBuffer* image) = 0;
};
IRegionScrollHost* RegionScrollHost();
void SetRegionScrollHost(IRegionScrollHost* host);

class cSC4WinRegionScroll : public cGZWin {
public:
    explicit cSC4WinRegionScroll(cIGZWinMgr* mgr);     // @ 0x100234e56
    ~cSC4WinRegionScroll() override;                   // @ 0x100234f16 (+ 0x100234f74 / 0x100234f7e)

    bool Init() override;                              // @ 0x100234fac
    bool Shutdown() override;                          // @ 0x100235000
    bool GZOnMouseDownR(int x, int y, std::uint32_t modifiers) override;   // @ 0x100235040
    bool GZOnMouseUpR(int x, int y, std::uint32_t modifiers) override;     // @ 0x100235306
    bool GZOnMouseMove(int x, int y, std::uint32_t modifiers) override;    // @ 0x1002353c4
    bool GZPaint() override;                           // @ 0x1002354de

    void SetMouseScroll(int x, int y);                 // @ 0x1002350b4
    void SetCursorByID(std::uint32_t id);              // @ 0x100235372

    // The second speed source at +0x174 / +0x178, written by cSC4WinRegionScreen::ProcessCommand's scroll commands
    // (0x10023a2f1..0x10023a54d) and by the tuning values below (+0x190 / +0x194 / +0x19c, 0x10023904e).
    void SetEdgeVector(float x, float y) { edgeVector_[0] = x; edgeVector_[1] = y; }
    float EdgeVectorX() const { return edgeVector_[0]; }   // +0x174
    float EdgeVectorY() const { return edgeVector_[1]; }   // +0x178
    float EdgeSpeed() const { return edgeSpeed_; }        // +0x17c
    void SetTunables(std::uint32_t deadZone, std::uint32_t maxDistance, float speedDivisor) {
        deadZone_ = deadZone;
        maxDistance_ = maxDistance;
        speedDivisor_ = speedDivisor;
    }

    // test access
    float VelocityX() const { return velocity_[0]; }
    float VelocityY() const { return velocity_[1]; }
    bool MouseScrolling() const { return mouseScrolling_; }   // test / kit accessor
    float MouseVectorX() const { return mouseVector_[0]; }
    float MouseVectorY() const { return mouseVector_[1]; }
    std::uint32_t CursorId() const { return cursorId_; }

private:
    std::uint32_t cursorId_ = 0;       // +0x150
    int mouseX_ = 0, mouseY_ = 0;      // +0x154 / +0x158: where the right button went down
    float velocity_[2] = {0, 0};       // +0x15c: the last scroll vector (coasts down when input stops)
    float decay_[2] = {0, 0};          // +0x164: velocity * 5 per second
    float mouseVector_[2] = {0, 0};    // +0x16c: scroll vector from the pointer
    float edgeVector_[2] = {0, 0};     // +0x174: second source added to it (never written by the recovered code)
    float edgeSpeed_ = 300.0f;         // +0x17c
    bool scrolling_ = false;           // +0x180: start message sent
    bool mouseScrolling_ = false;      // +0x181
    IWinBuffer* originImage_ = nullptr;   // +0x188
    std::uint32_t deadZone_ = 10;      // +0x190
    std::uint32_t maxDistance_ = 32;   // +0x194
    float decayFactor_ = 5.0f;         // +0x198
    float speedDivisor_ = 3.0f;        // +0x19c
    cRZTimer timer_{cRZTimer::kMilliseconds};   // +0x1a0
};

}  // namespace sc4
