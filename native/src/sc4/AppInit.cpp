// cSC4App::Init and PostFrameWorkInit: bringing the application up once the services exist.
#include <cstring>

#include "gz/CheatCodeManager.h"
#include "gz/GZCOMDirector.h"
#include "gz/RZServices.h"
#include "gz/cIGZFrameWork.h"
#include "lua/ISCLua.h"
#include "render/gd/VideoModes.h"
#include "rz/Threading.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/App.h"
#include "sc4/AppPlatform.h"
#include "sc4/ServiceLocator.h"

namespace {
// RZGetFramework()->GetSystemService(...)
cIGZUnknown* FrameworkService(std::uint32_t serviceId, std::uint32_t iid, cRZAutoRefCount<cIGZUnknown>& holder) {
    if (cIGZFrameWork* framework = RZGetFramework()) framework->GetSystemService(serviceId, iid, holder.AsOutParam());
    return holder.get();
}

// the original builds these messages inline in Init
constexpr std::uint32_t kAppInitializedMessage = 0x28e9397a, kAppReadyMessage = 0x28e9397b;
}  // namespace

void cSC4App::PostSC4AppMessage(std::uint32_t type, bool post) {
    cRZAutoRefCount<cIGZUnknown> servers;
    if (!FrameworkService(0x04fa845b, 0x652294c7, servers)) return;
    auto* messages = static_cast<cIGZMessageServer2*>(static_cast<void*>(servers.get()));
    cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
    message->SetType(type);
    message->SetVoid1(static_cast<cISC4App*>(this));
    message->SetIGZUnknown(static_cast<cISC4App*>(this));
    if (post) messages->MessagePost(message.get(), false);
    else messages->MessageSend(message.get());
}

// The application comes up: graphics checks, 3D renderer, video options, message subscriptions, the cheat code
// manager and the managers (glyph textures, string detokenizer, Lua, window locations, features, tool tips, missing
// plugin reports, the nation), then the resources. Returns false when the display or the resources cannot be set up.
bool cSC4App::Init() {
    cIGZFrameWork* framework = framework_ ? framework_ : RZGetFramework();
    cIGZWinMgr* windowManager = RZ::WM();
    gz::cIGZWinCtrlMgr* controlManager = RZ::WCM();
    IGraphicSystemControl* graphics = startupHooks_.graphicSystem ? startupHooks_.graphicSystem() : nullptr;
    cIGZCOM* com = GZCOM();
    std::uint8_t* prefs = preferences_.data();
    auto alert = [&](const std::string& text) { if (startupHooks_.messageBox) startupHooks_.messageBox(text, "SimCity 4"); };
    auto initialize = [&](const char* name, cIGZUnknown* object) { if (startupHooks_.initializeManager && object) startupHooks_.initializeManager(name, object); };

    // 1. no usable display: fall back to the software driver for the next launch
    if (!graphics || !graphics->CanvasReady()) {
        prefs[sc4::prefs::kVideoHardwareDriver] = 0;
        prefs[sc4::prefs::kVideoAutoConfigure] = 1;
        SavePreferences();
        alert("SimCity 4 will launch with the software driver when next launched.");
        return false;
    }

    // 2. the driver it came up with
    gd::cIGZGDriver* driver = graphics->GetGDriver();
    if (driver) {
        gd::sGDMode mode{};
        driver->GetVideoModeInfo(mode);
        bool accelerated = (mode.reserved30 & 0xff) != 0;
        if (!accelerated) {
            if (prefs[sc4::prefs::kVideoHardwareDriver] != 0) {
                alert("Could not initialize the hardware driver. Using the software driver instead.");
                prefs[sc4::prefs::kVideoHardwareDriver] = 0;
                prefs[sc4::prefs::kVideoAutoConfigure] = 1;
            }
        } else {
            prefs[sc4::prefs::kVideoHardwareDriver] = 1;
        }
    }
    if (startupHooks_.initialize3DRender) startupHooks_.initialize3DRender(driver);

    // 3. the configuration rule manager takes the render properties; first start-up picks the video preferences
    if (startupHooks_.createVideoOptionStore) videoOptions_ = startupHooks_.createVideoOptionStore();
    if (videoOptions_) {
        videoOptions_->SetRenderProperties(renderProperties_);
        videoOptions_->Initialize();
    }
    if (prefs[sc4::prefs::kVideoAutoConfigure] != 0) {
        GetAutoVideoPreferences(reinterpret_cast<SC4VideoPreferences*>(prefs + sc4::prefs::kVideoAutoConfigure));
        IGraphicSystemControl::DisplayMetrics game = graphics->GetGameMetrics();
        IGraphicSystemControl::DisplayMetrics screen = graphics->GetScreenMetrics();
        preferences_.Set<std::uint32_t>(sc4::prefs::kVideoWidth, game.width);
        preferences_.Set<std::uint32_t>(sc4::prefs::kVideoHeight, game.height);
        preferences_.Set<std::uint32_t>(sc4::prefs::kVideoColorDepth, screen.depth);
        prefs[0] = videoOptions_ && videoOptions_->GetOption(0xd) != 1;
        prefs[sc4::prefs::kVideoAutoConfigure] = 0;
        SavePreferences();
    }

    // 4. a "software rendering" render property swaps a hardware driver for the first software one
    if (renderProperties_ && renderProperties_->BoolValue(0) && driver) {
        gd::sGDMode mode{};
        driver->GetVideoModeInfo(mode);
        if ((mode.reserved30 & 0xff) != 0) {
            int count = graphics->GetGDriverCount();
            for (int i = 0; i < count; ++i) {
                gd::cIGZGDriver* candidate = graphics->GetGDriverByIndex(i);
                gd::sGDMode candidateMode{};
                candidate->GetVideoModeInfo(candidateMode);
                if ((candidateMode.reserved30 & 0xff) == 0) {
                    if (candidate != graphics->GetGDriver()) {
                        graphics->SetGDriver(candidate);
                        if (startupHooks_.setTextureBindingDriver) startupHooks_.setTextureBindingDriver(candidate);
                        if (startupHooks_.initialize3DRender) startupHooks_.initialize3DRender(candidate);
                    }
                    break;
                }
            }
        }
    }

    // 5. the video preferences take effect; translucency follows the render properties
    ApplyVideoPreferences(*reinterpret_cast<const SC4VideoPreferences*>(prefs + sc4::prefs::kVideoAutoConfigure));
    {
        bool forceNoTranslucency = renderProperties_ && renderProperties_->BoolValue(33);
        float threshold = forceNoTranslucency ? renderProperties_->FloatValue(14) : 0.0f;
        graphics->SetForceNoTranslucency(forceNoTranslucency, threshold);
    }

    // 6. "the application exists" and the messages the application listens to
    PostSC4AppMessage(kAppInitializedMessage, false);
    if (cIGZMessageServer* messages = RZ::MS()) messages->AddNotification(static_cast<cIGZMessageTarget*>(this), 0xc7aa61c6);
    if (cIGZMessageServer2* messages2 = RZ::MS2()) {
        for (std::uint32_t type : {0xfa0c6390u, 0x7a104750u, 0xba44eb51u, 0x49a780b2u, 0xea65224eu, 0xcbb5bb45u, 0x8bb5bb46u, 0xea8ae29au, 0x26d31ec2u})
            messages2->AddNotification(static_cast<cIGZMessageTarget2*>(this), type);
    }

    // 7. cheat codes
    {
        auto* cheats = new cGZCheatCodeManager();
        cheats->AddRef();
        cheatCodeManager_ = reinterpret_cast<cIGZUnknown*>(static_cast<cRZSystemService*>(cheats));
        cheats->AddNotification(static_cast<cIGZMessageTarget*>(this), 0);
        cheats->RegisterCheatCode(0x2ab5c431, cRZString("GP"));                // kSC4CheatCodeBackgroundGamePauseString
        cheats->RegisterCheatCode(0x2b60e45b, cRZString("CreateException"));   // kSC4CheatCodeCreateExceptionString
        cheats->RegisterCheatCode(0x8b6d4f8d, cRZString("ForceIdle"));         // kSC4CheatCodeForceIdleProcessingString
    }

    // 8. the managers
    if (!glyphTextureManager_) {
        if (auto it = serviceFactories_.find("cSC4GlyphTextureManager"); it != serviceFactories_.end()) {
            glyphTextureManager_ = it->second();
            if (glyphTextureManager_) glyphTextureManager_->AddRef();
            initialize("cSC4GlyphTextureManager", glyphTextureManager_);
        }
    }
    if (!gimexFactory_) {
        cRZAutoRefCount<cIGZUnknown> gimex;
        if (com->CreateInstance(0xa2ffb5d3, 0x02ffbb60, gimex.AsOutParam())) { gimexFactory_ = gimex.get(); if (gimexFactory_) gimexFactory_->AddRef(); }
    }
    if (!stringDetokenizer_) {
        cRZAutoRefCount<cIGZUnknown> detokenizer;
        if (com->CreateInstance(0xaa2c8c2a, 0x8a2c3240, detokenizer.AsOutParam())) {
            stringDetokenizer_ = detokenizer.get();
        } else if (auto it = serviceFactories_.find("cSCStringDetokenizer"); it != serviceFactories_.end()) {
            stringDetokenizer_ = it->second();
        }
        if (stringDetokenizer_) stringDetokenizer_->AddRef();
    }
    initialize("cSCStringDetokenizer", stringDetokenizer_);
    if (!luaInterpreter_) {
        cRZAutoRefCount<cIGZUnknown> lua;
        // The original asks for iid 0xea10c4a2 and keeps that pointer as its cIGZUnknown (one vtable); the port keeps the
        // cIGZUnknown view (iid 1) and opens the scripting view, as the original does at 0x0044C925 (lua-host.md 1.4).
        if (com->CreateInstance(0xea10c4a2, 1, lua.AsOutParam())) { luaInterpreter_ = lua.get(); if (luaInterpreter_) luaInterpreter_->AddRef(); }
        cISCLua* scripting = nullptr;
        if (luaInterpreter_ && luaInterpreter_->QueryInterface(0xea10c4a2, reinterpret_cast<void**>(&scripting)) && scripting) {
            scripting->Open(0x7e);
            scripting->Release();
        }
    }
    initialize("cSCLua", luaInterpreter_);
    if (!winLocationSaver_) {
        cRZAutoRefCount<cIGZUnknown> saver;
        if (com->CreateInstance(0xaa601692, 0xaa6016b1, saver.AsOutParam())) {
            winLocationSaver_ = saver.get();
        } else if (auto it = serviceFactories_.find("cSLWinLocationSaver"); it != serviceFactories_.end()) {
            winLocationSaver_ = it->second();
        }
        if (winLocationSaver_) winLocationSaver_->AddRef();
    }
    initialize("cSLWinLocationSaver", winLocationSaver_);

    // 9. the resources: without them the application cannot start, and the failure is reported in the player's language
    if (!SetupResources()) {
        gbAppResourcesInitializedOK = true;     // (sic: set when the set-up FAILED; SC4DisplayResourceError reads it)
        if (cIGZLanguageManager* languages = RZ::LM()) gbAppResourceErrorLanguage = languages->GetCurrentLanguage();
        return false;
    }

    // 10. the main window, fonts, feature manager, tool tips and the rest of the user interface plumbing
    if (startupHooks_.configureResourceCache) startupHooks_.configureResourceCache();
    if (windowManager) {
        mainWindow_ = windowManager->GetMainWindow();
        if (mainWindow_) mainWindow_->SetCaption(cRZString("SimCity 4"));
    }
    if (startupHooks_.setCanvasTitle) startupHooks_.setCanvasTitle("SimCity 4");
    if (auto it = serviceFactories_.find("cSC4FeatureManager"); it != serviceFactories_.end()) {
        featureManager_ = it->second();
        if (featureManager_) featureManager_->AddRef();
    }
    SL::OnAppInit1();
    initialize("cSC4FeatureManager", featureManager_);
    SetupWinCtrlMgr();
    if (controlManager) {
        // system font 0xa2cae9df := the font registered as 0x68963c4c
        controlManager->SetSystemFont(0xa2cae9df, controlManager->SystemFont(0x68963c4c));
    }
    if (!toolTipManager_) {
        if (auto it = serviceFactories_.find("cSC4ToolTipManager"); it != serviceFactories_.end()) {
            toolTipManager_ = it->second();
            if (toolTipManager_) toolTipManager_->AddRef();
        }
        initialize("cSC4ToolTipManager", toolTipManager_);
    }
    if (startupHooks_.configureDebugDisplay) startupHooks_.configureDebugDisplay(preferences_.data()[sc4::prefs::kSwitchXDD] != 0);

    if (auto it = serviceFactories_.find("cSC4MissingPluginDataReporter"); it != serviceFactories_.end()) {
        missingPluginReporter_ = it->second();
        if (missingPluginReporter_) missingPluginReporter_->AddRef();
        initialize("cSC4MissingPluginDataReporter", missingPluginReporter_);
    }
    WindowSetup();
    initialize("cSC4TutorialRegistry", reinterpret_cast<cIGZUnknown*>(tutorialRegistry_));
    if (!nation_) {
        if (auto it = serviceFactories_.find("cSC4Nation"); it != serviceFactories_.end()) {
            cIGZUnknown* nation = it->second();
            nation_ = reinterpret_cast<cISC4Nation*>(nation);
            if (nation) nation->AddRef();
            initialize("cSC4Nation", nation);
        }
    }
    SL::OnAppInit2();
    framework->AddToOnIdle(static_cast<cIGZSystemService*>(static_cast<cRZSystemService*>(this)));
    PostSC4AppMessage(kAppReadyMessage, false);
    return true;
}


// ---- the application window ---------------------------------------------------------------------------------

bool cSC4App::WindowSetup() {
    if (!winSC4App_ && startupHooks_.createApplicationWindow) {
        auto created = startupHooks_.createApplicationWindow(*this);
        winSC4App_ = created.application;
        applicationWindow_ = created.window;
        if (applicationWindow_ && mainWindow_) mainWindow_->ChildAdd(applicationWindow_);
    }
    if (mainWindow_) mainWindow_->ShowWindow();
    return true;
}

bool cSC4App::WindowShutdown() {
    if (winSC4App_) {
        if (mainWindow_ && applicationWindow_) mainWindow_->ChildDelete(applicationWindow_);
        winSC4App_ = nullptr;
        applicationWindow_ = nullptr;
    }
    mainWindow_ = nullptr;
    return true;
}

// ---- idle: the once-a-second housekeeping ---------------------------------------------------------------------

bool cSC4App::OnIdle(std::uint32_t) {
    if (timer410_.GetElapsedTimeU32() >= 1001) {
        timer410_.Restart();
        std::uint32_t seconds = 0;
        bool announce = false;
        if (timer440_.GetElapsedTimeU32() < 300) {
            seconds = timer428_.GetElapsedTimeU32();
            if (interval458_ < seconds) {
                timer428_.SetElapsedTimeU64(interval458_ - interval45c_);
                announce = true;
            }
        } else {
            timer428_.Restart();
            seconds = 0x3c;
            announce = true;
        }
        if (announce) {
            timer440_.Restart();
            if (cIGZMessageServer2* messages = SL::spMS2) {
                cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
                message->SetType(0xba44eb51);
                message->SetData1(seconds);
                messages->MessagePost(message.get(), false);
            }
        }
        if (interval3e8_ < static_cast<int>(timer3d0_.GetElapsedTimeU32())) {       // every 30 minutes
            timer3d0_.Restart();
            SavePreferences();
        }
        if (interval408_ < static_cast<int>(timer3f0_.GetElapsedTimeU32())) {       // every 5 minutes
            timer3f0_.Restart();
            if (SL::spRM) SL::spRM->ForceGarbageCollection();
        }
    }
    if (gamePaused_) cRZThread::SleepCurrentThread(250000);
    return false;
}

bool cSC4App::OnTick(std::uint32_t) { return true; }


// ---- window control manager ---------------------------------------------------------------------------------

// The default control colours (id -> r, g, b), then the fonts / bitmaps (host hook: the font system, the font-style
// script and the resource bitmaps are not recovered).
void cSC4App::SetupWinCtrlMgr() {
    gz::cIGZWinCtrlMgr* controls = RZ::WCM();
    if (controls) {
        struct Colour { int id; std::uint8_t r, g, b; };
        static constexpr Colour kColours[] = {
            {0x08963f6c, 0xff, 0x00, 0xff}, {0x68963f6f, 0xbd, 0xbc, 0xbc}, {static_cast<int>(0xc8963f7a), 0xff, 0xff, 0xff},
            {0x68963f7d, 0, 0, 0}, {0x28963fc5, 0x80, 0, 0}, {static_cast<int>(0xa8963f82), 0, 0x80, 0}, {0x48963f84, 0x80, 0x80, 0x40},
            {0x68963f8a, 0, 0, 0x80}, {static_cast<int>(0xa8963f8c), 0x80, 0, 0x80}, {0x68963f8f, 0, 0x80, 0x80}, {0x08963f91, 0x80, 0x80, 0x80},
            {0x28963f93, 0xc0, 0xc0, 0xc0}, {static_cast<int>(0xe8963f95), 0xff, 0, 0}, {0x28963f98, 0, 0xff, 0}, {static_cast<int>(0xe8963f9a), 0xff, 0xff, 0},
            {static_cast<int>(0xc8963fa2), 0, 0, 0xff}, {0x08963fa5, 0, 0xff, 0xff}, {0x28963fa8, 0xff, 0xff, 0xff}, {static_cast<int>(0xbb5b2ec6), 0x7c, 0x90, 0xff},
        };
        for (const Colour& c : kColours) controls->SetDefaultColor(c.id, c.r, c.g, c.b);
    }
    if (startupHooks_.setupWinCtrlResources) startupHooks_.setupWinCtrlResources(controls);
}
