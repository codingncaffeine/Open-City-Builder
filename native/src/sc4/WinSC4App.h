// cWinSC4App — the application window (24 functions, 0x1006cbd30..0x1006ccaa0 + the compiler thunks): the window that
// cSC4App::WindowSetup creates and adds to the desktop (cSC4App +0x88), that owns the frame timer and the fps text,
// and that creates and swaps the game's three screens: the splash screen (start-up), the region screen and the city
// (3D view) screen. A screen is swapped in by allocating it, taking one reference, replacing +0x178 (the current
// screen), adding it to the application window, showing it and giving it the keyboard focus; DestroyCurrentScreen
// deletes it through the window manager and drops the reference and the 3D-view pointer.
//
// Vtable groups: +0 the cGZWin / cIGZWin view (0x100ae2cb0), +0x150 the cIGZMessageTarget2 view (0x100ae3188,
// which answers iid 0x452294aa). QueryInterface answers the message-target view and 1 / 0xe98b2f57 / 0x22ba0121 with
// the window itself, and refuses everything else without reaching cGZWin::QueryInterface (0x1006cbf96).
//
// Layout (0x210 bytes, the size cSC4App's `new` requests): the 0x150-byte cGZWin, +0x150 the cIGZMessageTarget2 vptr,
// +0x158 cSC4App*, +0x160 unused (the constructor zeroes it and nothing else touches it), +0x168 the graphic system
// (SL::spGS, one reference) and +0x170 its 0xc47b747c view, +0x178 the current screen, +0x180 the city 3D view (the
// cISC4View3DWin*), +0x188 the realtime debug display (RZ::RDD(), one reference), +0x190 the key accelerator,
// +0x198 the frame timer (0x18 bytes, microseconds), +0x1b0 the frame rate (float, starts at 1.0), +0x1b8 the fps
// text (cRZString), +0x1e0 eight words initialised to 1,000,000 (not read by any function of this class),
// +0x200 the frame counter, +0x204 unused, +0x208 an externally set reference the destructor releases.
//
// Quirks kept:
// * Shutdown clears the initialised flag, tears its services down, then sets the flag *back* and calls
//   cGZWin::Shutdown, which clears it a second time (0x1006cc446); the flag is therefore false on return.
// * DoMessage(cGZMessage&) handles the key-unhandled message (0x1b) only for key down / key up (param2 == 5 / 6), and
//   the accelerator answers false for every other key message without reaching cGZWin::DoMessage (0x1006cc584).
// * DoMessage(cIGZMessage2*) returns true for every message, whether or not it was the command it acts on.
// * DoMessage(cGZMessage&) does *not* return after handling the focus-change message (0x4348b111): it falls through to
//   cGZWin::DoMessage and returns that answer.
// * the frame rate is 1e6 / the timer's elapsed microseconds in single precision (0x1006cc81a), so the first paint
//   after a restart divides by zero and yields infinity.
//
// Seams: IWinSC4AppHost holds the process services the original reaches through SL:: / RZ:: globals and the two
// neighbours that are not linked here (the cheat-code entry dialog, not recovered) or are engine globals (the music
// flag); it mirrors one original call per method. WinSC4AppApplication maps the window's View3D() onto
// SL::IWinSC4App for cSC4App::WindowSetup, and WinSC4AppView3D maps the six IView3DSystems calls onto the city view.
// The three screens the original allocates are the recovered classes (cSC4WinSplashScreen / cSC4WinRegionScreen /
// cSC4View3DWin), constructed here exactly where the original constructs them.
#pragma once
#include <cstdint>
#include <string>

#include "gz/Win.h"
#include "gz/WinInterfaces.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "sc4/ServiceLocator.h"

class cIGZString;
class cIGZMessageServer2;
class cGZCheatCodeManager;
class cSC4App;

namespace sc4 {

class cWinSC4App;
class cSC4View3DWin;

// The window manager id the window registers itself with (Init: SetID 0x6104489a) and the notification type it asks
// the message server for (the cheat-code entry command).
constexpr std::uint32_t kWinSC4AppId = 0x6104489a;
constexpr std::uint32_t kWinSC4AppCommandCheatEntry = 0x2a9496c9;
// Message 0x230e27ac (the cheat-code manager's "a cheat was typed") with this data1 opens the clock window.
constexpr std::uint32_t kWinSC4AppMsgCheatTyped = 0x230e27ac;
constexpr std::uint32_t kWinSC4AppCheatShowTime = 0xea5edb88;        // "ShowTime"
constexpr std::uint32_t kWinSC4AppMsgFocusChange = 0x4348b111;      // data1 = gained (forwarded to the app, rebroadcast)
constexpr std::uint32_t kWinSC4AppGraphicsViewIid = 0xc47b747c;     // +0x170: the graphic system's view
constexpr std::uint32_t kWinSC4AppIIDMessageTarget2 = 0x452294aa;

// Everything the window reaches outside itself: one method per original call.
class IWinSC4AppHost {
public:
    virtual ~IWinSC4AppHost() = default;

    // --- SL::spGS (the graphic system): the window keeps a reference at +0x168 and QueryInterface(0xc47b747c) into
    // +0x170 (RZ::GS() in the original).
    virtual cIGZUnknown* GraphicSystem() = 0;
    // --- RZ::RDD() @ 0x100127c5c (borrowed; the window takes its own reference): +0x188.
    virtual cIGZUnknown* RealtimeDebugDisplay() = 0;
    virtual void AddDebugTextDisplay(cIGZString* text) = 0;        // +0x188 vslot 0x30(&fps text)
    virtual void RemoveDebugTextDisplay(cIGZString* text) = 0;     // +0x188 vslot 0x38(&fps text)
    // MacPreferences::GetBoolean("show-fps", false) @ 0x100641da1 (the original's _kShowFps).
    virtual bool ShowFps() = 0;
    // --- SL::spMS2 (may be null): vslot 0x28 AddNotification(+0x150, 0x2a9496c9), vslot 0x30 RemoveNotification,
    // vslot 0x18 MessageSend (the focus-change broadcast).
    virtual cIGZMessageServer2* MessageServer2() = 0;
    // --- SL::spCCM (may be null): vslot 0x28 / 0x30 notification, vslot 0x18 RegisterCheatCode, 0x20 Unregister.
    virtual cGZCheatCodeManager* CheatCodeManager() = 0;
    // --- SL::spCommandServer vslot 0x98 ExecuteCommand(command, null, null) after a matched accelerator key.
    virtual void ExecuteCommand(std::uint32_t command) = 0;
    // --- SL::spPreferences byte 0x4ee: the clock window was shown; cSC4TimeDisplayMiniWindowWinProc::DoModelessWin
    // @ 0x1002f319e.
    virtual bool ClockWindowPreference() = 0;
    virtual void DoModelessClockWindow() = 0;
    // --- new cSC4CheatCodeEntryDialog (ctor @ 0x1003ac35c, 0x1c8 bytes) for ProcessCommand: returns the window with
    // reference count 0, exactly as `new` does (the caller AddRefs it before the window manager sees it).
    virtual cIGZWin* CreateCheatCodeEntryDialog() = 0;
    // --- cSC4App::GainLoseFocus(bool) @ 0x10000f2f2 (the app's vslot 0xe8), from the focus-change message.
    virtual void AppFocus(bool gained) = 0;
    // --- _sReadyToHearMusic = 1 (0x100b70ec8) at the end of CreateRegionScreen.
    virtual void ReadyToHearMusic() = 0;
    // --- SL::spRM vslot 0x18 GetResource(key {a2e3d533, 6a231eaa, 6a9362f1}, iid 0x42e3ea4b, ...) and then
    // GZCOM()->GetCOMObject(0x42e967be, 0xe2c1b3c4): the keyboard accelerator the key-unhandled path uses.
    virtual cIGZWinKeyAccelerator* CreateKeyAccelerator() = 0;
    // --- (Mac i386 cWinSC4App::Init 0x004959da) the framework command line's "intro" switch: true and its value when present.
    virtual bool CommandLineSwitch(const char* name, std::string& value) { (void)name; (void)value; return false; }
    // --- cWinSC4App::CreateIntroVideoScreen (i386 0x0049537c): `new cSC4WinIntroVideoScreen`. The screen and its movie player
    // are the host's (they need a movie decoder); null when it has none, and the window shows the splash screen instead.
    virtual cIGZWin* CreateIntroVideoScreen(cIGZWinMgr* mgr) { (void)mgr; return nullptr; }
};

class cWinSC4App final : public cGZWin, public cIGZMessageTarget2 {
public:
    cWinSC4App(cSC4App* app, cIGZWinMgr* mgr, IWinSC4AppHost* host);   // @ 0x1006cbd30
    ~cWinSC4App() override;                                            // @ 0x1006cbe76 (+0x1006cbf5e / 0x1006cbf68)

    bool QueryInterface(std::uint32_t iid, void** out) override;       // @ 0x1006cbf96
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }       // @ 0x1006cbfe6
    std::uint32_t Release() override { return cGZWin::Release(); }     // @ 0x1006cc002

    bool Init() override;                                              // @ 0x1006cc01e
    bool Shutdown() override;                                          // @ 0x1006cc446
    using cGZWin::DoMessage;
    bool DoMessage(cGZMessage& message) override;                      // @ 0x1006cc584
    bool DoMessage(cIGZMessage2* message) override;                    // @ 0x1006cc700
    bool GZPaint() override;                                           // @ 0x1006cc81a
    bool GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // @ 0x1006cc7f0
    bool GZOnKeyUp(std::uint32_t key, std::uint32_t modifiers) override;     // @ 0x1006cc7f8
    bool GZOnCommand(std::uint32_t command, std::uint64_t param) override;   // @ 0x1006cc912

    // --- the screen registry / swap path
    bool CreateSplashScreen();                                         // @ 0x1006cc39a
    bool CreateIntroVideoScreen();                                     // @ i386 0x0049537c (not in the 64-bit build)
    bool CreateRegionScreen();                                         // @ 0x1006cc934
    bool CreateCityScreen();                                           // @ 0x1006cc9e8
    bool DestroyCurrentScreen();                                       // @ 0x1006ccaa0
    bool ProcessCommand(std::uint32_t command);                        // @ 0x1006cc730

    // --- accessors
    float GetFrameRate() const { return frameRate_; }                  // @ 0x1006cc800
    std::uint32_t GetFrameCount() const { return frameCount_; }        // @ 0x1006cc80e
    cSC4View3DWin* View3D() const { return view3D_; }                  // @ 0x1006cc91a (the original returns +0x180)
    cIGZWin* CurrentScreen() const { return currentScreen_.get(); }
    cSC4App* App() const { return app_; }
    IWinSC4AppHost* Host() const { return host_; }

    // The fps text the RDD shows (+0x1b8) and the rdd pointer (+0x188), for the test / host harness.
    const cRZString& FpsText() const { return fpsText_; }
    cIGZUnknown* RealtimeDebugDisplay() const { return debugDisplay_.get(); }
    cIGZWinKeyAccelerator* KeyAccelerator() const { return accelerator_; }
    cIGZUnknown* GraphicSystem() const { return graphics_; }
    cIGZUnknown* GraphicSystemView() const { return graphics2_; }

private:
    cSC4App* app_ = nullptr;                          // +0x158
    cIGZUnknown* graphics_ = nullptr;                 // +0x168 SL::spGS (+1)
    cIGZUnknown* graphics2_ = nullptr;                // +0x170 its 0xc47b747c view (+1)
    cRZAutoRefCount<cIGZWin> currentScreen_;          // +0x178
    cSC4View3DWin* view3D_ = nullptr;                 // +0x180
    cRZAutoRefCount<cIGZUnknown> debugDisplay_;       // +0x188 RZ::RDD() (+1)
    cIGZWinKeyAccelerator* accelerator_ = nullptr;    // +0x190
    cRZTimer timer_;                                  // +0x198 (microseconds)
    float frameRate_ = 1.0f;                          // +0x1b0
    cRZString fpsText_;                               // +0x1b8
    [[maybe_unused]] std::uint32_t unknown1e0_[8] = {1000000, 1000000, 1000000, 1000000,
                                    1000000, 1000000, 1000000, 1000000};   // +0x1e0
    std::uint32_t frameCount_ = 0;                    // +0x200
    [[maybe_unused]] std::uint32_t unknown204_ = 0;   // +0x204
    cIGZUnknown* unknown208_ = nullptr;               // +0x208 (released by the destructor)
    // The port's one addition, after the original's 0x210 bytes: the original reaches these services through SL:: /
    // RZ:: process globals, which the host supplies through IWinSC4AppHost instead.
    IWinSC4AppHost* host_ = nullptr;                  // +0x210 (not in the original)
};

// SL::IView3DSystems over a cWinSC4App's current 3D view: the original's View3D() returns the cISC4View3DWin* and
// ServiceLocator asks its slots 0x18 / 0x20 / 0x28 / 0x30 and the renderer's 0x40 / 0x48. Each method is that call.
class WinSC4AppView3D final : public SL::IView3DSystems {
public:
    explicit WinSC4AppView3D(const cWinSC4App* window) : window_(window) {}
    cIGZUnknown* View3DWin() override;             // view vslot 0x18 AsIGZWin
    cIGZUnknown* Renderer() override;              // view vslot 0x20
    cIGZUnknown* ModelMaker() override;            // view vslot 0x28
    cIGZUnknown* ViewManager() override;           // view vslot 0x30
    cIGZUnknown* RendererViewUtilities() override; // renderer vslot 0x40
    cIGZUnknown* RendererLightingManager() override;   // renderer vslot 0x48
private:
    const cWinSC4App* window_;
};

// SL::IWinSC4App over a cWinSC4App (what SL::spWinSC4App points at): cSC4App::WindowSetup hands this object to the
// window's application slot, and ServiceLocator's OnView3DInit calls View3D() on it.
class WinSC4AppApplication final : public SL::IWinSC4App {
public:
    explicit WinSC4AppApplication(const cWinSC4App* window) : view_(window) {}
    SL::IView3DSystems* View3D() override { return &view_; }
private:
    WinSC4AppView3D view_;
};

}  // namespace sc4
