// cSC4ViewInputControlCitySituations (i386 0x00490d00..0x004910b2): the persistent tool cSC4View3DWin::DoPostCityInit installs.
// In the surface view a click on a dispatch-view indicator (a mission icon) starts the situation of the indicator's occupant:
// hovering locks the indicator (button state 3), pressing on it marks it pressed (2), and releasing on the same indicator
// activates it (1) and asks the city situation manager to start a situation for its occupant. The view-changed message
// 0xa6b79602 (and 0xa6b79612 / 0x06cdb65b, which it handles but does not register for) unlocks the hovered indicator.
// Layout (i386): +0x28 cIGZMessageTarget2, +0x2c the situation manager, +0x30 the dispatch manager, +0x34 the hovered
// indicator, +0x38 the pressed one; id 0xabec6cfe, cursor 0x43401c71.
#pragma once

#include <cstdint>

#include "gz/cIGZMessage2.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

class cISC4Occupant;

constexpr std::uint32_t kCitySituationsToolId = 0xabec6cfe;

class ICitySituationsToolHost {
public:
    virtual ~ICitySituationsToolHost() = default;
    virtual bool InSurfaceView() = 0;                                // spView3D vslot 0x58 == 1
    virtual bool HasManagers() = 0;                                  // the city's situation (0xc4) and dispatch (0x9c) managers
    // The dispatch manager's vehicle view (vslot 0x58): false without one; its indicator under a point (0 = none), the
    // indicator's button state, its lock, and the occupant of the active indicator.
    virtual bool HasDispatchView() = 0;
    virtual std::uint32_t FindIndicatorHandle(int x, int y) = 0;
    virtual void SetActiveIndicator(std::uint32_t handle, std::uint32_t buttonState) = 0;
    virtual void LockIndicator(std::uint32_t handle, bool lock) = 0;
    virtual cISC4Occupant* ActiveIndicatorOccupant() = 0;
    virtual void InitiateSituation(cISC4Occupant* occupant) = 0;    // situation manager vslot 0x58(occupant, none)
    virtual void Listen(cIGZMessageTarget2* target, std::uint32_t type, bool on) = 0;   // SL::spMS2 0x14 / 0x18
};
ICitySituationsToolHost* CitySituationsToolHost();
void SetCitySituationsToolHost(ICitySituationsToolHost* host);

class cSC4ViewInputControlCitySituations final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    cSC4ViewInputControlCitySituations();                                // i386 0x004910b2
    bool QueryInterface(std::uint32_t iid, void** out) override;         // i386 0x00490d00
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;                      // i386 0x00490e8a
    bool Init() override;                                                // i386 0x0049102a
    bool Shutdown() override;                                            // i386 0x00490d3e
    bool OnKeyDown(std::uint32_t, std::uint32_t) override { return false; }   // i386 0x00490daa
    bool OnKeyUp(std::uint32_t, std::uint32_t) override { return false; }     // i386 0x00490db2
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;   // i386 0x00490f14
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;     // i386 0x00490ddc
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;    // i386 0x00490f90

private:
    std::uint32_t hovered_ = 0;   // +0x34
    std::uint32_t pressed_ = 0;   // +0x38
    bool managers_ = false;       // +0x2c / +0x30 held
};

}  // namespace sc4
