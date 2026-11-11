// cWinSC4App — see sc4/WinSC4App.h for the recovered behaviour, the object layout and the quirks kept.
#include "sc4/WinSC4App.h"

#include <cctype>
#include <cstdio>

#include "gz/CheatCodeManager.h"
#include "gz/WinMgrBase.h"
#include "gz/cGZMessageServer2.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/View3DWin.h"
#include "sc4/WinRegionScreen.h"
#include "sc4/WinSplashScreen.h"

namespace sc4 {

namespace {

constexpr const char* kWindowCaption = "WinSC4App";
constexpr const char* kCheatShowTimeString = "ShowTime";   // _kSC4CheatCodeShowRealTimeString @ 0x1009fcf8c

// The original hands the window to the cheat-code manager's *legacy* target list (cIGZMessageTarget, whose third slot
// is DoMessage(cGZMessage&)); cIGZWin's third slot is the same call, so the window can be used through that view.
cIGZMessageTarget* AsLegacyMessageTarget(cGZWin* window) {
    return reinterpret_cast<cIGZMessageTarget*>(window);
}

}  // namespace

// ================================================================ construction / identity

cWinSC4App::cWinSC4App(cSC4App* app, cIGZWinMgr* mgr, IWinSC4AppHost* host)   // @ 0x1006cbd30
    : cGZWin(mgr), app_(app), timer_(cRZTimer::kMicroseconds), host_(host) {}

cWinSC4App::~cWinSC4App() {   // @ 0x1006cbe76 (+0x1006cbf5e / 0x1006cbf68)
    // The original's destructor body: the two vptrs, Shutdown, then the reference at +0x208, the fps string, the frame
    // timer and the current screen (+0x178). The accelerator, the graphic system, its view and the realtime debug
    // display are already released by Shutdown; the smart-pointer members release the last three here.
    Shutdown();
    if (unknown208_) {
        unknown208_->Release();
        unknown208_ = nullptr;
    }
    currentScreen_ = nullptr;
}

bool cWinSC4App::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x1006cbf96
    if (!out) return false;
    if (iid == kWinSC4AppIIDMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);   // +0x150
        AddRef();
        return true;
    }
    // The original answers these three with the window itself and refuses everything else: it does not fall back to
    // cGZWin::QueryInterface (0x1006cbff0..0x1006cc01c).
    if (iid == kGZIID_cGZWinAlt || iid == 1 || iid == kGZIID_cIGZWin) {
        *out = static_cast<cIGZWin*>(this);
        AddRef();
        return true;
    }
    return false;
}

// ================================================================ Init / Shutdown

bool cWinSC4App::Init() {   // @ 0x1006cc01e
    if (get_init_flag()) return true;
    cGZWin::Init();
    SetCaption(cRZString(kWindowCaption));   // vslot 0x250
    SetID(kWinSC4AppId);                     // vslot 0x200
    SetFlag(0x800, false);                   // vslot 0x220
    if (cIGZWin* main = GetMainWindow()) SetArea(0, 0, main->GetW(), main->GetH());   // vslot 0x1b0

    // The keyboard accelerator: the exemplar resource {a2e3d533, 6a231eaa, 6a9362f1} (iid 0x42e3ea4b) then the GZCOM
    // class 0x42e967be / iid 0xe2c1b3c4; the host mirrors both calls and answers the accelerator with one reference.
    if (host_) accelerator_ = host_->CreateKeyAccelerator();   // +0x190

    // The fps overlay: RZ::RDD() (+0x188, one reference) is told about the fps string (+0x1b8) through its slots 0x30.
    if (host_ && host_->ShowFps()) {
        debugDisplay_ = host_->RealtimeDebugDisplay();
        if (debugDisplay_) host_->AddDebugTextDisplay(&fpsText_);
    }
    timer_.Start();          // +0x198, cRZTimer::Start
    // The first screen. The 64-bit build always shows the splash; the 32-bit one (i386 0x004959da) plays the intro unless the
    // command line says -intro:off.
    std::string intro;
    if (host_ && host_->CommandLineSwitch("intro", intro)) {
        for (char& c : intro) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (intro == "off" || !CreateIntroVideoScreen()) CreateSplashScreen();

    // The graphic system (+0x168, one reference) and its 0xc47b747c view (+0x170, QueryInterface's own reference).
    if (host_) graphics_ = host_->GraphicSystem();
    if (graphics_) {
        graphics_->AddRef();
        void* view = nullptr;
        if (graphics_->QueryInterface(kWinSC4AppGraphicsViewIid, &view)) graphics2_ = static_cast<cIGZUnknown*>(view);
    }

    // Listen for the cheat-code entry command; register the "ShowTime" cheat and listen for all cheat notifications
    // (the clock window opens on 0x230e27ac with data1 0xea5edb88).
    if (host_) {
        if (cIGZMessageServer2* server = host_->MessageServer2())
            server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kWinSC4AppCommandCheatEntry);   // 0x28
        if (cGZCheatCodeManager* cheats = host_->CheatCodeManager()) {
            cheats->AddNotification(AsLegacyMessageTarget(static_cast<cGZWin*>(this)), 0);   // vslot 0x28
            cheats->RegisterCheatCode(kWinSC4AppCheatShowTime, cRZString(kCheatShowTimeString));   // vslot 0x18
        }
        if (host_->ClockWindowPreference()) host_->DoModelessClockWindow();
    }
    return true;
}

bool cWinSC4App::Shutdown() {   // @ 0x1006cc446
    if (!get_init_flag()) return true;
    set_init_flag(false);
    if (accelerator_) {
        accelerator_->Release();   // vslot 0x10
        accelerator_ = nullptr;
    }
    if (host_) {
        if (cGZCheatCodeManager* cheats = host_->CheatCodeManager()) {
            cheats->RemoveNotification(AsLegacyMessageTarget(static_cast<cGZWin*>(this)), 0);   // vslot 0x30
            cheats->UnregisterCheatCode(kWinSC4AppCheatShowTime);                              // vslot 0x20
        }
        if (cIGZMessageServer2* server = host_->MessageServer2())
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kWinSC4AppCommandCheatEntry);   // 0x30
    }
    if (view3D_) {
        view3D_->Shutdown();    // the cISC4View3DWin vslot 0x208
        view3D_->Release();     // vslot 0x10
        view3D_ = nullptr;
    }
    if (graphics_) {
        graphics_->Release();
        graphics_ = nullptr;
    }
    if (graphics2_) {
        graphics2_->Release();
        graphics2_ = nullptr;
    }
    if (debugDisplay_) {
        if (host_) host_->RemoveDebugTextDisplay(&fpsText_);   // +0x188 vslot 0x38
        debugDisplay_ = nullptr;                                // releases the reference
    }
    // Quirk kept: the flag is set again before cGZWin::Shutdown, which clears it once more (net: false).
    set_init_flag(true);
    cGZWin::Shutdown();
    return true;
}

// ================================================================ the screen registry / swap path

bool cWinSC4App::CreateSplashScreen() {   // @ 0x1006cc39a
    if (currentScreen_) return false;
    auto* splash = new cSC4WinSplashScreen(mgr_);   // 0x1b0 bytes
    cIGZWin* window = static_cast<cIGZWin*>(static_cast<cGZWin*>(splash));
    currentScreen_ = window;                        // +0x178 (the AddRef the original does at vslot 0x8)
    ChildAdd(window);                               // vslot 0x70 (which also runs its Init)
    window->ShowWindow();                           // vslot 0x228
    SetFocusTo(window);                             // cGZWin::SetFocusTo
    return true;
}

bool cWinSC4App::CreateIntroVideoScreen() {   // @ i386 0x0049537c
    if (currentScreen_ || !host_) return false;
    cIGZWin* window = host_->CreateIntroVideoScreen(mgr_);   // 0x108 bytes in the original
    if (!window) return false;
    currentScreen_ = window;                        // +0xf8 in the i386 layout (the original's AddRef)
    ChildAdd(window);                               // vslot 0x38
    window->ShowWindow();                           // vslot 0x114
    SetFocusTo(window);
    return true;
}

bool cWinSC4App::CreateRegionScreen() {   // @ 0x1006cc934
    if (currentScreen_) return false;
    auto* region = new cSC4WinRegionScreen(mgr_);   // 0x318 bytes
    cIGZWin* window = static_cast<cIGZWin*>(static_cast<cGZWin*>(region));
    currentScreen_ = window;                        // +0x178 (the original's AddRef)
    ChildAdd(window);
    window->ShowWindow();
    SetFocusTo(window);
    if (host_) host_->ReadyToHearMusic();           // _sReadyToHearMusic = 1
    return true;
}

bool cWinSC4App::CreateCityScreen() {   // @ 0x1006cc9e8
    if (currentScreen_) return false;
    auto* view = new cSC4View3DWin(mgr_);   // 0x6b0 bytes in the original (the port's layout is documented there)

    view3D_ = view;                                 // +0x180
    // Port: the view pointer holds its own reference. Shutdown releases +0x180 like the original, but the port's
    // window references (the current screen and the child list) already balance on their own, so without this the
    // view is released once too often when the application window goes away with a city open.
    view->AddRef();
    cIGZWin* window = view->AsIGZWin();             // its cGZWin subobject (+0x18 in the original)
    if (!window) return false;
    currentScreen_ = window;                        // +0x178 (the original's AddRef on the +0x18 view)
    ChildAdd(window);
    window->ShowWindow();
    SetFocusTo(window);
    return true;
}

bool cWinSC4App::DestroyCurrentScreen() {   // @ 0x1006ccaa0
    if (!currentScreen_) return false;
    ChildDelete(currentScreen_.get());   // vslot 0x80; the manager releases the child-list reference
    currentScreen_ = nullptr;            // the owner's reference
    if (view3D_) view3D_->Release();     // (port: the reference CreateCityScreen took)
    view3D_ = nullptr;                   // the view died with the screen
    return true;
}

// ================================================================ messages / commands

bool cWinSC4App::DoMessage(cGZMessage& message) {   // @ 0x1006cc584
    if (message.type == gz::kWinMgrMsgKeyUnhandled) {
        // param2 is the original key message's type (5 / 6) and param3 points at that message: its param1 is the
        // virtual key and its param2 the modifier word (0x1006cc655..0x1006cc68b).
        if (message.param2 == kGZMsgKeyDown || message.param2 == kGZMsgKeyUp) {
            const auto* keyMessage = reinterpret_cast<const cGZMessage*>(
                static_cast<std::uintptr_t>(message.param3));
            std::uint32_t command = 0;
            if (accelerator_ && keyMessage &&
                accelerator_->TestForAccelerator(static_cast<std::uint32_t>(keyMessage->param1),
                                                 static_cast<std::uint32_t>(keyMessage->param2),
                                                 message.param2 == kGZMsgKeyDown, command)) {
                if (host_) host_->ExecuteCommand(command);   // the command server's vslot 0x98
                return true;
            }
            return false;
        }
    } else if (message.type == kWinSC4AppMsgCheatTyped) {
        if (message.param1 == kWinSC4AppCheatShowTime) {
            if (host_) host_->DoModelessClockWindow();
            return true;
        }
    } else if (message.type == kWinSC4AppMsgFocusChange) {
        if (app_ && host_) host_->AppFocus(message.param1 != 0);   // cSC4App vslot 0xe8
        if (host_) {
            if (cIGZMessageServer2* server = host_->MessageServer2()) {
                auto* forwarded = new cRZMessage2Standard();
                forwarded->AddRef();
                forwarded->SetType(kWinSC4AppMsgFocusChange);   // 0x28
                forwarded->SetData1(message.param1);            // 0x58
                server->MessageSend(forwarded);                 // vslot 0x18
                forwarded->Release();
            }
        }
        // Falls through to cGZWin::DoMessage (the original has no return in this branch).
    }
    return cGZWin::DoMessage(message);
}

bool cWinSC4App::DoMessage(cIGZMessage2* message) {   // @ 0x1006cc700
    // Quirk kept: the original answers true whether or not the message was the command it acts on.
    if (message && message->GetType() == kWinSC4AppCommandCheatEntry) ProcessCommand(kWinSC4AppCommandCheatEntry);
    return true;
}

bool cWinSC4App::ProcessCommand(std::uint32_t command) {   // @ 0x1006cc730
    if (command == kWinSC4AppCommandCheatEntry && host_) {
        cIGZWin* dialog = host_->CreateCheatCodeEntryDialog();   // reference count 0, like `new`
        if (!dialog) return false;
        dialog->AddRef();                       // 0x8
        if (cIGZWinMgr* manager = GetWindowManager()) {
            // Slot 0x148, which cGZWinMgrBase implements as DoModalWin (this port's interface names it RunPopup);
            // the dialog's answer is discarded.
            manager->RunPopup(dialog);
            manager->DestroyWindow(dialog);     // +0xb8
        }
        dialog->Release();                      // 0x10
    }
    return false;   // the original always answers false
}

// ================================================================ paint / input

bool cWinSC4App::GZOnKeyDown(std::uint32_t, std::uint32_t) { return false; }   // @ 0x1006cc7f0
bool cWinSC4App::GZOnKeyUp(std::uint32_t, std::uint32_t) { return false; }     // @ 0x1006cc7f8
bool cWinSC4App::GZOnCommand(std::uint32_t, std::uint64_t) { return true; }    // @ 0x1006cc912

bool cWinSC4App::GZPaint() {   // @ 0x1006cc81a
    ++frameCount_;                                              // +0x200
    const std::uint32_t elapsed = timer_.GetElapsedTimeU32();   // microseconds
    frameRate_ = 1.0e6f / static_cast<float>(elapsed);          // +0x1b0 (infinite when nothing has elapsed)
    timer_.Restart();
    if (host_ && host_->ShowFps()) {
        if (frameRate_ >= 1.0f) {
            char text[264];
            std::snprintf(text, sizeof text, "FPS: %4.1f", static_cast<double>(frameRate_));
            fpsText_.Sprintf("%s", text);
        } else {
            fpsText_.Sprintf("We'll be right back...");
        }
    }
    return true;
}

// ================================================================ the SL::IWinSC4App view

cIGZUnknown* WinSC4AppView3D::View3DWin() {   // the view's vslot 0x18
    cSC4View3DWin* view = window_ ? window_->View3D() : nullptr;
    return view ? static_cast<cIGZUnknown*>(view->AsIGZWin()) : nullptr;
}

cIGZUnknown* WinSC4AppView3D::Renderer() {   // vslot 0x20
    cSC4View3DWin* view = window_ ? window_->View3D() : nullptr;
    return view ? static_cast<cIGZUnknown*>(static_cast<IRenderer3D*>(view->GetRenderer())) : nullptr;
}

cIGZUnknown* WinSC4AppView3D::ModelMaker() {   // vslot 0x28
    cSC4View3DWin* view = window_ ? window_->View3D() : nullptr;
    return view ? view->GetModelMaker() : nullptr;
}

cIGZUnknown* WinSC4AppView3D::ViewManager() {   // vslot 0x30
    cSC4View3DWin* view = window_ ? window_->View3D() : nullptr;
    return view ? view->GetViewManager() : nullptr;
}

cIGZUnknown* WinSC4AppView3D::RendererViewUtilities() {   // the renderer's vslot 0x40
    cSC4View3DWin* view = window_ ? window_->View3D() : nullptr;
    auto* renderer = view ? static_cast<IRenderer3D*>(view->GetRenderer()) : nullptr;
    return renderer ? renderer->ViewUtilities() : nullptr;
}

cIGZUnknown* WinSC4AppView3D::RendererLightingManager() {   // the renderer's vslot 0x48
    cSC4View3DWin* view = window_ ? window_->View3D() : nullptr;
    auto* renderer = view ? static_cast<IRenderer3D*>(view->GetRenderer()) : nullptr;
    return renderer ? renderer->LightingManager() : nullptr;
}

}  // namespace sc4
