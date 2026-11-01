// sc4_game on Linux — game_main.mm's twin: the recovered cSC4App boots against the game data; its application window
// (cWinSC4App) creates the splash and region screens from the game's own UI scripts; every window draws through the game's
// own draw context into cIGZGDriver, here cGLGDriver (OpenGL 3.3 core) in an SDL3 window. No audio output yet.
//
//   sc4_game                      open a window
//   sc4_game --shot out.png [--frames N]   run N frames (default 120) in a hidden window and write the frame as a PNG
//   sc4_game --script scenarios/first_city.txt   run a scenario in a hidden window (see RunScenario); exit code 0 = it held
//
// The game data: $SC4_GAME_CONTENTS (a folder with GameData/ inside), else game/ in the repository (platform/GameDataPath).
#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <png.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "platform/GameDataPath.h"
#include "platform/PointerState.h"
#include "sc4/RegionDialogs.h"
#include "app/NativeBoot.h"
#include "app/NativeCity.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeCityView.h"
#include "app/NativeFonts.h"
#include "app/NativeGraphics.h"
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
#include "gz/WinTextEdit.h"
#include "gz/WinStyle.h"
#include "render/gd/GLGDriver.h"
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
#include "app/NativeZoneTool.h"
#include "app/NativeRegionExtras.h"
#include "app/NativeAudio.h"
#include "app/NativeAudioDirector.h"
#include "app/GameAudio.h"

namespace linuxapp {   // StubClock.cpp: the game's stub SDL clock and timer
double NowMs();
void UpdateStubTicks();
void PumpTimers();
}  // namespace linuxapp

namespace {

using linuxapp::NowMs;
using linuxapp::PumpTimers;

// The game's (logical) resolution: the UI lays out in it and every rectangle it draws is in it. The frame target is the
// window's pixel size; the driver scales the game's rectangles onto it (the UI scale = pixels / logical).
int gW = 1024, gH = 768;

SDL_Window* gWindow = nullptr;
std::unique_ptr<gd::cGLGDriver> gDriver;
std::unique_ptr<native::Graphics> gGraphics;
std::unique_ptr<native::WinMgr> gMgr;

void Nap() { std::this_thread::sleep_for(std::chrono::milliseconds(4)); }

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
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(GameFontsDirectory(), error)) {
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".mxf") continue;
        FontFile f;
        f.path = entry.path().string();
        std::unique_ptr<IFontFace> face = gFontHost->NewFace(f.path);
        if (!face) continue;
        f.family = face->FamilyName();
        f.bold = (face->StyleFlags() & kFontStyleFlagBold) != 0;
        f.italic = (face->StyleFlags() & kFontStyleFlagItalic) != 0;
        files.push_back(f);
    }
    std::sort(files.begin(), files.end(), [](const FontFile& a, const FontFile& b) { return a.path < b.path; });   // directory order varies
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
std::map<std::string, std::uint32_t> gStyleNames;   // "GenBodyLight" -> its guid (lower case: scripts vary the case)

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
    std::string name = line.substr(0, at);
    name.erase(0, name.find_first_not_of(" \t`"));
    name.erase(name.find_last_not_of(" \t") + 1);
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!name.empty()) gStyleNames[name] = guid;
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
    for (const char* fallback : {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                 "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"}) {
        if (!path.empty()) break;
        std::error_code error;
        if (std::filesystem::exists(fallback, error)) path = fallback;
    }
    if (path.empty()) return false;
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
    gz::IWinFont* FontByName(const char* name) override {
        const std::uint32_t id = StyleId(name);
        return id ? Font(id) : nullptr;
    }
    std::uint32_t StyleId(const char* name) override {
        if (!gFont || !name) return 0;
        LoadFontStyles();
        std::string key(name);
        for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
        auto it = gStyleNames.find(key);
        return it == gStyleNames.end() ? 0 : it->second;
    }
} gFontSource;

// RZ::WCM()'s font styles as the edit controls ask for them (cGZWinTextEdit, cGZWinLineInput): the same fonts as the font
// source, as cIGZFont. Without it the edit boxes had no font and crashed painting.
struct StyleManager final : gz::IWinStyleManager {
    cIGZUnknown* GetFontStyle(std::uint32_t style) override {
        gz::IWinFont* font = gFontSource.Font(style);
        return font ? font->AsFont() : nullptr;
    }
} gStyleManager;

// ---- images: PNG through libpng, uncompressed BMP by hand -> a CPU buffer with straight alpha (B, G, R, A in memory) ------
cGZBuffer* NewBuffer(std::uint32_t w, std::uint32_t h) {
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    buffer->Init(w, h, cGZBufferColorType{kBufferColorARGB8888, 32});
    return buffer;
}

cGZBuffer* DecodeBmpBytes(const std::vector<std::uint8_t>& b) {
    auto u16 = [&](std::size_t o) { return std::uint32_t(b[o] | b[o + 1] << 8); };
    auto u32 = [&](std::size_t o) { return std::uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | std::uint32_t(b[o + 3]) << 24); };
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return nullptr;
    const std::uint32_t offset = u32(10), w = u32(18), bpp = u16(28), compression = u32(30);
    const std::int32_t rawH = std::int32_t(u32(22));
    const std::uint32_t h = std::uint32_t(rawH < 0 ? -rawH : rawH);
    if ((bpp != 24 && bpp != 32) || compression != 0 || w == 0 || h == 0 || w > 8192 || h > 8192) return nullptr;
    const std::size_t rowBytes = (std::size_t(w) * (bpp / 8) + 3) & ~std::size_t(3);
    if (offset + rowBytes * h > b.size()) return nullptr;
    cGZBuffer* buffer = NewBuffer(w, h);
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* src = &b[offset + rowBytes * (rawH < 0 ? y : h - 1 - y)];
        std::uint8_t* dst = bits + std::size_t(y) * stride;
        for (std::uint32_t x = 0; x < w; ++x, src += bpp / 8, dst += 4) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xff;
        }
    }
    buffer->SetTransparency(buffer->ConvertRGBValueToNative(0xff, 0, 0xff));
    return buffer;
}

// The application's identity on the desktop. SDL makes the identifier the Wayland app_id and the X11 WM_CLASS; the taskbar
// matches it to scripts/install_desktop_entry.sh's desktop entry of the same name, whose Icon= it shows.
constexpr const char* kAppId = "io.github.codingncaffeine.OpenCityBuilder";
#include "app_icon.inc"   // build/generated: art/logo-256.png

// The window's own icon (the title bar, X11 taskbars, and Wayland compositors with xdg-toplevel-icon).
void SetWindowIconFromLogo(SDL_Window* window) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, app_icon_png, app_icon_png_len)) return;
    image.format = PNG_FORMAT_RGBA;
    SDL_Surface* surface = SDL_CreateSurface(int(image.width), int(image.height), SDL_PIXELFORMAT_RGBA32);
    if (!surface) { png_image_free(&image); return; }
    if (png_image_finish_read(&image, nullptr, surface->pixels, png_int_32(surface->pitch), nullptr)) SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
}

cGZBuffer* DecodePngBytes(const std::vector<uint8_t>& png) {
    if (png.size() >= 2 && png[0] == 'B' && png[1] == 'M') return DecodeBmpBytes(png);
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, png.data(), png.size())) return nullptr;
    image.format = PNG_FORMAT_BGRA;   // straight (not premultiplied) alpha
    cGZBuffer* buffer = NewBuffer(image.width, image.height);
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    if (!png_image_finish_read(&image, nullptr, bits, png_int_32(stride), nullptr)) {
        png_image_free(&image);
        buffer->Release();
        return nullptr;
    }
    // The UI art marks its transparent parts in opaque magenta (the city toolbar's tiled background, for one): every image
    // comes with that colour key, as the shipped game shows them.
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
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        *out = DecodePngBytes(bytes);
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
    std::printf("%*s id=%08x vis=%d area=%d,%d,%d,%d kids=%u caption=\"%s\"\n", depth * 2, "", window->GetID(), window->IsVisible(), a.left, a.top, a.right, a.bottom, window->GetChildCount(), window->GetCaption().ToChar());
    if (depth >= maxDepth) return;
    struct Ctx { int depth, max; };
    Ctx ctx{depth + 1, maxDepth};
    window->EnumChildren(0x22ba0121, [](cIGZWin*, std::uint32_t, void* child, void* c) { auto* x = static_cast<Ctx*>(c); DumpTree(static_cast<cIGZWin*>(child), x->depth, x->max); return true; }, &ctx);
}

// ---- the frame loop ---------------------------------------------------------------------------------------------------------
native::HostWinSC4App* gAppHost = nullptr;
cGZMessageServer2* gMessages2 = nullptr;   // ticked by the frame loop, as the framework ticks the service in the game
sc4::cWinSC4App* gAppWindow = nullptr;

// What the splash screen reaches outside itself (SPLASH-1): the game's script window builder, the message server, the driver
// clear, and the application window's screen swap.
// The edit boxes' system services: the clipboard and the key state from SDL. The control lowercases A-Z unless shift or caps
// lock reads as down (the original saw upper-case virtual-key characters), so a missing host typed every name in lower case.
struct TextEditHost final : gz::IWinTextEditHost {
    bool ClipboardGetText(cIGZString& out) override {
        if (!SDL_HasClipboardText()) return false;
        char* text = SDL_GetClipboardText();
        if (!text) return false;
        out.AssignFromCString(text);
        SDL_free(text);
        return true;
    }
    void ClipboardSetText(const cIGZString& text) override { SDL_SetClipboardText(text.ToChar()); }
    std::uint32_t ModifierState() override { return std::uint32_t(SDL_GetModState()); }   // SDL_KMOD_LCTRL | RCTRL = 0xc0
    bool CapsLockOn() override { return (SDL_GetModState() & SDL_KMOD_CAPS) != 0; }
    bool ShiftDown() override { return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0; }
} gTextEditHost;

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

// The game's sound: Sound.dat opened through the recovered persistence layer, the radio station that follows the screen and
// the window buttons' sounds (the interactive run only, or SC4_AUDIO=1; shots and scenarios stay silent).
GameAudio* gAudio = nullptr;
bool gQuit = false;   // ends the main loop (the window's close button, the game's own quit)
void StartGameAudio() {
    static GameAudio audio;
    std::string error;
    const std::string dat = platform::GameDataDir() + "Sound.dat";
    if (!audio.Open(dat, error)) { std::printf("audio: %s\n", error.c_str()); return; }
    gAudio = &audio;
    native::SetGameAudio(&audio);
    if (cSC4App* app = native::BootApplication()) native::ApplyAudioPreferences(app->GetPreferences());   // the saved volumes and songs
    if (std::getenv("SC4_AUDIODIRECTORS") && native::BootFramework())
        native::StartAudioDirectors(*native::BootFramework(), dat.substr(0, dat.rfind('/') + 1));
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

// Where the frame lands in the window (window points): the frame keeps its aspect ratio, bars fill the rest.
struct Placement { float x = 0, y = 0, w = 1, h = 1; };
Placement FramePlacement() {
    int ww = gW, wh = gH;
    if (gWindow) SDL_GetWindowSize(gWindow, &ww, &wh);
    const float scale = std::min(float(ww) / float(gW), float(wh) / float(gH));
    Placement p;
    p.w = float(gW) * scale;
    p.h = float(gH) * scale;
    p.x = (float(ww) - p.w) / 2;
    p.y = (float(wh) - p.h) / 2;
    return p;
}

void Present() {
    if (!gWindow) return;
    int pw = gW, ph = gH, ww = gW, wh = gH;
    SDL_GetWindowSizeInPixels(gWindow, &pw, &ph);
    SDL_GetWindowSize(gWindow, &ww, &wh);
    const Placement p = FramePlacement();
    const float density = float(pw) / float(std::max(1, ww));   // window points -> pixels
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    gDriver->Present(int(std::lround(p.x * density)), int(std::lround(float(ph) - (p.y + p.h) * density)),
                     int(std::lround(p.w * density)), int(std::lround(p.h * density)));
    SDL_GL_SwapWindow(gWindow);
}

bool Boot() {
    native::SetPngDecoder(&DecodePngBytes);
    SetSLBufferHost(&gBufferHost);
    nSC4UI::SetHost(&gUiHost);
    gGraphics.reset(new native::Graphics(gDriver.get(), gW, gH));
    gMgr.reset(new native::WinMgr());
    gMgr->AddRef();
    gMgr->Init();
    gMgr->GetMainWindow()->SetArea(0, 0, gW, gH);
    if (!InitFont()) std::printf("font: FAILED to initialise\n");
    gz::SetWinFontSource(&gFontSource);
    gz::SetWinStyleManager(&gStyleManager);
    native::InstallButtonSounds();   // the window buttons' host: caption fonts (and sounds, once there is audio output)

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
        // The app's questions (quit, save) are scripted dialogs answered by a button id, and its quit ends the main loop.
        // The city load installs the rest of these hooks; these two are needed from the region screen on.
        app->CityHooks().scriptedModal = [](std::uint32_t instance, std::uint32_t window) {
            return static_cast<int>(sc4::SC4RunScriptedButtonDialog(gMgr->GetMainWindow(), instance, window));
        };
        app->CityHooks().quitFramework = [] { gQuit = true; };
        native::RegionScreenServices screen;
        screen.app = app;
        screen.graphics = gGraphics.get();
        screen.createWindowFromScript = &native::CreateWindowFromScript;
        screen.loadBitmap = [](std::uint32_t group, std::uint32_t instance) { return native::LoadBitmapResource(group, instance); };
        native::SetCityScreenCallbacks({[] { if (gAppWindow) gAppWindow->DestroyCurrentScreen(); }, [] {
            if (!gAppWindow || !gAppWindow->CreateCityScreen()) return;
            if (sc4::cSC4View3DWin* view = gAppWindow->View3D()) native::AttachCityRenderStack(*view, native::BootApplication() ? native::BootApplication()->GetCity() : nullptr, gDriver.get(), static_cast<gz::cIGZDrawContext*>(gGraphics->Primary()->GetDrawContext()), gW, gH);
        }});
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
    for (std::size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 0xff;   // the frame's alpha is not part of the picture
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = w;
    image.height = h;
    image.format = PNG_FORMAT_BGRA;
    if (png_image_write_to_file(&image, path.c_str(), 0, bgra.data(), png_int_32(w * 4), nullptr)) std::printf("wrote %s\n", path.c_str());
    else std::printf("cannot write %s\n", path.c_str());
}

// ---- input ------------------------------------------------------------------------------------------------------------------
std::uint32_t gButtons = 0;

std::uint32_t Flags() {
    const SDL_Keymod mod = SDL_GetModState();
    return ((mod & SDL_KMOD_SHIFT) ? 4u : 0u) | ((mod & SDL_KMOD_CTRL) ? 8u : 0u) | ((gButtons & SDL_BUTTON_LMASK) ? 0x10u : 0u);   // 0x10: the left button is down
}
void PostMouse(std::uint32_t type, float x, float y, std::uint32_t extra = 0) {
    const Placement p = FramePlacement();   // window points -> the game's logical screen
    const int gx = int((x - p.x) * float(gW) / p.w), gy = int((y - p.y) * float(gH) / p.h);
    static const bool trace = std::getenv("SC4_DEBUG") != nullptr;
    if (trace && type != kGZMsgMouseMove) {
        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(gWindow, &ww, &wh);
        SDL_GetWindowSizeInPixels(gWindow, &pw, &ph);
        std::fprintf(stderr, "mouse %#x at window %.0f,%.0f -> game %d,%d (window %dx%d points, %dx%d pixels, frame %.0f,%.0f %.0fx%.0f, game %dx%d)\n",
                     type, x, y, gx, gy, ww, wh, pw, ph, p.x, p.y, p.w, p.h, gW, gH);
    }
    gMgr->GZWinMsgPost_Mouse(type, gx, gy, extra | Flags());
}

// The Windows virtual-key code the game's windows test (VK_ESCAPE 0x1b, VK_RETURN 0x0d, letters and digits as upper-case ASCII).
std::uint32_t VirtualKey(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return std::uint32_t(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return std::uint32_t(k);
    switch (k) {
    case SDLK_ESCAPE: return 0x1b; case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0d; case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09; case SDLK_SPACE: return 0x20; case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23; case SDLK_HOME: return 0x24; case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28; case SDLK_INSERT: return 0x2d; case SDLK_DELETE: return 0x2e;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; case SDLK_LCTRL: case SDLK_RCTRL: return 0x11; case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_KP_PLUS: return 0x6b; case SDLK_KP_MINUS: return 0x6d; case SDLK_EQUALS: return 0xbb; case SDLK_MINUS: return 0xbd;
    case SDLK_COMMA: return 0xbc; case SDLK_PERIOD: return 0xbe;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return std::uint32_t(0x70 + (k - SDLK_F1));
    return 0;
}

// The city view's keys: what the game's key accelerator (the keyboard shortcut table, not loaded natively yet) turns
// into cSC4View3DWin commands - arrows scroll, +/- and Page Up/Down zoom, comma/period rotate.
bool CityViewKey(SDL_Keycode key, bool down, bool repeat) {
    sc4::cSC4View3DWin* view = gAppWindow ? gAppWindow->View3D() : nullptr;
    if (!view) {   // the region screen: the arrow keys scroll the map
        cIGZWin* screen = gAppWindow ? gAppWindow->CurrentScreen() : nullptr;
        if (!screen || screen->GetID() != 0xea659793) return false;
        std::uint32_t command = 0;
        switch (key) {
            case SDLK_LEFT: command = down ? sc4::kCmdRegionScrollLeft : sc4::kCmdRegionScrollLeftStop; break;
            case SDLK_RIGHT: command = down ? sc4::kCmdRegionScrollRight : sc4::kCmdRegionScrollRightStop; break;
            case SDLK_UP: command = down ? sc4::kCmdRegionScrollUp : sc4::kCmdRegionScrollUpStop; break;
            case SDLK_DOWN: command = down ? sc4::kCmdRegionScrollDown : sc4::kCmdRegionScrollDownStop; break;
            default: return false;
        }
        if (!(down && repeat)) static_cast<sc4::cSC4WinRegionScreen*>(screen)->ProcessCommand(command, nullptr, nullptr);
        return true;
    }
    std::uint32_t command = 0;
    switch (key) {
        case SDLK_LEFT: command = down ? 0x6a935cd8 : 0x2a94826b; break;
        case SDLK_RIGHT: command = down ? 0x6a935ce0 : 0x2a94826e; break;
        case SDLK_UP: command = down ? 0x6a935cdd : 0x2a948272; break;
        case SDLK_DOWN: command = down ? 0x6a935ce2 : 0x2a948275; break;
        case SDLK_EQUALS: case SDLK_KP_PLUS: case SDLK_PAGEUP: command = down ? 0x6a935cb7 : 0; break;     // zoom in
        case SDLK_MINUS: case SDLK_KP_MINUS: case SDLK_PAGEDOWN: command = down ? 0x6a935cba : 0; break;  // zoom out
        case SDLK_COMMA: command = down ? 0x6a935cc9 : 0; break;                                           // rotate left
        case SDLK_PERIOD: command = down ? 0x6a935ccc : 0; break;                                          // rotate right
        default: return false;
    }
    if (command && !(down && repeat)) view->ProcessViewCommand(command);
    return true;
}

double gWheel = 0;

void HandleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: gQuit = true; break;
    case SDL_EVENT_MOUSE_MOTION: gButtons = e.motion.state; PostMouse(kGZMsgMouseMove, e.motion.x, e.motion.y); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const std::uint32_t mask = SDL_BUTTON_MASK(e.button.button);
        gButtons = down ? (gButtons | mask) : (gButtons & ~mask);
        if (e.button.button == SDL_BUTTON_LEFT) PostMouse(down ? kGZMsgMouseDownL : kGZMsgMouseUpL, e.button.x, e.button.y);
        else if (e.button.button == SDL_BUTTON_RIGHT) PostMouse(down ? kGZMsgMouseDownR : kGZMsgMouseUpR, e.button.x, e.button.y);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {   // one GZ notch (120) per line of scrolling, posted as a Windows wParam (delta high, keys low)
        gWheel += e.wheel.y;
        if (std::fabs(gWheel) < 1.0) break;
        const int delta = gWheel > 0 ? 120 : -120;
        gWheel = 0;
        PostMouse(kGZMsgMouseWheel, e.wheel.mouse_x, e.wheel.mouse_y, std::uint32_t(delta) << 16);
        break;
    }
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        if (CityViewKey(e.key.key, down, e.key.repeat)) break;
        if (const std::uint32_t vk = VirtualKey(e.key.key)) gMgr->GZWinMsgPost_Keyboard(down ? kGZMsgKeyDown : kGZMsgKeyUp, vk, 0);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        for (const char* c = e.text.text; *c; ++c)
            if (static_cast<unsigned char>(*c) >= 0x20 && static_cast<unsigned char>(*c) < 0x80) gMgr->GZWinMsgPost_Keyboard(kGZMsgCharacter, std::uint32_t(*c), 0);
        break;
    default: break;
    }
}

void PumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) HandleEvent(e);
}

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
        if (name.rfind("panel@", 0) == 0 && v) {   // panel@HEX: ShowPanel(index)
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
        } else if (name.rfind("press@", 0) == 0 || name.rfind("hold@", 0) == 0 || name.rfind("release@", 0) == 0) {
            // press@XxY (move there, left button down), hold@XxY (move with the button held), release@XxY (button up):
            // a drag that can be photographed half way.
            int px = 0, py = 0;
            std::sscanf(name.c_str() + name.find('@') + 1, "%dx%d", &px, &py);
            if (name[0] == 'p') {
                gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, px, py, 0);
                gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, px, py, 0);
            } else if (name[0] == 'h') {
                gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, px, py, 0x10);
            } else {
                gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, px, py, 0);
            }
        } else if (name.rfind("click@", 0) == 0) {   // click@XxY: one left click at a window point
            int cx = 0, cy = 0;
            std::sscanf(name.c_str() + 6, "%dx%d", &cx, &cy);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, cx, cy, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, cx, cy, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, cx, cy, 0);
        } else if (name == "wheel+" || name == "wheel-")
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseWheel, gW / 2, gH / 2, std::uint32_t(name == "wheel+" ? 120 : -120) << 16);
        else if (auto it = kCommands.find(name); it != kCommands.end() && v)
            v->ProcessViewCommand(it->second.first);
        for (int i = 0; i < hold; ++i) { Frame(); Nap(); }
        if (auto it = kCommands.find(name); it != kCommands.end() && v && it->second.second) v->ProcessViewCommand(it->second.second);
        std::printf("input: %s for %d frames\n", name.c_str(), hold);
    }
}

// A synthetic double click (the SC4_CLICK variable and the scenario `click` command).
void DoubleClick(int cx, int cy) {
    gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, cx, cy, 0);
    // One frame between the two clicks (the first one selects, the second opens): the second must land inside the double-click time.
    for (int k = 0; k < 2; ++k) {
        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, cx, cy, 0);
        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, cx, cy, 0);
        const int frames = k == 0 ? 1 : 10;
        for (int i = 0; i < frames; ++i) { Frame(); Nap(); }
    }
}

bool OnRegionScreen() { return gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793; }

// A scenario (scenarios/*.txt): one command per line, `#` starts a comment. The run is repeatable and needs no visible window.
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
        if (gQuit) { std::printf("scenario %d: the game quit\n", number); return 0; }
        std::printf("scenario %d: %s\n", number, line.c_str());
        if (command == "frames") {
            int n = 0; words >> n;
            for (int i = 0; i < n; ++i) { Frame(); Nap(); }
        } else if (command == "sdlclick") {   // sdlclick X Y: a real SDL left click in window points, through the live event path
            float x = 0, y = 0; words >> x >> y;
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = SDL_GetWindowID(gWindow); e.motion.x = x; e.motion.y = y;
            SDL_PushEvent(&e); PumpEvents(); Frame(); Nap();
            for (const SDL_EventType type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                SDL_Event b{};
                b.type = type; b.button.windowID = SDL_GetWindowID(gWindow); b.button.button = SDL_BUTTON_LEFT;
                b.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN; b.button.clicks = 1; b.button.x = x; b.button.y = y;
                SDL_PushEvent(&b); PumpEvents(); Frame(); Nap();
            }
            for (int i = 0; i < 5; ++i) { PumpEvents(); Frame(); Nap(); }
        } else if (command == "tap") {   // a single click (toolbar buttons toggle: a double click turns them on and off again)
            int x = 0, y = 0; words >> x >> y;
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, x, y, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, x, y, 0);
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, x, y, 0);
            for (int i = 0; i < 5; ++i) { Frame(); Nap(); }
        } else if (command == "move") {   // the cursor: a scenario starts with it at (0, 0), where the region screen edge-scrolls
            int x = 0, y = 0; words >> x >> y;
            gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, x, y, 0);
            Frame();
        } else if (command == "wait") {   // run frames until MS milliseconds of real time passed (the game's eases and timers run on time)
            int ms = 0; words >> ms;
            const double end = NowMs() + ms;
            while (NowMs() < end) { Frame(); Nap(); }
        } else if (command == "wait-region" || command == "wait-city") {
            int limit = 3000; words >> limit;
            const bool region = command == "wait-region";
            int i = 0;
            for (; i < limit; ++i) {
                if (region ? OnRegionScreen() : (gAppWindow && gAppWindow->View3D() != nullptr)) break;
                Frame(); Nap();
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

}  // namespace

namespace platform {
bool RightMouseButtonDown() { return (gButtons & SDL_BUTTON_RMASK) != 0; }
}  // namespace platform

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The stub SDL clock (PumpTimers refreshes it every frame) must be valid before the boot: a cRZTimer restarted while it reads 0
    // counts as stopped and never advances (the animation tick manager's timer did).
    linuxapp::UpdateStubTicks();
    std::string shot;
    std::string scenario;   // --script FILE
    int frames = -1;
    int windowW = 0, windowH = 0;   // --size WxH: the window's size in pixels (default: most of the display; 1024x768 headless)
    double uiScale = -1;            // --scale S: the UI scale (0 or "auto": from the window height); headless default 1
    bool fullscreen = false;        // --fullscreen: borderless, the whole display
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--script") && i + 1 < argc) scenario = argv[++i];
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &windowW, &windowH);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) uiScale = std::strcmp(argv[i + 1], "auto") ? std::atof(argv[i + 1]) : 0.0, ++i;
        else if (!std::strcmp(argv[i], "--fullscreen")) fullscreen = true;
    }
    const bool interactive = shot.empty() && scenario.empty();

    SDL_SetAppMetadata("Open City Builder", nullptr, kAppId);
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // The window's pixel size and the UI scale fix the game's logical resolution (pixels / scale) for the session.
    SDL_Rect usable{0, 0, 1024, 768};
    float contentScale = 1.0f;
    if (const SDL_DisplayID display = SDL_GetPrimaryDisplay()) {
        SDL_GetDisplayUsableBounds(display, &usable);
        contentScale = std::max(1.0f, SDL_GetDisplayContentScale(display));
        if (fullscreen) if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display)) usable = {0, 0, mode->w, mode->h};
    }
    if (windowW <= 0 || windowH <= 0) {
        if (!interactive) { windowW = 1024; windowH = 768; }
        else if (fullscreen) { windowW = usable.w; windowH = usable.h; }
        else { windowW = usable.w * 9 / 10; windowH = usable.h * 9 / 10; }
    }
    if (uiScale < 0 && interactive)   // Graphic Options > Resolution: that many logical lines in the window
        if (const int chosen = native::LoadDisplayHeight(); chosen >= 480) uiScale = std::clamp(double(windowH) / chosen, 0.75, 4.0);
    if (uiScale < 0) uiScale = interactive ? 0 : 1;
    native::SetDisplayGeometry(windowW, windowH, 0);
    if (uiScale == 0) {   // auto: the resolution choice nearest 900 lines (or the desktop's own scaling, when it has one)
        const double target = contentScale > 1.0f ? windowH / contentScale : 900.0;
        int best = windowH;
        for (int i = 0; i < 4; ++i) {
            int w = 0, h = 0;
            if (native::ResolutionChoice(i, w, h) && std::fabs(h - target) < std::fabs(best - target)) best = h;
        }
        uiScale = std::max(1.0, double(windowH) / best);
    }
    gW = std::max(640, int(std::lround(windowW / uiScale)));
    gH = std::max(480, int(std::lround(windowH / uiScale)));
    std::printf("display: window %dx%d pixels, UI scale %.2f -> game resolution %dx%d\n", windowW, windowH, uiScale, gW, gH);
    native::SetGameResolution(gW, gH, usable.w, usable.h);
    native::SetDisplayGeometry(windowW, windowH, gH);
    // The shot and scenario runs draw into a hidden window: the frame target is offscreen anyway, the window only holds the context.
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!interactive) flags |= SDL_WINDOW_HIDDEN;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    gWindow = SDL_CreateWindow("Open City Builder", int(windowW / contentScale), int(windowH / contentScale), flags);
    if (!gWindow) { std::fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    SetWindowIconFromLogo(gWindow);
    SDL_GLContext context = SDL_GL_CreateContext(gWindow);
    if (!context) { std::fprintf(stderr, "OpenGL 3.3 context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    std::printf("gl: %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("game data: %s\n", platform::GameDataDir().c_str());
    if (interactive) SDL_StartTextInput(gWindow);

    gz::SetWinTextEditHost(&gTextEditHost);
    gDriver = gd::cGLGDriver::Create(std::uint32_t(gW), std::uint32_t(gH), std::uint32_t(windowW), std::uint32_t(windowH));
    if (!gDriver) { std::printf("no usable OpenGL driver\n"); return 1; }
    Boot();
    if (interactive || std::getenv("SC4_AUDIO")) StartGameAudio();
    // A modal dialog (RunModalLoop) keeps the windows painting: interactively the events go on being dispatched; a scenario
    // shows the dialog for 30 frames, writes work/shots/modal<N>.png and cancels it (the scenario cannot press its buttons).
    // The simulation does not tick under a dialog (the original holds a game pause around every one).
    native::SetActiveWinMgr(gMgr.get());
    gMgr->modalDialogs = shot.empty();
    gMgr->modalPump = [interactive] {
        static int frame = 0, dialog = 0;
        if (interactive) PumpEvents();
        Nap();
        PumpTimers();
        // the region map under the dialog: re-blitted every frame like Frame() does (the clouds keep drawing over it)
        const bool regionScreen = gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793;
        if (regionScreen && native::DrawRegionLayers()) InvalidateTree(gMgr->GetMainWindow());
        if (gAppWindow && gAppWindow->View3D()) InvalidateTree(gMgr->GetMainWindow());
        gMgr->Pump();
        gMgr->Plot();
        gGraphics->Services().Tick();
        Present();
        // debug: SC4_MODALINPUT="FRAME:X:Y,..." clicks inside the dialog at those frames of it (FRAME:type:TEXT, FRAME:key:VK type); SC4_MODALFRAMES its length (30)
        if (!interactive) {
            if (const char* input = std::getenv("SC4_MODALINPUT")) {
                std::stringstream list(input);
                std::string item;
                while (std::getline(list, item, ',')) {
                    int at = -1, x = 0, y = 0;
                    char text[128] = {};
                    // "FRAME:type:TEXT" types the characters, "FRAME:key:VK" presses and releases one virtual key
                    if (std::sscanf(item.c_str(), "%d:type:%127[^,]", &at, text) == 2) {
                        if (at != frame + 1) continue;
                        for (const char* c = text; *c; ++c) gMgr->GZWinMsgPost_Keyboard(kGZMsgCharacter, std::uint32_t(*c), 0);
                        std::printf("modal input: type \"%s\" at frame %d\n", text, at);
                        continue;
                    }
                    if (std::sscanf(item.c_str(), "%d:key:%d", &at, &x) == 2) {
                        if (at != frame + 1) continue;
                        gMgr->GZWinMsgPost_Keyboard(kGZMsgKeyDown, std::uint32_t(x), 0);
                        gMgr->GZWinMsgPost_Keyboard(kGZMsgKeyUp, std::uint32_t(x), 0);
                        std::printf("modal input: key %d at frame %d\n", x, at);
                        continue;
                    }
                    if (std::sscanf(item.c_str(), "%d:%d:%d", &at, &x, &y) == 3 && at == frame + 1) {
                        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseMove, x, y, 0);
                        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseDownL, x, y, 0);
                        gMgr->GZWinMsgPost_Mouse(kGZMsgMouseUpL, x, y, 0);
                        std::printf("modal input: click %d,%d at frame %d\n", x, y, at);
                    }
                }
            }
        }
        const char* lengthText = std::getenv("SC4_MODALFRAMES");
        const int length = lengthText ? std::max(1, std::atoi(lengthText)) : 30;
        if (!interactive && ++frame >= length) {
            frame = 0;
            if (const char* tree = std::getenv("SC4_MODALTREE"))   // debug: the window tree under the dialog's window id (hex)
                if (cIGZWin* w = gMgr->GetMainWindow()->GetChildWindowFromIDRecursive(std::uint32_t(std::strtoul(tree, nullptr, 16)))) DumpTree(w, 0, 6);
            WritePng("work/shots/modal" + std::to_string(dialog++) + ".png");
            gMgr->PostWinMessage(gz::kWinMgrMsgEndModal, 0, 0x80000001u, 0x80000001u);
        }
    };
    if (const char* dump = std::getenv("SC4_DUMPRES")) {   // debug: "type,group,instance[,file]" writes one resource's bytes
        unsigned type = 0, group = 0, instance = 0;
        char file[256] = "resource.bin";
        std::sscanf(dump, "%x,%x,%x,%255s", &type, &group, &instance, file);
        std::vector<std::uint8_t> bytes;
        if (native::LoadResourceBytes(type, group, instance, bytes)) {
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            std::printf("dump: %zu bytes -> %s\n", bytes.size(), file);
        } else {
            std::printf("dump: %08x %08x %08x not found\n", type, group, instance);
        }
    }
    if (!native::VerifyRuntimeWiring(gDriver.get(), gAudio != nullptr)) { std::fprintf(stderr, "boot halted: a required dependency is not wired\n"); return 3; }
    if (!scenario.empty()) return RunScenario(scenario);

    if (!shot.empty()) {
        const int n = frames > 0 ? frames : 120;
        for (int i = 0; i < n; ++i) { Frame(); Nap(); }
        if (const char* click = std::getenv("SC4_CLICK")) {   // "x,y": a synthetic double click after the frames
            int cx = 0, cy = 0;
            std::sscanf(click, "%d,%d", &cx, &cy);
            DoubleClick(cx, cy);
        }
        if (const char* script = std::getenv("SC4_INPUT")) RunInputScript(script);
        if (std::getenv("SC4_DEBUG")) {
            cIGZWin* desktop = gMgr->GetMainWindow();
            if (std::getenv("SC4_TREE")) DumpTree(desktop, 0, std::atoi(std::getenv("SC4_TREE")));
            std::printf("debug: desktop children=%u visible=%d draws=%u programs=%u\n", desktop->GetChildCount(), desktop->IsVisible(), gDriver->DrawCalls(), gDriver->PipelineCount());
            for (auto& kv : gDriver->UnsupportedCalls()) std::printf("debug: unsupported %s x%d\n", kv.first.c_str(), kv.second);
        }
        WritePng(shot);
        return 0;
    }
    int n = 0;
    while (!gQuit) {
        PumpEvents();
        Frame();
        Present();
        if (frames > 0 && ++n >= frames) break;
    }
    if (gAudio) gAudio->Close();   // its voices are SDL audio streams: they go before SDL_Quit (the static outlives it)
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(gWindow);
    SDL_Quit();
    return 0;
}
