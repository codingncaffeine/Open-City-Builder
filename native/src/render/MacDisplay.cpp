#include "render/MacDisplay.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

#include "render/DX7Structs.h"

namespace sgl {

namespace {
IDisplayPlatform* g_platform = nullptr;

struct DisplayInfo {
    std::uint32_t id;
    CGRect bounds;
};
std::vector<DisplayInfo> sDisplayList;
std::int32_t sDisplayIndex = 0;
CGRect sDisplayRect{};
bool sRememberDisplay = false;
std::uint32_t sDisplayID = 0;
bool sInitialized = false;
bool sInWindowMode = true;       // 0x100b13130
bool sEnableSwap = true;         // 0x100b13131
void* sMainWindow = nullptr;
std::int32_t sCurrentWidth = 0, sCurrentHeight = 0, sCurrentDepth = 0;
float sXScale = 0, sYScale = 0;
std::int32_t sSwapCount = 0;
OpaqueContextRef* sContextRef = nullptr;
std::vector<MacResItem> sResolutions, sNativeResolutions;
std::int32_t sMaxWindowWidth = 0, sMaxWindowHeight = 0;
std::int32_t sSystemLock = 0;
std::uint32_t sPCIInfoForDisplay = 0;
ASLPCIDisplayInfo_t sPCIInfo{};
CMacLogFile g_log;

// InitDisplayInfo()::kScreenSizes: the four stock sizes.
const double kScreenSizes[4][2] = {{800, 600}, {1024, 768}, {1280, 1024}, {1600, 1200}};

bool ContainsMode(const std::vector<MacResItem>& list, std::int32_t w, std::int32_t h, std::int32_t depth) {
    for (const MacResItem& item : list)
        if (item.width == w && item.height == h && item.depth == depth) return true;
    return false;
}
}  // namespace

IDisplayPlatform* DisplayPlatform() { return g_platform; }
void SetDisplayPlatform(IDisplayPlatform* platform) { g_platform = platform; }

bool MacResItem::byAspectDelta::operator()(const MacResItem& a, const MacResItem& b) const {
    return std::fabs(static_cast<double>(a.width) / static_cast<double>(a.height) - aspect) <
           std::fabs(static_cast<double>(b.width) / static_cast<double>(b.height) - aspect);
}

OpaqueContextRef::~OpaqueContextRef() {
    if (context) {
        Sync();
        DisplayPlatform()->DeleteGLContext(context);
        context = nullptr;
    }
}

void OpaqueContextRef::Sync() {
    if ((sSwapCount & 1) != 0 && swapOnSync) MacDisplay::SwapContext(this);
    else DisplayPlatform()->FlushGL();
}

void NCT_SetWindowBorderSizes(int left, int top, int right, int bottom) {
    sMaxWindowWidth = right - left;
    sMaxWindowHeight = bottom - top;
}
void NCT_GetWindowMaxSizes(int* width, int* height) {
    *width = sMaxWindowWidth;
    *height = sMaxWindowHeight;
}

void MacGlobals::LockSystem() { ++sSystemLock; }
void MacGlobals::UnlockSystem() {
    if (sSystemLock > 0) --sSystemLock;
}
bool MacGlobals::IsSystemLocked() { return sSystemLock > 0; }

void MacLogToConsole(const char* text) { std::fprintf(stderr, "%s\n", text); }
CMacLogFile::~CMacLogFile() {
    if (file) std::fclose(file);
}
void CMacLogFile::WriteMessage(const char* text) {
    MacLogToConsole(text);
    if (file) {
        const int length = static_cast<int>(std::strlen(text));
        if (length > 0) std::fwrite(text, static_cast<std::size_t>(length), 1, file);
    }
}
CMacLogFile& GetMacLogFile() { return g_log; }
void MacLogFile::WriteMessage(const char* text) { GetMacLogFile().WriteMessage(text); }

void MacDebug::MessageToConsole(const char* format, ...) {
    char buffer[0x400];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    MacLogFile::WriteMessage(buffer);
}

// Shows a Debug / Quit / Ignore / Continue box; Ignore answers 2, Quit exits, the others 0. From any thread but the main
// one it only logs and answers 2. (The original passes the formatted text back through MessageToConsole as a format
// string; here it is logged verbatim.)
std::int32_t MacDebug::MessageToAlert(const char* format, ...) {
    char buffer[0x400];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    MacLogFile::WriteMessage(buffer);
    IDisplayPlatform* platform = DisplayPlatform();
    if (!platform->IsMainThread()) {
        MessageToConsole("MessageToAlert called called from secondary thread\n");
        return 2;
    }
    MacGlobals::LockSystem();
    const int button = platform->ShowAlertBox(MacDisplay::GetMainWindow(), "SimCity 4 Message", buffer);
    platform->FlushInputEvents();
    std::int32_t answer = 0;
    if (button == 0x66) {
        answer = 2;
    } else if (button != 0x67 && button == 0x65) {
        platform->Exit(0);
    }
    MacGlobals::UnlockSystem();
    return answer;
}

std::int32_t GetPCIDisplayInfo(std::uint32_t display, ASLPCIDisplayInfo_t* out) {
    if (!out) return static_cast<std::int32_t>(0xe00002bc);
    if (display == 0) display = DisplayPlatform()->MainDisplayID();
    if (display == sPCIInfoForDisplay) {
        *out = sPCIInfo;
        return 0;
    }
    ASLPCIDisplayInfo_t info{};
    if (!DisplayPlatform()->QueryPCIDisplayInfo(display, info)) return static_cast<std::int32_t>(0xe00002bc);
    sPCIInfo = info;
    sPCIInfoForDisplay = display;
    *out = info;
    return 0;
}

bool IsDisplayValid(std::uint32_t display) { return DisplayPlatform()->DisplayHasHardwareRenderer(display); }

// Fills the display list the first time, with every active display that has a usable renderer.
void BuildDisplayList() {
    if (!sDisplayList.empty()) return;
    std::vector<std::uint32_t> displays;
    if (!DisplayPlatform()->ActiveDisplays(displays)) return;
    for (std::uint32_t id : displays) {
        if (IsDisplayValid(id)) sDisplayList.push_back({id, DisplayPlatform()->DisplayBounds(id)});
    }
}

std::int32_t ValidateDisplay(std::uint32_t index, const CGRect* rect, std::uint32_t* displayOut) {
    if (!rect) return static_cast<std::int32_t>(0xffffffce);
    BuildDisplayList();
    std::int32_t result = static_cast<std::int32_t>(0xffffe7ab);
    if (!sDisplayList.empty() && index < sDisplayList.size()) {
        const DisplayInfo& info = sDisplayList[index];
        const bool equal = info.bounds.x == rect->x && info.bounds.y == rect->y && info.bounds.width == rect->width &&
                           info.bounds.height == rect->height;
        if (equal && IsDisplayValid(info.id)) {
            result = 0;
            if (displayOut) *displayOut = info.id;
        }
    }
    return result;
}

void MacDisplay::SwapContext(OpaqueContextRef*) {
    if (sEnableSwap) {
        if (!sInWindowMode) DisplayPlatform()->SetMenuBarVisible(false);
        DisplayPlatform()->SwapWindow(sMainWindow);
        ++sSwapCount;
    }
}

// Reads the saved display choice, validates it (falling back to the main display), then builds the list of modes the game
// offers: every 32-bit non-scaled mode of at least 800x600 that fits the display, the four stock sizes, and a "native"
// list of up to four sizes closest to the smallest mode's aspect ratio.
std::int32_t MacDisplay::Initialize() {
    IDisplayPlatform* platform = DisplayPlatform();
    platform->IsMacOSSierraOrLater();
    sDisplayIndex = MacPreferences::GetInteger("display.index", 0);
    MacPreferences::GetRect("display.rect", sDisplayRect);
    sRememberDisplay = MacPreferences::GetBoolean("display.remember", false);
    if (ValidateDisplay(static_cast<std::uint32_t>(sDisplayIndex), &sDisplayRect, &sDisplayID) != 0) {
        sDisplayID = platform->MainDisplayID();
        sDisplayRect = platform->DisplayBounds(sDisplayID);
    }
    MacPreferences::PutInteger("display.index", sDisplayIndex);
    MacPreferences::PutRect("display.rect", sDisplayRect);
    MacPreferences::PutBoolean("display.remember", sRememberDisplay);
    MacPreferences::Synchronize();

    char renderer[0x80];
    const char* name = platform->GLRendererString();
    std::strncpy(renderer, name ? name : "", 0x7f);
    renderer[0x7f] = '\0';
    for (char* c = renderer; *c; ++c) *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    bool irisPro6200 = true;
    if (!std::strstr(renderer, "iris pro graphics 6200")) irisPro6200 = std::strstr(renderer, "iris pro 6200") != nullptr;

    const std::vector<IDisplayPlatform::Mode> modes = platform->DisplayModes(sDisplayID);
    if (modes.empty()) return static_cast<std::int32_t>(0xffffeca0);
    for (const IDisplayPlatform::Mode& mode : modes) {
        if (irisPro6200 && mode.width == 0x690 && mode.height == 0x3b0) continue;   // 1680x944 is broken on that board
        if (mode.width != mode.pixelWidth || mode.height != mode.pixelHeight) continue;   // skip scaled modes
        const CGRect bounds = platform->DisplayBounds(sDisplayID);
        if (!(mode.width <= bounds.width) || !(mode.height <= bounds.height)) continue;
        if (mode.width < 800 || mode.height < 600 || mode.depth < 0x20) continue;
        if (ContainsMode(sResolutions, mode.width, mode.height, mode.depth)) continue;
        sResolutions.push_back({mode.width, mode.height, mode.depth});
    }
    std::sort(sResolutions.begin(), sResolutions.end(), MacResItem::byResolution{});
    for (const auto& size : kScreenSizes) {
        const MacResItem item{static_cast<std::int32_t>(size[0]), static_cast<std::int32_t>(size[1]), 0x20};
        if (!ContainsMode(sResolutions, item.width, item.height, 0x20)) sResolutions.push_back(item);
    }
    sNativeResolutions.clear();
    std::vector<MacResItem> candidates;
    for (const MacResItem& item : sResolutions)
        if (item.depth == 0x20) candidates.push_back(item);
    const double aspect = static_cast<double>(candidates[0].width) / static_cast<double>(candidates[0].height);
    auto takeWithin = [&](double tolerance) {
        for (std::size_t i = 0; i < candidates.size();) {
            const MacResItem item = candidates[i];
            if (tolerance <= std::fabs(static_cast<double>(item.width) / static_cast<double>(item.height) - aspect)) {
                ++i;
            } else {
                sNativeResolutions.push_back(item);
                candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    };
    takeWithin(0.01);
    if (sNativeResolutions.size() < 4 && !candidates.empty()) takeWithin(0.2);
    std::sort(sNativeResolutions.begin(), sNativeResolutions.end(), MacResItem::byResolution{});
    if (sNativeResolutions.size() < 4) {
        for (std::size_t k = 0; k < 4 && sNativeResolutions.size() < 4; ++k) {
            const MacResItem item{static_cast<std::int32_t>(kScreenSizes[k][0]), static_cast<std::int32_t>(kScreenSizes[k][1]), 0x20};
            if (!ContainsMode(sNativeResolutions, item.width, item.height, 0x20)) sNativeResolutions.push_back(item);
        }
        std::sort(sNativeResolutions.begin(), sNativeResolutions.end(), MacResItem::byResolution{});
    }
    if (sNativeResolutions.size() > 4) {
        // keep the largest size plus the three closest in aspect
        const MacResItem largest = sNativeResolutions.back();
        sNativeResolutions.pop_back();
        std::sort(sNativeResolutions.begin(), sNativeResolutions.end(), MacResItem::byAspectDelta{aspect});
        sNativeResolutions.resize(3);
        sNativeResolutions.push_back(largest);
        std::sort(sNativeResolutions.begin(), sNativeResolutions.end(), MacResItem::byResolution{});
    }
    std::reverse(sNativeResolutions.begin(), sNativeResolutions.end());
    sInitialized = true;
    return 0;
}

std::int32_t MacDisplay::SetupDisplay(std::uint8_t windowed, int width, int height) {
    if (!sInitialized) return static_cast<std::int32_t>(0xffffffce);
    IDisplayPlatform* platform = DisplayPlatform();
    sInWindowMode = windowed != 0;
    sCurrentWidth = width;
    sCurrentHeight = height;
    sXScale = static_cast<float>(static_cast<double>(width) / sDisplayRect.width);
    sYScale = static_cast<float>(static_cast<double>(sCurrentHeight) / sDisplayRect.height);
    if (windowed == 0) {
        sMainWindow = platform->CreateWindow("SimCity\xe2\x84\xa2" "4 Deluxe Edition", false, 0, 0, sDisplayRect);
    } else {
        sMainWindow = platform->CreateWindow("SimCity\xe2\x84\xa2" "4 Deluxe Edition", true, width, height, sDisplayRect);
        const CGRect bounds = platform->DisplayBounds(sDisplayID);
        platform->SetWindowPosition(sMainWindow, static_cast<int>(bounds.x), static_cast<int>(bounds.y));
        platform->SetWindowSize(sMainWindow, width, height);
        platform->CenterWindow(sMainWindow);
    }
    return 0;
}

void MacDisplay::ReleaseDisplay() {
    if (sInitialized) {
        sInWindowMode = true;
        if (sMainWindow) {
            DisplayPlatform()->DestroyWindow(sMainWindow);
            sMainWindow = nullptr;
        }
    }
}

void* MacDisplay::GetMainWindow() { return sMainWindow; }

void MacDisplay::GetDisplayScale(float* x, float* y) {
    if (!sInWindowMode) {
        *x = sXScale;
        *y = sYScale;
    } else {
        *x = 1.0f;
        *y = 1.0f;
    }
}

std::uint32_t MacDisplay::GetDisplayID() { return sDisplayID; }
bool MacDisplay::IsWindowMode() { return sInWindowMode; }

bool MacDisplay::PtInDisplay(std::int32_t point) {
    int width = 0, height = 0;
    DisplayPlatform()->GetWindowSize(sMainWindow, width, height);
    const std::int32_t x = point >> 16;
    const std::int32_t y = static_cast<std::int32_t>(static_cast<std::uint32_t>(point) << 16) >> 16;
    if (x < 0) return false;
    if (y < 0) return false;
    return x <= width && y <= height;
}

void MacDisplay::GetCurrentMode(int& width, int& height, int& depth) {
    IDisplayPlatform* platform = DisplayPlatform();
    if (!sMainWindow) {
        width = platform->DisplayPixelsWide(sDisplayID);
        height = platform->DisplayPixelsHigh(sDisplayID);
    } else {
        width = sCurrentWidth;
        height = sCurrentHeight;
    }
    if (sCurrentDepth == 0) sCurrentDepth = platform->CurrentDisplayDepth(sDisplayID);
    depth = sCurrentDepth;
}

void MacDisplay::SetMode(int width, int height, int depth) {
    IDisplayPlatform* platform = DisplayPlatform();
    if (platform->WindowOpacity(sMainWindow) > 0.0f) {
        int currentWidth, currentHeight, currentDepth;
        GetCurrentMode(currentWidth, currentHeight, currentDepth);
        if (currentWidth == width && currentHeight == height) return;
        platform->HideWindow(sMainWindow);
    }
    sCurrentDepth = depth;
    if (sInWindowMode) {
        platform->SetWindowSize(sMainWindow, width, height);
        platform->CenterWindow(sMainWindow);
    }
    platform->ShowWindow(sMainWindow);
}

int MacDisplay::GetNumModes() { return static_cast<int>(sResolutions.size()); }

void MacDisplay::GetNthMode(int n, int& width, int& height, int& depth) {
    width = sResolutions[static_cast<std::size_t>(n)].width;
    height = sResolutions[static_cast<std::size_t>(n)].height;
    depth = sResolutions[static_cast<std::size_t>(n)].depth;
}

void MacDisplay::GetNthNativeResolution(int n, int& width, int& height) {
    height = sNativeResolutions[static_cast<std::size_t>(n)].height;
    width = sNativeResolutions[static_cast<std::size_t>(n)].width;
}

OpaqueContextRef* MacDisplay::CreateScreenContext(int, std::uint8_t& fastFlag) {
    MacPreferences::GetBoolean("32-bit-z-buffer", false);   // (read, unused)
    fastFlag = 0;
    auto* ref = new OpaqueContextRef;
    void* context = DisplayPlatform()->CreateGLContext(sMainWindow, !sInWindowMode, sCurrentWidth, sCurrentHeight);
    if (!context) {
        delete ref;   // (the original leaks it here)
        return nullptr;
    }
    ref->context = context;
    ref->swapOnSync = true;
    sContextRef = ref;
    return ref;
}

void MacDisplay::ReleaseContext(OpaqueContextRef*& context) {
    if (context) delete context;
    context = nullptr;
}

void MacDisplay::SetCurrentContext(OpaqueContextRef* context) { DisplayPlatform()->MakeCurrent(sMainWindow, context->context); }

void MacDisplay::GetVideoMemoryInfo(std::uint32_t& a, std::uint32_t& b) {
    b = 0x10;
    a = 0x10;
    ASLPCIDisplayInfo_t info{};
    if (GetPCIDisplayInfo(sDisplayID, &info) == 0) {
        b = info.vramMB;
        a = info.vramMB;
    }
}

std::int32_t MacDisplay::GetClientRect(void* window, tagRECT* rect) {
    int width = 0, height = 0;
    DisplayPlatform()->GetWindowSize(window, width, height);
    auto* r = reinterpret_cast<std::int32_t*>(rect);
    r[0] = 0;
    r[1] = 0;
    r[2] = width;
    r[3] = height;
    if (!sInWindowMode) {
        r[0] = 0;
        r[1] = 0;
        r[2] = sCurrentWidth;
        r[3] = sCurrentHeight;
    }
    return 1;
}

}  // namespace sgl
