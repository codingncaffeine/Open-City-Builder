// sc4_game — the native Metal application. The recovered cSC4App boots against the retail game data; its application window
// (cWinSC4App) creates the splash and region screens from the game's own UI scripts; every window draws through the game's
// own draw context, whose screen engine is the recovered hardware engine issuing cIGZGDriver calls into cMetalGDriver. The frame
// is whatever the driver holds: there is no CPU presentation.
//
//   sc4_game                      open a window
//   sc4_game --shot out.png [--frames N]   run N frames (default 120) headless and write the frame target as a PNG
//   sc4_game --script scenarios/first_city.txt [--threads=1]   run a scenario headless (see RunScenario); exit code 0 = it held
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL.h>   // the test stub: its timers never fire by themselves, the frame loop fires them (see PumpTimers)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <map>
#include <sstream>
#include <cctype>
#include <string>
#include <vector>

#include "platform/GameDataPath.h"
#include "app/NativeBoot.h"
#include "app/NativeCity.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeCityView.h"
#include "app/NativeFonts.h"
#include "app/NativeGraphics.h"
#include "app/NativeCity.h"
#include "app/NativeRegion.h"
#include "app/NativeRegionScreen.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "app/NativeWinSC4App.h"
#include "gz/BufferFontRenderer.h"
#include "gz/cGZMessageServer2.h"
#include "gz/DrawFontRenderer.h"
#include "gz/GZFont.h"
#include "gz/GZFramework.h"
#include "gz/WinFont.h"
#include "render/gd/MetalGDriver.h"
#include "app/NativeAutomata.h"
#include "app/NativeLighting.h"
#include "app/NativeJobs.h"
#include "app/NativeRuntimeWire.h"
#include "sc4/View3DWin.h"
#include "gz/FontServices.h"
#include "sc4/App.h"
#include "sc4/SC4UI.h"
#include "sc4/SLBuffer.h"
#include "sc4/ServiceLocator.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/WinSC4App.h"
#include "sc4/WinSplashScreen.h"
#include "app/GameAudio.h"
#include "app/NativeZoneTool.h"
#include "app/NativeRegionExtras.h"
#include "app/NativeAudio.h"
#include "app/NativeAudioDirector.h"

namespace {

constexpr int kW = 1024, kH = 768;

std::unique_ptr<gd::cMetalGDriver> gDriver;
std::unique_ptr<native::Graphics> gGraphics;
std::unique_ptr<native::WinMgr> gMgr;

// ---- fonts: the recovered cGZFont over FreeType, drawing through the surface's context --------------------------------------
native::FreeTypeFontHost* gFontHost;
cGZFont* gFont;

struct DrawFontHost final : IDrawFontRendererHost {
    cIGZFontRenderer* CreateBufferFontRenderer() override { auto* r = new cGZBufferFontRenderer(); r->AddRef(); return r; }
    cIGZBuffer* CreateBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
    void NativeColorToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { r = n >> 16; g = n >> 8; b = n; }
} gDrawFontHost;

// The game's fonts: the .mxf files of GameData/Fonts (FreeType reads them through NativeFonts' MXF decoder) and the
// [Font Styles] table of the font-style resource {0, 0x4a87bfe8, 0x2a87bffc} ("<name> = <faces>, <size>, <params>,
// <guid>"), the specifications cSC4App::SetFontStyle hands CreateFontStyleFromSpecification (0x1003d9360). Each style
// becomes one cGZFont; the windows ask for their style by guid.
std::string GameFontsDirectory() {
    return platform::GameDataDir() + "Fonts/";
}

struct FontFile { std::string path, family; bool bold = false, italic = false; };
const std::vector<FontFile>& GameFontFiles() {
    static std::vector<FontFile> files;
    static bool scanned = false;
    if (scanned) return files;
    scanned = true;
    NSArray* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:[NSString stringWithUTF8String:GameFontsDirectory().c_str()] error:nil];
    for (NSString* name in names) {
        if (![[name.pathExtension lowercaseString] isEqualToString:@"mxf"]) continue;
        FontFile f;
        f.path = GameFontsDirectory() + name.UTF8String;
        std::unique_ptr<IFontFace> face = gFontHost->NewFace(f.path);
        if (!face) continue;
        f.family = face->FamilyName();
        f.bold = (face->StyleFlags() & kFontStyleFlagBold) != 0;
        f.italic = (face->StyleFlags() & kFontStyleFlagItalic) != 0;
        files.push_back(f);
    }
    return files;
}

std::string Squash(const std::string& s) {   // case and space insensitive family names ("StocletITC TT" = "Stoclet ITC TT")
    std::string out;
    for (char c : s) if (c != ' ') out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The file of the first listed face that exists, preferring the requested weight / slant.
std::string FindFontFile(const std::string& faces, bool bold, bool italic) {
    std::stringstream list(faces);
    std::string face;
    while (std::getline(list, face, ',')) {
        const std::string want = Squash(face);
        const FontFile* best = nullptr;
        int bestScore = -1;
        for (const FontFile& f : GameFontFiles()) {
            if (Squash(f.family) != want) continue;
            const int score = (f.bold == bold ? 2 : 0) + (f.italic == italic ? 1 : 0);
            if (score > bestScore) { best = &f; bestScore = score; }
        }
        if (best) return best->path;
    }
    return {};
}

struct StyleFont { cGZFont* font = nullptr; };
std::map<std::uint32_t, StyleFont> gStyleFonts;

cGZFont* MakeFont(const std::string& path, std::uint32_t size) {
    auto* font = new cGZFont(gFontHost);
    font->AddRef();
    font->SetFilePath(cRZString(path.c_str()));
    font->SetSize(size);
    font->SetAntialiasing(3);
    font->SetCodePage(8);
    font->SetRenderer(new cGZDrawFontRenderer(&gDrawFontHost));
    return font;
}

// One [Font Styles] line: the quoted faces, size and parameters, then the guid.
void AddFontStyle(const std::string& line) {
    std::vector<std::string> quoted;
    std::size_t at = line.find('=');
    if (at == std::string::npos) return;
    std::size_t pos = at + 1;
    while (quoted.size() < 3) {
        const std::size_t open = line.find('"', pos);
        if (open == std::string::npos) return;
        const std::size_t close = line.find('"', open + 1);
        if (close == std::string::npos) return;
        quoted.push_back(line.substr(open + 1, close - open - 1));
        pos = close + 1;
    }
    const std::size_t guidAt = line.find("0x", pos);
    if (guidAt == std::string::npos) return;
    const std::uint32_t guid = std::uint32_t(std::strtoul(line.c_str() + guidAt, nullptr, 16));
    std::string params = quoted[2];
    for (char& c : params) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* word) { return params.find(word) != std::string::npos; };
    auto number = [&](const char* key, float fallback) {
        const std::size_t k = params.find(key);
        return k == std::string::npos ? fallback : std::strtof(params.c_str() + k + std::strlen(key), nullptr);
    };
    const std::string path = FindFontFile(quoted[0], has("bold"), has("italic"));
    if (path.empty()) return;
    cGZFont* font = MakeFont(path, std::uint32_t(std::atoi(quoted[1].c_str())));
    font->SetScaling(number("xscale=", 1.0f), number("yscale=", 1.0f));
    font->SetAdvanceScaling(number("xadvancescale=", 1.0f), 1.0f);
    font->SetSpacingAdjustment(0, int(number("linespacing=", 0.0f)));
    if (has("aa=none")) font->SetAntialiasing(1);
    if (has("underline") || has("under|") ) font->SetStyle(4, true);
    if (has("strikethrough")) font->SetStyle(8, true);
    if (has("gamma=")) font->SetAntialiasingGammaCorrection(number("gamma=", 1.0f));
    if (!font->Init()) { font->Release(); return; }
    if (gStyleFonts[guid].font) gStyleFonts[guid].font->Release();
    gStyleFonts[guid].font = font;
}

void LoadFontStyles() {
    static bool loaded = false;
    if (loaded) return;
    std::vector<std::uint8_t> bytes;
    if (!native::LoadResourceBytes(0, 0x4a87bfe8, 0x2a87bffc, bytes)) return;   // the resources are not up yet: try again later
    loaded = true;
    std::stringstream text(std::string(bytes.begin(), bytes.end()));
    std::string line;
    bool inStyles = false;
    while (std::getline(text, line)) {
        const std::size_t first = line.find_first_not_of(" \t\r`");
        if (first == std::string::npos || line[first] == ';') continue;
        if (line[first] == '[') { inStyles = line.find("[Font Styles]") != std::string::npos; continue; }
        if (inStyles) AddFontStyle(line);
    }
    std::printf("font: %zu game font styles\n", gStyleFonts.size());
}

bool InitFont() {
    gFontHost = new native::FreeTypeFontHost();
    // Until the font-style resource can be read (it needs the game's resources), the default style's face and size.
    std::string path = FindFontFile("Arta", false, true);
    if (path.empty()) path = "/System/Library/Fonts/Supplemental/Arial.ttf";
    gFont = MakeFont(path, 13);
    return gFont->Init();
}

class WinFont final : public gz::IWinFont {
public:
    cGZFont* font = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZFont* AsFont() override { return font; }
    std::uint32_t DefaultTextColor() override { return 0xffffffffu; }
    std::uint32_t DefaultShadowColor() override { return 0xff000000u; }
    std::int16_t LineHeight(int, int) override { return std::int16_t(font->GetLineHeight()); }
    int CountLines(const char* t, std::uint32_t n, int width) override { return int(font->CalculateNumberOfLines(t, n, std::uint32_t(width))); }
    int WrappedWidth(const char* t, std::uint32_t n, int width) override { return int(font->CalculateWidthOfLines(t, n, std::uint32_t(width))); }
    void MeasureText(const char* t, std::uint32_t n, cRZRect& out) override { font->TextArea(t, n, out); }
    int TextWidth(const char* t, std::uint32_t n) override { return font->GetStringWidth(t, n); }
    void DrawText(IWinBuffer* target, int x, int y, const char* t, std::uint32_t n, std::uint32_t color, std::uint32_t color2,
                  const cRZRect* clip, int) override {
        font->DrawTextLine(static_cast<native::Buffer*>(target)->AsBuffer(), x, y, t, n, color & 0xffffffu,
                           color2 == gz::kWinUnsetTextColor ? kFontDefaultColor : (color2 & 0xffffffu), clip, nullptr);
    }
    void DrawTextBox(IWinBuffer* target, const cRZRect& box, const char* t, std::uint32_t n, std::uint32_t alignment, std::uint32_t color,
                     std::uint32_t color2, const cRZRect* clip, int) override {
        font->DrawTextParagraph(static_cast<native::Buffer*>(target)->AsBuffer(), box, t, n, int(alignment), color & 0xffffffu,
                                color2 == gz::kWinUnsetTextColor ? kFontDefaultColor : (color2 & 0xffffffu), clip, nullptr);
    }
};
struct FontSource final : gz::IWinFontSource {
    // never destroyed: windows still hold their fonts while the process exits
    std::map<std::uint32_t, WinFont>& fonts = *new std::map<std::uint32_t, WinFont>();
    gz::IWinFont* Font(std::uint32_t style) override {
        if (!gFont) return nullptr;
        LoadFontStyles();
        auto it = gStyleFonts.find(style);
        if (it == gStyleFonts.end()) it = gStyleFonts.find(gz::kWinDefaultFontStyle);
        WinFont& f = fonts[it == gStyleFonts.end() ? 0u : it->first];
        f.font = it == gStyleFonts.end() ? gFont : it->second.font;
        return &f;
    }
    bool Available() override { return gFont != nullptr; }
} gFontSource;

// ---- PNG resources: ImageIO -> a CPU buffer with straight alpha ------------------------------------------------------------
cGZBuffer* DecodePngBytes(const std::vector<uint8_t>& png) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)[NSData dataWithBytes:png.data() length:png.size()], nullptr);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    if (!img) return nullptr;
    const std::uint32_t w = std::uint32_t(CGImageGetWidth(img)), h = std::uint32_t(CGImageGetHeight(img));
    const CGImageAlphaInfo alphaInfo = CGImageGetAlphaInfo(img);
    const bool opaque = alphaInfo == kCGImageAlphaNone || alphaInfo == kCGImageAlphaNoneSkipFirst || alphaInfo == kCGImageAlphaNoneSkipLast;
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    buffer->Init(w, h, cGZBufferColorType{kBufferColorARGB8888, 32});
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    std::memset(bits, 0, std::size_t(stride) * h);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(bits, w, h, 8, stride, cs, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGContextDrawImage(cg, CGRectMake(0, 0, w, h), img);
    CGContextRelease(cg); CGColorSpaceRelease(cs); CGImageRelease(img); CFRelease(src);
    for (std::uint32_t y = 0; y < h; ++y) {   // un-premultiply (B, G, R, A in memory)
        std::uint8_t* row = bits + std::size_t(y) * stride;
        for (std::uint32_t x = 0; x < w; ++x, row += 4) {
            const unsigned a = row[3];
            if (a != 0 && a != 255) for (int c = 0; c < 3; ++c) row[c] = std::uint8_t(std::min(255u, (row[c] * 255u + a / 2) / a));
        }
    }
    // The UI art marks its transparent parts in opaque magenta (the city toolbar's tiled background, for one): every image
    // comes with that colour key, as the shipped game shows them.
    (void)opaque;
    buffer->SetTransparency(buffer->ConvertRGBValueToNative(0xff, 0, 0xff));
    return buffer;
}

// ---- cSLBuffer's loader: every bitmap the UI names by resource key or by file ------------------------------------------------
class BufferHost final : public ISLBufferHost {
public:
    bool GetBufferResource(const cGZPersistResourceKey& key, std::int32_t, bool, cIGZBuffer** out) override {
        cGZBuffer* buffer = native::LoadBitmapResource(key.group, key.instance);   // PNG resources decode to ARGB8888, the display's 32-bit type
        *out = buffer;
        return buffer != nullptr;
    }
    bool HasResource(const cGZPersistResourceKey& key) override {
        cGZBuffer* buffer = native::LoadBitmapResource(key.group, key.instance);
        if (buffer) buffer->Release();
        return buffer != nullptr;
    }
    bool LowQualityTextures() override { return false; }
    bool LoadBufferFromFile(const std::string& path, std::int32_t, cIGZBuffer** out) override {
        NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
        if (!data) return false;
        std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(data.bytes), static_cast<const std::uint8_t*>(data.bytes) + data.length);
        *out = DecodePngBytes(bytes);   // ImageIO reads BMP and PNG alike
        return *out != nullptr;
    }
    cIGZBuffer* CreateBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
} gBufferHost;

// nSC4UI's services: the script window builder and the window manager (the default host reaches for engine globals the app lacks)
class UiHost final : public nSC4UI::INSC4UIHost {
public:
    bool PopupModalDialogsEnabled() override { return gMgr && gMgr->modalDialogs; }
    bool LoadExemplarString(cIGZString&, sc4::cISCPropertyHolder*, std::uint32_t, sc4::cSCStringDetokenizer*) override { return false; }
    void MakeNumberString(std::int64_t value, cIGZString& out) override { char t[32]; std::snprintf(t, sizeof t, "%lld", (long long)value); out.AssignFromCString(t); }
    cIGZWinMgr* WindowManager() override { return gMgr.get(); }
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override { return native::CreateWindowFromScript(key, parent, id); }
    bool CreateWindowFromScriptAs(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        cIGZWin* window = CreateWindowFromScript(key, parent, id);
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out);
        window->Release();
        return ok;
    }
    cSLWinLocationSaver* WindowLocationSaver() override { return nullptr; }
    void BeginGamePause() override {}
    void EndGamePause() override {}
} gUiHost;

// Repaints a window and everything below it (a plain InvalidateSelf marks one window only).
bool InvalidateTreeCallback(cIGZWin*, std::uint32_t, void* child, void*);
void InvalidateTree(cIGZWin* window) {
    window->InvalidateSelf();
    window->EnumChildren(0x22ba0121, InvalidateTreeCallback, nullptr);
}
bool InvalidateTreeCallback(cIGZWin*, std::uint32_t, void* child, void*) { InvalidateTree(static_cast<cIGZWin*>(child)); return true; }

// Debug: prints the window tree below `window` (id, visibility, area) to a depth.
void DumpTree(cIGZWin* window, int depth, int maxDepth) {
    const cRZRect a = window->GetArea();
    std::printf("%*s id=%08x vis=%d area=%d,%d,%d,%d kids=%u\n", depth * 2, "", window->GetID(), window->IsVisible(), a.left, a.top, a.right, a.bottom, window->GetChildCount());
    if (depth >= maxDepth) return;
    struct Ctx { int depth, max; };
    Ctx ctx{depth + 1, maxDepth};
    window->EnumChildren(0x22ba0121, [](cIGZWin*, std::uint32_t, void* child, void* c) { auto* x = static_cast<Ctx*>(c); DumpTree(static_cast<cIGZWin*>(child), x->depth, x->max); return true; }, &ctx);
}

// ---- the frame loop ---------------------------------------------------------------------------------------------------------
// The SDL stub's timers never fire by themselves: fire the armed one when its period has passed.
// SDL_GetTicks for the stub SDL clock: milliseconds since start-up, never 0 (a cRZTimer started at tick 0 counts as stopped). The
// wall-clock milliseconds (~1.7e12) do not fit 32 bits, and on arm64 the float -> uint32 conversion saturates to 0xffffffff instead of
// wrapping, which froze every RZ timer in the game.
std::uint32_t StubTicksNow() {
    static const double start = [[NSDate date] timeIntervalSince1970] * 1000.0;
    return 1u + static_cast<std::uint32_t>(static_cast<std::uint64_t>([[NSDate date] timeIntervalSince1970] * 1000.0 - start));
}

void PumpTimers() {
    static double armedAt = -1; static int armedId = 0;
    const double now = [[NSDate date] timeIntervalSince1970] * 1000.0;
    g_ticks = StubTicksNow();
    if (g_activeTimerId != armedId) { armedId = g_activeTimerId; armedAt = armedId ? now : -1; }
    if (armedId && g_timerCallback && now - armedAt >= g_timerInterval) {
        if (std::getenv("SC4_DEBUG")) std::printf("debug: timer %d fires\n", armedId);
        auto cb = g_timerCallback; void* param = g_timerParam;
        armedId = 0; armedAt = -1;
        cb(g_timerInterval, param);
    }
}

native::HostWinSC4App* gAppHost = nullptr;
cGZMessageServer2* gMessages2 = nullptr;   // ticked by the frame loop, as the framework ticks the service in the game
sc4::cWinSC4App* gAppWindow = nullptr;

// What the splash screen reaches outside itself (SPLASH-1): the game's script window builder, the message server, the driver
// clear, and the application window's screen swap.
struct SplashHost final : sc4::ISplashScreenHost {
    struct Unknown final : cIGZUnknown {
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
    } graphics;
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        return native::CreateWindowFromScript(key, parent, id);
    }
    cIGZUnknown* GraphicSystemObject() override { return &graphics; }
    cIGZMessageServer2* MessageServer2() override { return SL::spMS2; }
    cIGZMessage2Standard* CreateStandardMessage() override { auto* m = new cRZMessage2Standard(); m->AddRef(); return m; }
    void ClearScreenToBlack() override {
        gDriver->ClearColor(0, 0, 0, 0);
        gDriver->Clear(gd::kClearColor);
    }
    void DestroyCurrentScreen() override {
        std::printf("splash: DestroyCurrentScreen\n");
        if (gAppWindow) gAppWindow->DestroyCurrentScreen();
        ClearScreenToBlack();   // the frame persists between paints; the next screen draws over a clean one
        gMgr->GetMainWindow()->InvalidateSelf();
    }
    void CreateRegionScreen() override { std::printf("splash: CreateRegionScreen\n"); if (gAppWindow) gAppWindow->CreateRegionScreen(); }
} gSplashHost;

// The game's sound: Sound.dat opened through the recovered persistence layer, an ambient bed that follows the screen
// and a click on every mouse press (the interactive run only; shots stay silent).
GameAudio* gAudio = nullptr;
void StartGameAudio() {
    static GameAudio audio;
    std::string error;
    const std::string dat = platform::GameDataDir() + "Sound.dat";
    if (!audio.Open(dat, error)) { std::printf("audio: %s\n", error.c_str()); return; }
    gAudio = &audio;
    native::SetGameAudio(&audio);
    if (std::getenv("SC4_AUDIODIRECTORS") && native::BootFramework())
        native::StartAudioDirectors(*native::BootFramework(), dat.substr(0, dat.rfind('/') + 1));
    native::InstallButtonSounds();
    std::printf("audio: %zu sounds\n", audio.SoundCount());
}

void Frame() {
    native::PumpToolTicks(16);
    native::TickLighting();
    native::TickAutomata();
    {   // the framework tick of the simulator host's services (cSC4Simulator::OnTick: the date and every simulator agent)
        static std::uint32_t tickNumber = 0;
        native::TickSimulatorServices(++tickNumber);
    }
    // In a city the animation tick manager is ticked here (the region screen ticks it from its GZPaint; the city view's per-frame call is
    // not recovered). Init is idempotent: the region screen shut it down when the city replaced it.
    if (gAppWindow && gAppWindow->View3D() && native::StartAnimationTicks()) native::TickAnimation();
    native::EndFrameJobs();
    if (gAudio) gAudio->Tick();
    PumpTimers();
    cSC4App* app = native::BootApplication();
    if (app) {
        app->RunMessageServerPump(0, 64, 4);
        app->RunMessageServer2Pump(0, 64, 4);
        if (gMessages2) gMessages2->OnTick(16);   // posted and general messages (the splash timer, screen swaps)
        app->OnTick(16);
    }
    const bool regionScreen = gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793;
    if (gAudio && gAppWindow && gAppWindow->CurrentScreen()) {   // the radio follows the screen: the region station on the map, the mayor's in a city
        static const std::string stations = platform::GameDataDir() + "Radio/Stations/";
        gAudio->SetStation(stations + (regionScreen ? "Region" : (gAppWindow->View3D() ? "Mayor" : "Region")) + "/Music");
    }
    if (regionScreen && native::DrawRegionLayers()) InvalidateTree(gMgr->GetMainWindow());   // the map is drawn under the windows: they repaint over it
    // The city view redraws the 3D scene into the whole screen every frame, so the windows over it repaint every frame too.
    if (gAppWindow && gAppWindow->View3D() && !regionScreen) InvalidateTree(gMgr->GetMainWindow());
    gMgr->Pump();
    gMgr->Plot();
    gGraphics->Services().Tick();
}

bool Boot() {
    native::SetPngDecoder(&DecodePngBytes);
    SetSLBufferHost(&gBufferHost);
    nSC4UI::SetHost(&gUiHost);
    gGraphics.reset(new native::Graphics(gDriver.get(), kW, kH));
    gMgr.reset(new native::WinMgr());
    gMgr->AddRef();
    gMgr->Init();
    gMgr->GetMainWindow()->SetArea(0, 0, kW, kH);
    if (!InitFont()) std::printf("font: FAILED to initialise\n");
    gz::SetWinFontSource(&gFontSource);

    const bool ok = native::RunBoot(true, true, gDriver.get(), [&](cGZFramework& framework, cSC4App& app) {
        SL::SetFramework(&framework);             // the locator's RZGetFramework(): without it every SL:: service stays null
        framework.AddSystemService(gMgr.get());   // RZ::WM(): the application finds the main window through it
        // The second message server (service 0x04fa845b): the splash timer, the screen swaps and every posted command go through it.
        // The frame loop ticks it by hand (RunMessageServer2Pump), so the framework tick registration is a no-op.
        static struct : IMessageServerFramework {
            void AddTickClient(cIGZSystemService*) override {}
            void RemoveTickClient(cIGZSystemService*) override {}
        } messageFramework;
        SetMessageServerFramework(&messageFramework);
        auto* messages2 = new cGZMessageServer2();
        framework.AddSystemService(messages2);
        messages2->Init();
        gMessages2 = messages2;
        static native::HostWinSC4App host(&framework);
        gAppHost = &host;
        // The window scripts need the UI script service, which needs the resources Init has just registered; the application window
        // (created a few steps later, inside Init) builds the splash from them.
        app.StartupHooks().configureResourceCache = [] {
            std::printf("ui scripts: %s\n", native::InitUiScripts(gMgr.get()) ? "service ready" : "NOT available");
        };
        app.StartupHooks().createApplicationWindow = [&](cSC4App& a) -> sc4::AppStartupHooks::ApplicationWindow {
            sc4::SetSplashScreenHost(&gSplashHost);
            auto* window = new sc4::cWinSC4App(&a, gMgr.get(), &host);
            gAppWindow = window;
            static sc4::WinSC4AppApplication appView(window);
            return {static_cast<cIGZWin*>(window), &appView};
        };
    });
    if (!ok) std::printf("boot: FAILED\n");
    native::RegisterExemplarFactories();   // tuning exemplars are read on the region screen too (the audio options)
    if (cSC4App* app = native::BootApplication()) {
        native::RegionCallbacks callbacks;
        callbacks.dataDirectory = platform::GameDataDir();
        callbacks.isRegionScreenCurrent = [] { return gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793; };
        callbacks.destroyCurrentScreen = [] { if (gAppWindow) gAppWindow->DestroyCurrentScreen(); };
        callbacks.createRegionScreen = [] { if (gAppWindow) gAppWindow->CreateRegionScreen(); };
        native::InstallRegionServices(*app, callbacks);
        native::RegionScreenServices screen;
        screen.app = app;
        screen.graphics = gGraphics.get();
        screen.createWindowFromScript = &native::CreateWindowFromScript;
        screen.loadBitmap = [](std::uint32_t group, std::uint32_t instance) { return native::LoadBitmapResource(group, instance); };
        native::SetCityScreenCallbacks({[] { if (gAppWindow) gAppWindow->DestroyCurrentScreen(); }, [] {
            if (!gAppWindow || !gAppWindow->CreateCityScreen()) return;
            if (sc4::cSC4View3DWin* view = gAppWindow->View3D()) native::AttachCityRenderStack(*view, native::BootApplication() ? native::BootApplication()->GetCity() : nullptr, gDriver.get(), static_cast<gz::cIGZDrawContext*>(gGraphics->Primary()->GetDrawContext()), kW, kH);
        }, [] { if (gAppWindow) gAppWindow->CreateRegionScreen(); }});
        screen.loadCity = [app](const std::string& path) { return native::LoadCityFile(*app, path); };
        native::InstallRegionScreenHost(screen);
        native::InstallRegionExtras(gDriver.get(), []() -> std::uint8_t* { return native::BootApplication() ? native::BootApplication()->GetPreferences() : nullptr; });
        if (std::uint8_t* preferences = app->GetPreferences()) {   // the game's defaults: clouds over the region map, scrolling at the screen edges
            preferences[sc4::kPrefsCloudsVisible] = 1;
            preferences[sc4::kPrefsEdgeScrollEnabled] = 1;
        }
        cISC4Nation* nation = native::CreateNation();
        app->SetNation(nation);
        std::printf("nation: regions dir \"%s\"\n", cSC4Nation::msRegionDirectory.ToChar());
        std::printf("nation: %u regions, active %d\n", nation->GetRegionCount(), nation->GetActiveRegionIndex());
        if (std::getenv("SC4_DEBUG")) {
            for (std::uint32_t i = 0; i < nation->GetRegionCount(); ++i) {
                cISC4Region* region = nation->GetRegion(i);
                RegionCityList cities;
                region->Init();
                region->GetAllCities(cities);
                std::vector<cISC4Region::cLocation> locations;
                region->GetCityLocations(locations);
                std::printf("  region %u locations=%zu dir=%s\n", i, locations.size(), region->GetDirectoryName()->ToChar());
                std::printf("  region %u \"%s\": %zu cities\n", i, region->GetDirectoryName() ? region->GetDirectoryName()->ToChar() : "?", cities.size());
            }
        }
    }
    if (gAppHost) gAppHost->SetMessageServer(SL::spMS2);
    gMgr->GetMainWindow()->SetBufferToDrawToRecursive();
    gMgr->GetMainWindow()->InvalidateSelf();
    return ok;
}

void WritePng(const std::string& path) {
    std::vector<std::uint8_t> bgra;
    std::uint32_t w = 0, h = 0;
    if (!gDriver->ReadBack(bgra, w, h)) { std::printf("read-back failed\n"); return; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(bgra.data(), w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(d, img, nullptr); CGImageDestinationFinalize(d);
    CFRelease(d); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    std::printf("wrote %s\n", path.c_str());
}

}  // namespace

@interface GameView : NSView
@end

@implementation GameView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (NSPoint)gz:(NSEvent*)e { NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; return NSMakePoint(p.x * kW / self.bounds.size.width, p.y * kH / self.bounds.size.height); }
- (std::uint32_t)flags:(NSEvent*)e { return (e.modifierFlags & NSEventModifierFlagShift ? 4 : 0) | (e.modifierFlags & NSEventModifierFlagControl ? 8 : 0) | (([NSEvent pressedMouseButtons] & 1) ? 0x10 : 0); }   // 0x10: the left button is down (the drag tools end when it is not)
- (void)post:(std::uint32_t)type e:(NSEvent*)e { NSPoint p = [self gz:e]; gMgr->GZWinMsgPost_Mouse(type, int(p.x), int(p.y), [self flags:e]); }
- (void)mouseMoved:(NSEvent*)e { [self post:kGZMsgMouseMove e:e]; }
- (void)mouseDragged:(NSEvent*)e { [self post:kGZMsgMouseMove e:e]; }
- (void)mouseDown:(NSEvent*)e { [self post:kGZMsgMouseDownL e:e]; }
- (void)mouseUp:(NSEvent*)e { [self post:kGZMsgMouseUpL e:e]; }
- (void)rightMouseDown:(NSEvent*)e { [self post:kGZMsgMouseDownR e:e]; }
- (void)rightMouseUp:(NSEvent*)e { [self post:kGZMsgMouseUpR e:e]; }
// The city view's keys: what the game's key accelerator (the keyboard shortcut table, not loaded natively yet) turns
// into cSC4View3DWin commands - arrows scroll, +/- and Page Up/Down zoom, comma/period rotate.
static bool CityViewKey(NSEvent* e, bool down) {
    sc4::cSC4View3DWin* view = gAppWindow ? gAppWindow->View3D() : nullptr;
    if (!view) {   // the region screen: the arrow keys scroll the map
        cIGZWin* screen = gAppWindow ? gAppWindow->CurrentScreen() : nullptr;
        if (!screen || screen->GetID() != 0xea659793) return false;
        std::uint32_t command = 0;
        switch (e.keyCode) {
            case 123: command = down ? sc4::kCmdRegionScrollLeft : sc4::kCmdRegionScrollLeftStop; break;
            case 124: command = down ? sc4::kCmdRegionScrollRight : sc4::kCmdRegionScrollRightStop; break;
            case 126: command = down ? sc4::kCmdRegionScrollUp : sc4::kCmdRegionScrollUpStop; break;
            case 125: command = down ? sc4::kCmdRegionScrollDown : sc4::kCmdRegionScrollDownStop; break;
            default: return false;
        }
        if (!(down && e.isARepeat)) static_cast<sc4::cSC4WinRegionScreen*>(screen)->ProcessCommand(command, nullptr, nullptr);
        return true;
    }
    std::uint32_t command = 0;
    switch (e.keyCode) {
        case 123: command = down ? 0x6a935cd8 : 0x2a94826b; break;   // left
        case 124: command = down ? 0x6a935ce0 : 0x2a94826e; break;   // right
        case 126: command = down ? 0x6a935cdd : 0x2a948272; break;   // up
        case 125: command = down ? 0x6a935ce2 : 0x2a948275; break;   // down
        case 24: case 69: case 116: command = down ? 0x6a935cb7 : 0; break;   // = / keypad + / Page Up: zoom in
        case 27: case 78: case 121: command = down ? 0x6a935cba : 0; break;   // - / keypad - / Page Down: zoom out
        case 43: command = down ? 0x6a935cc9 : 0; break;                      // , rotate left
        case 47: command = down ? 0x6a935ccc : 0; break;                      // . rotate right
        default: return false;
    }
    if (command && !(down && e.isARepeat)) view->ProcessViewCommand(command);
    return true;
}
- (void)keyDown:(NSEvent*)e { if (!CityViewKey(e, true)) gMgr->GZWinMsgPost_Keyboard(kGZMsgKeyDown, e.keyCode, 0); }
- (void)keyUp:(NSEvent*)e { if (!CityViewKey(e, false)) gMgr->GZWinMsgPost_Keyboard(kGZMsgKeyUp, e.keyCode, 0); }
- (void)rightMouseDragged:(NSEvent*)e { [self post:kGZMsgMouseMove e:e]; }
// The wheel: one GZ notch (120) per accumulated line of scrolling, posted as a Windows wParam (delta high, keys low).
- (void)scrollWheel:(NSEvent*)e {
    static double accumulated = 0;
    accumulated += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
    if (std::fabs(accumulated) < 1.0) return;
    const int delta = accumulated > 0 ? 120 : -120;
    accumulated = 0;
    NSPoint p = [self gz:e];
    gMgr->GZWinMsgPost_Mouse(kGZMsgMouseWheel, int(p.x), int(p.y), (std::uint32_t(delta) << 16) | [self flags:e]);
}
- (void)updateTrackingAreas {
    for (NSTrackingArea* a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil]];
}
@end

// "token:frames,..." city view input (the SC4_INPUT variable and the scenario `input` command).
void RunInputScript(const std::string& all) {
    std::size_t at = 0;
    while (at < all.size()) {
        std::size_t end = all.find(',', at);
        if (end == std::string::npos) end = all.size();
        const std::string token = all.substr(at, end - at);
        at = end + 1;
        const std::size_t colon = token.find(':');
        const std::string name = token.substr(0, colon);
        const int hold = colon == std::string::npos ? 30 : std::atoi(token.c_str() + colon + 1);
        sc4::cSC4View3DWin* v = gAppWindow ? gAppWindow->View3D() : nullptr;
        static const std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> kCommands = {
            {"left", {0x6a935cd8, 0x2a94826b}}, {"right", {0x6a935ce0, 0x2a94826e}}, {"up", {0x6a935cdd, 0x2a948272}},
            {"down", {0x6a935ce2, 0x2a948275}}, {"zin", {0x6a935cb7, 0}}, {"zout", {0x6a935cba, 0}},
            {"rl", {0x6a935cc9, 0}}, {"rr", {0x6a935ccc, 0}}};
        if (name.rfind("sfx@", 0) == 0) {   // sfx@HEX: a game sound id
            native::PlayGameSound(static_cast<std::uint32_t>(std::strtoul(name.c_str() + 4, nullptr, 16)));
        } else if (name.rfind("panel@", 0) == 0 && v) {   // panel@HEX: ShowPanel(index)
            v->ShowMayorSubPanel(static_cast<std::uint32_t>(std::strtoul(name.c_str() + 6, nullptr, 16)));
        } else if (name.rfind("zone", 0) == 0 && name.size() > 4 && v) {   // zoneN: the zoning tool for zone type N
            v->InvokeZoningTool(static_cast<std::uint32_t>(std::atoi(name.c_str() + 4)));
        } else if (name.rfind("drag@", 0) == 0) {   // drag@X1xY1xX2xY2: a left drag
            int a = 0, b = 0, c = 0, d = 0;
            std::sscanf(name.c_str() + 5, "%dx%dx%dx%d", &a, &b, &c, &d);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, a, b, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, a, b, 0);
            for (int step = 1; step <= 8; ++step) {
                gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, a + (c - a) * step / 8, b + (d - b) * step / 8, 0x10);
                Frame();
            }
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, c, d, 0);
        } else if (name.rfind("click@", 0) == 0) {   // click@XxY: one left click at a window point
            int cx = 0, cy = 0;
            std::sscanf(name.c_str() + 6, "%dx%d", &cx, &cy);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, cx, cy, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, cx, cy, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, cx, cy, 0);
        } else if (name == "wheel+" || name == "wheel-")
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseWheel, kW / 2, kH / 2, std::uint32_t(name == "wheel+" ? 120 : -120) << 16);
        else if (auto it = kCommands.find(name); it != kCommands.end() && v)
            v->ProcessViewCommand(it->second.first);
        for (int i = 0; i < hold; ++i) { Frame(); [NSThread sleepForTimeInterval:0.004]; }
        if (auto it = kCommands.find(name); it != kCommands.end() && v && it->second.second) v->ProcessViewCommand(it->second.second);
        std::printf("input: %s for %d frames\n", name.c_str(), hold);
    }
}

// A synthetic double click (the SC4_CLICK variable and the scenario `click` command).
void DoubleClick(int cx, int cy) {
    gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, cx, cy, 0);
    // One frame between the two clicks (the first one selects, the second opens): the second must land inside the double-click time,
    // which ten frames of a slow (ASan) build exceeded once the timers ran.
    for (int k = 0; k < 2; ++k) {
        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, cx, cy, 0);
        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, cx, cy, 0);
        const int frames = k == 0 ? 1 : 10;
        for (int i = 0; i < frames; ++i) { Frame(); [NSThread sleepForTimeInterval:0.004]; }
    }
}


bool OnRegionScreen() { return gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793; }

// A scenario (scenarios/*.txt): one command per line, `#` starts a comment. The run is repeatable and needs no window.
//   frames N              run N frames           wait MS  run frames for MS ms of real time      move X Y  the cursor
//                                                wait-region [N]   run until the region screen shows (at most N frames, default 3000)
//   wait-city [N]         run until a city view exists                  click X Y        a synthetic double click   tap X Y  a single click
//   input TOKENS          the SC4_INPUT tokens (left:20,zin:30,zone1,...)  shot PATH      write the frame as a PNG
//   expect-region / expect-city   fail the run (exit 5) unless that screen is up       quit    stop here
// Returns 0 when every command ran and every expectation held.
int RunScenario(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::printf("scenario: cannot open %s\n", path.c_str()); return 4; }
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        std::string command;
        if (!(words >> command)) continue;
        std::printf("scenario %d: %s\n", number, line.c_str());
        if (command == "frames") {
            int n = 0; words >> n;
            for (int i = 0; i < n; ++i) { Frame(); [NSThread sleepForTimeInterval:0.004]; }
        } else if (command == "tap") {   // a single click (toolbar buttons toggle: a double click turns them on and off again)
            int x = 0, y = 0; words >> x >> y;
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, x, y, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, x, y, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, x, y, 0);
            for (int i = 0; i < 5; ++i) { Frame(); [NSThread sleepForTimeInterval:0.004]; }
        } else if (command == "move") {   // the cursor: a scenario starts with it at (0, 0), where the region screen edge-scrolls
            int x = 0, y = 0; words >> x >> y;
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, x, y, 0);
            Frame();
        } else if (command == "wait") {   // run frames until MS milliseconds of real time passed (the game's eases and timers run on time)
            int ms = 0; words >> ms;
            const double end = [[NSDate date] timeIntervalSince1970] + ms / 1000.0;
            while ([[NSDate date] timeIntervalSince1970] < end) { Frame(); [NSThread sleepForTimeInterval:0.004]; }
        } else if (command == "wait-region" || command == "wait-city") {
            int limit = 3000; words >> limit;
            const bool region = command == "wait-region";
            int i = 0;
            for (; i < limit; ++i) {
                if (region ? OnRegionScreen() : (gAppWindow && gAppWindow->View3D() != nullptr)) break;
                Frame(); [NSThread sleepForTimeInterval:0.004];
            }
            if (i >= limit) { std::printf("scenario %d: %s gave up after %d frames\n", number, command.c_str(), limit); return 5; }
            std::printf("scenario %d: reached after %d frames\n", number, i);
        } else if (command == "click") {
            int x = 0, y = 0; words >> x >> y;
            DoubleClick(x, y);
        } else if (command == "input") {
            std::string tokens; words >> tokens;
            RunInputScript(tokens);
        } else if (command == "shot") {
            std::string target; words >> target;
            WritePng(target);
        } else if (command == "expect-region") {
            if (!OnRegionScreen()) { std::printf("scenario %d: expected the region screen\n", number); return 5; }
        } else if (command == "expect-city") {
            if (!(gAppWindow && gAppWindow->View3D())) { std::printf("scenario %d: expected a city view\n", number); return 5; }
        } else if (command == "where") {   // where HEXID: the screen rectangle of the first window with that id (finding a button to tap)
            std::string hex;
            words >> hex;
            const std::uint32_t id = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
            if (cIGZWin* w = gMgr->GetMainWindow()->GetChildWindowFromIDRecursive(id)) {
                cRZRect a = w->GetArea();
                int x = a.left, y = a.top;
                for (cIGZWin* p = w->GetParentWin(); p; p = p->GetParentWin()) { const cRZRect pa = p->GetArea(); x += pa.left; y += pa.top; }
                std::printf("where %08x: %d,%d %dx%d visible=%d centre %d,%d\n", id, x, y, a.right - a.left, a.bottom - a.top, w->IsVisible(),
                            x + (a.right - a.left) / 2, y + (a.bottom - a.top) / 2);
            } else {
                std::printf("where %08x: not found\n", id);
            }
        } else if (command == "quit") {
            return 0;
        } else {
            std::printf("scenario %d: unknown command %s\n", number, command.c_str());
            return 4;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The stub SDL clock (PumpTimers refreshes it every frame) must be valid before the boot: a cRZTimer restarted while it reads 0
    // counts as stopped and never advances (the animation tick manager's timer did).
    g_ticks = StubTicksNow();
    std::string shot;
    std::string scenario;   // --script FILE
    int frames = -1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--script") && i + 1 < argc) scenario = argv[++i];
        else if (!std::strncmp(argv[i], "--threads=", 10)) {
            // The reference mode: everything runs on the main thread. More threads are the performance phase (after M6).
            if (std::atoi(argv[i] + 10) != 1) { std::fprintf(stderr, "--threads=%s: only --threads=1 exists yet\n", argv[i] + 10); return 1; }
        }
    }
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        NSWindow* window = nil;
        CAMetalLayer* layer = nil;
        if (shot.empty() && scenario.empty()) {
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, kW, kH)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
            window.title = @"SimCity 4";
            // ARC owns the window: AppKit's default for a window made in code (release when closed) freed it a second time when the
            // window closed, and the autorelease pool's release then hit the dead object (CRASH-3).
            window.releasedWhenClosed = NO;
            GameView* view = [[GameView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
            view.wantsLayer = YES;
            layer = (CAMetalLayer*)view.layer;
            layer.device = MTLCreateSystemDefaultDevice();
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.drawableSize = CGSizeMake(kW, kH);
            layer.framebufferOnly = NO;   // the driver blits its frame target into the drawable
            window.contentView = view;
            window.acceptsMouseMovedEvents = YES;
            [window makeFirstResponder:view];
            [window makeKeyAndOrderFront:nil];
            [app activateIgnoringOtherApps:YES];
            [app finishLaunching];
        }
        gDriver = gd::cMetalGDriver::Create((__bridge void*)layer, kW, kH);
        if (!gDriver) { std::printf("no Metal device\n"); return 1; }
        Boot();
        if ((shot.empty() && scenario.empty()) || std::getenv("SC4_AUDIO")) StartGameAudio();
        // A modal dialog (RunModalLoop) keeps the windows painting: interactively the Cocoa events go on being dispatched; a scenario
        // shows the dialog for 30 frames, writes work/shots/modal<N>.png and cancels it (the scenario cannot press its buttons).
        // The simulation does not tick under a dialog (the original holds a game pause around every one).
        native::SetActiveWinMgr(gMgr.get());
        gMgr->modalDialogs = shot.empty();
        gMgr->modalPump = [app, interactive = scenario.empty() && shot.empty()] {
            static int frame = 0, dialog = 0;
            @autoreleasepool {
                if (interactive) {
                    NSEvent* e;
                    while ((e = [app nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.008] inMode:NSDefaultRunLoopMode dequeue:YES]))
                        [app sendEvent:e];
                } else {
                    [NSThread sleepForTimeInterval:0.004];
                }
                PumpTimers();
                const bool regionScreen = gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793;
                if (regionScreen && native::DrawRegionLayers()) InvalidateTree(gMgr->GetMainWindow());
                if (gAppWindow && gAppWindow->View3D()) InvalidateTree(gMgr->GetMainWindow());
                gMgr->Pump();
                gMgr->Plot();
                gGraphics->Services().Tick();
                gDriver->Present();
            }
            if (!interactive && ++frame >= 30) {
                frame = 0;
                WritePng("work/shots/modal" + std::to_string(dialog++) + ".png");
                gMgr->PostWinMessage(gz::kWinMgrMsgEndModal, 0, 0x80000001u, 0x80000001u);
            }
        };
        if (!native::VerifyRuntimeWiring(gDriver.get(), gAudio != nullptr)) { std::fprintf(stderr, "boot halted: a required dependency is not wired\n"); return 3; }
        if (!scenario.empty()) return RunScenario(scenario);
        if (const char* cityPath = std::getenv("SC4_LOADCITY")) {   // development: load one city file straight after boot and exit
            cSC4App* loadApp = native::BootApplication();
            const bool loaded = loadApp && native::LoadCityFile(*loadApp, cityPath);
            return loaded ? 0 : 2;
        }

        if (!shot.empty()) {
            const int n = frames > 0 ? frames : 120;
            for (int i = 0; i < n; ++i) { Frame(); [NSThread sleepForTimeInterval:0.004]; }
            if (const char* click = std::getenv("SC4_CLICK")) {   // "x,y": a synthetic double click after the frames
                int cx = 0, cy = 0;
                std::sscanf(click, "%d,%d", &cx, &cy);
                DoubleClick(cx, cy);
            }
            if (const char* script = std::getenv("SC4_INPUT")) RunInputScript(script);
            if (std::getenv("SC4_DEBUG")) {
                cIGZWin* desktop = gMgr->GetMainWindow();
                if (cIGZWin* w = desktop->GetChildWindowFromIDRecursive(0xaaa9c9d9)) {
                    const cRZRect a = w->GetArea();
                    std::printf("debug: splash script window found: area %d,%d,%d,%d visible=%d children=%u\n", a.left, a.top, a.right, a.bottom, w->IsVisible(), w->GetChildCount());
                } else std::printf("debug: splash script window NOT found\n");
                if (std::getenv("SC4_DUMP")) {
                    int i = 0;
                    for (native::Buffer* b : native::Buffer::All()) {
                        if (b->OnScreen()) continue;
                        cGZBuffer* px = b->Pixels();
                        std::printf("debug: private buffer %d: %ux%u\n", i, px->Width(), px->Height());
                        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                        CGContextRef cg = CGBitmapContextCreate(px->GetColorSurfaceBits(), px->Width(), px->Height(), 8, px->GetColorSurfaceStride(), cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                        CGImageRef img = CGBitmapContextCreateImage(cg);
                        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithFormat:@"work/shots/pbuf%d.png", i++]];
                        CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
                        CGImageDestinationAddImage(d, img, nullptr); CGImageDestinationFinalize(d);
                        CFRelease(d); CGImageRelease(img); CGContextRelease(cg); CGColorSpaceRelease(cs);
                    }
                }
                std::printf("debug: spMS2=%p timer id=%d interval=%u cb=%p ticks=%u\n", (void*)SL::spMS2, int(g_activeTimerId), unsigned(g_timerInterval), (void*)g_timerCallback, unsigned(g_ticks));
                if (std::getenv("SC4_TREE")) DumpTree(gMgr->GetMainWindow(), 0, std::atoi(std::getenv("SC4_TREE")));
                std::printf("debug: desktop children=%u visible=%d draws=%u\n", desktop->GetChildCount(), desktop->IsVisible(), gDriver->DrawCalls());
                for (auto& kv : gDriver->UnsupportedCalls()) std::printf("debug: unsupported %s x%d\n", kv.first.c_str(), kv.second);
                if (auto* w = native::BootApplication()) std::printf("debug: app tick ok, window=%p\n", (void*)w->GetMainWindow());
            }
            WritePng(shot);
            return 0;
        }
        int n = 0;
        for (;;) {
            @autoreleasepool {
                NSEvent* e;
                while ((e = [app nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.008] inMode:NSDefaultRunLoopMode dequeue:YES]))
                    [app sendEvent:e];
                if (!window.visible) break;
                Frame();
                gDriver->Present();
                if (frames > 0 && ++n >= frames) break;
            }
        }
    }
    return 0;
}
