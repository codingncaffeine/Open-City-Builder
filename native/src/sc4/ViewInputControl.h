// cSC4ViewInputControl — base of the 3D view's input tools (37 functions, 0x1000a94a4..0x1000a98a2): holds the
// tool id, its cursor and the window it listens on, and gives the view's mouse / keyboard hooks default
// do-nothing bodies. Class id 0xc6bb8345. Layout: +8 initialized, +0xc refcount, +0x10 id, +0x14 cursor id,
// +0x18 cursor, +0x20 window, +0x28 view3D (referenced), +0x30 window manager (referenced).
#pragma once
#include <cstdint>

#include "gz/WinInterfaces.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4ViewInputControl = 0xc6bb8345;

class cSC4ViewInputControl;

// cISC4View3DWin as the tools use it.
class IViewInputView : public cIGZUnknown {
public:
    virtual void EndInput(int result) = 0;                           // vslot 0xd0
    virtual cSC4ViewInputControl* TopControl() = 0;                  // vslot 0xc0
};

// Services behind SL::spView3D / RZ::WM() / SL::spSS.
class IViewInputHost {
public:
    virtual ~IViewInputHost() = default;
    virtual IViewInputView* View3D() = 0;
    virtual cIGZWinMgr* WindowManager() = 0;
    virtual void PlaySound(int channel, std::uint32_t soundId) = 0;  // sound system vslot 0x50(1, id, 0, 0, 0)
};
IViewInputHost* ViewInputHost();
void SetViewInputHost(IViewInputHost* host);

class cSC4ViewInputControl : public cIGZUnknown {
public:
    cSC4ViewInputControl() = default;
    virtual ~cSC4ViewInputControl() = default;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    virtual bool Init();
    virtual bool Shutdown();
    virtual std::uint32_t GetID() { return id_; }
    virtual void SetID(std::uint32_t id) { id_ = id; }
    virtual cIGZCursor* GetCursor() { return cursor_; }
    std::uint32_t CursorId() const { return cursorId_; }
    virtual void SetCursor(cIGZCursor* cursor);
    virtual void SetCursor(std::uint32_t cursorId);
    virtual void SetWindow(cIGZWin* window) { window_ = window; }
    virtual bool IsSelfScrollingView() { return false; }
    virtual bool ShouldStack() { return true; }
    virtual bool OnCharacter(char) { return false; }
    virtual bool OnKeyDown(std::uint32_t, std::uint32_t) { return false; }
    virtual bool OnKeyUp(std::uint32_t, std::uint32_t) { return false; }
    virtual bool OnMouseDownL(int, int, std::uint32_t) { return false; }
    virtual bool OnMouseDownR(int, int, std::uint32_t) { return false; }
    virtual bool OnMouseUpL(int, int, std::uint32_t) { return false; }
    virtual bool OnMouseUpR(int, int, std::uint32_t) { return false; }
    virtual bool OnMouseMove(int, int, std::uint32_t) { return false; }
    virtual bool OnMouseWheel(int, int, std::uint32_t, int) { return false; }
    virtual bool OnMouseExit() { return false; }
    virtual void Activate() {}
    virtual void Deactivate() {}
    virtual bool AmCapturing();
    virtual void EndInput();
    virtual void PlayResultSound(int soundId);
    virtual void FinalRelease();

    bool SetCapture();
    bool ReleaseCapture();
    bool IsOnTop();
    bool IsKeyToBePassed(std::uint32_t, std::uint32_t) const { return true; }

protected:
    bool initialized_ = false;       // +8
    std::uint32_t refCount_ = 0;     // +0xc
    std::uint32_t id_ = 0;           // +0x10
    std::uint32_t cursorId_ = 0;     // +0x14
    cIGZCursor* cursor_ = nullptr;   // +0x18
    cIGZWin* window_ = nullptr;      // +0x20
    IViewInputView* view3D_ = nullptr;   // +0x28
    cIGZWinMgr* winMgr_ = nullptr;   // +0x30
};

}  // namespace sc4
