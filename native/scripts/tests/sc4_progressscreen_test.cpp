// cSC4ProgressScreen: the fonts, tips and buffers Init sets up, the tip rotation and title of SetCaption, the default snapshot
// DoMessage falls back to, the thumbnail blend and the centred text of Repaint, and the obliteration screen.
#include <cstdio>
#include <string>
#include <vector>

#include "gz/Buffer.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/ProgressScreen.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Draw { int x, y; std::string text; std::uint32_t color; };

struct FakeFont final : cIGZFont {
    int height = 10;
    int refs = 0;
    std::vector<Draw>* draws = nullptr;
    int locks = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int GetLineHeight() override { return height; }
    int GetStringWidth(const void*, std::uint32_t length) override { return 6 * static_cast<int>(length); }
    bool DrawTextLine(void*, int x, int y, const void* text, std::uint32_t length, std::uint32_t color, std::uint32_t, const cRZRect*, cRZPoint*) override {
        draws->push_back({x, y, std::string(static_cast<const char*>(text), length), color});
        return true;
    }
    bool LockSurfaceForFontUse(void*) override { ++locks; return true; }
    bool UnlockSurfaceForFontUse(void*) override { --locks; return true; }
    bool Init() override { return {}; }
    bool Shutdown() override { return {}; }
    void GetName(cIGZString& out) const override {}
    std::uint32_t GetStyle() const override { return {}; }
    bool GetStyle(int style) const override { return {}; }
    bool SetStyle(int style, bool on) override { return {}; }
    std::uint32_t GetType() const override { return {}; }
    std::uint32_t GetSize() const override { return {}; }
    bool SetSize(std::uint32_t size) override { return {}; }
    std::uint32_t GetFontColor() const override { return {}; }
    std::uint32_t GetFontBackgroundColor() const override { return {}; }
    void SetFontColor(std::uint32_t color, std::uint32_t background) override {}
    int GetAntialiasing() override { return {}; }
    void SetAntialiasing(int mode) override {}
    float GetAntialiasingGammaCorrection() override { return {}; }
    void SetAntialiasingGammaCorrection(float gamma) override {}
    float GetAntialiasingSharpness() override { return {}; }
    void SetAntialiasingSharpness(float sharpness) override {}
    void GetFilePath(cIGZString& out) const override {}
    void SetFilePath(const cIGZString& path) override {}
    bool GetCodePage(int& codePage) const override { return {}; }
    bool SetCodePage(int codePage) override { return {}; }
    bool IsCodePageAvailable(int codePage) override { return {}; }
    bool IsCharEncodingAvailable(int encoding) override { return {}; }
    bool IsCharAvailable(void* chars, std::uint32_t length) override { return {}; }
    bool PreloadChars(void* chars, std::uint32_t length) override { return {}; }
    void GetScaling(float& x, float& y) const override {}
    void SetScaling(float x, float y) override {}
    void GetAdvanceScaling(float& x, float& y) const override {}
    void SetAdvanceScaling(float x, float y) override {}
    void GetSpacingAdjustment(int& x, int& y) const override {}
    void SetSpacingAdjustment(int x, int y) override {}
    int CharWidth(const void* chars, std::uint32_t length) override { return {}; }
    int CharHeight(const void* chars, std::uint32_t length) override { return {}; }
    int GetSizeAboveBaseline() override { return {}; }
    int GetSizeBelowBaseline() override { return {}; }
    int GetAdvanceWidth(const void* chars, std::uint32_t length) override { return {}; }
    std::uint32_t CalculateWordsToFitInWidth(const void* text, std::uint32_t length, std::uint32_t width, std::uint32_t* fitted, WordWrapMode mode) override { return {}; }
    std::uint32_t CalculateCharsToFitInWidth(const void* text, std::uint32_t length, std::uint32_t width, std::uint32_t* fitted) override { return {}; }
    std::uint32_t CalculateNumberOfLines(const void* text, std::uint32_t length, std::uint32_t width) override { return {}; }
    std::uint32_t CalculateWidthOfLines(const void* text, std::uint32_t length, std::uint32_t width) override { return {}; }
    bool CalculateTextArea(const void* text, std::uint32_t length, cRZRect& area, bool a, bool b) override { return {}; }
    bool CalculateTextArea(const void* text, std::uint32_t length, cRZRect& area, int a, int b, int c) override { return {}; }
    bool TextArea(const void* text, std::uint32_t length, cRZRect& area) override { return {}; }
    int GetStringVisibleWidth(const void* text, std::uint32_t length, std::uint32_t& visible) override { return {}; }
    bool FormatForEllipsis(cIGZString& out, const cIGZString& in, const cRZRect& area, bool a) override { return {}; }
    bool DrawTextParagraph(void* surface, const cRZRect& area, const void* text, std::uint32_t length, int alignment, std::uint32_t color, std::uint32_t background, const cRZRect* clip, cRZPoint* end) override { return {}; }
    cIGZFontRenderer* GetRenderer() const override { return {}; }
    void SetRenderer(cIGZFontRenderer* renderer) override {}
};

cIGZBuffer* MakeImage(int w, int h, const std::vector<std::uint32_t>& pixels) {
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    buffer->Init(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), cGZBufferColorType{kBufferColorARGB8888, 32});
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) reinterpret_cast<std::uint32_t*>(bits + y * buffer->GetColorSurfaceStride())[x] = pixels[y * w + x];
    return buffer;
}

struct Host final : IProgressScreenHost {
    FakeFont line, title;
    std::vector<Draw> draws;
    std::vector<std::uint32_t> added, removed, imagesAsked;
    bool cityThumbnails = false;
    int begins = 0, ends = 0, shows = 0, clears = 0;
    cRZRect shownSource, shownTarget;
    float clearColor[4] = {};
    Host() { line.draws = &draws; title.draws = &draws; title.height = 14; }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t type) override { added.push_back(type); }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override { removed.push_back(type); }
    cIGZFont* Font(std::uint32_t id) override { return id == kProgressLineFontId ? &line : id == kProgressTitleFontId ? &title : nullptr; }
    cIGZBuffer* CreateScreenBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
    cIGZBuffer* LoadImage(std::uint32_t, std::uint32_t instance) override {
        imagesAsked.push_back(instance);
        if (instance == kProgressBackgroundInstance) return MakeImage(1, 1, {0x00204080});
        if (instance == kDefaultSnapInstances[5][0]) return MakeImage(2, 2, {0x00ff0000, 0x0000ff00, 0x00000080, 0x000000ff});
        if (instance == kDefaultSnapInstances[5][1]) return MakeImage(2, 2, {0xffffffff, 0x00000000, 0x80808080, 0xffffffff});
        return nullptr;
    }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        if (group == kProgressTipGroup && instance >= kProgressFirstTipInstance && instance < kProgressFirstTipInstance + 3) {
            out.AssignFromCString(("tip " + std::to_string(instance - kProgressFirstTipInstance)).c_str());
            return true;
        }
        if (group == kProgressTextGroup && instance == kProgressObliteratingInstance) { out.AssignFromCString("Obliterating"); return true; }
        return false;
    }
    bool ReadCityThumbnails(cIGZBuffer** image, cIGZBuffer** mask) override { *image = nullptr; *mask = nullptr; return cityThumbnails; }
    int BaseTerrainType() override { return 1; }
    int CitySizeType() override { return 2; }
    void BeginScreenDraw() override { ++begins; }
    void ShowBuffer(cIGZBuffer*, const cRZRect& source, const cRZRect& target) override { ++shows; shownSource = source; shownTarget = target; }
    void ClearScreen(float r, float g, float b, float a) override { ++clears; clearColor[0] = r; clearColor[1] = g; clearColor[2] = b; clearColor[3] = a; }
    void EndScreenDraw() override { ++ends; }
};

void SendCityInitBegins(cSC4ProgressScreen& screen) {
    cRZMessage2Standard message;
    message.SetType(kMsgCityInitBegins);
    CHECK(screen.DoMessage(&message));
}
}  // namespace

int main() {
    // ---- the loading screen
    {
        Host host;
        const cRZRect area{0, 0, 100, 80};
        auto* screen = new cSC4ProgressScreen(&host, area, false);
        screen->AddRef();
        screen->Init();
        CHECK(screen->Initialized());
        CHECK(host.added.size() == 1 && host.added[0] == kMsgCityInitBegins);
        CHECK(screen->TipsLeft() == 3);
        CHECK(host.line.refs == 1 && host.title.refs == 1);
        CHECK(screen->ScreenBuffer() && screen->ScreenBuffer()->Width() == 100 && screen->ScreenBuffer()->Height() == 80);

        // the title clears the lines under it; without a thumbnail nothing is drawn or shown
        screen->SetCaption(0, cRZString("Loading Town"));
        CHECK(std::string(screen->Caption(0).ToChar()) == "Loading Town");
        CHECK(screen->Caption(1).Strlen() == 0);
        CHECK(screen->TipsLeft() == 2);   // a tip was taken (the timer starts a second in), then erased by the title
        CHECK(host.begins == 1 && host.shows == 0 && host.ends == 0);

        // the city has no pictures of its own: the default snapshot of terrain type 1, size 2
        SendCityInitBegins(*screen);
        CHECK(screen->HasThumbnail());
        bool askedImage = false, askedMask = false;
        for (std::uint32_t asked : host.imagesAsked) {
            askedImage |= asked == kDefaultSnapInstances[5][0];
            askedMask |= asked == kDefaultSnapInstances[5][1];
        }
        CHECK(askedImage && askedMask);
        cRZMessage2Standard other;
        other.SetType(0x26d31ec1);
        CHECK(!screen->DoMessage(&other));

        // index 1: the empty line takes the next tip; the text passed is not used
        screen->SetCaption(1, cRZString("ignored"));
        const std::string tip = screen->Caption(1).ToChar();
        CHECK(tip.rfind("tip ", 0) == 0);
        CHECK(std::string(screen->Caption(0).ToChar()) == "Loading Town");
        CHECK(screen->TipsLeft() == 1);
        CHECK(host.shows == 1 && host.ends == 1);
        CHECK(host.shownTarget.left == 0 && host.shownTarget.right == 100 && host.shownTarget.bottom == 80);
        CHECK(host.shownSource.right == 100 && host.shownSource.bottom == 80);
        // within the second the tip stays
        screen->SetCaption(1, cRZString());
        CHECK(screen->Caption(1).ToChar() == tip);
        CHECK(screen->TipsLeft() == 1);

        // the text: title centred at top + 12 in the title font, the tip under it (title height + 2) in the line font
        CHECK(host.draws.size() >= 2);
        const std::size_t first = host.draws.size() - 2;
        const Draw& t = host.draws[first];
        const Draw& l = host.draws[first + 1];
        CHECK(t.text == "Loading Town" && t.y == 12 && t.x == (100 - 6 * 12) / 2);
        CHECK(l.text == tip && l.y == 12 + 14 + 2 && l.x == (100 - 6 * static_cast<int>(tip.size())) / 2);
        cIGZBuffer* s = screen->ScreenBuffer();
        CHECK(t.color == s->ConvertRGBValueToNative(0xff, 0xff, 0xff));
        CHECK(host.line.locks == 0);

        // the thumbnail: 2 x 2 centred across ((100 - 2) / 2 = 49) and lifted ((80 - 2 - 60) / 2 = 9); a full mask shows the
        // (premultiplied) image, an empty one the background tile, half a mask half of each
        const std::uint32_t tile = s->GetPixel(0, 0);
        CHECK(s->GetPixel(49, 9) == s->ConvertRGBValueToNative(0xff, 0, 0));
        CHECK(s->GetPixel(50, 9) == tile);
        CHECK(s->GetPixel(50, 10) == s->ConvertRGBValueToNative(0, 0, 0xff));
        std::uint8_t tr = 0, tg = 0, tb = 0, r = 0, g = 0, b = 0;
        s->ConvertNativeValueToRGB(tile, tr, tg, tb);
        s->ConvertNativeValueToRGB(s->GetPixel(49, 10), r, g, b);
        const auto half = [](std::uint8_t v, std::uint8_t add) { return static_cast<std::uint8_t>(((v * (0xff - 0x80) + 0x80) >> 8) + add); };
        std::uint8_t er = 0, eg = 0, eb = 0;
        s->ConvertNativeValueToRGB(s->ConvertRGBValueToNative(half(tr, 0), half(tg, 0), half(tb, 0x80)), er, eg, eb);
        CHECK(r == er && g == eg && b == eb);
        CHECK(s->GetPixel(48, 9) == tile && s->GetPixel(51, 11) == tile);

        screen->Shutdown();
        CHECK(!screen->Initialized());
        CHECK(host.removed.size() == 1 && host.removed[0] == kMsgCityInitBegins);
        CHECK(screen->TipsLeft() == 0 && !screen->ScreenBuffer());
        CHECK(host.line.refs == 0 && host.title.refs == 0);
        screen->Release();
    }
    // ---- the city's own pictures win: no default snapshot is asked for
    {
        Host host;
        host.cityThumbnails = true;
        auto* screen = new cSC4ProgressScreen(&host, cRZRect{0, 0, 50, 50}, false);
        screen->AddRef();
        screen->Init();
        host.imagesAsked.clear();
        SendCityInitBegins(*screen);
        CHECK(host.imagesAsked.empty());
        screen->Release();
    }
    // ---- an obliterated city: a white frame, the red "obliterating" line
    {
        Host host;
        auto* screen = new cSC4ProgressScreen(&host, cRZRect{0, 0, 120, 90}, true);
        screen->AddRef();
        screen->Init();
        screen->SetCaption(0, cRZString("ignored"));
        CHECK(host.clears == 1 && host.clearColor[0] == 1.0f && host.clearColor[3] == 1.0f);
        CHECK(host.begins == 0 && host.shows == 1 && host.ends == 1);
        CHECK(!host.draws.empty() && host.draws.back().text == "Obliterating" && host.draws.back().y == 12);
        CHECK(host.draws.back().color == screen->ScreenBuffer()->ConvertRGBValueToNative(0xff, 0, 0));
        screen->Release();
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("sc4_progressscreen ok\n");
    return 0;
}
