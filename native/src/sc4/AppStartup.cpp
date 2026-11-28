// cSC4App start-up: the COM directors and the system services the framework runs.
#include "gz/GZCOMDirector.h"
#include "gz/cGZCOM.h"
#include "gz/cIGZFrameWork.h"
#include "gz/cIGZSystemService.h"
#include "rz/COMDirectors.h"
#include "rz/cRZOStream.h"
#include "gz/RZServices.h"
#include "rz/cRZFile.h"
#include "rz/cRZVersion.h"
#include "rz/LanguageManager.h"
#include "rz/Threading.h"
#include "rz/cRZIniFile.h"
#include "rz/cRZSysServPtr.h"
#include "rz/cRZTimer.h"
#include "sc4/App.h"
#include "sc4/AppPlatform.h"
#include "sc4/RenderProperties.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>
#include <fnmatch.h>

namespace {
constexpr std::uint32_t kSystemServiceIid = kGZIID_cIGZSystemService;   // 0x287fb697

// The recurring step: QueryInterface(system service) -> AddSystemService -> Release.
void RegisterAsService(cIGZFrameWork& framework, cIGZUnknown* object) {
    if (!object) return;
    cIGZSystemService* service = nullptr;
    if (object->QueryInterface(kSystemServiceIid, reinterpret_cast<void**>(&service))) {
        framework.AddSystemService(service);
        service->Release();
    }
}
}  // namespace

void cSC4App::AddCOMDirectors() {
    cRZCOMDllDirector* process = RZGetCOMDllDirector();
    cIGZCOM* com = GZCOM();
    auto add = [&](cRZCOMDllDirector* director) {
        process->AddDirector(director);
        director->OnStart(com);
    };
    add(gz::SC4GetCOMDirector());
    if (preferences_.Get<std::uint8_t>(sc4::prefs::kAudioEnabledMirror) != 0 && startupHooks_.audioDirector)
        add(startupHooks_.audioDirector());
    if (startupHooks_.showFpsPreference && startupHooks_.showFpsPreference()) {
        static gz::cGZToolsCOMDirector tools;       // cGZToolsCOMDirector::Get()
        add(&tools);
    }
    add(gz::GetGZWWWCOMDllDirector());
}

// Every service follows one of two shapes: ask COM for the class and fall back to constructing the built-in one
// (`fallbackClass` names it), or construct it unconditionally. Either way the object is registered through its
// cIGZSystemService view and then released.
void cSC4App::AddSystemServices() {
    cIGZFrameWork& framework = *framework_;
    cIGZCOM& com = *GZCOM();

    // The object as the requested interface (a pointer of that interface's type, held through cIGZUnknown).
    auto create = [&](std::uint32_t clsid, std::uint32_t iid, const char* fallbackClass) -> cRZAutoRefCount<cIGZUnknown> {
        cRZAutoRefCount<cIGZUnknown> object;
        if (com.CreateInstance(clsid, iid, object.AsOutParam())) return object;
        if (fallbackClass) {
            auto factory = serviceFactories_.find(fallbackClass);
            if (factory != serviceFactories_.end()) {
                cRZAutoRefCount<cIGZUnknown> built(factory->second());
                if (built && built->QueryInterface(iid, object.AsOutParam())) return object;
                return built;
            }
        }
        return object;
    };
    auto createAndRegister = [&](std::uint32_t clsid, std::uint32_t iid, const char* fallbackClass) {
        cRZAutoRefCount<cIGZUnknown> object = create(clsid, iid, fallbackClass);
        RegisterAsService(framework, object.get());
        return object;
    };
    auto build = [&](const char* className) -> cRZAutoRefCount<cIGZUnknown> {
        auto factory = serviceFactories_.find(className);
        return factory != serviceFactories_.end() ? cRZAutoRefCount<cIGZUnknown>(factory->second()) : cRZAutoRefCount<cIGZUnknown>();
    };

    createAndRegister(0xa9a1db23, 0x3ae4beaa, "cGZAllocatorService");
    createAndRegister(0x2073215d, 0xe0203660, "cGZFileSystem");
    FindAndPossiblyFixIniFiles();
    if (auto server = createAndRegister(0x04063236, 0xc1085722, "cGZMessageServer"))
        static_cast<cIGZMessageServer*>(static_cast<void*>(server.get()))->SetAlwaysClearQueueOnTick(false);
    if (auto server = createAndRegister(0x252294e9, 0x652294c7, "cGZMessageServer2"))
        static_cast<cIGZMessageServer2*>(static_cast<void*>(server.get()))->SetAlwaysClearQueueOnTick(false);

    // the log service, and the standard output / error streams go quiet
    if (auto log = build("nGZLog::cService")) RegisterAsService(framework, log.get());
    {
        cRZAutoRefCount<cRZNullOStream> quiet(new cRZNullOStream());
        framework.SetStream(0, static_cast<cIGZOStream*>(quiet.get()));
        framework.SetStream(1, static_cast<cIGZOStream*>(quiet.get()));
    }

    createAndRegister(0x8b985476, 0xcb90253a, "cGZCommandServer");
    createAndRegister(0x8898ed08, 0xe898ed03, "cSC4GameDataRegistry");
    createAndRegister(0xcbd101a9, 0x4bd10129, "cGZLuaScriptServer");
    createAndRegister(0xa3024279, 0x0054b7d5, "cRZLanguageManager");

    // the graphics system learns where the application's .ini is
    {
        cRZAutoRefCount<cIGZUnknown> graphics = create(0xa499932c, 0x0073283c, nullptr);
        if (graphics) {
            cRZAutoRefCount<cIGZUnknown> graphics2;
            if (graphics->QueryInterface(0xc47b747c, graphics2.AsOutParam()) && startupHooks_.configureGraphicSystem) {
                cRZString iniPath;
                GetAppIniFilePath(iniPath);
                startupHooks_.configureGraphicSystem(graphics2.get(), iniPath);
            }
            RegisterAsService(framework, graphics.get());
        }
    }
    createAndRegister(0x445fabbb, 0x66b217a1, nullptr);                   // cGZCanvasW32
    createAndRegister(0x20673960, 0x00673960, "cGZFontSys");
    createAndRegister(0x84174448, 0x000005a4, nullptr);                   // the window manager
    createAndRegister(0xc2c2eb0f, 0x22c2eb1f, "cGZWinCtrlMgr");
    createAndRegister(0xc7ec0014, 0x07ec0010, "cGZClipboardManager");
    if (preferences_.Get<std::uint8_t>(sc4::prefs::kImeEnabled) != 0) {
        createAndRegister(0x2ac4516b, 0x2ac44ae7, "cGZWinIMEServer");
        if (preferences_.Get<std::uint8_t>(sc4::prefs::kImeEnabled) != 0)
            createAndRegister(0x2ac4516d, 0x2ac44ae9, "cGZWinIMEProxy");
    }
    createAndRegister(0x656b8f27, 0x656b8efc, "cGZPersistResourceManager");
    if (auto registry = build("cGZRegistry")) RegisterAsService(framework, registry.get());
    createAndRegister(0x9ac0e23c, 0xfac0e219, "cS3DTextureBindingFactory");
    createAndRegister(0x9ac0e23d, 0xfac0e21a, nullptr);                   // the material manager

    // the UI script service takes the script resource factory
    {
        cRZAutoRefCount<cIGZUnknown> scripts;
        if (framework.GetSystemService(0x5a356e15, 0xfa3562fa, scripts.AsOutParam()) && scripts && startupHooks_.registerScriptResourceFactory)
            startupHooks_.registerScriptResourceFactory(scripts.get());
    }

    // the WWW system gets the HTTP cache directory
    {
        cRZAutoRefCount<cIGZUnknown> www = create(0x849b190a, 0x449b1912, "cGZWWWSystem");
        if (www) {
            RegisterAsService(framework, www.get());
            if (startupHooks_.configureWWWSystem) {
                cRZString cacheDirectory;
                GetHTTPCacheDirectory(cacheDirectory);
                startupHooks_.configureWWWSystem(www.get(), cacheDirectory);
            }
        }
    }
    if (auto events = build("cSC4AudioAEMSEventSystem")) RegisterAsService(framework, events.get());
}

// The original looks up the file system service, then reads the Directories/Data and Directories/PlugIn entries of
// the application .ini and checks that those directories exist (and the file system's own defaults otherwise); the
// outcome is never used, so only the "already done" flag has an effect here.
void cSC4App::FindAndPossiblyFixIniFiles() {
    if (iniFilesChecked_) return;
    iniFilesChecked_ = true;
}


// ---- PreFrameWorkInit ---------------------------------------------------------------------------------------

namespace {
std::string LowerCase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool CommandLineValue(cIGZCmdLine& commandLine, const char* name, std::string& value) {
    cRZString text;
    if (!commandLine.IsSwitchPresent(cRZString(name), text, true)) return false;
    value = text.ToChar();
    return true;
}

// each of the derived directories: "<base>Plugins/" and friends
void DeriveFrom(cRZString& target, const cRZString& base, const char* name) {
    if (target.Strlen() != 0) return;
    target = base.ToChar();
    target.AppendChars(name, static_cast<std::uint32_t>(std::strlen(name)));
    target.AppendChars("/", 1);
}
}  // namespace

// Start-up before the framework initialises the services: debug level, the executable's directories, preferences,
// the single-instance guard, then the directors and services. Returns false when another instance is running.
bool cSC4App::PreFrameWorkInit() {
    cIGZFrameWork* framework = framework_ ? framework_ : RZGetFramework();
    SC4SetupFPU();
    SC4DisableOSMessageBoxAlerts();

    cIGZCmdLine& commandLine = *framework->CommandLine();
    std::string value;
    if (CommandLineValue(commandLine, "DebugLevel", value)) {
        value = LowerCase(value);
        framework->SetDebugLevel(value == "none" ? 0 : value == "logging" ? 1 : 2);
    }

    // the executable: <app directory>/<name>; the data directory starts out as the application directory
    // (cRZFile::GetPathDirectory yields an empty string - kept - so the file system service supplies the real ones below)
    cRZVersion version;
    cRZString modulePath, name;
    version.GetModulePath(modulePath);
    cRZFile::GetPathDirectory(modulePath, appDirectory_);
    cRZFile::GetPathName(modulePath, appName_, false);
    dataDirectory_ = appDirectory_.ToChar();
    if (startupHooks_.bundleGameDataDirectory) {
        std::string bundleData = startupHooks_.bundleGameDataDirectory();
        if (!bundleData.empty()) dataDirectory_ = (bundleData + "/").c_str();
    }
    SC4GetUserDataDirectory(userDataDirectory_, true);

    SetRenderProperties(new sc4::cSC4RenderProperties());
    ReadPreferences();
    ReadCommandLine();

    // only one instance at a time, unless -AllowMultipleInstances; -Restart waits up to 30 s for the old one to leave
    bool locked = instanceMutex_.TryLock();
    bool allowMultiple = commandLine.IsSwitchPresent(cRZString("AllowMultipleInstances"));
    if (!allowMultiple && !locked) {
        if (!commandLine.IsSwitchPresent(cRZString("Restart"))) return false;
        cRZTimer timer(cRZTimer::kSeconds);
        timer.Start();
        bool acquired = false;
        while (timer.GetElapsedTimeU32() < 30) {
            if (instanceMutex_.TryLock()) { acquired = true; break; }
            cRZThread::SleepCurrentThread(200000);
        }
        if (!acquired) return false;
    }

    AddCOMDirectors();
    AddSystemServices();

    // the graphics system: no hardware cursor for -cursormode b..., a 1024x768 default game resolution
    IGraphicSystemControl* graphics = startupHooks_.graphicSystem ? startupHooks_.graphicSystem() : nullptr;
    if (graphics) {
        graphics->DisableScreenBuffer();
        bool blitCursor = false;
        if (CommandLineValue(commandLine, "cursormode", value)) {
            value = LowerCase(value);
            blitCursor = !value.empty() && value[0] == 'b';
        }
        graphics->EnableCursorEmulation(blitCursor);
        if (blitCursor && graphics->IsPageFlippingEnabled()) graphics->EnablePageFlipping(false);
        IGraphicSystemControl::DisplayMetrics screen = graphics->GetScreenMetrics();
        graphics->PreInitSetDesiredGameResolution({1024, 768, screen.depth});
    }

    // the directories the file system service reports replace the guesses; the plugin / sku directories derive from them
    sc4::FileSystemDirectories directories;
    bool haveDirectories = false;
    if (startupHooks_.fileSystemDirectories) {
        haveDirectories = startupHooks_.fileSystemDirectories(directories);
    } else if (cIGZFileSystem* files = RZ::FS()) {
        directories = {files->AppDirectory(), files->CDAppDirectory(), files->DataDirectory(), files->CDDataDirectory()};
        haveDirectories = true;
    }
    if (haveDirectories) {
        appDirectory_ = directories.appDirectory.c_str();
        cdAppDirectory_ = directories.cdAppDirectory.c_str();
        dataDirectory_ = directories.dataDirectory.c_str();
        cdDataDirectory_ = directories.cdDataDirectory.c_str();
        DeriveFrom(pluginDirectory_, dataDirectory_, "Plugins");
        DeriveFrom(cdPluginDirectory_, cdDataDirectory_, "Plugins");
        DeriveFrom(skuSpecificDirectory_, dataDirectory_, "Sku_Data");
        if (userDataDirectory_.Strlen() == 0) SC4GetUserDataDirectory(userDataDirectory_, true);
    }

    AddCodePlugins(cRZString());
    if (startupHooks_.configureLogging) startupHooks_.configureLogging(commandLine);
    FindAndRegisterAllAvailableLanguages();
    DoPreferencesChanged(true);
    return true;
}

// ---- code plugins -------------------------------------------------------------------------------------------

// The shared libraries ("*.so") of a plugin directory are added to the COM registry unless their .ini says
// [Admin] Enabled=false|0; an empty `directory` means the plugin directory and then the user plugin directory.
int cSC4App::AddCodePlugins(const cRZString& directory) {
    if (directory.Strlen() == 0) {
        int added = 0;
        cRZString plugins;
        GetPluginDirectory(plugins);
        if (plugins.Strlen() != 0) added = AddCodePlugins(plugins);
        GetUserPluginDirectory(plugins);
        if (plugins.Strlen() != 0) added += AddCodePlugins(plugins);
        return added;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(directory.ToChar(), error)) return 0;
    auto* com = dynamic_cast<cGZCOM*>(GZCOM());
    if (!com) return 0;
    // libraries that are already registered are skipped
    std::vector<std::string> known;
    com->EnumerateLibraries(+[](cGZCOMLibrary* library, void* context) {
        cRZString path;
        library->GetPath(path);
        static_cast<std::vector<std::string>*>(context)->push_back(path.ToChar());
        return true;
    }, &known);
    int added = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.ToChar(), error)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".so") continue;
        cRZString libraryPath(directory);
        libraryPath.AppendChars(entry.path().filename().c_str(), static_cast<std::uint32_t>(entry.path().filename().string().size()));
        cRZString iniPath(directory);
        std::string stem = entry.path().stem().string() + ".ini";
        iniPath.AppendChars(stem.c_str(), static_cast<std::uint32_t>(stem.size()));
        if (std::find(known.begin(), known.end(), std::string(libraryPath.ToChar())) != known.end()) continue;
        bool add = true;
        if (cRZFile::FileExists(iniPath)) {
            cRZIniFile ini(iniPath);
            cRZString enabled;
            if (ini.ReadEntry(cRZString("Admin"), cRZString("Enabled"), enabled)) {
                std::string text = LowerCase(enabled.ToChar());
                if (text == "false" || text == "0") add = false;
            }
        }
        if (add) {
            com->AddLibrary(libraryPath);
            ++added;                           // counted whether or not the library loaded
        }
    }
    return added;
}


// ---- languages and preference changes -------------------------------------------------------------------------

namespace {
using LanguageManagerPtr = cRZSysServPtr<cIGZLanguageManager, kGZSID_cRZLanguageManager, kGZIID_cIGZLanguageManager>;

// cRZDirectory::DoesAnyEntryExistThatMatchesPattern: a file name in the pattern's directory matches the glob
bool AnyEntryMatches(const std::string& directory, const char* pattern) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (fnmatch(pattern, entry.path().filename().c_str(), FNM_CASEFOLD) == 0) return true;   // the PC data spells ".DAT"
    }
    return false;
}
}  // namespace

// A language is available when its folder under the data directory holds string tables (*.str*) or data (*.dat).
void cSC4App::FindAndRegisterAllAvailableLanguages() {
    LanguageManagerPtr languages;
    cRZString directoryName;
    for (std::uint32_t language = 1; language <= 0x2c; ++language) {
        if (!languages->GetLanguageDirectoryName(directoryName, language)) continue;
        const std::string base = dataDirectory_.ToChar();
        std::string folder = base + SC4FolderNameOnDisk(base, directoryName.ToChar()) + "/";
        if (AnyEntryMatches(folder, "*.str*") || AnyEntryMatches(folder, "*.dat")) languages->AddAvailableLanguage(language);
    }
}

// Applies the preferences: pause-on-focus-loss, and (at start-up) the language, the window mode / resolution, the
// graphics driver, the simulator speed and the "-x" cheat switches.
void cSC4App::DoPreferencesChanged(bool initial) {
    std::uint8_t* prefs = preferences_.data();
    EnableFullGamePauseOnAppFocusLoss(prefs[sc4::prefs::kPauseOnFocusLoss] != 0);
    if (initial) {
        prefs[0x4eb] = 0;
        LanguageManagerPtr languages;
        if (languages) {
            std::uint32_t language = 1;
            cRZString requested(reinterpret_cast<const char*>(prefs + sc4::prefs::kLanguageName));
            bool chosen = false;
            // 1. the language asked for with -l (by English name, else by local name)
            if (requested.Strlen() != 0) {
                bool found = languages->GetLanguageIDFromLanguageEnglishName(requested, language) ||
                             languages->GetLanguageIDFromLanguageLocalName(requested, language);
                chosen = found && languages->CanSwitchToLanguage(language);
            }
            if (!chosen) {
                // 2. the installer's language, 3. the system language, 4. the first language that can be switched to
                std::uint32_t installedCountry = 0;
                bool installed = startupHooks_.installedLanguage ? startupHooks_.installedLanguage(language, installedCountry)
                                                                 : SC4GetInstalledLanguageAndCountryIDs(language, installedCountry);
                chosen = installed && languages->CanSwitchToLanguage(language);
                if (!chosen) {
                    language = languages->GetCurrentSystemLanguage();
                    chosen = languages->CanSwitchToLanguage(language);
                    for (std::uint32_t candidate = 1; !chosen && candidate < 0x2d; ++candidate) {
                        if (languages->CanSwitchToLanguage(candidate)) { language = candidate; chosen = true; }
                    }
                }
            }
            if (chosen) {
                languages->SetCurrentLanguage(language);
                preferences_.Set<std::uint32_t>(sc4::prefs::kLanguageId, language);
            }
        }

        if (IGraphicSystemControl* graphics = startupHooks_.graphicSystem ? startupHooks_.graphicSystem() : nullptr) {
            IGraphicSystemControl::DisplayMetrics screen = graphics->GetScreenMetrics();
            if (screen.depth != 16 && screen.depth != 32) prefs[sc4::prefs::kFullScreen] = 1;
            // a window that does not fit on the desktop becomes full screen
            if (prefs[sc4::prefs::kFullScreen] == 0 &&
                (screen.width < preferences_.Get<std::uint32_t>(sc4::prefs::kVideoWidth) ||
                 screen.height < preferences_.Get<std::uint32_t>(sc4::prefs::kVideoHeight)))
                prefs[sc4::prefs::kFullScreen] = 1;
            IGraphicSystemControl::DisplayMetrics desired;
            desired.width = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoWidth);
            desired.height = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoHeight);
            desired.depth = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoColorDepth);
            if (desired.depth == 0) {
                if (screen.depth != 0) desired.depth = screen.depth;
            } else if (desired.depth != screen.depth && prefs[sc4::prefs::kFullScreen] == 0) {
                desired.depth = screen.depth;
            }
            graphics->PreInitSetDesiredGameResolution(desired);
            graphics->PreInitSetWindowedMode(prefs[sc4::prefs::kFullScreen] == 0);

            // the graphics driver: -d wins; otherwise the stored hardware flag decides (0 = software, 1 = hardware)
            std::uint32_t driver = requestedGraphicsDriver_;
            std::uint8_t& hardware = prefs[sc4::prefs::kVideoHardwareDriver];
            if (driver == 0) {
                driver = hardware == 1 ? 0xbadb6906u : 0u;
                if (hardware == 0) driver = 0x7aca35c6u;
                if (driver != 0) graphics->SetDefaultDriverClassID(driver);
            } else {
                if (driver == 0x7aca35c6u) {
                    hardware = 0;
                } else {
                    hardware = driver == 0xbadb6906u ? 1 : 0xff;
                }
                graphics->SetDefaultDriverClassID(driver);
            }
        }
    }

    // the simulator speed setting picks one of eight tick intervals
    if (cISC4City* city = GetCity()) {
        (void)city;
        static constexpr std::uint32_t kTickTable[8] = {5, 10, 15, 20, 30, 45, 60, 80};
        int index = static_cast<int>((preferences_.Get<float>(sc4::prefs::kSimulatorParameter) * 8.0f) / 101.0f);
        index = index < 0 ? 0 : index;
        if (startupHooks_.setSimulatorTickInterval) startupHooks_.setSimulatorTickInterval(kTickTable[index < 8 ? index : 7]);
    }

    // -x:<cheat> switches run their cheat code once each and are consumed from a private copy of the command line
    if (cheatCodeManager_ && startupHooks_.processCheatCode) {
        cRZCmdLine copy(*(framework_ ? framework_ : RZGetFramework())->CommandLine());
        cRZString code;
        while (copy.IsSwitchPresent(cRZString("x"), code, true)) {
            startupHooks_.processCheatCode(code);
            int index = copy.GetIndexOfSwitch(cRZString("x"), 0);
            if (index < 0) break;
            copy.EraseArgument(index);
        }
    }
}
