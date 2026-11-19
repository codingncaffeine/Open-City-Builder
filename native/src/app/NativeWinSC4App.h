// Platform glue of the native Metal application (no recovered counterpart on macOS); derived from the early host.
// application window: it creates the splash screen, the region screen and the city screen, and swaps between them. Every
// engine call it makes goes through IWinSC4AppHost, so this file is the whole surface the host has to provide for the
// game's own screen chain to run instead of the host's painted stand-in.
//
// The services the host does not have yet are answered with "absent" (null / false), which is what the original does when
// the service is missing: the window skips that feature and keeps going. Nothing here fakes a game behaviour.
#pragma once
#include <string>

#include "app/NativeCheats.h"
#include "app/NativeIntro.h"
#include "app/NativeKeyboard.h"
#include "gz/WinKeyAcceleratorRes.h"
#include "sc4/WinSC4App.h"

namespace native {

class HostWinSC4App final : public sc4::IWinSC4AppHost {
public:
    explicit HostWinSC4App(cGZFramework* framework) : framework_(framework) {}

    // SL::spGS and its 0xc47b747c view: the host's graphic system object is the app's stub.
    cIGZUnknown* GraphicSystem() override { return graphicSystem_; }
    void SetGraphicSystem(cIGZUnknown* graphics) { graphicSystem_ = graphics; }

    // RZ::RDD: the realtime debug display. The host has none, so the fps overlay is simply not registered.
    cIGZUnknown* RealtimeDebugDisplay() override { return nullptr; }
    void AddDebugTextDisplay(cIGZString*) override {}
    void RemoveDebugTextDisplay(cIGZString*) override {}
    bool ShowFps() override { return false; }

    cIGZMessageServer2* MessageServer2() override { return messageServer_; }
    void SetMessageServer(cIGZMessageServer2* server) { messageServer_ = server; }

    // The application's cheat code manager, the framework command server and the cheat code box (Ctrl+X). The clock
    // preference and its window are not built yet; the window skips that path.
    cGZCheatCodeManager* CheatCodeManager() override { return native::GameCheatCodeManager(); }
    void ExecuteCommand(std::uint32_t command) override { native::ExecuteGameCommand(command); }
    bool ClockWindowPreference() override { return false; }
    void DoModelessClockWindow() override {}
    cIGZWin* CreateCheatCodeEntryDialog() override { return native::CreateCheatCodeEntryDialog(); }
    void AppFocus(bool) override {}

    // _sReadyToHearMusic = 1 at the end of CreateRegionScreen. The host records it so a caller can see the region screen
    // got far enough to ask for its music, which is the signal the audio path keys off.
    // The keyboard accelerator the key-unhandled path uses: the general table {a2e3d533, 6a231eaa, 6a9362f1} (Ctrl+X
    // opens the cheat code dialog).
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { return native::CreateGameKeyAccelerator(gz::kKeyAcceleratorGeneral); }

    void ReadyToHearMusic() override { readyToHearMusic_ = true; }

    // The game's -intro:<value> switch (the main program reads it from its arguments) and the intro screen (NativeIntro.cpp).
    bool CommandLineSwitch(const char* name, std::string& value) override {
        if (std::string(name) != "intro" || !hasIntroSwitch_) return false;
        value = introSwitch_;
        return true;
    }
    void SetIntroSwitch(const std::string& value) { introSwitch_ = value; hasIntroSwitch_ = true; }
    cIGZWin* CreateIntroVideoScreen(cIGZWinMgr* mgr) override;
    bool readyToHearMusic() const { return readyToHearMusic_; }

private:
    cGZFramework* framework_ = nullptr;
    cIGZUnknown* graphicSystem_ = nullptr;
    cIGZMessageServer2* messageServer_ = nullptr;
    bool readyToHearMusic_ = false;
    std::string introSwitch_;
    bool hasIntroSwitch_ = false;
};

inline cIGZWin* HostWinSC4App::CreateIntroVideoScreen(cIGZWinMgr* mgr) { return native::CreateIntroVideoScreen(mgr); }

}  // namespace native
