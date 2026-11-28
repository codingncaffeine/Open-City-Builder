#include "sc4/App.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gz/GZCOMDirector.h"
#include "gz/cIGZFrameWork.h"
#include "rz/LanguageManager.h"
#include "rz/cRZFile.h"
#include "rz/cRZMessage2Standard.h"
#include "gz/cGZDBSegmentPackedFile.h"
#include "rz/PathUtils.h"
#include "sc4/AppPlatform.h"
#include "sc4/AutoRecord.h"
#include "rz/cRZSysServPtr.h"

namespace {
using LanguageManagerPtr = cRZSysServPtr<cIGZLanguageManager, kGZSID_cRZLanguageManager, kGZIID_cIGZLanguageManager>;

// "<directory>/": the language folder of the current language (language 0) followed by a slash. The PC data comes from a
// file system that ignores case: the folder under `out` (the data directory) is matched without regard to case.
void AppendLanguageFolder(cIGZString& out) {
    cRZString folder("English");
    LanguageManagerPtr languages;
    if (languages) languages->GetLanguageDirectoryName(folder, 0);
    const std::string name = SC4FolderNameOnDisk(out.ToChar(), folder.ToChar());
    out.AppendChars(name.c_str(), static_cast<std::uint32_t>(name.size()));
    out.AppendChars("/", 1);
}

// The directories that are derived on first use: fill `target` from the user data directory plus "<name>/".
template <class Fill>
void DeriveDirectory(cRZString& target, Fill&& fillBase, const char* name) {
    if (target.Strlen() == 0) {
        fillBase(target);
        target.AppendChars(name, static_cast<std::uint32_t>(std::strlen(name)));
        target.AppendChars("/", 1);
    }
}
}  // namespace

bool gbAppResourcesInitializedOK = false;
std::uint32_t gbAppResourceErrorLanguage = 0;

cSC4App::cSC4App(const char* moduleName, IGZAppFramework* framework) : cGZApp(moduleName, framework) {
    timer3d0_.Start();
    timer3f0_.Start();
    timer410_.Start();
    timer428_.Start();
    timer440_.Start();
    SL::OnAppCreate(static_cast<cISC4App*>(this));
    // (the original also builds the tutorial registry here)
}

cSC4App::~cSC4App() {
    instanceMutex_.Unlock();
    for (cIGZUnknown* object : {luaInterpreter_, field788_, miscCommandSupport_, toolCommandSupport_, winCommandSupport_,
                                 static_cast<cIGZUnknown*>(renderProperties_), field668_}) {
        if (object) object->Release();
    }
}

bool cSC4App::QueryInterface(std::uint32_t iid, void** out) {
    switch (iid) {
        case 0xca263481:
        case 0xe98b2f57: *out = static_cast<cIGZMessageTarget*>(this); break;
        case 0x452294aa: *out = static_cast<cIGZMessageTarget2*>(this); break;
        case 0xc6ae7085: *out = static_cast<cIGZWinMessageFilter*>(this); break;
        case kGZIID_cISC4App: *out = static_cast<cISC4App*>(this); break;
        default: return cGZApp::QueryInterface(iid, out);
    }
    AddRef();
    return true;
}

std::uint32_t cSC4App::AddRef() { return cGZApp::AddRef(); }
std::uint32_t cSC4App::Release() { return cRZUnknown::Release(); }

void cSC4App::SetDirectories(const char* appDirectory, const char* dataDirectory, const char* userDataDirectory) {
    appDirectory_ = appDirectory;
    dataDirectory_ = dataDirectory;
    userDataDirectory_ = userDataDirectory;
}

void cSC4App::GetAppName(cIGZString& out) const { out.AssignFromCString(appName_.ToChar()); }

void cSC4App::GetDataDirectory(cIGZString& out, int language) {
    out.AssignFromCString(dataDirectory_.ToChar());
    if (language >= 0) AppendLanguageFolder(out);   // quirk: always the current language, whatever the argument
}

void cSC4App::GetCDDataDirectory(cIGZString& out, int language) {
    out.AssignFromCString(cdDataDirectory_.ToChar());
    if (language >= 0) AppendLanguageFolder(out);
}

void cSC4App::GetUserPluginDirectory(cIGZString& out) {
    DeriveDirectory(userPluginDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "Plugins");
    out.AssignFromCString(userPluginDirectory_.ToChar());
}

void cSC4App::GetRegionsDirectory(cIGZString& out) {
    DeriveDirectory(regionsDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "Regions");
    out.AssignFromCString(regionsDirectory_.ToChar());
}

void cSC4App::GetMySimDirectory(cIGZString& out) {
    DeriveDirectory(mySimDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "MySim");
    out.AssignFromCString(mySimDirectory_.ToChar());
}

void cSC4App::GetAlbumDirectory(cIGZString& out) {
    DeriveDirectory(albumDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "Albums");
    out.AssignFromCString(albumDirectory_.ToChar());
}

void cSC4App::GetHTTPCacheDirectory(cIGZString& out) {
    DeriveDirectory(httpCacheDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "HTTPCache");
    out.AssignFromCString(httpCacheDirectory_.ToChar());
}

void cSC4App::GetTempDirectory(cIGZString& out) {
    DeriveDirectory(tempDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "Temp");
    out.AssignFromCString(tempDirectory_.ToChar());
}

void cSC4App::GetExceptionReportsDirectory(cIGZString& out) {
    DeriveDirectory(exceptionReportsDirectory_, [this](cRZString& s) { GetUserDataDirectory(s); }, "Exception Reports");
    out.AssignFromCString(exceptionReportsDirectory_.ToChar());
}

// "<data directory>/Scripts/TestScripts/" (the data directory without a language folder)
void cSC4App::GetTestScriptDirectory(cIGZString& out) {
    if (testScriptDirectory_.Strlen() == 0) {
        GetDataDirectory(testScriptDirectory_, -1);
        testScriptDirectory_.AppendChars("Scripts", 7);
        testScriptDirectory_.AppendChars("/", 1);
        testScriptDirectory_.AppendChars("TestScripts", 11);
        testScriptDirectory_.AppendChars("/", 1);
    }
    out.AssignFromCString(testScriptDirectory_.ToChar());
}

void cSC4App::GetAppIniFileName(cIGZString& out) const {
    if (appName_.Strlen() != 0) {
        out.AssignFromCString(appName_.ToChar());
        out.AppendChars(".ini", 4);
    } else {
        out.AssignFromCString("SimCity 4.ini");
    }
}

// The .ini next to the executable when it exists, else the one in the user data directory.
void cSC4App::GetAppIniFilePath(cIGZString& out) const {
    cRZString name;
    GetAppIniFileName(name);
    cRZString beside(appDirectory_);
    beside.AppendString(name);
    if (cRZFile::FileExists(beside)) {
        out.Copy(beside);
    } else {
        out.AssignFromChars(userDataDirectory_.ToChar(), userDataDirectory_.Strlen());
        out.AppendChars(name.ToChar(), name.Strlen());
    }
}

void cSC4App::GetAppPreferencesFileName(cIGZString& out) const {
    if (appName_.Strlen() != 0) {
        out.AssignFromCString(appName_.ToChar());
        out.AppendChars(".cfg", 4);
    } else {
        out.AssignFromCString("SimCity 4.cfg");
    }
}

void cSC4App::GetAppPreferencesFilePath(cIGZString& out) const {
    cRZString name;
    GetAppPreferencesFileName(name);
    out.AssignFromChars(userDataDirectory_.ToChar(), userDataDirectory_.Strlen());
    out.AppendChars(name.ToChar(), name.Strlen());
}

bool cSC4App::RegisterShutdownCallbackFunction(ShutdownCallback callback, void* context) {
    shutdownCallbacks_.push_back({callback, context});
    return true;
}

bool cSC4App::UnregisterShutdownCallbackFunction(ShutdownCallback callback, void* context) {
    for (auto it = shutdownCallbacks_.begin(); it != shutdownCallbacks_.end(); ++it) {
        if (it->callback == callback && it->context == context) {
            shutdownCallbacks_.erase(it);
            return true;
        }
    }
    return false;
}

// option number -> field of SC4VideoPreferences (the same map serves Apply and GetAuto)
namespace {
struct VideoOptionField { int option; int byteOffset; };
constexpr VideoOptionField kVideoOptionFields[] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4}, {6, 7}, {4, 5}, {5, 6}, {8, 9}, {7, 8}, {9, 10}, {10, 0xc}, {0xf, 0x1c},
};
}  // namespace

void cSC4App::ApplyVideoPreferences(const SC4VideoPreferences& preferences) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&preferences);
    for (const VideoOptionField& field : kVideoOptionFields) videoOptions_->SetOption(field.option, bytes[field.byteOffset]);
    // tell everyone: message 0xc9f775bb through the message server 2
    void* server = nullptr;
    if (cRZServiceResolver resolve = GetRZServiceResolver()) {
        if (resolve(kGZSID_MessageServer2, 0x652294c7, &server) && server) {
            auto* messages = static_cast<cIGZMessageServer2*>(server);
            cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
            message->SetType(0xc9f775bb);
            messages->MessagePost(message.get(), false);
            messages->Release();
        }
    }
}

void cSC4App::GetAutoVideoPreferences(SC4VideoPreferences* preferences) {
    auto* bytes = reinterpret_cast<std::uint8_t*>(preferences);
    for (const VideoOptionField& field : kVideoOptionFields) bytes[field.byteOffset] = static_cast<std::uint8_t>(videoOptions_->GetOption(field.option));
    bytes[0xb] = 1;
    int width = 0, height = 0;
    if (display_) display_->GetNthNativeResolution(videoOptions_->GetOption(0xb), width, height);
    preferences->screenWidth = width;
    preferences->screenHeight = height;
    switch (videoOptions_->GetOption(0xc)) {          // colour depth option: 0 = 16 bit, 1 = 32 bit, otherwise untouched
        case 0: preferences->colorDepth = 16; break;
        case 1: preferences->colorDepth = 32; break;
        default: break;
    }
}

cISC4Region* cSC4App::GetRegion() { return nation_ && regionOf_ ? regionOf_(nation_) : nullptr; }

cISC4RegionalCity* cSC4App::GetRegionalCity() {
    if (!regionalCity_) return nullptr;
    return regionalCity_->get();   // (the original AddRefs and Releases once, a no-op)
}

// The command line the switches are read from: the injected one, else the framework's (cGZFramework::CommandLine()).
cIGZCmdLine* cSC4App::CommandLineInUse() {
    if (commandLine_) return commandLine_;
    cIGZFrameWork* framework = framework_ ? framework_ : RZGetFramework();
    return framework ? framework->CommandLine() : nullptr;
}

// ---- command line -------------------------------------------------------------------------------------------

namespace {
// "off"/"disabled" -> false, "on"/"enabled" -> true (the text is lower-cased first); anything else leaves `value`.
bool ParseOnOff(const std::string& text, bool& value) {
    if (text == "off" || text == "disabled") { value = false; return true; }
    if (text == "on" || text == "enabled") { value = true; return true; }
    return false;
}

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// IsSwitchPresent(name) -> value text
bool SwitchValue(cIGZCmdLine& commandLine, const char* name, std::string& value, bool stripQuotes = true) {
    cRZString text;
    if (!commandLine.IsSwitchPresent(cRZString(name), text, stripQuotes)) return false;
    value = text.ToChar();
    return true;
}

// The graphics driver ids the -d switch selects (a substring of the value picks the driver).
constexpr std::uint32_t kDriverOpenGL = 0xc4554841, kDriverSoftware = 0x7aca35c6, kDriverDirectX = 0xbadb6906;
}  // namespace

void cSC4App::SetRenderProperties(sc4::cSC4RenderProperties* properties) {
    if (renderProperties_ != properties) {
        if (properties) properties->AddRef();
        if (renderProperties_) renderProperties_->Release();
        renderProperties_ = properties;
    }
}

void cSC4App::ReadCommandLine() {
    cIGZCmdLine& commandLine = *CommandLineInUse();
    std::uint8_t* prefs = preferences_.data();
    std::string value;
    bool flag = false;

    if (SwitchValue(commandLine, "IME", value)) {
        value = Lower(value);
        if (ParseOnOff(value, flag)) prefs[sc4::prefs::kImeEnabled] = flag ? 1 : 0;
    }

    // -r<width>x<height>x<depth>
    cRZString resolution;
    if (commandLine.IsSwitchPresent('r', resolution, true)) {
        std::uint32_t width = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoWidth);
        std::uint32_t height = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoHeight);
        std::uint32_t depth = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoColorDepth);
        std::sscanf(resolution.ToChar(), "%ux%ux%u", &width, &height, &depth);
        if (width == 0 || height == 0) {
            width = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoWidth);
            height = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoHeight);
        }
        FixStartupResolutionValuesIfNeeded(width, height);
        if (depth == 0) depth = preferences_.Get<std::uint32_t>(sc4::prefs::kVideoColorDepth);
        preferences_.Set<std::uint32_t>(sc4::prefs::kVideoWidth, width);
        preferences_.Set<std::uint32_t>(sc4::prefs::kVideoHeight, height);
        preferences_.Set<std::uint32_t>(sc4::prefs::kVideoColorDepth, depth);
    }

    // -f full screen, -w windowed (windowed wins when both are given)
    if (commandLine.GetIndexOfSwitch(cRZString("f"), 0) >= 0) prefs[sc4::prefs::kFullScreen] = 1;
    if (commandLine.GetIndexOfSwitch(cRZString("w"), 0) >= 0) prefs[sc4::prefs::kFullScreen] = 0;

    // -g<p...>
    if (SwitchValue(commandLine, "g", value) && !value.empty() && (static_cast<unsigned char>(value[0]) | 0x20) == 'p')
        prefs[sc4::prefs::kPauseOnFocusLoss] = 1;

    // -d<driver>: "opengl", "soft", "dx" / "directx", else a hexadecimal id
    requestedGraphicsDriver_ = 0;
    if (SwitchValue(commandLine, "d", value, false)) {
        value = Lower(value);
        std::uint32_t driver;
        if (value.find("opengl") != std::string::npos) driver = kDriverOpenGL;
        else if (value.find("soft") != std::string::npos) driver = kDriverSoftware;
        else if (value.find("dx") != std::string::npos || value.find("directx") != std::string::npos) driver = kDriverDirectX;
        else driver = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 16));   // RZAtoUint32HexA
        requestedGraphicsDriver_ = driver;
    }

    // -l[:]["]name["]  (at most 63 characters)
    if (SwitchValue(commandLine, "l", value)) {
        if (!value.empty() && value[0] == ':') {
            value.erase(0, 1);
            if (!value.empty() && value[0] == '"') value.erase(0, 1);
            if (!value.empty() && value.back() == '"') value.pop_back();
        }
        if (value.size() > 0x3f) value.resize(0x3f);
        std::memcpy(prefs + sc4::prefs::kLanguageName, value.c_str(), value.size() + 1);
    }

    // -audio / -sound
    if (SwitchValue(commandLine, "audio", value) || SwitchValue(commandLine, "sound", value)) {
        if (ParseOnOff(Lower(value), flag)) {
            prefs[sc4::prefs::kAudioEnabled] = flag ? 1 : 0;
            prefs[sc4::prefs::kAudioEnabledMirror] = flag ? 1 : 0;
        }
    }
    // -radio / -music
    if (SwitchValue(commandLine, "radio", value) || SwitchValue(commandLine, "music", value)) {
        if (ParseOnOff(Lower(value), flag)) preferences_.Set<std::uint32_t>(sc4::prefs::kMusicEnabled, flag ? 1 : 0);
    }
    // -sim<number>
    if (SwitchValue(commandLine, "sim", value))
        preferences_.Set<float>(sc4::prefs::kSimulatorParameter, static_cast<float>(std::strtod(value.c_str(), nullptr)));
    // -XDD
    if (SwitchValue(commandLine, "XDD", value)) {
        if (ParseOnOff(Lower(value), flag)) prefs[sc4::prefs::kSwitchXDD] = flag ? 1 : 0;
    }
    // -UserDir:<off|path>: "off" puts the user data next to the game data, a full path replaces it
    if (SwitchValue(commandLine, "UserDir", value)) {
        std::string lowered = Lower(value);
        if (lowered == "off" || lowered == "disabled") {
            userDataDirectory_ = dataDirectory_.ToChar();
        } else if (IsFullPath(cRZString(value.c_str()))) {
            userDataDirectory_ = value.c_str();
        }
    }
    // -CPUCount:<n> (a no-op on this platform: SC4SetCPUUsageCount does nothing)
    if (SwitchValue(commandLine, "CPUCount", value)) (void)std::atoi(value.c_str());
    // -AutoZoomChange:<on|off> -> render property 37
    if (SwitchValue(commandLine, "AutoZoomChange", value) && renderProperties_) {
        if (ParseOnOff(Lower(value), flag)) renderProperties_->SetBoolValue(0x25, flag);
    }
}

// ---- start-up resolution ------------------------------------------------------------------------------------

void cSC4App::FixStartupResolutionValuesIfNeeded(std::uint32_t& width, std::uint32_t& height) {
    if (IsStartupResolutionOK(width, height)) return;
    std::uint32_t area = height * width;
    std::uint32_t newHeight, newWidth;
    if (area < 480000) { newHeight = 600; newWidth = 800; }
    else if (area < 0xc0000) { newHeight = 0x300; newWidth = 0x400; }
    else if (area < 0xf3000) { newHeight = 0x360; newWidth = 0x480; }
    else if (area < 0x140000) { newHeight = 0x400; newWidth = 0x500; }
    else if (area < 0x1d4c00) { newHeight = 0x4b0; newWidth = 0x640; }
    else if (area < 0x278d00) { newHeight = 0x5a0; newWidth = 0x708; }
    else { newWidth = area < 0x300000 ? 0x800 : 0x400; newHeight = area < 0x300000 ? 0x600 : 0x300; }
    width = newWidth;
    height = newHeight;
}

// The listed sizes (the width may be larger, the height must match), or anything when -CustomResolution is on.
bool cSC4App::IsStartupResolutionOK(std::uint32_t width, std::uint32_t height) {
    std::string value;
    bool custom = false;
    if (CommandLineInUse() && SwitchValue(*CommandLineInUse(), "CustomResolution", value) && ParseOnOff(Lower(value), custom) && custom) return true;
    if (height < 600) return false;
    struct Size { std::uint32_t width, height; };
    static constexpr Size kSizes[] = {{800, 600}, {0x400, 0x300}, {0x480, 0x360}, {0x500, 0x400}, {0x690, 0x41a},
                                      {0x640, 0x4b0}, {0x708, 0x5a0}, {0x800, 0x600}};
    for (const Size& size : kSizes)
        if (width >= size.width && height == size.height) return true;
    return false;
}

// ---- preferences file ---------------------------------------------------------------------------------------

// Reads the 0x1734-byte record (version 0x15) of "<user data>/<app>.cfg"; a missing or foreign file leaves the
// defaults in place. A successful read also clears "first run after install".
void cSC4App::ReadPreferences() {
    cRZAutoRefCount<cGZDBSegmentPackedFile> segment(new cGZDBSegmentPackedFile());
    if (!segment->Init()) return;
    cRZString path;
    GetAppPreferencesFilePath(path);
    segment->SetPath(path);
    if (segment->Open(true, false)) {
        {
            sc4::cSC4AutoLoadRecord record(static_cast<cIGZPersistDBSegment*>(segment.get()), sc4::kPreferencesRecordType,
                                           sc4::kPreferencesRecordGroup, sc4::kPreferencesRecordInstance);
            if (record.GetRecord() && record.GetVersion() == sc4::kPreferencesRecordVersion) {
                sc4::SC4Preferences loaded;                    // starts from the defaults
                if (record.GetRecord()->GetFieldVoid(loaded.data(), sc4::kPreferencesSize)) {
                    std::memcpy(preferences_.data(), loaded.data(), sc4::kPreferencesSize);
                    runFirstTimeAfterInstall_ = false;
                }
            }
            record.Close();
        }
        segment->Close();
    }
    segment->Shutdown();
}

// Writes the preferences over the old file. The per-run fields (player name from the command line, IME flag) are
// reset in memory first, so they are never saved.
void cSC4App::SavePreferences() {
    cRZAutoRefCount<cGZDBSegmentPackedFile> segment(new cGZDBSegmentPackedFile());
    if (!segment->Init()) return;
    std::uint8_t* prefs = preferences_.data();
    preferences_.Set<std::uint32_t>(sc4::prefs::kAppResetFlag, 1);
    std::memset(prefs + sc4::prefs::kAppResetBlock, 0, 0x40);
    prefs[sc4::prefs::kAppResetByte] = 1;
    cRZString path;
    GetAppPreferencesFilePath(path);
    cRZFile::Remove(path);
    segment->SetPath(path);
    if (segment->Open(true, true)) {
        {
            sc4::cSC4AutoSaveRecord record(static_cast<cIGZPersistDBSegment*>(segment.get()), sc4::kPreferencesRecordType,
                                           sc4::kPreferencesRecordGroup, sc4::kPreferencesRecordInstance,
                                           sc4::kPreferencesRecordVersion);
            if (record.GetRecord()) {
                record.GetRecord()->SetFieldVoid(prefs, sc4::kPreferencesSize);
                record.Close();
            }
        }
        segment->Shutdown();
        return;
    }
    segment->Shutdown();
}


// ---- focus and the message pumps ----------------------------------------------------------------------------

void cSC4App::EnableFullGamePauseOnAppFocusLoss(bool enable) {
    preferences_.data()[sc4::prefs::kPauseOnFocusLoss] = enable ? 1 : 0;
    pauseOnFocusLoss_ = enable;
}

void cSC4App::GainLoseFocus(bool gained) {
    if (pauseOnFocusLoss_ && gamePaused_ != !gained) FullGamePause(!gained);
}

void cSC4App::FullGamePause(bool pause) {
    if (gamePaused_ == pause) return;
    gamePaused_ = pause;
    if (startupHooks_.gamePause) startupHooks_.gamePause(pause);
}

namespace {
// Delivers queued messages one tick at a time: at most `maximum` ticks, stopping when the queue is empty or, once more
// than `minimum` ticks ran, when `timeLimitMs` have passed.
template <class Server>
void PumpMessages(Server* server, std::uint32_t minimum, std::uint32_t maximum, std::uint32_t timeLimitMs) {
    if (!server) return;
    cRZTimer timer(cRZTimer::kMilliseconds);
    timer.Start();
    if (maximum == 0) return;
    std::uint32_t ticks = 0;
    do {
        if (server->GetMessageQueueSize() == 0) break;
        server->OnTick(0);
        if (minimum < ticks && timeLimitMs < timer.GetElapsedTimeU32()) break;
        ++ticks;
    } while (ticks < maximum);
}
}  // namespace

void cSC4App::RunMessageServerPump(std::uint32_t minimum, std::uint32_t maximum, std::uint32_t timeLimitMs) {
    PumpMessages(SL::spMS, minimum, maximum, timeLimitMs);
}

void cSC4App::RunMessageServer2Pump(std::uint32_t minimum, std::uint32_t maximum, std::uint32_t timeLimitMs) {
    PumpMessages(SL::spMS2, minimum, maximum, timeLimitMs);
}
