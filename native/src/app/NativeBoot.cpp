// Platform glue of the native Metal application (no recovered counterpart on macOS); derived from the early host.
// The boot_probe sequence (src/tools/boot_probe.cpp) run inside the host: the recovered cSC4App spine against the real
// game data, with the host's window manager and graphics behind the application's hooks.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "sc4/DBSegment.h"
#include "platform/GameDataPath.h"
#include "app/NativeBoot.h"
#include "gz/GZFramework.h"
#include "lua/SCLuaInstance.h"
#include "rz/COMDirectors.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/ServiceLocator.h"
#include "rz/LanguageManager.h"
#include "rz/cRZCmdLine.h"
#include "sc4/App.h"
#include "sc4/AppPlatform.h"

static int g_gameW = 1024, g_gameH = 768;       // the game's (logical) resolution: the UI lays out in it
static int g_screenW = 1440, g_screenH = 900;   // the display the window is on
namespace native {
void SetGameResolution(int width, int height, int screenWidth, int screenHeight) {
    g_gameW = width; g_gameH = height; g_screenW = screenWidth; g_screenH = screenHeight;
}
}  // namespace native
namespace {
struct Object final : cIGZUnknown {
    bool QueryInterface(std::uint32_t, void** out) override { *out = this; return true; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int refs = 0;
};
struct Graphics final : IGraphicSystemControl {
    gd::cIGZGDriver* driver = nullptr;   // the native Metal driver the application window made
    DisplayMetrics GetScreenMetrics() override { return {std::uint32_t(g_screenW), std::uint32_t(g_screenH), 32}; }
    void PreInitSetDesiredGameResolution(const DisplayMetrics&) override {}
    void PreInitSetWindowedMode(bool) override {}
    void DisableScreenBuffer() override {}
    void EnableCursorEmulation(bool) override {}
    void EnablePageFlipping(bool) override {}
    bool IsPageFlippingEnabled() override { return false; }
    void SetDefaultDriverClassID(std::uint32_t) override {}
    bool CanvasReady() override { return true; }
    DisplayMetrics GetGameMetrics() override { return {std::uint32_t(g_gameW), std::uint32_t(g_gameH), 32}; }
    gd::cIGZGDriver* GetGDriver() override { return driver; }
    int GetGDriverCount() override { return driver ? 1 : 0; }
    gd::cIGZGDriver* GetGDriverByIndex(int) override { return driver; }
    void SetGDriver(gd::cIGZGDriver*) override {}
    void SetForceNoTranslucency(bool, float) override {}
    cIGZBuffer* CreateBuffer() override { return nullptr; }
    void RenderBufferToScreen(cIGZBuffer*, const cRZRect&, const cRZRect&) override {}
    void LazyFlush() override {}
    cRZRect CanvasRect() override { return cRZRect{0, 0, 640, 480}; }
    bool IsFullScreenMode() override { return false; }
    std::uint32_t GetScreenPixelFormatBits() override { return 32; }
};
// The video options (cSC4App::ApplyVideoPreferences writes them, GetAutoVideoPreferences reads the automatic choice).
// Any machine that runs this build is far past the game's high tier: the automatic choice is High everywhere, every
// effect on, hardware rendering, 32-bit colour. Option numbers: 0 visual effects, 1 cars / sims, 2 shadows, 3 texture
// quality, 9 city detail, 0xf building draw speed (1..3); 4 clouds / fog, 5 waves, 7 colour cursor, 8 UI translucency,
// 10 variable speed automata (0 / 1); 6 renderer (1 hardware); 0xb the native resolution's index; 0xc colour depth
// (1 = 32 bit); 0xd (1).
struct Options final : sc4::IVideoOptionStore {
    std::map<int, int> values = {{0, 3}, {1, 3}, {2, 3}, {3, 3}, {9, 3}, {0xf, 3}, {4, 1}, {5, 1}, {7, 1}, {8, 1},
                                 {10, 1}, {6, 1}, {0xb, 0}, {0xc, 1}, {0xd, 1}};
    void SetOption(int option, int value) override { values[option] = value; }
    int GetOption(int option) override { auto it = values.find(option); return it == values.end() ? 0 : it->second; }
};
}  // namespace


namespace native {
std::string DocumentsDirectory() {
    if (const char* forced = std::getenv("SC4_DOCUMENTS"); forced && *forced) return forced;
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return std::string(data) + "/open-city-builder";
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/.local/share/open-city-builder";
}

static cGZFramework* g_framework = nullptr;
static cSC4App* g_app = nullptr;
cGZFramework* BootFramework() { return g_framework; }
cSC4App* BootApplication() { return g_app; }

bool RunBoot(bool verbose, bool keepAlive, gd::cIGZGDriver* driver, const std::function<void(cGZFramework&, cSC4App&)>& beforeInit) {
    int failures = 0;
    auto Step = [&](const char* name, bool ok) { std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name); std::fflush(stdout); if (!ok) ++failures; };

    const std::string bundle = platform::GameContentsDir();
    // The game's documents (preferences, regions, saved cities): $XDG_DATA_HOME/open-city-builder (~/.local/share/...),
    // kept between runs. SC4_DOCUMENTS overrides it (the tests point it into their sandbox).
    static const std::string docs = DocumentsDirectory();
    std::error_code docsError;
    std::filesystem::create_directories(docs, docsError);
    SetDocumentsFolderProvider([](std::string& out) { out = docs; return true; });

    auto* framework = new cGZFramework();
    g_framework = framework;
    cGZFramework::sSetFrameWork(framework);
    framework->COM().RealInit();
    auto* app = new cSC4App("SimCity 4", framework->AppView());
    g_app = app;
    app->AddRef();
    app->SetFramework(framework);
    auto* languages = new cRZLanguageManager();
    framework->AddSystemService(languages);
    auto* resources = new cGZPersistResourceManager();
    framework->AddSystemService(resources);
    SL::spRM = resources;   // the service locator's own lookup (ServiceLocator.cpp) does not find the manager under its id yet
    sc4::SetPersistResourceManagerService(resources);   // the occupants' exemplar holders resolve their exemplars through it

    // static: the start-up hooks outlive this function when the application is kept alive
    static Graphics graphics;
    static Options options;
    graphics.driver = driver;
    app->StartupHooks().graphicSystem = [&] { return &graphics; };
    app->StartupHooks().createVideoOptionStore = [&] { return &options; };
    app->StartupHooks().messageBox = [](const std::string& text, const std::string&) { std::printf("  alert: %s\n", text.c_str()); };
    app->StartupHooks().initializeManager = [](const char* name, cIGZUnknown*) { std::printf("  init %s\n", name); };
    app->StartupHooks().fileSystemDirectories = [&](sc4::FileSystemDirectories& d) {
        d.appDirectory = bundle + "/MacOS/"; d.cdAppDirectory = bundle + "/MacOS/"; d.dataDirectory = bundle + "/GameData/"; d.cdDataDirectory = bundle + "/GameData/";
        return true;
    };
    app->StartupHooks().bundleGameDataDirectory = [&] { return bundle + "/GameData"; };
    for (const char* name : {"cSC4GlyphTextureManager", "cSCStringDetokenizer", "cSLWinLocationSaver", "cSC4FeatureManager", "cSC4ToolTipManager", "cSC4MissingPluginDataReporter"})   // the nation is created by the application (NativeRegion)
        app->SetServiceFactory(name, []() -> cIGZUnknown* { return new Object(); });
    cRZCmdLine line("sc4");
    framework->SetCommandLine(line);

    // GZCOM class cSCLua (0xea10c4a2 / 0x4b2811aa): the scripting host on the vendored Lua 5.0 (the app's Lua, advisors, tutorial).
    gz::RegisterDirectorMaker("cSCLua", +[]() -> cIGZUnknown* { return lua5::MakeSCLuaObject(); });
    if (beforeInit) beforeInit(*framework, *app);
    Step("PreFrameWorkInit", app->PreFrameWorkInit());
    Step("Init", app->Init());
    Step("PostFrameWorkInit", app->PostFrameWorkInit());
    std::printf("  resource segments: %u\n", static_cast<unsigned>(resources->GetSegmentCount()));
    if (!keepAlive) {   // the host keeps the application alive when it goes on to build UI from the game data
        Step("PreFrameWorkShutdown", app->PreFrameWorkShutdown());
        Step("Shutdown", app->Shutdown());
    }
    return failures == 0;
}
}  // namespace native
