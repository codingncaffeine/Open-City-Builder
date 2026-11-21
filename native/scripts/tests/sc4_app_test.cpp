// cSC4App: accessors, derived directories, ini / preferences file names, shutdown callbacks, video option mapping,
// and the preferences blob defaults.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unistd.h>

#include "rz/cRZCmdLine.h"
#include "gz/FontSys.h"
#include "gz/GZFramework.h"
#include "gz/RZServices.h"
#include "gz/WinFont.h"
#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/LanguageManager.h"
#include "sc4/App.h"
#include "sc4/AppPlatform.h"
#include "sc4/AppPreferences.h"
#include "sc4/ServiceLocator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Object final : cIGZUnknown {
    bool QueryInterface(std::uint32_t, void** out) override { *out = this; return true; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int refs = 0;
};
struct Options final : sc4::IVideoOptionStore {
    std::map<int, int> values;
    void SetOption(int option, int value) override { values[option] = value; }
    int GetOption(int option) override { auto it = values.find(option); return it == values.end() ? 0 : it->second; }
};
struct Display final : sc4::IDisplayInfo {
    void GetNthNativeResolution(int index, int& w, int& h) override { w = 1000 + index; h = 700 + index; }
};
std::string Get(void (cSC4App::*getter)(cIGZString&), cSC4App& app) { cRZString s; (app.*getter)(s); return s.ToChar(); }
std::string Get(void (cSC4App::*getter)(cIGZString&) const, cSC4App& app) { cRZString s; (app.*getter)(s); return s.ToChar(); }
bool Shut(void* flag) { ++*static_cast<int*>(flag); return true; }
}  // namespace

int main() {
    auto* app = new cSC4App("SimCity 4");
    app->AddRef();
    CHECK(app->GetServiceID() == 0x66 && app->GetServicePriority() == -2000000);
    CHECK(app->GetAppState() == 0 && !app->GetDebugFunctionalityEnabled() && app->GetPopupDialogsEnabled());
    CHECK(app->IsRunFirstTimeAfterInstall());
    app->SetDebugFunctionalityEnabled(true); CHECK(app->GetDebugFunctionalityEnabled());
    app->SetPopupDialogsEnabled(false); CHECK(!app->GetPopupDialogsEnabled());

    // directories: cached ones return what was set, derived ones come from the user data directory
    app->SetDirectories("/Games/SC4/", "/Games/SC4/Data/", "/Users/me/SC4/");
    CHECK(Get(&cSC4App::GetAppDirectory, *app) == "/Games/SC4/");
    cRZString out;
    app->GetDataDirectory(out, -1); CHECK(std::strcmp(out.ToChar(), "/Games/SC4/Data/") == 0);
    CHECK(Get(&cSC4App::GetUserPluginDirectory, *app) == "/Users/me/SC4/Plugins/");
    CHECK(Get(&cSC4App::GetRegionsDirectory, *app) == "/Users/me/SC4/Regions/");
    CHECK(Get(&cSC4App::GetMySimDirectory, *app) == "/Users/me/SC4/MySim/");
    CHECK(Get(&cSC4App::GetAlbumDirectory, *app) == "/Users/me/SC4/Albums/");
    CHECK(Get(&cSC4App::GetHTTPCacheDirectory, *app) == "/Users/me/SC4/HTTPCache/");
    CHECK(Get(&cSC4App::GetTempDirectory, *app) == "/Users/me/SC4/Temp/");
    CHECK(Get(&cSC4App::GetExceptionReportsDirectory, *app) == "/Users/me/SC4/Exception Reports/");
    CHECK(Get(&cSC4App::GetTestScriptDirectory, *app) == "/Games/SC4/Data/Scripts/TestScripts/");

    // ini / cfg names: default names while the executable name is not known
    CHECK(Get(&cSC4App::GetAppIniFileName, *app) == "SimCity 4.ini");
    app->AppNameString() = "SC4Mac";
    CHECK(Get(&cSC4App::GetAppIniFileName, *app) == "SC4Mac.ini");
    CHECK(Get(&cSC4App::GetAppPreferencesFileName, *app) == "SC4Mac.cfg");
    CHECK(Get(&cSC4App::GetAppPreferencesFilePath, *app) == "/Users/me/SC4/SC4Mac.cfg");
    CHECK(Get(&cSC4App::GetAppIniFilePath, *app) == "/Users/me/SC4/SC4Mac.ini");   // no .ini beside the (fake) app directory

    // shutdown callbacks
    int calls = 0;
    CHECK(app->RegisterShutdownCallbackFunction(Shut, &calls));
    CHECK(app->UnregisterShutdownCallbackFunction(Shut, &calls));
    CHECK(!app->UnregisterShutdownCallbackFunction(Shut, &calls));

    // video preferences <-> option store
    Options options; Display display;
    app->SetVideoOptionStore(&options); app->SetDisplayInfo(&display);
    cISC4App::SC4VideoPreferences prefs;
    auto* b = reinterpret_cast<std::uint8_t*>(&prefs);
    for (int i = 1; i <= 0xc; ++i) b[i] = static_cast<std::uint8_t>(i);
    b[0x1c] = 9;
    app->ApplyVideoPreferences(prefs);
    CHECK(options.values[0] == 1 && options.values[1] == 2 && options.values[2] == 3 && options.values[3] == 4);
    CHECK(options.values[4] == 5 && options.values[5] == 6 && options.values[6] == 7 && options.values[7] == 8);
    CHECK(options.values[8] == 9 && options.values[9] == 10 && options.values[10] == 0xc && options.values[0xf] == 9);
    options.values[0xb] = 2; options.values[0xc] = 1;
    cISC4App::SC4VideoPreferences got;
    app->GetAutoVideoPreferences(&got);
    CHECK(reinterpret_cast<std::uint8_t*>(&got)[1] == 1 && reinterpret_cast<std::uint8_t*>(&got)[0xb] == 1);
    CHECK(got.screenWidth == 1002 && got.screenHeight == 702 && got.colorDepth == 32);
    options.values[0xc] = 0; app->GetAutoVideoPreferences(&got); CHECK(got.colorDepth == 16);
    options.values[0xc] = 7; got.colorDepth = 24; app->GetAutoVideoPreferences(&got); CHECK(got.colorDepth == 24);

    // interface lookup
    void* iface = nullptr;
    CHECK(app->QueryInterface(kGZIID_cISC4App, &iface) && iface == static_cast<cISC4App*>(app));
    CHECK(app->QueryInterface(0x452294aa, &iface) && iface == static_cast<cIGZMessageTarget2*>(app));
    CHECK(app->QueryInterface(0xc6ae7085, &iface) && iface == static_cast<cIGZWinMessageFilter*>(app));
    CHECK(app->QueryInterface(0xca263481, &iface) && iface == static_cast<cIGZMessageTarget*>(app));
    CHECK(app->QueryInterface(kGZIID_cIGZApp, &iface) && iface == static_cast<cIGZApp*>(app));
    CHECK(!app->QueryInterface(0x1234, &iface));


    // ---- start-up resolution
    {
        auto* a = new cSC4App("T"); a->AddRef();
        CHECK(a->IsStartupResolutionOK(1024, 768) && a->IsStartupResolutionOK(1280, 1024) && a->IsStartupResolutionOK(1400, 1050 - 0 + 0) == false);
        CHECK(a->IsStartupResolutionOK(1680, 1050) && a->IsStartupResolutionOK(1920, 1200) && !a->IsStartupResolutionOK(1000, 768));
        CHECK(!a->IsStartupResolutionOK(800, 480));
        std::uint32_t w = 1000, h = 700;                    // area 700000 -> 1024x768
        a->FixStartupResolutionValuesIfNeeded(w, h); CHECK(w == 0x400 && h == 0x300);
        w = 640; h = 480; a->FixStartupResolutionValuesIfNeeded(w, h); CHECK(w == 800 && h == 600);
        w = 3000; h = 2000; a->FixStartupResolutionValuesIfNeeded(w, h); CHECK(w == 0x400 && h == 0x300);   // quirk: huge areas fall back to 1024x768
        w = 1800; h = 1000; a->FixStartupResolutionValuesIfNeeded(w, h); CHECK(w == 0x640 && h == 0x4b0);
        w = 2000; h = 1400; a->FixStartupResolutionValuesIfNeeded(w, h); CHECK(w == 0x800 && h == 0x600);
        w = 1024; h = 768; a->FixStartupResolutionValuesIfNeeded(w, h); CHECK(w == 1024 && h == 768);
        a->Release();
    }

    // ---- command line
    {
        auto* a = new cSC4App("T"); a->AddRef();
        auto* props = new sc4::cSC4RenderProperties(); a->SetRenderProperties(props);
        a->SetDirectories("/g/", "/g/Data/", "/u/");
        cRZCmdLine line("sc4 -IME:off -r1280x1024x32 -w -d:OpenGL -l:\"Mayor Bob\" -audio:off -radio:enabled -sim:75.5 -XDD:on -CPUCount:2 -AutoZoomChange:off -UserDir:off");
        a->SetCommandLineSource(&line);
        a->ReadCommandLine();
        auto& p = a->Preferences();
        CHECK(p.Get<std::uint8_t>(sc4::prefs::kImeEnabled) == 0);
        CHECK(p.Get<std::uint32_t>(sc4::prefs::kVideoWidth) == 1280 && p.Get<std::uint32_t>(sc4::prefs::kVideoHeight) == 1024 && p.Get<std::uint32_t>(sc4::prefs::kVideoColorDepth) == 32);
        CHECK(p.Get<std::uint8_t>(sc4::prefs::kFullScreen) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(p.data() + sc4::prefs::kLanguageName), "Mayor Bob") == 0);
        CHECK(p.Get<std::uint8_t>(sc4::prefs::kAudioEnabled) == 0 && p.Get<std::uint8_t>(sc4::prefs::kAudioEnabledMirror) == 0);
        CHECK(p.Get<std::uint32_t>(sc4::prefs::kMusicEnabled) == 1);
        CHECK(p.Get<float>(sc4::prefs::kSimulatorParameter) == 75.5f && p.Get<std::uint8_t>(sc4::prefs::kSwitchXDD) == 1);
        CHECK(!props->BoolValue(0x25));
        CHECK(Get(&cSC4App::GetUserDataDirectory, *a) == "/g/Data/");
        a->Release();            // the application owns (and frees) the render properties
    }

    // ---- preferences file round trip
    {
        char dir[] = "/tmp/sc4appXXXXXX"; CHECK(mkdtemp(dir));
        std::string user = std::string(dir) + "/";
        auto* a = new cSC4App("Tst"); a->AddRef();
        a->SetDirectories("/g/", "/g/Data/", user.c_str());
        a->AppNameString() = "Tst";
        a->Preferences().Set<std::uint32_t>(sc4::prefs::kVideoWidth, 1600);
        a->Preferences().Set<float>(sc4::prefs::kSimulatorParameter, 12.5f);
        std::memcpy(a->Preferences().data() + sc4::prefs::kLanguageName, "temp", 5);
        a->SavePreferences();
        CHECK(a->Preferences().Get<std::uint32_t>(sc4::prefs::kAppResetFlag) == 1 && a->Preferences().data()[sc4::prefs::kLanguageName] == 0);
        auto* b = new cSC4App("Tst"); b->AddRef();
        b->SetDirectories("/g/", "/g/Data/", user.c_str());
        b->AppNameString() = "Tst";
        CHECK(b->IsRunFirstTimeAfterInstall());
        b->ReadPreferences();
        CHECK(!b->IsRunFirstTimeAfterInstall());
        CHECK(b->Preferences().Get<std::uint32_t>(sc4::prefs::kVideoWidth) == 1600 && b->Preferences().Get<float>(sc4::prefs::kSimulatorParameter) == 12.5f);
        CHECK(std::memcmp(a->Preferences().data(), b->Preferences().data(), sc4::kPreferencesSize) == 0);
        a->Release(); b->Release();
        std::string cleanup = "rm -rf " + std::string(dir); std::system(cleanup.c_str());
    }


    // ---- AddCOMDirectors / AddSystemServices against the real framework
    {
        struct Fake final : cRZSystemService {
            Fake(std::uint32_t id) : cRZSystemService(id, 1000) {}
            bool QueryInterface(std::uint32_t iid, void** out) override { (void)iid; *out = static_cast<cIGZSystemService*>(this); AddRef(); return true; }
            std::uint32_t AddRef() override { return cRZSystemService::AddRef(); }
            std::uint32_t Release() override { return cRZSystemService::Release(); }
        };
        auto* f = new cGZFramework();
        cGZFramework::sSetFrameWork(f);
        f->COM().RealInit();
        auto* a = new cSC4App("T", f->AppView()); a->AddRef();
        a->SetFramework(f);
        a->SetDirectories("/g/", "/g/Data/", "/u/");
        struct { const char* name; std::uint32_t id; } fakes[] = {{"cGZAllocatorService", 0x3a}, {"cGZFileSystem", 0x3b}, {"nGZLog::cService", 0x3e}, {"cGZRegistry", 0x3f},
            {"cRZLanguageManager", 0x40}, {"cGZPersistResourceManager", 0x41}, {"cSC4AudioAEMSEventSystem", 0x42}};
        for (auto& fake : fakes) { std::uint32_t id = fake.id; a->SetServiceFactory(fake.name, [id]() -> cIGZUnknown* { return static_cast<cIGZUnknown*>(static_cast<cIGZSystemService*>(new Fake(id))); }); }
        a->StartupHooks().showFpsPreference = [] { return true; };
        a->AddCOMDirectors();
        a->AddSystemServices();
        void* out = nullptr;
        for (auto& fake : fakes) {
            CHECK(f->GetSystemService(fake.id, kGZIID_cIGZSystemService, &out));
            if (out) static_cast<cIGZSystemService*>(out)->Release();   // GetSystemService hands out a reference
            out = nullptr;
        }
        CHECK(!f->GetSystemService(0x99, kGZIID_cIGZSystemService, &out));
        a->Release();
        delete f;
    }


    // ---- PreFrameWorkInit end to end (real framework, real language manager, fake graphics system)
    {
        struct Graphics final : IGraphicSystemControl {
            DisplayMetrics screen{1440, 900, 32};
            std::vector<std::string> log; DisplayMetrics desired; bool windowed = true; std::uint32_t driver = 0;
            DisplayMetrics GetScreenMetrics() override { return screen; }
            void PreInitSetDesiredGameResolution(const DisplayMetrics& m) override { desired = m; log.push_back("resolution"); }
            void PreInitSetWindowedMode(bool w) override { windowed = w; log.push_back("windowed"); }
            void DisableScreenBuffer() override { log.push_back("disable"); }
            void EnableCursorEmulation(bool e) override { log.push_back(e ? "cursor-on" : "cursor-off"); }
            void EnablePageFlipping(bool) override { log.push_back("flip"); }
            bool IsPageFlippingEnabled() override { return true; }
            void SetDefaultDriverClassID(std::uint32_t d) override { driver = d; log.push_back("driver"); }
            bool CanvasReady() override { return true; }
            DisplayMetrics GetGameMetrics() override { return {1024, 768, 32}; }
            gd::cIGZGDriver* GetGDriver() override { return nullptr; }
            int GetGDriverCount() override { return 0; }
            gd::cIGZGDriver* GetGDriverByIndex(int) override { return nullptr; }
            void SetGDriver(gd::cIGZGDriver*) override {}
            void SetForceNoTranslucency(bool, float) override {}
            cIGZBuffer* CreateBuffer() override { return nullptr; }
            void RenderBufferToScreen(cIGZBuffer*, const cRZRect&, const cRZRect&) override {}
            void LazyFlush() override {}
            cRZRect CanvasRect() override { return cRZRect{0, 0, 640, 480}; }
            bool IsFullScreenMode() override { return false; }
            std::uint32_t GetScreenPixelFormatBits() override { return 32; }
        };
        char dir[] = "/tmp/sc4pfiXXXXXX"; CHECK(mkdtemp(dir));
        std::string root = dir;
        std::filesystem::create_directories(root + "/Data/English");
        std::ofstream(root + "/Data/English/text.str") << "x";
        std::filesystem::create_directories(root + "/Data/German");
        std::ofstream(root + "/Data/German/data.dat") << "x";
        std::filesystem::create_directories(root + "/Data/French");        // no string tables: not available
        SetDocumentsFolderProvider([&](std::string& out) { out = root + "/Documents"; return true; });
        std::filesystem::create_directories(root + "/Documents");

        auto* f = new cGZFramework();
        cGZFramework::sSetFrameWork(f);
        f->COM().RealInit();
        auto* a = new cSC4App("T", f->AppView()); a->AddRef();
        a->SetFramework(f);
        auto* languages = new cRZLanguageManager();
        a->SetServiceFactory("cRZLanguageManager", [languages]() -> cIGZUnknown* { return static_cast<cIGZUnknown*>(static_cast<cIGZSystemService*>(languages)); });
        Graphics graphics;
        a->StartupHooks().graphicSystem = [&] { return &graphics; };
        a->StartupHooks().fileSystemDirectories = [&](sc4::FileSystemDirectories& d) { d.appDirectory = root + "/App/"; d.cdAppDirectory = root + "/CD/"; d.dataDirectory = root + "/Data/"; d.cdDataDirectory = root + "/CDData/"; return true; };
        a->StartupHooks().bundleGameDataDirectory = [&] { return root + "/Data"; };
        cRZCmdLine line("sc4 -r1280x1024x32 -d:opengl -cursormode:blit");
        f->SetCommandLine(line);
        CHECK(a->PreFrameWorkInit());
        CHECK(a->Preferences().Get<std::uint32_t>(sc4::prefs::kVideoWidth) == 1280);
        CHECK(graphics.log.size() >= 6 && graphics.log[0] == "disable" && graphics.log[1] == "cursor-on" && graphics.log[2] == "flip");
        // the data directories, the derived ones, the user data directory
        CHECK(Get(&cSC4App::GetPluginDirectory, *a) == root + "/Data/Plugins/");
        CHECK(Get(&cSC4App::GetUserDataDirectory, *a) == root + "/Documents/SimCity 4/");
        CHECK(std::filesystem::is_directory(root + "/Documents/SimCity 4"));
        CHECK(Get(&cSC4App::GetAppDirectory, *a) == root + "/App/");
        // languages: English and German are available (string tables / data), French is not
        for (std::uint32_t id = 1; id <= 0x2c; ++id) {
            cRZString folder;
            if (!languages->GetLanguageDirectoryName(folder, id)) continue;
            std::string name = folder.ToChar();
            if (name == "English" || name == "German") CHECK(languages->CanSwitchToLanguage(id));
            if (name == "French") CHECK(!languages->CanSwitchToLanguage(id));
        }
        CHECK(languages->GetCurrentLanguage() == a->Preferences().Get<std::uint32_t>(sc4::prefs::kLanguageId));
        CHECK(graphics.driver == 0xc4554841u);                           // -d:opengl
        CHECK(a->Preferences().data()[sc4::prefs::kVideoHardwareDriver] == 0xff);   // not the DirectX id: flagged "other"
        CHECK(!graphics.windowed);                                        // 1280x1024 does not fit the 1440x900 desktop -> full screen
        CHECK(a->GetRenderProperties() != nullptr);
        a->Release();
        delete f;
        std::string cleanup = "rm -rf " + root; std::system(cleanup.c_str());
    }


    // ---- Init: failure without a canvas, success with the resources set up
    {
        struct Graphics final : IGraphicSystemControl {
            bool ready = true; bool forced = false;
            DisplayMetrics GetScreenMetrics() override { return {1440, 900, 32}; }
            void PreInitSetDesiredGameResolution(const DisplayMetrics&) override {}
            void PreInitSetWindowedMode(bool) override {}
            void DisableScreenBuffer() override {}
            void EnableCursorEmulation(bool) override {}
            void EnablePageFlipping(bool) override {}
            bool IsPageFlippingEnabled() override { return false; }
            void SetDefaultDriverClassID(std::uint32_t) override {}
            bool CanvasReady() override { return ready; }
            DisplayMetrics GetGameMetrics() override { return {1024, 768, 32}; }
            gd::cIGZGDriver* GetGDriver() override { return nullptr; }
            int GetGDriverCount() override { return 0; }
            gd::cIGZGDriver* GetGDriverByIndex(int) override { return nullptr; }
            void SetGDriver(gd::cIGZGDriver*) override {}
            void SetForceNoTranslucency(bool force, float) override { forced = force; }
            cIGZBuffer* CreateBuffer() override { return nullptr; }
            void RenderBufferToScreen(cIGZBuffer*, const cRZRect&, const cRZRect&) override {}
            void LazyFlush() override {}
            cRZRect CanvasRect() override { return cRZRect{0, 0, 640, 480}; }
            bool IsFullScreenMode() override { return false; }
            std::uint32_t GetScreenPixelFormatBits() override { return 32; }
        };
        struct Options final : sc4::IVideoOptionStore {
            void SetOption(int, int) override {}
            int GetOption(int option) override { return option == 0xd ? 1 : 0; }
        };
        char dir[] = "/tmp/sc4initXXXXXX"; CHECK(mkdtemp(dir));
        std::string root = dir;
        std::filesystem::create_directories(root + "/Data/English");
        std::filesystem::create_directories(root + "/Docs");
        SetDocumentsFolderProvider([&](std::string& out) { out = root + "/Docs"; return true; });

        auto* f = new cGZFramework();
        cGZFramework::sSetFrameWork(f);
        f->COM().RealInit();
        auto* a = new cSC4App("T", f->AppView()); a->AddRef();
        a->SetFramework(f);
        a->SetDirectories((root + "/App/").c_str(), (root + "/Data/").c_str(), (root + "/Docs/SimCity 4/").c_str());
        a->SetPluginDirectories((root + "/Data/Plugins/").c_str(), (root + "/Docs/SimCity 4/Plugins/").c_str(), (root + "/Data/Sku/").c_str());
        for (std::string sub : {"/Data/Sku", "/Data/Plugins", "/Docs/SimCity 4/Plugins", "/Data/English"}) {   // one (empty) database in each directory
            std::filesystem::create_directories(root + sub);
            cRZAutoRefCount<cGZDBSegmentPackedFile> database(new cGZDBSegmentPackedFile());
            database->Init();
            database->SetPath(cRZString((root + sub + "/empty.dat").c_str()));
            CHECK(database->Open(true, true));
            database->Close();
        }
        auto* languages = new cRZLanguageManager(); f->AddSystemService(languages);
        auto* resources = new cGZPersistResourceManager(); f->AddSystemService(resources);
        Graphics graphics; Options options;
        std::vector<std::string> initialized; std::vector<std::string> alerts;
        a->StartupHooks().graphicSystem = [&] { return &graphics; };
        a->StartupHooks().createVideoOptionStore = [&] { return &options; };
        a->StartupHooks().messageBox = [&](const std::string& text, const std::string&) { alerts.push_back(text); };
        a->StartupHooks().initializeManager = [&](const char* name, cIGZUnknown*) { initialized.push_back(name); };
        for (const char* name : {"cSC4GlyphTextureManager", "cSCStringDetokenizer", "cSLWinLocationSaver", "cSC4FeatureManager", "cSC4ToolTipManager", "cSC4MissingPluginDataReporter", "cSC4Nation"}) {
            a->SetServiceFactory(name, [] () -> cIGZUnknown* { static int dummy; return reinterpret_cast<cIGZUnknown*>(new Object()); });
        }
        graphics.ready = false;
        CHECK(!a->Init());
        CHECK(alerts.size() == 1 && a->Preferences().data()[sc4::prefs::kVideoHardwareDriver] == 0 && a->Preferences().data()[sc4::prefs::kVideoAutoConfigure] == 1);
        graphics.ready = true;
        a->Preferences().data()[sc4::prefs::kVideoAutoConfigure] = 0;
        CHECK(a->Init());
        CHECK(a->GetCheatCodeManager() != nullptr && SL::spSC4 == static_cast<cISC4App*>(a));
        CHECK(resources->GetSegmentCount() >= 3);
        CHECK(initialized.size() >= 5);
        for (std::uint32_t i = 0; i < resources->GetSegmentCount(); ++i) {   // Init opened these; close them before the manager lets go
            cIGZPersistDBSegment* segment = resources->GetSegmentByIndex(i);
            segment->Close();
            segment->Shutdown();
        }
        resources->Shutdown();
        std::string cleanup = "rm -rf " + root; std::system(cleanup.c_str());
        a->Release();
        delete f;
    }

    // ---- the app's remaining work: saving, obliteration, the registry, the fonts, the exception uploader
    {
        struct Server final : cIGZMessageServer2 {
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return 2; }
            std::uint32_t Release() override { return 1; }
            bool MessageSend(cIGZMessage2* m) override { sent.push_back(m->GetType()); return true; }
            bool MessagePost(cIGZMessage2* m, bool) override { posted.push_back(m->GetType()); return true; }
            bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
            bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
            bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
            bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
            bool OnTick(std::uint32_t) override { return true; }
            std::uint32_t GetMessageQueueSize() override { return 0; }
            void SetAlwaysClearQueueOnTick(bool) override {}
            std::uint32_t GetRefCount() const override { return 1; }
            bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
            std::vector<std::uint32_t> sent, posted;
        };
        struct Files final : cRZSystemService {
            Files() : cRZSystemService(kGZSID_cGZFileSystem, 100) {}
            struct View final : cIGZFileSystem {
                std::string data = "/data/";
                const char* AppDrive() override { return ""; }
                const char* AppDirectory() override { return "/app/"; }
                const char* AppName() override { return "sc4"; }
                const char* AppExtension() override { return ""; }
                const char* DataDirectory() override { return data.c_str(); }
                const char* PlugInDirectory() override { return ""; }
                const char* CDAppDrive() override { return ""; }
                const char* CDAppDirectory() override { return ""; }
                const char* CDDataDirectory() override { return ""; }
                const char* CDPlugInDirectory() override { return ""; }
                bool FindCDDrive() override { return false; }
                void UseCDIniFile(bool) override {}
                void AllowRecursiveCDSearch(bool) override {}
                void SetExpectedCDName(const cIGZString&) override {}
                void SetExpectedCDAppPath(const cIGZString&) override {}
                void SetDataDirectory(const cIGZString&) override {}
                void SetPluginDirectory(const cIGZString&) override {}
                void GetPluginDirectoryName(cIGZString&) override {}
                void SetPluginDirectoryName(const cIGZString&) override {}
                bool QueryInterface(std::uint32_t, void**) override { return false; }
                std::uint32_t AddRef() override { return 1; }
                std::uint32_t Release() override { return 1; }
            } view;
            bool QueryInterface(std::uint32_t iid, void** out) override {
                if (iid == kGZIID_cIGZFileSystem) { *out = static_cast<cIGZFileSystem*>(&view); return true; }
                return cRZSystemService::QueryInterface(iid, out);
            }
            std::uint32_t AddRef() override { return cRZSystemService::AddRef(); }
            std::uint32_t Release() override { return cRZSystemService::Release(); }
        };
        struct Fonts final : cRZSystemService, cIGZFontSys {
            Fonts() : cRZSystemService(kGZFontSysDefaultServiceID, 100) {}
            std::vector<std::string> directories, aliases, removed, acquired;
            bool QueryInterface(std::uint32_t iid, void** out) override {
                if (iid == kGZIID_cIGZFontSys) { *out = static_cast<cIGZFontSys*>(this); AddRef(); return true; }
                return cRZSystemService::QueryInterface(iid, out);
            }
            std::uint32_t AddRef() override { return cRZSystemService::AddRef(); }
            std::uint32_t Release() override { return cRZSystemService::Release(); }
            bool Init() override { return true; }
            bool Shutdown() override { return true; }
            bool SetDefaultCodePage(int) override { return true; }
            void SetDefaultFontRenderer(cIGZFontRenderer*) override {}
            void SetDefaultFontAntialiasing(int) override {}
            void SetDefaultUnantialiasedSizeRange(int, int, const cIGZString*) override {}
            cIGZFont* FontAcquire(const cIGZString& names, int type, std::uint32_t size, int style, bool reuse) override {
                acquired.push_back(std::string(names.ToChar()) + "/" + std::to_string(type) + "/" + std::to_string(size) + "/" +
                                   std::to_string(style) + "/" + (reuse ? "reuse" : "new"));
                return nullptr;
            }
            cIGZFont* FontAcquire(const cIGZString&, std::uint32_t, int, bool) override { return nullptr; }
            cIGZFont* FontAcquire(int, std::uint32_t, int, std::uint32_t, bool) override { return nullptr; }
            int FontRelease(cIGZFont*) override { return 0; }
            bool AddFont(const cIGZString&) override { return false; }
            int AddAllFontsFromDirectory(const cIGZString& directory) override { directories.push_back(directory.ToChar()); return 0; }
            bool AddFont(cIGZFont*) override { return false; }
            bool RemoveFontByFile(const cIGZString&) override { return false; }
            bool RemoveFontByName(const cIGZString& name) override { removed.push_back(name.ToChar()); return true; }
            std::uint32_t EnumerateFontInfo(cIGZFontSys::FontInfo*, std::uint32_t) override { return 0; }
            std::uint32_t EnumerateManagedFonts(cIGZFont**, std::uint32_t) override { return 0; }
            bool AddSubstitution(const cIGZString& from, const cIGZString& to) override { aliases.push_back(std::string(from.ToChar()) + "->" + to.ToChar()); return true; }
            std::uint32_t EnumerateFontSubstitutions(const cIGZString&, cIGZString**, std::uint32_t) override { return 0; }
            bool IsFontAvailable(const cIGZString&, std::uint32_t, int) override { return false; }
            bool IsSubstitutionAvailable(const cIGZString&, cIGZString&) override { return false; }
        };
        struct City final : cISC4City {
            std::string name = "Twin Peaks", path, newPath;
            std::uint32_t serial = 0x1234;
            int refs = 1, obliterations = 0, establishedCalls = 0, nameChangedCalls = 0, shutdowns = 0;
            bool established = true;
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return ++refs; }
            std::uint32_t Release() override { return --refs; }
            bool Init() override { return true; }
            bool Shutdown() override { ++shutdowns; return true; }
            std::uint32_t GetCitySerialNumber() const override { return serial; }
            void GetCityName(cIGZString& out) const override { out.AssignFromCString(name.c_str()); }
            void SetCitySerialNumber(std::uint32_t value) override { serial = value; }
            void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString(path.c_str()); }
            void SetCitySaveFilePath(const cIGZString& value) override { newPath = value.ToChar(); }
            void SetCityNameChanged(bool) override { ++nameChangedCalls; }
            void SetEstablished(bool value) override { ++establishedCalls; established = value; }
            bool IsSaveDisabled() const override { return false; }
            bool SaveObliterated(cIGZPersistDBSegment*) const override { const_cast<City*>(this)->obliterations++; return true; }
        };

        char dir[] = "/tmp/sc4saveXXXXXX"; CHECK(mkdtemp(dir));
        std::string root = dir;
        std::filesystem::create_directories(root + "/Data/Sys");
        std::ofstream(root + "/Data/Sys/Resources.ini")
            << "[DBSegments]\npackedexemplars=off\npackedtextures=on\n"
            << "[DBSegmentDirectories]\n0x11111111=0x22222222 %DataDirectory%/Foo\n";
        RZ::ResetCaches();
        auto* f = new cGZFramework();
        cGZFramework::sSetFrameWork(f);
        f->COM().RealInit();
        auto* files = new Files(); files->view.data = root + "/Data/";
        f->AddSystemService(files);
        auto* fonts = new Fonts();
        f->AddSystemService(fonts);
        auto* a = new cSC4App("T", f->AppView()); a->AddRef();
        a->SetFramework(f);
        a->SetDirectories((root + "/App/").c_str(), (root + "/Data/").c_str(), (root + "/Docs/").c_str());
        a->SetPluginDirectories((root + "/Data/Plugins/").c_str(), (root + "/Docs/Plugins/").c_str(), (root + "/Data/Sku/").c_str());
        Server server; SL::spMS2 = &server;

        // the registry: -packedexemplars / -packedtextures override the globals (only when Resources.ini exists), then the
        // [DBSegments] and [DBSegmentDirectories] entries go to their callbacks
        cRZCmdLine switches("sc4 -packedexemplars:off -packedtextures:on");
        f->SetCommandLine(switches);
        CHECK(a->LoadRegistry());
        CHECK(!gbUsingPackedExemplars && gbUsingPackedTextures);
        cRZCmdLine other("sc4");
        f->SetCommandLine(other);
        gbUsingPackedExemplars = true;
        CHECK(a->LoadRegistry());
        CHECK(gbUsingPackedExemplars);
        std::filesystem::remove(root + "/Data/Sys/Resources.ini");
        CHECK(a->LoadRegistry());                                    // no ini: the switches are not read
        CHECK(gbUsingPackedExemplars);

        // fonts: an alias is AddSubstitution(value, key); the font set adds the plugin directory and <data>/Fonts/
        cSC4App::SetFontAliasStatic(cRZString("Arial"), cRZString("Helvetica"), a);
        CHECK(fonts->aliases.size() == 1 && fonts->aliases[0] == "Helvetica->Arial");
        a->SetupFontSet();
        CHECK(fonts->directories.size() == 2 && fonts->directories[0] == root + "/Data/Plugins/" && fonts->directories[1] == root + "/Data/Fonts/");

        // [Font Styles]: the specification's faces, size and style flags acquire the font (type 0, reused); this font
        // system has no such font, so nothing is registered
        a->SetFontStyle(cRZString("Status"), cRZString("\"Arta\", \"14\", \"italic|aa=bg|underline\", 0x5a809915"));
        CHECK(fonts->acquired.size() == 1 && fonts->acquired[0] == "Arta/0/14/6/reuse");
        cSC4App::SetFontStyleStatic(cRZString("Status"), cRZString("\"StocletITC TT\", \"15\", \"bold\", 0xea85d303"), a);
        CHECK(fonts->acquired.size() == 2 && fonts->acquired[1] == "StocletITC TT/0/15/1/reuse");

        // the exception report uploader: found in the user plugin directory, then the plugin directory, then the app's
        std::vector<std::string> spawned;
        a->StartupHooks().spawnWait = [&](const std::string& path) { spawned.push_back(path); };
        a->LaunchExceptionReportUploader();
        CHECK(spawned.empty());
        std::filesystem::create_directories(root + "/Docs/Plugins");
        std::ofstream(root + "/Docs/Plugins/Exception Report Uploader.exe") << "x";
        a->LaunchExceptionReportUploader();
        CHECK(spawned.size() == 1 && spawned[0] == root + "/Docs/Plugins/Exception Report Uploader.exe");

        // saving: the canonical path is asked from the host; on failure the city keeps its old path and the file is not
        // replaced. The messages carry the segment and both paths.
        City* city = new City();
        city->path = root + "/docs/Twin Peaks.sc4";
        a->SetCity(city);
        std::vector<std::string> savePaths;
        a->CityHooks().createSavePathForCity = [&](const cRZString& name, std::uint32_t serial, cRZString& out) {
            std::string p = root + "/save/" + name.ToChar() + "." + std::to_string(serial) + ".sc4";
            savePaths.push_back(p);
            out.AssignFromCString(p.c_str());
        };
        std::filesystem::create_directories(root + "/save");
        int versionRecords = 0;
        a->CityHooks().writeLoadSaveVersion = [&](cIGZPersistDBSegment*) { ++versionRecords; return true; };
        server.sent.clear();
        CHECK(a->SaveCity(cRZString(city->path.c_str()), true));
        CHECK(savePaths.size() == 1 && savePaths[0] == root + "/save/Twin Peaks." + std::to_string(city->serial) + ".sc4");
        CHECK(std::find(server.sent.begin(), server.sent.end(), 0x26c63343u) != server.sent.end());
        CHECK(std::find(server.sent.begin(), server.sent.end(), 0x26c63346u) != server.sent.end());
        CHECK(std::find(server.sent.begin(), server.sent.end(), 0x26c63344u) != server.sent.end());
        CHECK(std::find(server.sent.begin(), server.sent.end(), 0x26c63345u) != server.sent.end());
        CHECK(versionRecords == 1);
        CHECK(city->newPath == savePaths[0]);                        // the canonical path became the city's save path
        CHECK(std::filesystem::exists(savePaths[0]));                // the temporary file was renamed into place
        CHECK(city->nameChangedCalls == 1);

        // obliterating: the empty state is written into a temporary segment, the city's own file removed, a new city made
        // and saved to the old path with the camera kept
        Object viewUtilities;
        SL::spViewUtilities = reinterpret_cast<SC4ViewUtilities*>(&viewUtilities);   // only its presence is read
        SL::spView3D = reinterpret_cast<SL::IView3DSystems*>(&viewUtilities);
        a->CityHooks().viewZoomAndRotation = [&](int& z, int& r) { z = 3; r = 5; };
        std::vector<std::pair<int, int>> restored;
        a->CityHooks().setViewZoomAndRotation = [&](int z, int r) { restored.emplace_back(z, r); };
        City* created = nullptr;
        a->CityHooks().createCity = [&]() -> cISC4City* { created = new City(); created->path = root + "/docs/Obliterated.sc4"; return created; };
        savePaths.clear();
        CHECK(a->DoCityObliteration());
        CHECK(city->obliterations == 1);                             // SaveCityForObliteration used the city
        CHECK(restored.size() == 1 && restored[0].first == 3 && restored[0].second == 5);
        CHECK(created != nullptr && created->establishedCalls >= 1);
        CHECK(savePaths.size() >= 1);                                // the empty city went through SaveCity

        // the two registry entry callbacks never crash, whatever the entries are
        cSC4App::RegisterDBSegmentCallback(cRZString("BEGIN_GROUP"), cRZString("NON_PACKED_EXEMPLARS"), a);
        cSC4App::RegisterDBSegmentCallback(cRZString("0x1234"), cRZString("0x5678 x"), a);
        cSC4App::RegisterDBSegmentCallback(cRZString("END_GROUP"), cRZString("NON_PACKED_EXEMPLARS"), a);
        cSC4App::RegisterDBSegmentDirectoryFilesCallback(cRZString("0x1234"), cRZString("0x5678 x"), a);
        SL::spMS2 = nullptr;
        SL::spViewUtilities = nullptr;
        SL::spView3D = nullptr;
        std::string cleanup = "rm -rf " + root; std::system(cleanup.c_str());
        a->Release();
        delete created;   // the city mocks refcount down without freeing
        delete f;   // Release never destroys the framework
        delete city;
    }

    // preferences blob defaults
    sc4::SC4Preferences p;
    CHECK(sc4::SC4Preferences::size() == 0x1734);
    CHECK(p.Get<std::uint8_t>(0x004) == 1 && p.Get<std::uint32_t>(0x008) == 0x400 && p.Get<std::uint32_t>(0x00c) == 0x320);
    CHECK(p.Get<std::uint8_t>(0x04 + 0x28) == 1 && p.Get<std::uint8_t>(0x04 + 0x24) == 0 && p.Get<std::uint8_t>(0x04 + 0x63) == 1);
    CHECK(p.Get<std::uint32_t>(0x470) == 0x42480000 && p.Get<std::uint8_t>(0x49c) == 1);
    CHECK(p.Get<std::uint32_t>(0x4a8) == 1 && p.Get<std::uint32_t>(0x4a4 + 0x48) == 0x1000101);
    CHECK(p.Get<std::uint8_t>(sc4::prefs::kVideoAutoConfigure) == 1 && p.Get<std::uint8_t>(sc4::prefs::kVideoHardwareDriver) == 1);
    CHECK(p.Get<std::uint32_t>(sc4::prefs::kVideoWidth) == 1024 && p.Get<std::uint32_t>(sc4::prefs::kVideoHeight) == 768);
    CHECK(p.Get<std::uint8_t>(0xf28) == 3 && p.Get<std::uint16_t>(0xf16) == 0x103);

    for (int i = 0; i < 4; ++i) app->Release();   // the successful QueryInterface calls above
    app->Release(); app->Release();
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
