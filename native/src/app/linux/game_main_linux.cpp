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
#include <strings.h>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gz/CheatCodeManager.h"
#include "platform/GameDataPath.h"
#include "platform/PointerState.h"
#include "sc4/Simulator.h"
#include "sc4/RegionDialogs.h"
#include "app/NativeGlyphTextures.h"
#include "app/NativeCitySystems.h"
#include "app/NativeSnapshot.h"
#include "app/NativeCheats.h"
#include "app/NativeKeyboard.h"
#include "rz/cRZKeyboard.h"
#include "gz/CommandServer.h"
#include "app/NativeNetworkManager.h"
#include "app/NativeNetworkTool.h"
#include "app/NativeWinControls.h"
#include "app/NativeNetworkLots.h"
#include "app/NativeUnderground.h"
#include "app/NativeBudgetUi.h"
#include "app/NativeAnimationCoordinator.h"
#include "app/NativeGameData.h"
#include "app/NativeBoot.h"
#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeLotConstruction.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeIntro.h"
#include "platform/PerfLog.h"
#include "sc4/CameraControl.h"
#include "sc4/Render3D.h"
#include "app/NativeCityView.h"
#include "app/NativeDisasterTools.h"
#include "app/NativeModelMaker.h"
#include "app/NativeFonts.h"
#include "app/NativeGraphics.h"
#include "app/NativeRegion.h"
#include "app/NativeRegionScreen.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "app/NativeWinSC4App.h"
#include "gz/BufferFontRenderer.h"
#include "gz/cGZMessageServer.h"
#include "gz/cGZMessageServer2.h"
#include "gz/DrawFontRenderer.h"
#include "gz/GZFont.h"
#include "gz/GZFramework.h"
#include "gz/Cursor.h"
#include "gz/FontSys.h"
#include "gz/RZServices.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinFont.h"
#include "rz/COMDirectors.h"
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
#include "sc4/RegionalCity.h"
#include "sc4/SC4UI.h"
#include "sc4/SLBuffer.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinLocationSaver.h"
#include "rz/cRZMessage2Standard.h"
#include "app/NativeEffects.h"
#include "sc4/WinRegionScreen.h"
#include "sc4/WinSC4App.h"
#include "sc4/WinSplashScreen.h"
#include "app/NativeZoneTool.h"
#include "app/NativeRegionExtras.h"
#include "app/NativeAudio.h"
#include "app/NativeAudioDirector.h"
#include "app/NativeDemolition.h"
#include "app/NativePower.h"
#include "sc4/Demolition.h"

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
std::uint32_t gButtons = 0;   // the SDL mouse button mask (the right button drives scrolling)

SDL_Window* gWindow = nullptr;
std::unique_ptr<gd::cGLGDriver> gDriver;
std::unique_ptr<native::Graphics> gGraphics;
std::unique_ptr<native::WinMgr> gMgr;

// SC4_SCRIPTPRESENT=1: a scenario run shows its window and presents every frame, so its frames are paced like the interactive
// loop's (the swap) instead of by the 4 ms nap.
bool gScriptPresent = false;
void Present();
void Nap() {
    if (gScriptPresent) { Present(); return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

// ---- fonts: the recovered cGZFont over FreeType, drawing through the surface's context --------------------------------------
native::FreeTypeFontHost* gFontHost;

struct DrawFontHost final : IDrawFontRendererHost {
    cIGZFontRenderer* CreateBufferFontRenderer() override { auto* r = new cGZBufferFontRenderer(); r->AddRef(); return r; }
    cIGZBuffer* CreateBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
    void NativeColorToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { r = n >> 16; g = n >> 8; b = n; }
} gDrawFontHost;

// The game's fonts: the .mxf files of GameData/Fonts (FreeType reads them through NativeFonts' MXF decoder). The font
// system service (cGZFontSys, RZ::FO()) and the window control manager (RZ::WCM()) are the original's; cSC4App::
// SetupWinCtrlMgr fills the manager with the system fonts and the [Font Styles] of FontStyle.ini. The HTML windows keep
// their own per-size fonts (HtmlFont below).
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

// What the font system service (cGZFontSys) needs from the platform: files and directories through the standard
// library, the FreeType host for faces, and new cGZFont objects drawn through the surface's context.
class FontSysHost final : public IFontSysHost {
public:
    IFontRasterizer* Rasterizer() override { return gFontHost->Rasterizer(); }
    bool FileExists(const cIGZString& path) override { return gFontHost->FileExists(path); }
    bool GetPathExtension(const cIGZString& path, cIGZString& extension) override { return gFontHost->GetPathExtension(path, extension); }
    std::uint32_t GetCurrentLanguage() override { return gFontHost->GetCurrentLanguage(); }
    void NativeColorToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { gFontHost->NativeColorToRGB(n, r, g, b); }
    std::uint32_t RGBToNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return gFontHost->RGBToNativeColor(r, g, b); }
    bool ReadFileHeader(const cIGZString& path, void* buffer, std::uint32_t count) override {
        std::ifstream file(std::string(path.ToChar(), path.Strlen()), std::ios::binary);
        return file.read(static_cast<char*>(buffer), count) && std::uint32_t(file.gcount()) == count;
    }
    // cRZDirectory's entry filter: '*' any run, '?' any one character, compared without case.
    void ListDirectoryFiles(const cIGZString& directory, const cIGZString& filter, std::list<cRZString>& out) override {
        std::string pattern(filter.ToChar(), filter.Strlen());
        for (char& c : pattern) c = char(std::tolower(static_cast<unsigned char>(c)));
        std::error_code error;
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(std::string(directory.ToChar(), directory.Strlen()), error)) {
            if (!entry.is_regular_file(error)) continue;
            std::string name = entry.path().filename().string();
            std::string lower = name;
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (fnmatch(pattern.c_str(), lower.c_str(), 0) == 0) names.push_back(name);
        }
        std::sort(names.begin(), names.end());   // directory order varies between file systems
        for (const std::string& name : names) out.emplace_back(name.c_str());
    }
    cIGZFont* CreateFontObject() override {
        auto* font = new cGZFont(gFontHost);
        font->AddRef();
        return font;
    }
    int GetCurrentFrameworkCodePage() override { return 8; }
} gFontSysHost;

bool InitFont() {
    gFontHost = new native::FreeTypeFontHost();
    return gFontHost->Ok();
}

// The cursor platform of the recovered cursor classes (gz/Cursor.h) over SDL: a loaded game cursor becomes a named SDL
// color cursor (the Mac build's named cursor path: ARGB pixels, the masked ones transparent), the system cursor indices
// map to SDL's system cursors, a buffer handed in as a hardware cursor becomes a color cursor of its pixels.
class CursorPlatform final : public gz::ICursorPlatform {
public:
    cIGZBuffer* CreateBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
    void SetHardwareCursor(cIGZBuffer* buffer, int hotX, int hotY) override {
        if (!buffer) return;
        auto* gz = static_cast<cGZBuffer*>(buffer);
        const int w = int(gz->Width()), h = int(gz->Height());
        std::vector<std::uint32_t> pixels(std::size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) pixels[std::size_t(y) * w + x] = gz->GetPixel(x, y);
        SDL_Cursor* cursor = MakeCursor(pixels.data(), w, h, w * 4, hotX, hotY);
        if (!cursor) return;
        if (hardware_) SDL_DestroyCursor(hardware_);
        hardware_ = cursor;
        SDL_SetCursor(cursor);
        gz::gMacHardwareCursorSet = true;
    }
    void SetSystemCursor(int index) override {
        // the Windows-95 style stock cursors in cRZSystemCursorW95's order: arrow, I-beam, wait, cross, up arrow, the four
        // sizing shapes, size all, no, app starting, help
        static constexpr SDL_SystemCursor kMap[] = {SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_WAIT,
            SDL_SYSTEM_CURSOR_CROSSHAIR, SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE,
            SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NS_RESIZE, SDL_SYSTEM_CURSOR_MOVE, SDL_SYSTEM_CURSOR_NOT_ALLOWED,
            SDL_SYSTEM_CURSOR_PROGRESS, SDL_SYSTEM_CURSOR_DEFAULT};
        const SDL_SystemCursor which = index >= 0 && index < int(std::size(kMap)) ? kMap[index] : SDL_SYSTEM_CURSOR_DEFAULT;
        SDL_Cursor*& cursor = system_[which];
        if (!cursor) cursor = SDL_CreateSystemCursor(which);
        if (cursor) SDL_SetCursor(cursor);
    }
    void SetNamedCursor(const char* name) override {
        auto it = named_.find(name ? name : "");
        if (it == named_.end()) return;
        SDL_SetCursor(it->second);
        current_ = it->first;
    }
    void AddNamedCursor(const char* name, const std::uint32_t* argb, int width, int height, int stride, double hotX, double hotY) override {
        (void)stride;   // the pixels come packed, one row of `width` after another
        SDL_Cursor* cursor = MakeCursor(argb, width, height, width * 4, int(hotX), int(hotY));
        if (!cursor) return;
        SDL_Cursor*& slot = named_[name ? name : ""];
        if (slot) SDL_DestroyCursor(slot);
        slot = cursor;
    }
    void ShowCursor() override { SDL_ShowCursor(); }
    void HideCursor() override { SDL_HideCursor(); }
    std::size_t NamedCount() const { return named_.size(); }
    const std::string& Current() const { return current_; }
private:
    static SDL_Cursor* MakeCursor(const std::uint32_t* argb, int w, int h, int pitch, int hotX, int hotY) {
        SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, const_cast<std::uint32_t*>(argb), pitch);
        if (!surface) return nullptr;
        SDL_Cursor* cursor = SDL_CreateColorCursor(surface, hotX, hotY);
        SDL_DestroySurface(surface);
        return cursor;
    }
    std::map<std::string, SDL_Cursor*> named_;
    std::map<SDL_SystemCursor, SDL_Cursor*> system_;
    SDL_Cursor* hardware_ = nullptr;
    std::string current_;
} gCursorPlatform;

// The window font of a cIGZFont (gz::MakeWinFont): the controls draw on the platform's surfaces through it. The text and
// shadow colors are the font's own (cIGZFont vslots 0x60 / 0x68 of the original's controls).
class WinFont final : public gz::IWinFont {
public:
    explicit WinFont(cIGZFont* f) : font(f) { font->AddRef(); }
    cIGZFont* font;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        font->Release();
        delete this;
        return 0;
    }
    cIGZFont* AsFont() override { return font; }
    std::uint32_t DefaultTextColor() override { return font->GetFontColor(); }
    std::uint32_t DefaultShadowColor() override { return font->GetFontBackgroundColor(); }
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
private:
    std::uint32_t refs_ = 1;
};
gz::IWinFont* MakeWinFont(cIGZFont* font) { return new WinFont(font); }

// The controls' font seam (gz::WinFontSource) answers from RZ::WCM(): the system font of a style id (RZ::WCM() vslot 0x28),
// a style id by name from the manager's font mappings. The port's controls ask for a style even when it was never
// registered; those get the default system font 0x68963c4c.
struct FontSource final : gz::IWinFontSource {
    gz::IWinFont* Font(std::uint32_t style) override {
        gz::cIGZWinCtrlMgr* controls = RZ::WCM();
        if (!controls) return nullptr;
        gz::IWinFont* font = controls->SystemFont(static_cast<int>(style));
        return font ? font : controls->SystemFont(static_cast<int>(gz::kWinDefaultFontStyle));
    }
    bool Available() override { return RZ::WCM() && RZ::WCM()->SystemFont(static_cast<int>(gz::kWinDefaultFontStyle)); }
    gz::IWinFont* FontByName(const char* name) override {
        const std::uint32_t id = StyleId(name);
        return id ? Font(id) : nullptr;
    }
    // The names are the [Font Styles] keys SetFontStyle registered as font mappings and as words of the font-style
    // vocabulary; the vocabulary compares without case (cGZScriptVocab), and so does this.
    std::uint32_t StyleId(const char* name) override {
        gz::cIGZWinCtrlMgr* controls = RZ::WCM();
        if (!controls || !name) return 0;
        const cRZString wanted(name);
        if (const int id = controls->GetFontMappingID(&wanted)) return static_cast<std::uint32_t>(id);
        auto* manager = dynamic_cast<gz::cGZWinCtrlMgr*>(controls);
        return manager ? static_cast<std::uint32_t>(manager->FontMappingIDIgnoringCase(wanted)) : 0;
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
    bool LoadExemplarString(cIGZString& out, sc4::cISCPropertyHolder* holder, std::uint32_t property, sc4::cSCStringDetokenizer* detokenizer) override {
        return native::LoadExemplarString(out, holder, property, detokenizer);
    }
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
    cSLWinLocationSaver* WindowLocationSaver() override { return static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver); }
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
std::string gIntroSwitch;   // the game's -intro:<value> switch (scenarios and shots default to off)
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

// The game's sound runs in the interactive game, or with SC4_AUDIO=1 (shots and scenarios stay without it): the game's own
// audio director is installed before the boot (NativeAudioDirector.cpp).
bool gAudioWanted = false;
bool gQuit = false;   // ends the main loop (the window's close button, the game's own quit)

// SC4_FRAMEPROF (stderr) / SC4_PERFLOG=<file>: milliseconds spent in each part of every frame, the time since the previous
// frame began (it includes presenting and the window events), the draw count and the city view's scroll state.
struct FrameLaps {
    bool toStderr = std::getenv("SC4_FRAMEPROF") != nullptr;
    bool on = toStderr || platform::PerfLogOn();
    double begin = 0, last = 0, previousBegin = 0;
    std::string line;
    void Start() {
        if (!on) return;
        previousBegin = begin;
        begin = last = NowMs();
        line.clear();
    }
    void Lap(const char* name) {
        if (!on) return;
        const double now = NowMs();
        char part[64];
        std::snprintf(part, sizeof part, " %s %.1f", name, now - last);
        line += part;
        last = now;
    }
    std::uint32_t draws = 0;
    void End(std::uint32_t total, sc4::cSC4View3DWin* view, bool rightButton) {
        if (!on) return;
        const double interval = previousBegin > 0 ? begin - previousBegin : 0;
        char state[128] = "";
        if (view)
            std::snprintf(state, sizeof state, " scroll %d stop %d hidden %d", int(view->IsScrolling()), int(view->IsScrollStopping()),
                          int(view->IsSimHiddenPaused()));
        if (toStderr) std::fprintf(stderr, "frame:%s draws %u\n", line.c_str(), total - draws);
        platform::PerfNote("frame %6.1f ms%s |%s draws %u%s rbutton %d", interval, interval > 25 ? " SLOW" : "", line.c_str(),
                           total - draws, state, int(rightButton));
        draws = total;
    }
} gLaps;

void Frame() {
    gLaps.Start();
    native::PumpToolTicks(16);
    native::TickLighting();
    native::TickAutomata();
    gLaps.Lap("tools+light+automata");
    {   // the framework tick of the simulator host's services (cSC4Simulator::OnTick: the date and every simulator agent)
        static std::uint32_t tickNumber = 0;
        native::TickSimulatorServices(++tickNumber);
    }
    gLaps.Lap("sim");
    // In a city the animation tick manager is ticked here (the region screen ticks it from its GZPaint; the city view's per-frame call is
    // not recovered). Init is idempotent: the region screen shut it down when the city replaced it.
    if (gAppWindow && gAppWindow->View3D() && native::StartAnimationTicks()) native::TickAnimation();
    gLaps.Lap("anim");
    native::EndFrameJobs();
    native::TickAudio();   // the framework's tick of the radio and the soundscape and ambience managers
    PumpTimers();
    gLaps.Lap("jobs+audio");
    cSC4App* app = native::BootApplication();
    if (app) {
        app->RunMessageServerPump(0, 64, 4);
        app->RunMessageServer2Pump(0, 64, 4);
        if (gMessages2) gMessages2->OnTick(16);   // posted and general messages (the splash timer, screen swaps)
        app->OnTick(16);
    }
    native::TickGlyphTextures(static_cast<std::uint32_t>(SDL_GetTicks()));   // the framework's tick of the glyph texture service
    gLaps.Lap("messages");
    const bool regionScreen = gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793;
    if (regionScreen && native::DrawRegionLayers()) InvalidateTree(gMgr->GetMainWindow());   // the map is drawn under the windows: they repaint over it
    // The city view redraws the 3D scene into the whole screen every frame, so the windows over it repaint every frame too.
    if (gAppWindow && gAppWindow->View3D() && !regionScreen) InvalidateTree(gMgr->GetMainWindow());
    // The start-up screens (intro movies, logo, splash) repaint only what changed, but the frame alternates between two GL buffers: the
    // whole tree repaints so neither buffer keeps a picture of the screen before (a movie frame around the splash, a faded logo).
    if (gAppWindow && gAppWindow->CurrentScreen() && !regionScreen && !gAppWindow->View3D()) {
        gDriver->SetViewport();   // the whole frame (no window clip), then black under the windows
        gDriver->ClearColor(0, 0, 0, 0);
        gDriver->Clear(gd::kClearColor);
        InvalidateTree(gMgr->GetMainWindow());
    }
    gMgr->Pump();
    gLaps.Lap("pump");
    gMgr->Plot();
    gLaps.Lap("plot");
    gGraphics->Services().Tick();
    gLaps.Lap("gfx");
    gLaps.End(gDriver ? gDriver->DrawCalls() : 0, gAppWindow ? gAppWindow->View3D() : nullptr, (gButtons & SDL_BUTTON_RMASK) != 0);
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

// Holds each frame to the display's refresh period. The swap normally waits for the display; when it does not (a compositor or
// driver that ignores the swap interval, a window that is not on screen) the loop would spin at thousands of frames a second,
// frames would come faster than the animation tick manager's 2 ms floor and every animation would run fast.
void PaceFrame() {
    static const bool off = std::getenv("SC4_NOPACE") != nullptr;   // debug: the loop as the swap alone paces it
    if (off) return;
    static double last = 0.0, period = 1000.0 / 60.0, asked = -1e9;
    double now = NowMs();
    if (now - asked > 1000.0) {   // the window can move to a display with another refresh rate
        asked = now;
        float hz = 60.0f;
        if (gWindow)
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(gWindow)))
                if (mode->refresh_rate > 1.0f) hz = mode->refresh_rate;
        period = 1000.0 / double(hz);
    }
    const double due = last + period * 0.95;   // a swap that did wait lands on the period: nothing is added to it
    if (now < due) {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(due - now));
        now = NowMs();
    }
    last = now;
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
    native::NoteFramePresented();
    PaceFrame();
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
    SetWinClock([]() -> std::uint32_t { return static_cast<std::uint32_t>(SDL_GetTicks()); });   // the windows' fade clock (ms)
    if (!InitFont()) std::printf("font: FAILED to initialise\n");
    gz::SetWinFontSource(&gFontSource);
    gz::SetWinFontMaker(&MakeWinFont);
    gz::SetCursorPlatform(&gCursorPlatform);
    gz::SetWinStyleManager(&gStyleManager);
    native::InstallButtonSounds();   // the window buttons' host: caption fonts (and sounds, once there is audio output)

    const bool ok = native::RunBoot(true, true, gDriver.get(), [&](cGZFramework& framework, cSC4App& app) {
        SL::SetFramework(&framework);             // the locator's RZGetFramework(): without it every SL:: service stays null
        if (gAudioWanted) native::InstallAudioDirector(app, platform::GameDataDir());   // cSC4App::AddCOMDirectors adds it
        framework.AddSystemService(gMgr.get());   // RZ::WM(): the application finds the main window through it
        // The font system and the window control manager, made by cSC4App::AddSystemServices as the original makes them;
        // the draw font renderer is the class the font system gets as its default renderer (SetupWinCtrlMgr).
        app.SetServiceFactory("cGZFontSys", []() -> cIGZUnknown* { return static_cast<cIGZFontSys*>(new cGZFontSys(&gFontSysHost)); });
        app.SetServiceFactory("cGZWinCtrlMgr", []() -> cIGZUnknown* { return static_cast<gz::cIGZWinCtrlMgr*>(new gz::cGZWinCtrlMgr(gMgr.get())); });
        gz::RegisterDirectorMaker("cGZDrawFontRenderer", []() -> cIGZUnknown* { return new cGZDrawFontRenderer(&gDrawFontHost); });
        app.StartupHooks().drawSystemAvailable = [] { return true; };   // the platform's surfaces draw the text
        app.StartupHooks().graphicSystemColor = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) { return gFontHost->RGBToNativeColor(r, g, b); };
        // The second message server (service 0x04fa845b): the splash timer, the screen swaps and every posted command go through it.
        // The frame loop ticks it by hand (RunMessageServer2Pump), so the framework tick registration is a no-op.
        static struct : IMessageServerFramework {
            void AddTickClient(cIGZSystemService*) override {}
            void RemoveTickClient(cIGZSystemService*) override {}
        } messageFramework;
        SetMessageServerFramework(&messageFramework);
        // The first message server (service 0x64063387, cSC4App::AddSystemServices makes it before the second): the tool tips, the
        // quit request and the sound events go through it; the frame loop pumps it (RunMessageServerPump).
        auto* messages = new cGZMessageServer();
        framework.AddSystemService(messages);
        messages->Init();
        auto* messages2 = new cGZMessageServer2();
        // As cSC4App::AddSystemServices sets it: a tick delivers only what was queued when it began, so a message that posts
        // itself again (the advisor ticker) waits for the next tick instead of keeping one tick going for ever.
        messages2->SetAlwaysClearQueueOnTick(false);
        framework.AddSystemService(messages2);
        messages2->Init();
        gMessages2 = messages2;
        static native::HostWinSC4App host(&framework);
        gAppHost = &host;
        // The app window subscribes in its Init (the cheat code box command 0x2a9496c9, the focus change forward), so the
        // server is there before the window is made, as SL::spMS2 is in the original.
        host.SetMessageServer(messages2);
        if (!gIntroSwitch.empty()) host.SetIntroSwitch(gIntroSwitch);
        native::SetIntroServices({[] { gSplashHost.DestroyCurrentScreen(); }, [] { gSplashHost.CreateRegionScreen(); },
                                  [] { if (gAppWindow) gAppWindow->CreateSplashScreen(); }, [] { gSplashHost.ClearScreenToBlack(); }});
        // The window scripts need the UI script service, which needs the resources Init has just registered; the application window
        // (created a few steps later, inside Init) builds the splash from them.
        app.StartupHooks().configureResourceCache = [] {
            std::printf("ui scripts: %s\n", native::InitUiScripts(gMgr.get()) ? "service ready" : "NOT available");
        };
        // cSC4App::SetupWinCtrlMgr's stock images and captions; the generic dialogs (the cheat code box) need them.
        app.StartupHooks().setupWinCtrlResources = [](gz::cIGZWinCtrlMgr* controls) { native::InstallWindowControlManager(gMgr.get(), controls); };
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
    // The framework's HookPostAppInit: the audio director reads the sound configuration and starts the game's sound services.
    if (native::BootFramework()) native::AudioPostAppInit(*native::BootFramework());
    if (cSC4App* app = native::BootApplication()) {
        native::RegionCallbacks callbacks;
        callbacks.dataDirectory = platform::GameDataDir();
        callbacks.isRegionScreenCurrent = [] { return gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793; };
        callbacks.destroyCurrentScreen = [] { if (gAppWindow) gAppWindow->DestroyCurrentScreen(); };
        callbacks.createRegionScreen = [] { if (gAppWindow) gAppWindow->CreateRegionScreen(); };
        native::InstallLanguageFormatter();
        native::InstallRegionServices(*app, callbacks);
        // The app's questions (quit, save) are scripted dialogs answered by a button id, and its quit ends the main loop.
        // The city load installs the rest of these hooks; these two are needed from the region screen on.
        app->CityHooks().scriptedModal = [](std::uint32_t instance, std::uint32_t window) {
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "app: scripted dialog %08x window %08x\n", instance, window);
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
            if (sc4::cSC4View3DWin* view = gAppWindow->View3D()) native::SetCityRenderTarget(*view, gDriver.get(), static_cast<gz::cIGZDrawContext*>(gGraphics->Primary()->GetDrawContext()), gW, gH);
        }, [] { if (gAppWindow) gAppWindow->CreateRegionScreen(); }});
        screen.loadCity = [app](const std::string& path, cRZAutoRefCount<cISC4RegionalCity>* holder) { return native::LoadCityFile(*app, path, holder); };
        native::InstallRegionScreenHost(screen);
        native::InstallRegionExtras(gDriver.get(), []() -> std::uint8_t* { return native::BootApplication() ? native::BootApplication()->GetPreferences() : nullptr; });
        if (std::uint8_t* preferences = app->GetPreferences()) {   // the game's defaults: clouds over the region map, scrolling at the screen edges
            preferences[sc4::kPrefsCloudsVisible] = 1;
            preferences[sc4::kPrefsEdgeScrollEnabled] = 1;
        }
        cISC4Nation* nation = native::CreateNation();
        app->SetNation(nation);
        if (const char* wanted = std::getenv("SC4_REGION")) {   // development: open this region's folder (the boot picks the first one listed)
            for (std::uint32_t i = 0; i < nation->GetRegionCount(); ++i) {
                cISC4Region* region = nation->GetRegion(i);
                cIGZString* folder = region ? region->GetDirectoryName() : nullptr;
                if (folder && !std::strcmp(folder->Data(), wanted)) nation->SetActiveRegion(i);
            }
        }
        std::printf("nation: regions dir \"%s\"\n", cSC4Nation::msRegionDirectory.ToChar());
        std::printf("nation: %u regions, active %d\n", nation->GetRegionCount(), nation->GetActiveRegionIndex());
    }
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

// The progress screen's frames while a city loads (the frame loop is not running then): its buffer through the primary buffer's
// draw context onto the frame, the frame shown. SC4_LOADINGSHOT=PREFIX also writes each shown frame to PREFIX_N.png (the first
// 16): a scenario cannot take a shot in the middle of a load.
struct LoadingPresenter final : native::LoadingScreenPresenter {
    void Show(cIGZBuffer* buffer, const cRZRect& source, const cRZRect& target) override {
        auto* context = static_cast<gz::cIGZDrawContext*>(gGraphics->Primary()->GetDrawContext());
        if (context && buffer) context->Blt(buffer, source, target);
    }
    void Clear(float r, float g, float b, float a) override {
        gDriver->SetViewport();
        gDriver->ClearColor(r, g, b, a);
        gDriver->Clear(gd::kClearColor);
    }
    void Present() override {
        static const char* prefix = std::getenv("SC4_LOADINGSHOT");
        static int shots = 0;
        if (prefix && shots < 16) WritePng(std::string(prefix) + "_" + std::to_string(shots++) + ".png");
        ::Present();
    }
} gLoadingPresenter;

// cRZKeyboard's key state (the original asks the system by Windows virtual-key code: GetAsyncKeyState / GetKeyState): SDL's
// keyboard answers for the modifiers, letters, digits, function keys, arrows and editing keys; the toggle state is the lock
// keys'. Bit 15 is "down now", bit 0 "toggled on", as the system calls answer.
std::int16_t AsyncKeyState(std::uint32_t vk) {
    const SDL_Keymod mod = SDL_GetModState();
    bool down = false;
    if (vk == 0x10) down = (mod & SDL_KMOD_SHIFT) != 0;
    else if (vk == 0x11) down = (mod & SDL_KMOD_CTRL) != 0;
    else if (vk == 0x12) down = (mod & SDL_KMOD_ALT) != 0;
    else {
        SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
        if (vk >= 'A' && vk <= 'Z') code = SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
        else if (vk >= '1' && vk <= '9') code = SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
        else if (vk == '0') code = SDL_SCANCODE_0;
        else if (vk >= 0x70 && vk <= 0x7b) code = SDL_Scancode(SDL_SCANCODE_F1 + (vk - 0x70));
        else {
            static constexpr std::pair<std::uint32_t, SDL_Scancode> kKeys[] = {
                {0x08, SDL_SCANCODE_BACKSPACE}, {0x09, SDL_SCANCODE_TAB}, {0x0d, SDL_SCANCODE_RETURN}, {0x1b, SDL_SCANCODE_ESCAPE},
                {0x20, SDL_SCANCODE_SPACE}, {0x21, SDL_SCANCODE_PAGEUP}, {0x22, SDL_SCANCODE_PAGEDOWN}, {0x23, SDL_SCANCODE_END},
                {0x24, SDL_SCANCODE_HOME}, {0x25, SDL_SCANCODE_LEFT}, {0x26, SDL_SCANCODE_UP}, {0x27, SDL_SCANCODE_RIGHT},
                {0x28, SDL_SCANCODE_DOWN}, {0x2d, SDL_SCANCODE_INSERT}, {0x2e, SDL_SCANCODE_DELETE}};
            for (const auto& [key, scancode] : kKeys) if (key == vk) code = scancode;
        }
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        down = code != SDL_SCANCODE_UNKNOWN && keys && int(code) < count && keys[code];
    }
    return down ? std::int16_t(-0x8000) : std::int16_t(0);
}
std::int16_t ToggleKeyState(std::uint32_t vk) {
    const SDL_Keymod mod = SDL_GetModState();
    const bool on = (vk == 0x14 && (mod & SDL_KMOD_CAPS)) || (vk == 0x90 && (mod & SDL_KMOD_NUM)) || (vk == 0x91 && (mod & SDL_KMOD_SCROLL));
    return static_cast<std::int16_t>((on ? 1 : 0) | (AsyncKeyState(vk) & 0x8000));
}

// ---- input ------------------------------------------------------------------------------------------------------------------

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
    case SDLK_SLASH: return 0xbf; case SDLK_GRAVE: return 0xc0; case SDLK_SEMICOLON: return 0xba; case SDLK_APOSTROPHE: return 0xde;
    case SDLK_LEFTBRACKET: return 0xdb; case SDLK_RIGHTBRACKET: return 0xdd; case SDLK_BACKSLASH: return 0xdc;
    case SDLK_KP_MULTIPLY: return 0x6a; case SDLK_KP_PERIOD: return 0x6e; case SDLK_KP_DIVIDE: return 0x6f;
    case SDLK_NUMLOCKCLEAR: return 0x90; case SDLK_CAPSLOCK: return 0x14; case SDLK_SCROLLLOCK: return 0x91;
    case SDLK_PAUSE: return 0x13; case SDLK_PRINTSCREEN: return 0x2c;
    default: break;
    }
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return std::uint32_t(0x61 + (k - SDLK_KP_1));
    if (k == SDLK_KP_0) return 0x60;
    if (k >= SDLK_F1 && k <= SDLK_F12) return std::uint32_t(0x70 + (k - SDLK_F1));
    return 0;
}

// The GZ key modifier word of a key event: 1 shift, 2 control, 4 alt (what the game's key accelerators test).
std::uint32_t KeyModifierWord(SDL_Keymod mod) {
    return ((mod & SDL_KMOD_SHIFT) ? 1u : 0u) | ((mod & SDL_KMOD_CTRL) ? 2u : 0u) | ((mod & SDL_KMOD_ALT) ? 4u : 0u);
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
        // Every key goes to the window manager as the game's key message; the windows' keyboard accelerators (the
        // game's own shortcut tables, NativeKeyboard.cpp) turn the keys nobody takes into commands.
        if (const std::uint32_t vk = VirtualKey(e.key.key))
            gMgr->GZWinMsgPost_Keyboard(down ? kGZMsgKeyDown : kGZMsgKeyUp, vk, KeyModifierWord(e.key.mod));
        // Windows' TranslateMessage follows these keys' WM_KEYDOWN (and each repeat) with a WM_CHAR of their control
        // character, which the game reads: Enter finishes a sign and the establish-city dialog, Tab moves between its fields,
        // Backspace deletes in the sign tool. SDL sends no text event for them.
        if (down) {
            std::uint32_t control = 0;
            switch (e.key.key) {
                case SDLK_RETURN: case SDLK_KP_ENTER: control = 0x0d; break;
                case SDLK_BACKSPACE: control = 0x08; break;
                case SDLK_TAB: control = 0x09; break;
                case SDLK_ESCAPE: control = 0x1b; break;
                default: break;
            }
            if (control) gMgr->GZWinMsgPost_Keyboard(kGZMsgCharacter, control, 0);
        }
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
            {"rl", {0x6a935cc9, 0}}, {"rr", {0x6a935ccc, 0}}, {"bulldoze", {0x6a935d36, 0}},
            {"firetool", {0x6a935d39, 0}}, {"policetool", {0xca935dbd, 0}}, {"query", {0x6a935cf3, 0}},
            {"pipetool", {0x6a935dee, 0}}, {"escape", {0x6a935cf1, 0}}};
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

}  // namespace
namespace native {
std::uint32_t NativeSimDayNumber();   // app/NativeCityPanel.cpp

// The HTML windows' fonts (app/NativeWinHtml.cpp, cIGZFontSys::CreateFont): the game font file of the first listed face
// that exists, bold / italic preferred, one shared cGZFont per (file, pixel size). Null when no listed face exists.
cGZFont* HtmlFont(const std::string& faces, std::uint32_t size, bool bold, bool italic) {
    if (!gFontHost || size == 0) return nullptr;
    const std::string path = FindFontFile(faces, bold, italic);
    if (path.empty()) return nullptr;
    // never destroyed: documents still hold their fonts while the process exits
    static auto& cache = *new std::map<std::pair<std::string, std::uint32_t>, cGZFont*>();
    cGZFont*& font = cache[{path, size}];
    if (!font) {
        font = MakeFont(path, size);
        if (!font->Init()) std::printf("font: html face %s size %u did not load\n", path.c_str(), size);
    }
    return font;
}

// A [Font Styles] entry named as an HTML face (the advisor popups name their system fonts, MessageHeader / MessageBody):
// the style's own font file, scaling, advance scaling and line spacing at the page's pixel size, one cGZFont per
// (style, size). Null when no style has that name.
cGZFont* HtmlStyleFont(const std::string& styleName, std::uint32_t size) {
    if (size == 0) return nullptr;
    const std::uint32_t id = gFontSource.StyleId(styleName.c_str());
    gz::IWinFont* style = id && RZ::WCM() ? RZ::WCM()->SystemFont(static_cast<int>(id)) : nullptr;
    cIGZFont* source = style ? style->AsFont() : nullptr;
    if (!source) return nullptr;
    std::string key = styleName;
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    static auto& cache = *new std::map<std::pair<std::string, std::uint32_t>, cGZFont*>();
    cGZFont*& font = cache[{key, size}];
    if (font) return font;
    cRZString path;
    source->GetFilePath(path);
    font = MakeFont(std::string(path.ToChar(), path.Strlen()), size);
    float x = 1, y = 1;
    source->GetScaling(x, y);
    font->SetScaling(x, y);
    source->GetAdvanceScaling(x, y);
    font->SetAdvanceScaling(x, y);
    int dx = 0, dy = 0;
    source->GetSpacingAdjustment(dx, dy);
    font->SetSpacingAdjustment(dx, dy);
    font->SetAntialiasing(source->GetAntialiasing());
    if (!font->Init()) std::printf("font: html style %s size %u did not load\n", styleName.c_str(), size);
    return font;
}

// The [Font Styles] name of a style id: the control manager's font mapping ("" when unknown).
std::string FontStyleName(std::uint32_t guid) {
    gz::cIGZWinCtrlMgr* controls = RZ::WCM();
    cIGZString* name = controls ? controls->GetFontMapping(static_cast<int>(guid)) : nullptr;
    return name ? std::string(name->ToChar(), name->Strlen()) : std::string();
}
}  // namespace native
namespace {
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
        } else if (command == "sdlhold") {   // sdlhold X Y MS: the left button pressed at a window point, held for MS ms of frames, released
            float x = 0, y = 0;
            int ms = 0;
            words >> x >> y >> ms;
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = SDL_GetWindowID(gWindow); e.motion.x = x; e.motion.y = y;
            SDL_PushEvent(&e); PumpEvents(); Frame(); Nap();
            for (const SDL_EventType type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                SDL_Event b{};
                b.type = type; b.button.windowID = SDL_GetWindowID(gWindow); b.button.button = SDL_BUTTON_LEFT;
                b.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN; b.button.clicks = 1; b.button.x = x; b.button.y = y;
                SDL_PushEvent(&b); PumpEvents(); Frame(); Nap();
                if (type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    const double end = NowMs() + ms;
                    while (NowMs() < end) { Frame(); Nap(); }
                }
            }
        } else if (command == "disasters") {   // disasters: the disaster menu's items (exemplar ids with their effect and manager)
            native::NativeDisasterMenuReport();
        } else if (command == "disaster") {   // disaster HEXID: what picking that item of the disaster menu does (the tool becomes current)
            std::string hex;
            words >> hex;
            const std::uint32_t id = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
            sc4::cSC4View3DWin* view = gAppWindow ? gAppWindow->View3D() : nullptr;
            if (view) view->InvokeCatalogItemCommand(0x4a2b2464u, id, false);
            std::printf("disaster %08x: %s\n", id, view ? (view->GetCurrentViewInputControl() ? "a tool is current" : "no tool") : "no view");
        } else if (command == "rdrag") {   // rdrag X Y DX DY MS: a real SDL right-button drag (window points), held for MS ms of frames
            float x = 0, y = 0, dx = 0, dy = 0;
            int ms = 0;
            words >> x >> y >> dx >> dy >> ms;
            const auto push = [&](SDL_EventType type, float px, float py) {
                SDL_Event e{};
                e.type = type;
                if (type == SDL_EVENT_MOUSE_MOTION) {
                    e.motion.windowID = SDL_GetWindowID(gWindow); e.motion.x = px; e.motion.y = py;
                    e.motion.state = type == SDL_EVENT_MOUSE_MOTION && (gButtons & SDL_BUTTON_RMASK) ? SDL_BUTTON_RMASK : 0;
                } else {
                    e.button.windowID = SDL_GetWindowID(gWindow); e.button.button = SDL_BUTTON_RIGHT;
                    e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.clicks = 1; e.button.x = px; e.button.y = py;
                }
                SDL_PushEvent(&e);
            };
            push(SDL_EVENT_MOUSE_MOTION, x, y); PumpEvents(); Frame(); Nap();
            push(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y); PumpEvents(); Frame(); Nap();
            for (int step = 1; step <= 10; ++step) { push(SDL_EVENT_MOUSE_MOTION, x + dx * step / 10, y + dy * step / 10); PumpEvents(); Frame(); Nap(); }
            const double end = NowMs() + ms;
            for (int n = 0; NowMs() < end; ++n) {   // the hand is never quite still: a pixel either way
                push(SDL_EVENT_MOUSE_MOTION, x + dx + float(n % 3 - 1), y + dy);
                PumpEvents(); Frame(); Nap();
            }
            push(SDL_EVENT_MOUSE_BUTTON_UP, x + dx, y + dy); PumpEvents(); Frame(); Nap();
            for (int i = 0; i < 5; ++i) { PumpEvents(); Frame(); Nap(); }
        } else if (command == "audit") {   // audit: renderer models made for another zoom (stale models drawing over new ones)
            native::AuditRendererModels();
        } else if (command == "probe") {   // probe X Y: the buildings around the ground under a logical view point
            int px = 0, py = 0;
            words >> px >> py;
            float hit[3] = {};
            sc4::cSC4View3DWin* view = gAppWindow ? gAppWindow->View3D() : nullptr;
            if (view && view->PickTerrain(px, py, hit, true)) {
                std::printf("probe %d,%d: ground %.0f,%.0f\n", px, py, hit[0], hit[2]);
                native::ProbeBuildingsAt(hit[0], hit[2], 24.0f);
            } else {
                std::printf("probe %d,%d: no ground\n", px, py);
            }
        } else if (command == "dispatches") {   // dispatches: the records, their vehicles and states
            native::NativeDispatchReport();
        } else if (command == "generators") {   // generators: the automata the generators made, where they are on screen
            native::NativeGeneratorReport();
        } else if (command == "trailers") {   // trailers [N]: the trailer vehicles (first N on screen) and their cars
            int limit = 8;
            words >> limit;
            native::NativeTrailerReport(limit);
        } else if (command == "decline") {   // decline: the lots by state and what the tract developer abandoned / reoccupied
            native::NativeDeclineReport();
        } else if (command == "lotdev") {   // lotdev: the lot developer's life cycle (initialized, messages by type, night-light deque)
            native::NativeLotDeveloperReport();
        } else if (command == "aircraft") {   // aircraft: the aircraft manager's airports, runway markers and every aircraft
            native::NativeAircraftReport();
        } else if (command == "trains") {   // trains [N]: the city's track cells, the trains and buses, and the first N engines with their cars
            int limit = 8;
            words >> limit;
            native::NativeTrainReport(limit);
        } else if (command == "lua") {   // lua CODE: run CODE in the city's automata script VM and print what it returns
            std::string code;
            std::getline(words, code);
            native::NativeRunCityLua(code.c_str());
        } else if (command == "alua") {   // alua CODE: run CODE in the app's Lua (the advisors' VM) and print what it returns
            std::string code;
            std::getline(words, code);
            native::NativeRunAppLua(code.c_str());
        } else if (command == "advisors") {   // advisors: every advisor and its active advices
            native::NativePrintAdvisors();
        } else if (command == "ticker") {   // ticker: the news ticker's queue, flipper ring and scroll
            native::NativeTickerReport();
        } else if (command == "terrainflags") {   // terrainflags: hidden-terrain cell flags against the city file's (SC4_TERRAINHIDEDEBUG=1)
            native::NativeTerrainHidingReport();
        } else if (command == "redraw") {   // redraw: the renderer redraws the whole view (static view and backing store included)
            if (sc4::cSC43DRender* render = native::CityRenderer()) render->ForceFullRedraw();
        } else if (command == "wintree") {   // wintree [DEPTH]: the window tree under the main window (id, visible, area, kids)
            int depth = 4; words >> depth;
            DumpTree(gMgr->GetMainWindow(), 0, depth);
        } else if (command == "cityview") {   // cityview: the city view's compass, alert border and other parts as they stand
            native::NativeCityViewReport();
        } else if (command == "registry") {   // registry: the game-data registry's entry tree
            native::NativeRegistryReport();
        } else if (command == "hour") {   // hour N: the city clock jumps to hour N (its hour, minute and day/night messages)
            int hour = 12; words >> hour;
            native::NativeSetClockHour(hour);
        } else if (command == "propdev") {   // propdev: the prop developer's lists (night lights, timed, seasonal, power, signs)
            native::NativePropDeveloperReport();
        } else if (command == "animated") {   // animated [N]: the animation coordinator and the first N ticking props' frames
            int limit = 8;
            words >> limit;
            native::NativeAnimationReport(limit);
        } else if (command == "simspeed") {   // simspeed N: the simulator's speed (0 turtle, 1 llama, 2 cheetah)
            int speed = 2;
            words >> speed;
            if (sc4::cSC4Simulator* simulator = native::CitySimulatorObject()) {
                simulator->SetSimSpeed(speed);
                std::printf("simspeed %d\n", speed);
            }
        } else if (command == "simpause" || command == "simresume") {   // the simulator's pause count (a check that compares with the save)
            if (sc4::cSC4Simulator* simulator = native::CitySimulatorObject()) {
                if (command == "simpause") simulator->Pause();
                else simulator->Resume();
                std::printf("%s: paused %d\n", command.c_str(), simulator->IsPaused() ? 1 : 0);
            }
        } else if (command == "ignite") {   // ignite X Z [force]: sets the occupants of a city cell alight (the torch's call)
            int x = 0, z = 0;
            std::string force;
            words >> x >> z >> force;
            native::NativeIgniteCell(x, z, force == "force");
        } else if (command == "pipeburst") {   // pipeburst X Z: the pipe-burst manager starts at a cell (-1 -1: its own search)
            int x = -1, z = -1;
            words >> x >> z;
            native::NativePipeBurstStart(x, z);
        } else if (command == "toxic") {   // toxic X Z: a toxic spill starts at a city cell
            int x = 0, z = 0;
            words >> x >> z;
            native::NativeToxicStart(x, z);
        } else if (command == "toxics") {   // toxics: the toxic waste occupants
            native::NativeToxicReport();
        } else if (command == "netmanager") {   // netmanager: what the network manager's host answers, its remaps, tools and filters
            native::PrintNetworkManagerHost();
        } else if (command == "powerline") {   // powerline X1 Z1 X2 Z2: the power line tool lays a line between two cells (its placement API)
            int x1 = 0, z1 = 0, x2 = 0, z2 = 0;
            words >> x1 >> z1 >> x2 >> z2;
            native::NativeLayPowerLine(x1, z1, x2, z2);
        } else if (command == "poles") {   // poles X0 Z0 X1 Z1: the poles and wires of a cell rectangle and the power grid there
            int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
            words >> x0 >> z0 >> x1 >> z1;
            native::NativePowerPoleReport(x0, z0, x1, z1);
        } else if (command == "bridges") {   // bridges [X0 Z0 X1 Z1]: a water map of the city (or the rectangle) and every bridge piece
            int x0 = -1, z0 = -1, x1 = -1, z1 = -1;
            words >> x0 >> z0 >> x1 >> z1;
            native::NativeBridgeReport(x0, z0, x1, z1);
        } else if (command == "bridgechoose") {   // bridgechoose TYPE X1 Z1 X2 Z2 [ROW]: the network tool UI's drag and bridge chooser (rows, then ROW and Place)
            std::uint32_t type = 0;
            int x1 = 0, z1 = 0, x2 = 0, z2 = 0, row = -1;
            words >> type >> x1 >> z1 >> x2 >> z2 >> row;
            native::NativeBridgeChooser(type, x1, z1, x2, z2, row);
        } else if (command == "netline") {   // netline TYPE X1 Z1 X2 Z2: a network tool lays a line as the drag's release does (neighbor connections too)
            std::uint32_t type = 0;
            int x1 = 0, z1 = 0, x2 = 0, z2 = 0;
            words >> type >> x1 >> z1 >> x2 >> z2;
            native::NativeLayNetworkLine(type, x1, z1, x2, z2);
        } else if (command == "terrainheights") {   // terrainheights X0 Z0 X1 Z1: the terrain altitude at every vertex of a rectangle (read-only)
            int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
            words >> x0 >> z0 >> x1 >> z1;
            native::NativeTerrainHeightsReport(x0, z0, x1, z1);
        } else if (command == "tunnelparams") {   // tunnelparams TYPE: a network type's tunnel rules (slopes, portal exemplars, clearances)
            std::uint32_t type = 0;
            words >> type;
            native::NativeTunnelParams(type);
        } else if (command == "heights") {   // heights X1 Z1 X2 Z2: the mean terrain height of each cell along a row or column
            int x1 = 0, z1 = 0, x2 = 0, z2 = 0;
            words >> x1 >> z1 >> x2 >> z2;
            native::NativeTerrainLine(x1, z1, x2, z2);
        } else if (command == "tunnels") {   // tunnels: every tunnel portal, its model, its other end and its paths
            native::NativeTunnelReport();
            native::NativeTrafficTunnelReport();
        } else if (command == "powerfunding") {   // powerfunding PERCENT: the power department's funding (as the budget window sets it)
            int percent = 100;
            words >> percent;
            native::NativeSetPowerFunding(percent);
        } else if (command == "demolish") {   // demolish X0 Z0 X1 Z1: the bulldozer's commit on a cell rectangle (the player's privilege, effects on)
            sc4::DemolitionRegion region;
            words >> region.bounds.x0 >> region.bounds.z0 >> region.bounds.x1 >> region.bounds.z1;
            std::int64_t cost = 0;
            const bool ok = native::DemolishRegion(true, region, 1, 1, false, &cost);
            std::printf("demolish %d,%d..%d,%d -> %s, cost %lld\n", region.bounds.x0, region.bounds.z0, region.bounds.x1, region.bounds.z1,
                        ok ? "ok" : "refused", static_cast<long long>(cost));
        } else if (command == "walls") {   // walls: the lot retaining walls and their models
            native::NativeWallReport();
        } else if (command == "ports") {   // ports: the airport and seaport developers' ports
            native::NativePortReport();
        } else if (command == "landfill") {   // landfill: the landfill zones, their garbage and tiles, and the city's garbage figures
            native::NativeLandfillReport();
        } else if (command == "pipebursts") {   // pipebursts: every burst's leaking and waiting pipes
            native::NativePipeBurstReport();
        } else if (command == "netlotpothole") {   // netlotpothole X Z [0|1]: the pothole message's handler on a cell
            int x = 0, z = 0, damaged = 1;
            words >> x >> z >> damaged;
            native::NativeNetworkLotPothole(x, z, damaged != 0);
        } else if (command == "netlotsverify") {   // netlotsverify: restyle every piece and compare with the saved style
            native::NativeNetworkLotVerify();
        } else if (command == "netlots") {   // netlots X Z [R]: the network lot manager and the pieces around a cell
            int x = 0, z = 0, r = 1;
            words >> x >> z >> r;
            native::NativeNetworkLotReport(x, z, r);
        } else if (command == "pipecells") {   // pipecells X Z: the pipes and network pieces on the 3x3 cells around a cell
            int x = 0, z = 0;
            words >> x >> z;
            native::NativeUndergroundCellsReport(x, z);
        } else if (command == "subways") {   // subways [N]: the subway pieces and subway connections of the city (the first N of each)
            int limit = 12;
            words >> limit;
            native::NativeSubwayReport(limit);
        } else if (command == "fires") {   // fires: the fire disaster's state, one line per fire
            native::NativeFireReport();
        } else if (command == "policepower") {   // policepower X0 Z0 X1 Z1 STEP: the police power map the riots read (read-only)
            int x0 = 0, z0 = 0, x1 = 0, z1 = 0, step = 8;
            words >> x0 >> z0 >> x1 >> z1 >> step;
            native::NativePolicePowerReport(x0, z0, x1, z1, step);
        } else if (command == "riot") {   // riot X Z: a riot starts on the road nearest to a city cell (what a mayor rating hotspot does)
            int x = 0, z = 0;
            words >> x >> z;
            native::NativeStartRiot(x, z);
        } else if (command == "riots") {   // riots: every riot's phase and leaders
            native::NativeRiotReport();
        } else if (command == "querylist") {   // querylist [SKIPUI] [N]: buildings with a query window other than SKIPUI (hex)
            std::string skip;
            int limit = 40;
            words >> skip >> limit;
            native::NativeQueryReport(skip.empty() ? 0u : static_cast<std::uint32_t>(std::strtoul(skip.c_str(), nullptr, 16)), limit);
        } else if (command == "pedestrians") {   // pedestrians [N]: the pedestrian manager's count and the first N on screen
            int limit = 8;
            words >> limit;
            native::NativePedestrianReport(limit);
        } else if (command == "dispatchat") {   // dispatchat TYPE PX PY: the same, to the ground under a screen point
            std::uint32_t type = 0;
            int px = 0, py = 0;
            words >> type >> px >> py;
            float hit[3] = {};
            sc4::cSC4View3DWin* view = gAppWindow ? gAppWindow->View3D() : nullptr;
            if (view && view->PickTerrain(px, py, hit, true))
                std::printf("dispatchat %u %d,%d = %.0f,%.0f: id %u\n", type, px, py, hit[0], hit[2], native::NativeDispatchAt(type, hit[0], hit[2]));
            else
                std::printf("dispatchat %u %d,%d: no ground\n", type, px, py);
        } else if (command == "dispatchfigure") {   // dispatchfigure N [DX DZ]: police to the Nth crime figure (group 0x4102), offset by DX,DZ
            int n = 0;
            float dx = 0.0f, dz = 0.0f;
            words >> n >> dx >> dz;
            const float lo[3] = {-3.4e38f, -3.4e38f, -3.4e38f}, hi[3] = {3.4e38f, 3.4e38f, 3.4e38f};
            std::vector<sc4::cISC4Occupant*> all;
            native::NativePedestriansInBox(lo, hi, all);
            int seen = 0;
            bool sent = false;
            for (sc4::cISC4Occupant* o : all) {
                if (!sent && o->IsOccupantGroup(0x4102) && seen++ == n) {
                    float p[3] = {};
                    o->GetPosition(p);
                    std::printf("dispatchfigure %d at %.0f,%.0f: id %u\n", n, p[0], p[2], native::NativeDispatchAt(0, p[0] + dx, p[2] + dz));
                    sent = true;
                }
                o->Release();
            }
            if (!sent) std::printf("dispatchfigure %d: no such figure (%d)\n", n, seen);
        } else if (command == "dispatch") {   // dispatch TYPE X Z: send a unit (0 police car, 1 fire engine, 5 fire plane, 6 police helicopter) to a world position
            std::uint32_t type = 0;
            float x = 0.0f, z = 0.0f;
            words >> type >> x >> z;
            std::printf("dispatch %u %.0f,%.0f: id %u\n", type, x, z, native::NativeDispatchAt(type, x, z));
        } else if (command == "look") {   // look X Z: the camera's target to a world position (on the ground)
            float x = 0.0f, z = 0.0f;
            words >> x >> z;
            sc4::cSC43DRender* render = native::CityRenderer();
            if (sc4::cSC4CameraControl* camera = render ? render->GetCameraControl() : nullptr) {
                cS3DVector3 target;
                target.x = x;
                target.y = 270.0f;
                target.z = z;
                native::AnnounceCameraScroll(true);
                camera->SetViewTargetPosition(target);
                native::AnnounceCameraScroll(false);
                std::printf("look %.0f,%.0f\n", x, z);
            }
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
        } else if (command == "wait-days") {   // run frames until the simulator is DAYS days further (or LIMIT ms passed): a wait in game time
            int days = 0, limit = 120000; words >> days >> limit;
            const std::uint32_t start = native::NativeSimDayNumber();
            const double began = NowMs(), end = began + limit;
            while (native::NativeSimDayNumber() - start < static_cast<std::uint32_t>(days) && NowMs() < end) { Frame(); Nap(); }
            const std::uint32_t passed = native::NativeSimDayNumber() - start;
            std::printf("scenario %d: %u days in %.0f ms\n", number, passed, NowMs() - began);
            if (passed < static_cast<std::uint32_t>(days)) { std::printf("scenario %d: wait-days gave up\n", number); return 5; }
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
        } else if (command == "where" || command == "tapid") {
            // where HEXID: the screen rectangle of the first window with that id (finding a button to tap);
            // tapid HEXID: the same, then a single click at its center.
            std::string hex;
            words >> hex;
            const std::uint32_t id = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
            if (cIGZWin* w = gMgr->GetMainWindow()->GetChildWindowFromIDRecursive(id)) {
                cRZRect a = w->GetArea();
                int x = a.left, y = a.top;
                for (cIGZWin* p = w->GetParentWin(); p; p = p->GetParentWin()) { const cRZRect pa = p->GetArea(); x += pa.left; y += pa.top; }
                const int cx = x + (a.right - a.left) / 2, cy = y + (a.bottom - a.top) / 2;
                std::printf("%s %08x: %d,%d %dx%d visible=%d center %d,%d\n", command.c_str(), id, x, y, a.right - a.left,
                            a.bottom - a.top, w->IsVisible(), cx, cy);
                if (command == "tapid") {   // a real SDL click (as sdlclick) at the center, in window points
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(gWindow, &ww, &wh);
                    const float x = float(cx) * float(ww) / float(gW), y = float(cy) * float(wh) / float(gH);
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
                }
            } else {
                std::printf("%s %08x: not found\n", command.c_str(), id);
            }
        } else if (command == "adlink") {   // adlink ADVISORID CODE: follow an advice link to CODE for that advisor
            int advisor = 0;
            words >> advisor;
            std::string code;
            std::getline(words, code);
            native::NativeAdviceLinkProbe(advisor, code.c_str() + (code.empty() ? 0 : 1));
        } else if (command == "cursors") {   // the game cursors loaded into the cursor platform and the one in force
            std::printf("cursors: %zu loaded, current %s\n", gCursorPlatform.NamedCount(),
                        gCursorPlatform.Current().empty() ? "(none)" : gCursorPlatform.Current().c_str());
        } else if (command == "advwin") {   // the advisor window's state
            native::NativeAdvisorWindowReport();
        } else if (command == "command") {   // command HEXID: a framework command run as a shortcut runs it (menus without keys)
            std::string hex;
            words >> hex;
            const std::uint32_t id = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
            std::printf("command %08x -> status %u\n", id, native::ExecuteGameCommand(id));
        } else if (command == "viewcmd") {   // viewcmd HEXID [P0]: the city view's command switch, as its toolbar buttons reach it (P0 = its first flag)
            std::string hex;
            int flag = 0;
            words >> hex >> flag;
            const std::uint32_t id = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
            sc4::cSC4View3DWin* view = native::CityView3DWin();
            std::printf("viewcmd %08x -> %s\n", id, view ? (view->ProcessViewCommand(id, flag != 0) ? "handled" : "not handled") : "no city view");
        } else if (command == "key") {
            // key [ctrl+][shift+][alt+]NAME [FRAMES]: a key press and release as SDL events through the player's key path
            // (NAME as SDL names keys: X, Left, F1, Escape, /, Keypad 8 ...; "Keypad 8" is written Keypad_8), the key held
            // down for FRAMES frames (default 1).
            std::string spec; words >> spec;
            int hold = 1; words >> hold;
            SDL_Keymod mod = SDL_KMOD_NONE;
            for (std::size_t plus; (plus = spec.find('+')) != std::string::npos && plus + 1 < spec.size();) {
                const std::string word = spec.substr(0, plus);
                if (word == "ctrl") mod |= SDL_KMOD_LCTRL;
                else if (word == "shift") mod |= SDL_KMOD_LSHIFT;
                else if (word == "alt") mod |= SDL_KMOD_LALT;
                else break;
                spec.erase(0, plus + 1);
            }
            for (char& c : spec) if (c == '_') c = ' ';
            const SDL_Keycode keycode = SDL_GetKeyFromName(spec.c_str());
            std::printf("key %s (mod %04x): keycode %08x\n", spec.c_str(), unsigned(mod), unsigned(keycode));
            for (const SDL_EventType type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
                SDL_Event k{};
                k.type = type; k.key.windowID = SDL_GetWindowID(gWindow); k.key.key = keycode; k.key.mod = mod;
                k.key.down = type == SDL_EVENT_KEY_DOWN;
                SDL_PushEvent(&k); PumpEvents(); Frame(); Nap();
                for (int i = 1; k.key.down && i < hold; ++i) { PumpEvents(); Frame(); Nap(); }
            }
            for (int i = 0; i < 5; ++i) { PumpEvents(); Frame(); Nap(); }
        } else if (command == "type") {
            // type TEXT: each character as an SDL text-input event through the player's path (what typing sends besides the
            // key presses: the characters text fields and the sign tool read); "_" stands for a space.
            std::string text; words >> text;
            for (char& c : text) if (c == '_') c = ' ';
            static std::string character;   // the event keeps a pointer: it lives until the event is pumped below
            for (char c : text) {
                character.assign(1, c);
                SDL_Event t{};
                t.type = SDL_EVENT_TEXT_INPUT; t.text.windowID = SDL_GetWindowID(gWindow); t.text.text = character.c_str();
                SDL_PushEvent(&t); PumpEvents(); Frame(); Nap();
            }
            std::printf("type \"%s\"\n", text.c_str());
            for (int i = 0; i < 5; ++i) { PumpEvents(); Frame(); Nap(); }
        } else if (command == "audio") {   // audio: the radio's station and song, the sound manager's sounds, the listener
            native::NativeAudioReport();
        } else if (command == "sound") {   // sound HEXID: starts a game sound by id, as a button or a tool does
            std::string id; words >> id;
            native::PlayGameSound(static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 16)));
            std::printf("sound %s\n", id.c_str());
        } else if (command == "cheats") {   // every registered cheat string, as the cheat code box's list has them
            auto* cheats = native::GameCheatCodeManager();
            std::uint32_t none = 0;
            const std::uint32_t count = cheats ? cheats->GetListOfAllCheatStrings(nullptr, none) : 0;
            std::vector<const cIGZString*> names(count);
            std::uint32_t got = count;
            if (count) cheats->GetListOfAllCheatStrings(names.data(), got);
            std::printf("cheats: %u registered:", count);
            for (std::uint32_t i = 0; i < got && i < count; ++i) std::printf(" \"%s\"", names[i]->ToChar());
            std::printf("\n");
        } else if (command == "savecity") {   // savecity: what the Save menu item does (cSC4App::RequestSaveCity) without the "saved" message box
            cSC4App* app = native::BootApplication();
            std::printf("savecity: %s\n", app && app->RequestSaveCity(false, false) ? "saved" : "not saved");
        } else if (command == "regionview") {   // regionview: back to the region without the save question (RequestGoToRegionView(false), after a savecity)
            cSC4App* app = native::BootApplication();
            std::printf("regionview: %s\n", app && app->RequestGoToRegionView(false) ? "done" : "refused");
        } else if (command == "regioncity") {   // regioncity: the open city's regional record (what the region view and the neighbors read)
            auto* record = dynamic_cast<cSC4RegionalCity*>(native::NativeCurrentRegionalCity());
            std::vector<cISC4RegionalCity::PointOfInterest> airports, seaports;
            if (!record) std::printf("regioncity: none\n");
            else record->GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestAirport, airports);
            if (record) record->GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestSeaport, seaports);
            if (record) std::printf("regioncity: population %d (R$ %d), workforce %.3f, commercial jobs %d, industrial jobs %d, mayor rating %d, tax R$ %.2f, tutorial %d, airports %zu, seaports %zu\n",
                        record->GetPopulation(), record->GetPopulation(0x1010), record->GetWorkforcePercentage(), record->GetCommercialJobs(), record->GetIndustrialJobs(),
                        record->GetMayorRating(), record->GetTaxRate(0x1010), record->GetTutorialGUID(), airports.size(), seaports.size());
        } else if (command == "open-region") {   // open-region NAME: switches the region screen to a region the way the Load Region dialog does (no input)
            std::string want; std::getline(words >> std::ws, want);
            auto* screen = gAppWindow ? dynamic_cast<sc4::cSC4WinRegionScreen*>(gAppWindow->CurrentScreen()) : nullptr;
            const bool loaded = screen && screen->LoadRegion(cRZString(want.c_str()));
            std::printf("open-region %s: %s\n", want.c_str(), loaded ? "loaded" : (!screen ? "no region screen" : "not loaded"));
        } else if (command == "open-city") {   // open-city NAME: enters a region city the way the region screen's launch does (no input)
            std::string want; std::getline(words >> std::ws, want);   // the rest of the line (names have spaces)
            auto lower = [](std::string t) { for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c))); return t; };
            sc4::IRegionScreenHost* host = sc4::RegionScreenHost();
            auto* screen = gAppWindow ? dynamic_cast<sc4::cSC4WinRegionScreen*>(gAppWindow->CurrentScreen()) : nullptr;
            std::vector<cISC4Region::cLocation> locations;
            if (host) host->CityLocations(locations);
            sc4::RegionCityRef* found = nullptr;
            for (const cISC4Region::cLocation& at : locations) {
                sc4::RegionCityRef* ref = host->FindCity(at.x, at.z);
                cRZString name;
                if (ref && ref->get()) ref->get()->GetName(name);
                std::printf("open-city: city at %d,%d \"%s\"\n", at.x, at.z, name.ToChar());
                if (!found && ref && lower(name.ToChar()) == lower(want)) found = ref;
            }
            const bool launched = screen && found && screen->LaunchRegionalCity(found);
            std::printf("open-city %s: %s\n", want.c_str(), launched ? "launched" : (!screen ? "no region screen" : !found ? "no such city" : "refused"));
        } else if (command == "snow") {   // snow [low high bias]: the effects manager's Snow cheat (a debug cheat the cheat box only takes in debug mode)
            std::string rest; std::getline(words >> std::ws, rest);
            const std::string text = "Snow " + rest;
            std::printf("snow: %s\n", native::RunSnowCheat(text.c_str()) ? "done" : "no effects manager");
        } else if (command == "cheat") {   // cheat TEXT: what the cheat box does with its text on Enter (the rest of the line)
            std::string text; std::getline(words >> std::ws, text);
            auto* cheats = dynamic_cast<cGZCheatCodeManager*>(SL::spCCM);
            std::uint32_t id = 0;
            const bool done = cheats && cheats->DoDefaultCheatCodeProcessing(cRZString(text.c_str()), id);
            std::printf("cheat %s: %s (id %08x)\n", text.c_str(), done ? "accepted" : "not accepted", id);
        } else if (command == "cmdtext") {   // cmdtext TEXT: a command line run as text through the command server (the debug console's path), with its output
            std::string text; std::getline(words >> std::ws, text);
            cIGZCommandServer* server = native::GameCommandServer();
            cIGZCommandParameterSet* output = nullptr;
            if (server) server->CreateCommandParameterSet(&output);
            const std::uint32_t status = server ? server->ExecuteCommand(text.c_str(), std::uint32_t(text.size()), output) : 2;
            cRZString shown;
            if (server && output) server->ConvertCommandOutputToString(output, shown);
            std::printf("cmdtext %s -> status %u output \"%s\"\n", text.c_str(), status, shown.ToChar());
            if (output) output->Release();
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

// The game's windows are closed here, while everything they use is still alive. Left to the window manager's destructor (a
// global's), the city view shut down after other globals were already destroyed (the sprite cache, the audio device): an effect
// that drives automata, stopped then, made them load a sprite from the freed cache.
void CloseWindows() {
    if (!gMgr) return;
    if (cIGZWin* desktop = gMgr->GetMainWindow()) desktop->ChildDeleteAll();
}

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
        else if (!strncasecmp(argv[i], "-intro:", 7)) gIntroSwitch = argv[i] + 7;   // the game's own switch (-intro:off)
    }
    const bool interactive = shot.empty() && scenario.empty();
    // A scenario or a shot starts at the region screen as before unless SC4_INTRO=on asks for the intro.
    if (!interactive && gIntroSwitch.empty()) {
        const char* intro = std::getenv("SC4_INTRO");
        gIntroSwitch = intro && !strcasecmp(intro, "on") ? "on" : "off";
    }

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
    sc4::cSC4CameraControl::SetDevicePixelScale(float(windowH) / float(gH));   // the city at window resolution, texel for pixel
    native::SetGameResolution(gW, gH, usable.w, usable.h);
    native::SetDisplayGeometry(windowW, windowH, gH);
    // The shot and scenario runs draw into a hidden window: the frame target is offscreen anyway, the window only holds the context.
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    gScriptPresent = !interactive && std::getenv("SC4_SCRIPTPRESENT") != nullptr;
    if (!interactive && !gScriptPresent) flags |= SDL_WINDOW_HIDDEN;
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
    native::SetSnapshotFrameReader([](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        return gDriver && gDriver->ReadBack(bgra, w, h);
    });
    // However main returns, the driver stops touching GL before the static windows are torn down after it (their buffers
    // delete textures; through a dead context that crashed at exit on NVIDIA).
    struct DetachAtReturn { ~DetachAtReturn() { if (gDriver) gDriver->DetachContext(); } } detachAtReturn;
    if (!gDriver) { std::printf("no usable OpenGL driver\n"); return 1; }
    gAudioWanted = interactive || std::getenv("SC4_AUDIO");
    Boot();
    native::SetLoadingScreenPresenter(&gLoadingPresenter);
    cRZKeyboard::SetKeyStateFunctions(&AsyncKeyState, &ToggleKeyState);
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
        // The original's modal loop idles the framework, so the message servers and the sound services keep ticking under a
        // dialog: what a dialog posts is delivered while it is open (the Audio Options dialog's switches, sliders, Preview
        // and Stop reach the radio at once) and the radio goes on to its next song.
        native::TickAudio();
        if (cSC4App* app = native::BootApplication()) {
            app->RunMessageServerPump(0, 64, 4);
            app->RunMessageServer2Pump(0, 64, 4);
            if (gMessages2) gMessages2->OnTick(16);
        }
        // the region map under the dialog: re-blitted every frame like Frame() does (the clouds keep drawing over it)
        const bool regionScreen = gAppWindow && gAppWindow->CurrentScreen() && gAppWindow->CurrentScreen()->GetID() == 0xea659793;
        if (regionScreen && native::DrawRegionLayers()) InvalidateTree(gMgr->GetMainWindow());
        if (gAppWindow && gAppWindow->View3D()) InvalidateTree(gMgr->GetMainWindow());
        // A dialog pauses the simulation but not the animation (cSC4AutoGamePause(false, false, true, true)): the advisor
        // heads keep talking and the props keep moving under it.
        if (gAppWindow && gAppWindow->View3D() && native::StartAnimationTicks()) native::TickAnimation();
        gMgr->Pump();
        gMgr->Plot();
        gGraphics->Services().Tick();
        Present();
        // debug: SC4_MODALINPUT="FRAME:X:Y,..." clicks inside the dialog at those frames of it (FRAME:type:TEXT, FRAME:key:VK type,
        // FRAME:shot:NAME photographs it); SC4_MODALFRAMES its length (30)
        if (!interactive) {
            // each item fires once: a dialog's own popup (a combo's list) runs a nested modal loop that re-enters this pump at
            // the same frame, and a re-posted click would land outside the popup
            static std::set<std::string> fired;
            // "N@FRAME:..." items fire only in the N-th dialog of the run (0-based) and count FRAME from that dialog's start
            static int lastOrdinal = -1, dialogFrame = 0;
            if (gMgr->modalCount != lastOrdinal) { lastOrdinal = gMgr->modalCount; dialogFrame = 0; std::printf("modal: dialog %d begins\n", lastOrdinal); }
            ++dialogFrame;
            if (const char* input = std::getenv("SC4_MODALINPUT")) {
                std::stringstream list(input);
                std::string entry;
                while (std::getline(list, entry, ',')) {
                    if (fired.count(entry)) continue;
                    std::string item = entry;
                    int current = frame + 1;   // plain items: the frame count since the last dialog that ran its full length
                    if (const std::size_t atSign = entry.find('@'); atSign != std::string::npos) {
                        if (std::atoi(entry.c_str()) != lastOrdinal) continue;
                        item = entry.substr(atSign + 1);
                        current = dialogFrame;
                    }
                    int at = -1, x = 0, y = 0;
                    char text[128] = {};
                    // "FRAME:type:TEXT" types the characters, "FRAME:key:VK" presses and releases one virtual key
                    if (std::sscanf(item.c_str(), "%d:type:%127[^,]", &at, text) == 2) {
                        if (at != current) continue;
                        fired.insert(entry);
                        for (const char* c = text; *c; ++c) gMgr->GZWinMsgPost_Keyboard(kGZMsgCharacter, std::uint32_t(*c), 0);
                        std::printf("modal input: type \"%s\" at frame %d\n", text, at);
                        continue;
                    }
                    if (std::sscanf(item.c_str(), "%d:shot:%127[^,]", &at, text) == 2) {   // FRAME:shot:NAME writes work/shots/NAME.png
                        if (at != current) continue;
                        fired.insert(entry);
                        WritePng(std::string("work/shots/") + text + ".png");
                        continue;
                    }
                    if (std::sscanf(item.c_str(), "%d:key:%d", &at, &x) == 2) {
                        if (at != current) continue;
                        fired.insert(entry);
                        gMgr->GZWinMsgPost_Keyboard(kGZMsgKeyDown, std::uint32_t(x), 0);
                        if (x == 0x0d || x == 0x08 || x == 0x09 || x == 0x1b)   // the character the player's key path adds (HandleEvent)
                            gMgr->GZWinMsgPost_Keyboard(kGZMsgCharacter, std::uint32_t(x), 0);
                        gMgr->GZWinMsgPost_Keyboard(kGZMsgKeyUp, std::uint32_t(x), 0);
                        std::printf("modal input: key %d at frame %d\n", x, at);
                        continue;
                    }
                    if (std::sscanf(item.c_str(), "%d:%d:%d", &at, &x, &y) == 3 && at == current) {
                        fired.insert(entry);
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
    if (!native::VerifyRuntimeWiring(gDriver.get(), native::AudioRunning())) { std::fprintf(stderr, "boot halted: a required dependency is not wired\n"); return 3; }
    if (!scenario.empty()) {
        const int result = RunScenario(scenario);
        CloseWindows();
        return result;
    }

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
        CloseWindows();
        return 0;
    }
    int n = 0;
    while (!gQuit) {
        PumpEvents();
        Frame();
        Present();
        if (frames > 0 && ++n >= frames) break;
    }
    CloseWindows();
    native::AudioPreAppShutdown();   // the sound services and the output: its voices are SDL audio streams, they go before SDL_Quit
    if (gDriver) gDriver->DetachContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(gWindow);
    SDL_Quit();
    return 0;
}
