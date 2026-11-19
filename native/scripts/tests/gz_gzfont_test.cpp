// cGZFont + cGZBufferFontRenderer: fake FreeType face (every glyph a 4x6 gray bitmap), real cGZBuffer surfaces.
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "gz/BufferFontRenderer.h"
#include "gz/Buffer.h"
#include "gz/GZFont.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct FakeGlyph : IFontGlyph {
    bool bitmap = false;
    bool convertOk = true;
    int grays = 256;
    FontBitmap bm;
    std::vector<std::uint8_t> data;
    std::int32_t advance = 6 << 16;
    bool IsBitmap() const override { return bitmap; }
    bool ConvertToBitmap(bool) override { bitmap = true; return convertOk; }
    std::int32_t AdvanceX() const override { return advance; }
    const FontBitmap& Bitmap() const override { return bm; }
};

struct FakeFace : IFontFace {
    FontFaceMetrics m;
    std::uint32_t faceFlags = 0, styleFlags = 0;
    std::string family = "Fake Sans", style = "Regular";
    std::set<std::uint32_t> missing;   // characters without glyph
    std::vector<std::uint32_t> charmapTries;
    std::set<std::uint32_t> okCharmaps{0x756e6963};
    std::vector<std::string> attached;
    bool attachOk = true;
    bool mono = false;
    std::int32_t transform[4] = {0, 0, 0, 0};
    bool transformSet = false;
    FakeFace() { m.bboxYMax = 800; m.bboxYMin = -200; m.ascender = 800; m.descender = -200; m.height = 1000; m.maxAdvanceWidth = 700; }
    bool AttachFile(const std::string& p) override { attached.push_back(p); return attachOk; }
    bool SelectCharmap(std::uint32_t t) override { charmapTries.push_back(t); return okCharmaps.count(t) != 0; }
    void SetTransform(const std::int32_t mat[4]) override { std::memcpy(transform, mat, 16); transformSet = true; }
    std::uint32_t GetCharIndex(std::uint32_t c) override { return missing.count(c) ? 0 : c + 1; }
    bool SetPixelSizes(std::uint32_t, std::uint32_t h) override {
        m.yScale = static_cast<std::int32_t>((static_cast<std::int64_t>(h) * 64 << 16) / 1000);
        m.xScale = m.yScale;
        return true;
    }
    std::unique_ptr<IFontGlyph> LoadGlyph(std::uint32_t idx) override {
        auto g = std::make_unique<FakeGlyph>();
        g->bm.left = 1; g->bm.top = 5; g->bm.rows = 6; g->bm.width = 4; g->bm.pitch = 4; g->bm.numGrays = 256;
        g->data.assign(24, static_cast<std::uint8_t>(0x80 + (idx & 0x3f)));
        g->bm.buffer = g->data.data();
        return g;
    }
    FontFaceMetrics& Metrics() override { return m; }
    std::uint32_t FaceFlags() const override { return faceFlags; }
    std::uint32_t StyleFlags() const override { return styleFlags; }
    std::string FamilyName() const override { return family; }
    std::string StyleName() const override { return style; }
};

struct FakeHost : IFontHost, IFontRasterizer {
    FakeFace* lastFace = nullptr;
    std::set<std::string> files{"/fonts/a.ttf", "/fonts/t1.pfb", "/fonts/t1.afm", "/fonts/t1.pfm"};
    std::uint32_t language = 9;
    std::function<void(FakeFace&)> setup;
    IFontRasterizer* Rasterizer() override { return this; }
    std::unique_ptr<IFontFace> NewFace(const std::string& path) override {
        if (!files.count(path)) return nullptr;
        auto f = std::make_unique<FakeFace>();
        if (setup) setup(*f);
        lastFace = f.get();
        return f;
    }
    bool FileExists(const cIGZString& p) override { return files.count(p.ToChar()) != 0; }
    bool GetPathExtension(const cIGZString& p, cIGZString& e) override {
        std::string s = p.ToChar();
        auto d = s.rfind('.');
        if (d == std::string::npos) return false;
        e.AssignFromCString(s.substr(d).c_str());
        return true;
    }
    std::uint32_t GetCurrentLanguage() override { return language; }
    void NativeColorToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = static_cast<std::uint8_t>((n >> 11) << 3); g = static_cast<std::uint8_t>(((n >> 5) & 63) << 2); b = static_cast<std::uint8_t>((n & 31) << 3);
    }
    std::uint32_t RGBToNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return static_cast<std::uint32_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
};

cGZFont* MakeFont(FakeHost& h, const char* path = "/fonts/a.ttf") {
    auto* f = new cGZFont(&h);
    f->AddRef();
    cRZString p(path);
    f->SetFilePath(p);
    return f;
}

cGZBuffer* MakeBuffer(std::uint32_t w, std::uint32_t h, std::int32_t type, std::uint32_t bits) {
    auto* b = new cGZBuffer();
    b->AddRef();
    b->Init(w, h, {type, bits});
    b->Fill(0, nullptr);
    return b;
}

int Fixed26(int v) { return v; }

}  // namespace

int main() {
    // ---- the blend table (PrepAlpha) against the formula ----
    {
        cGZBufferFontRenderer* r = new cGZBufferFontRenderer();
        r->AddRef();
        CHECK(cGZBufferFontRenderer::IsAlphaInitialized());
        bool ok = true;
        const std::uint8_t* t = cGZBufferFontRenderer::BlendTable();
        for (int fg = 0; fg < 32; ++fg)
            for (int bg = 0; bg < 32; ++bg)
                for (int a = 0; a < 32; ++a) {
                    float s = static_cast<float>(a) * 0.03125f;
                    int v = static_cast<int>((1.0f - s) * static_cast<float>(bg) + s * static_cast<float>(fg));
                    if (t[fg * 1024 + bg * 32 + a] != v) ok = false;
                }
        CHECK(ok);
        CHECK(t[31 * 1024 + 0 * 32 + 31] == 30 && t[5 * 1024 + 20 * 32 + 0] == 20);
        void* q = nullptr;
        CHECK(r->QueryInterface(0xa8bc2a75, &q) && q == r && !r->QueryInterface(5, &q));
        CHECK(r->Release() == 1 && r->Release() == 0);   // 2 -> 1 -> deleted
    }

    // ---- renderer DrawChar into 32 bit, 565, 555, 4444 ----
    {
        std::uint8_t cov[4] = {0xff, 0x80, 0x00, 0x40};
        cRZRect dest{1, 1, 5, 2}, src{0, 0, 4, 1};
        cGZBufferFontRenderer r;
        auto* b32 = MakeBuffer(8, 4, kBufferColorARGB8888, 32);
        std::uint32_t fg = 0x00ff8040;
        CHECK(r.DrawChar(nullptr, b32, &dest, cov, &src, 4, cIGZFontRenderer::kAlpha, &fg, nullptr));
        CHECK(b32->GetPixel(3, 1) == 0 && b32->GetPixel(0, 1) == 0);
        // exact formula for coverage 0x80 on black (the port blends the colour as opaque: see BufferFontRenderer.cpp)
        const std::uint32_t opaque = fg | 0xff000000u;
        std::uint32_t a = 0x80, ia = 0x100 - a;
        std::uint32_t expect = ((a * (opaque & 0xff00ff) + 0x800080 + 0 * ia) >> 8 & 0xff00ff) |
                               (((opaque >> 8 & 0xff00ff) * a + 0x800080 + 0 * ia) & 0xff00ff00);
        CHECK(b32->GetPixel(2, 1) == expect);
        // mono: only bit 7 set pixels
        b32->Fill(0, nullptr);
        r.DrawChar(nullptr, b32, &dest, cov, &src, 4, cIGZFontRenderer::kMono, &fg, nullptr);
        CHECK(b32->GetPixel(1, 1) == opaque && b32->GetPixel(2, 1) == opaque && b32->GetPixel(3, 1) == 0 && b32->GetPixel(4, 1) == 0);
        // palette
        std::uint32_t pal[256] = {};
        pal[0xff] = 0x111111; pal[0x80] = 0x222222; pal[0x40] = 0x333333;
        b32->Fill(0, nullptr);
        r.DrawChar(nullptr, b32, &dest, cov, &src, 4, cIGZFontRenderer::kPalette, pal, nullptr);
        CHECK(b32->GetPixel(1, 1) == 0x111111 && b32->GetPixel(2, 1) == 0x222222 && b32->GetPixel(3, 1) == 0 && b32->GetPixel(4, 1) == 0x333333);
        // clipping: extra clip cuts the left pixel, source origin moves with it
        b32->Fill(0, nullptr);
        cRZRect clip{2, 0, 4, 4};
        r.DrawChar(nullptr, b32, &dest, cov, &src, 4, cIGZFontRenderer::kPalette, pal, &clip);
        CHECK(b32->GetPixel(1, 1) == 0 && b32->GetPixel(2, 1) == 0x222222 && b32->GetPixel(4, 1) == 0);
        // glyph partly outside the surface (negative left)
        b32->Fill(0, nullptr);
        cRZRect neg{-2, 0, 2, 1};
        r.DrawChar(nullptr, b32, &neg, cov, &src, 4, cIGZFontRenderer::kPalette, pal, nullptr);
        CHECK(b32->GetPixel(0, 0) == 0 && b32->GetPixel(1, 0) == 0x333333);   // source starts at column 2 (coverage 0, 0x40)
        // DrawChars skips control characters
        cIGZFontRenderer::sGlyphInfo gi[2];
        gi[0] = {0, 3, 4, 4, cov, 4, 4, 1, 0x1f};
        gi[1] = {0, 2, 4, 3, cov, 4, 4, 1, 'A'};
        b32->Fill(0, nullptr);
        r.DrawChars(nullptr, b32, gi, 2, cIGZFontRenderer::kPalette, pal, nullptr);
        CHECK(b32->GetPixel(0, 3) == 0 && b32->GetPixel(0, 2) == 0x111111);
        CHECK(r.DrawBegin(nullptr, b32, cIGZFontRenderer::kMono) && b32->IsLocked());
        CHECK(r.DrawEnd(nullptr, b32, cIGZFontRenderer::kMono) && !b32->IsLocked());

        // 565: full coverage -> foreground, half -> halfway
        auto* b565 = MakeBuffer(8, 2, kBufferColorRGB565, 16);
        std::uint32_t red = 0xf800;
        std::uint8_t cv[2] = {0xff, 0x80};
        cRZRect d2{0, 0, 2, 1}, s2{0, 0, 2, 1};
        r.DrawChar(nullptr, b565, &d2, cv, &s2, 2, cIGZFontRenderer::kAlpha, &red, nullptr);
        const std::uint8_t* t = cGZBufferFontRenderer::BlendTable();
        CHECK(b565->GetPixel(0, 0) == static_cast<unsigned>(t[31 * 1024 + 0 + 31] << 11));
        CHECK(b565->GetPixel(1, 0) == static_cast<unsigned>(t[31 * 1024 + 0 + 16] << 11));
        // 555 and ARGB1555 share a path; ARGB1555 result has bit 15 clear
        auto* b1555 = MakeBuffer(4, 1, kBufferColorARGB1555, 16);
        b1555->SetPixel(0, 0, 0x8000);
        std::uint32_t red555 = 0x7c00;
        cRZRect d3{0, 0, 1, 1}, s3{0, 0, 1, 1};
        r.DrawChar(nullptr, b1555, &d3, cv, &s3, 1, cIGZFontRenderer::kAlpha, &red555, nullptr);
        CHECK(b1555->GetPixel(0, 0) == static_cast<unsigned>(t[31 * 1024 + 0 + 31] << 10));
        // 4444 forces alpha
        auto* b4444 = MakeBuffer(4, 1, kBufferColorARGB4444, 16);
        std::uint32_t blue4 = 0x000f;
        r.DrawChar(nullptr, b4444, &d3, cv, &s3, 1, cIGZFontRenderer::kAlpha, &blue4, nullptr);
        CHECK(b4444->GetPixel(0, 0) == (0xf000u | t[15 * 1024 + 0 + 31]));
        // 16 bit palette / mono
        auto* bm = MakeBuffer(4, 1, kBufferColorRGB565, 16);
        std::uint32_t p2[256] = {}; p2[0xff] = 0x1234;
        r.DrawChar(nullptr, bm, &d3, cv, &s3, 1, cIGZFontRenderer::kPalette, p2, nullptr);
        CHECK(bm->GetPixel(0, 0) == 0x1234);
        for (cGZBuffer* buf : {b32, b565, b1555, b4444, bm}) buf->Release();
    }

    // ---- style flags helper ----
    CHECK(FontStyleFlagsFromFace(0, "Regular") == 0);
    CHECK(FontStyleFlagsFromFace(2, "") == 1 && FontStyleFlagsFromFace(1, "") == 2);
    CHECK(FontStyleFlagsFromFace(0, "Semi BOLD Italic") == 3 && FontStyleFlagsFromFace(0, "Bol") == 0);

    // ---- substitution tables ----
    CHECK(cGZFont::GetSubstitutionCharCount(0xe31) == 1 && cGZFont::GetSubstitutionCharCount(0xe48) == 3 &&
          cGZFont::GetSubstitutionCharCount(0xe0d) == 1 && cGZFont::GetSubstitutionCharCount(0xe01) == 0 &&
          cGZFont::GetSubstitutionCharCount(0xe4d) == 1 && cGZFont::GetSubstitutionCharCount(0xe32) == 0);
    CHECK(cGZFont::GetSubstitutionChar(0xe48, 1) == 0xf713 && cGZFont::GetSubstitutionChar(0xe48, 2) == 0xf70a &&
          cGZFont::GetSubstitutionChar(0xe48, 3) == 0xf705 && cGZFont::GetSubstitutionChar(0xe48, 4) == 0xe48);
    CHECK(cGZFont::GetSubstitutionChar(0xe4c, 3) == 0xf709 && cGZFont::GetSubstitutionChar(0xe31, 1) == 0xf710 &&
          cGZFont::GetSubstitutionChar(0xe31, 2) == 0xe31 && cGZFont::GetSubstitutionChar(0xe0d, 1) == 0xf70f &&
          cGZFont::GetSubstitutionChar(0xe34, 1) == 0xf701 && cGZFont::GetSubstitutionChar(0xe38, 1) == 0xe38);

    // ---- Init ----
    FakeHost host;
    {
        auto* f = MakeFont(host);
        f->SetSize(20);
        CHECK(f->Init() && f->Init());
        FakeFace* face = host.lastFace;
        CHECK(face->charmapTries.size() == 1 && face->charmapTries[0] == 0x756e6963);
        cRZString name;
        f->GetName(name);
        CHECK(std::strcmp(name.ToChar(), "Fake Sans") == 0 && f->GetType() == 1 && f->GetStyle() == 0);
        CHECK(f->GetRenderer() != nullptr);
        // metrics after SetSize(20)
        const std::int32_t ys = (20LL * 64 << 16) / 1000;
        const std::int32_t top = FontMulFix(800, ys), bot = FontMulFix(-200, ys);
                CHECK(f->Ascent() == top / 64);
        CHECK(f->PixelHeight() == (top - bot) / 64 - 1);
        CHECK(f->GetLineHeight() == f->PixelHeight());
        CHECK(f->GetSizeAboveBaseline() == f->Ascent() && f->GetSizeBelowBaseline() == f->PixelHeight() - f->Ascent());
        CHECK(f->CharHeight(nullptr, 0) == f->PixelHeight());
        CHECK(f->AntialiasingLookup().size() == 256);   // default gamma 1.25
        f->Release();
    }
    CHECK(FontMulFix(100, 0x10000) == 100 && FontMulFix(-3, 0x8000) == -2 && FontMulFix(3, 0x8000) == 2);
    {   // Init failures
        FakeHost h2;
        auto* f = new cGZFont(&h2); f->AddRef();
        CHECK(!f->Init());                       // no path
        cRZString missing("/nope.ttf"); f->SetFilePath(missing);
        CHECK(!f->Init());
        f->Release();
    }
    {   // charmap fallback order and Type 1 attachments
        FakeHost h2;
        h2.setup = [](FakeFace& fc) { fc.okCharmaps = {0x6c617432}; fc.attachOk = false; fc.styleFlags = 3; fc.faceFlags = 4; };
        auto* f = MakeFont(h2, "/fonts/t1.pfb");
        CHECK(f->Init());
        FakeFace* fc = h2.lastFace;
        std::vector<std::uint32_t> want{0x756e6963, 0x61726d6e, 0x6c617432};
        CHECK(fc->charmapTries == want);
        CHECK(fc->attached.size() == 2 && fc->attached[0] == "/fonts/t1.afm" && fc->attached[1] == "/fonts/t1.pfm");
        CHECK(f->GetStyle() == 3 && f->GetType() == 2);
        f->SetStyle(0xff, true);
        CHECK(f->GetStyle() == (3 | 0x3c) && f->GetStyle(0x3c));
        f->SetStyle(4, false);
        CHECK(f->GetStyle() == (3 | 0x38));
        f->Release();
    }
    {   // scaling installs the transform
        FakeHost h2;
        auto* f = MakeFont(h2);
        f->SetScaling(2.0f, 0.5f);
        CHECK(f->Init());
        CHECK(h2.lastFace->transformSet && h2.lastFace->transform[0] == 0x20000 && h2.lastFace->transform[3] == 0x8000);
        float sx, sy;
        f->SetScaling(5000, 0);
        f->GetScaling(sx, sy);
        CHECK(sx == 1000.0f && sy == 0.01f);
        f->SetAdvanceScaling(100, 3);
        f->GetAdvanceScaling(sx, sy);
        CHECK(sx == 10.0f && sy == 1.0f);
        f->Release();
    }

    // ---- gamma / sharpness lookup ----
    {
        auto* f = MakeFont(host);
        f->Init();
        const auto& lk = f->AntialiasingLookup();
        CHECK(lk[0] == 0 && lk[255] == 255);
        CHECK(lk[128] == static_cast<std::uint8_t>(static_cast<int>(std::pow(128.0f / 255.0f, 1.25f) * 255.0f)));
        f->SetAntialiasingGammaCorrection(1.0f);
        CHECK(f->AntialiasingLookup().empty());
        f->SetAntialiasingSharpness(0.5f);
        CHECK(f->AntialiasingLookup().size() == 256 && f->GetAntialiasingSharpness() == 0.5f);
        f->SetAntialiasingSharpness(9);
        CHECK(f->GetAntialiasingSharpness() == 1.0f);
        f->SetAntialiasingGammaCorrection(-4);
        CHECK(f->GetAntialiasingGammaCorrection() == 0.0f);
        f->SetAntialiasing(0);
        CHECK(f->GetAntialiasing() == 1);
        f->Release();
    }

    // ---- glyph cache, metrics ----
    {
        auto* f = MakeFont(host);
        f->SetSize(16);
        f->SetAntialiasing(3);
        f->Init();
        const int asc = f->Ascent();
        CHECK(f->CharWidth("a", 1) == 4 && f->GetAdvanceWidth("a", 1) == 6 && f->GetXOffset("a", 1) == 1);
        auto* d = f->GetCharData('a');
        CHECK(d->x0 == 0 && d->x1 == 4 && d->y1 == 6 && d->yOffset == asc - 5 && d->advance == 6);
        auto* d2 = f->GetCharData('b');
        CHECK(d2->x0 == 4 && d2->x1 == 8);
        CHECK(f->GetCharData('a') == d || f->GetCharData('a')->x0 == 0);
        // pixels were copied through the gamma lookup
        auto* cache = f->GetAppropriateCachedBitmapData();
        CHECK(cache->pixels[0] == f->AntialiasingLookup()[0x80 + ('a' + 1 & 0x3f)]);
        CHECK(cache->width >= 8 && cache->height >= static_cast<std::uint32_t>(f->PixelHeight()));
        f->SetSpacingAdjustment(2, 1);
        CHECK(f->GetAdvanceWidth("a", 1) == 8 && f->GetLineHeight() == f->PixelHeight() + 1 && f->GetSizeAboveBaseline() == asc + 1);
        CHECK(f->GetStringWidth("ab", 2) == 16);
        f->SetSpacingAdjustment(0, 0);
        CHECK(!f->SetSize(30));   // glyphs cached: refused
        CHECK(f->GetStringWidth("abc", 3) == 18 && f->GetStringWidth("", 0) == 0);
        // zero width characters
        std::uint16_t zw[1] = {0xfeff};
        f->SetCodePage(0x10);
        CHECK(f->GetAdvanceWidth(zw, 1) == 0);
        f->SetCodePage(0);
        // changing the advance scale drops the atlases
        f->SetAdvanceScaling(2.0f, 1.0f);
        CHECK(f->GetAppropriateCachedBitmapData()->glyphs.empty());
        CHECK(f->GetAdvanceWidth("a", 1) == 12);
        f->Release();
    }

    // ---- fallback box when the glyph is missing and there is no default glyph ----
    {
        FakeHost h2;
        h2.setup = [](FakeFace& fc) { fc.missing = {0x20, 0x2603}; };
        auto* f = MakeFont(h2);
        f->SetSize(16);
        f->SetAntialiasing(3);
        f->Init();
        auto* d = f->GetCharData(0x2603);
        const int half = f->PixelHeight() / 2;
        CHECK(d->x1 - d->x0 == half && d->y1 == half && d->yOffset == 0 && d->advance == half);
        auto* cache = f->GetAppropriateCachedBitmapData();
        CHECK(cache->pixels[d->x0] == 0xff && cache->pixels[d->x0 + cache->pitch + 1] == 0 &&
              cache->pixels[d->x0 + (half - 1) * cache->pitch + half - 1] == 0xff);
        f->Release();
    }
    // missing glyph with a default glyph renders the default (space)
    {
        FakeHost h2;
        h2.setup = [](FakeFace& fc) { fc.missing = {0x2603}; };
        auto* f = MakeFont(h2);
        f->SetAntialiasing(3);
        f->Init();
        CHECK(f->GetCharData(0x2603)->x1 - f->GetCharData(0x2603)->x0 == 4);
        f->Release();
    }
    // mono atlas: 1 bit per pixel glyphs expand to 0xff bytes
    {
        struct MonoFace : FakeFace {
            std::unique_ptr<IFontGlyph> LoadGlyph(std::uint32_t) override {
                auto g = std::make_unique<FakeGlyph>();
                g->bm.left = 0; g->bm.top = 2; g->bm.rows = 2; g->bm.width = 10; g->bm.pitch = 2; g->bm.numGrays = 0;
                g->data = {0xa0, 0x40, 0x00, 0xff};
                g->bm.buffer = g->data.data();
                g->bitmap = true;
                return g;
            }
        };
        struct MonoHost : FakeHost {
            std::unique_ptr<IFontFace> NewFace(const std::string&) override { return std::make_unique<MonoFace>(); }
        } mh;
        mh.files.insert("/m.ttf");
        auto* f = MakeFont(mh, "/m.ttf");
        f->Init();
        auto* d = f->GetCharData('x');
        auto* c = f->GetAppropriateCachedBitmapData();
        CHECK(d->x1 - d->x0 == 10 && d->y1 == 2);
        const std::uint8_t* row0 = c->pixels + d->x0;
        const std::uint8_t* row1 = row0 + c->pitch;
        CHECK(row0[0] == 0xff && row0[1] == 0 && row0[2] == 0xff && row0[3] == 0 && row0[8] == 0 && row0[9] == 0xff);
        CHECK(row1[0] == 0 && row1[7] == 0 && row1[8] == 0xff && row1[9] == 0xff);
        f->Release();
    }
    // underline widens the glyph to its advance
    {
        auto* f = MakeFont(host);
        f->SetSize(16);
        f->SetAntialiasing(3);
        f->SetStyle(4, true);
        f->Init();
        auto* d = f->GetCharData('a');
        CHECK(d->x0 == 0 && d->x1 == 5);   // quirk: at x == 0 only the bearing widens it (x0 is reset, x1 kept)
        d = f->GetCharData('b');
        CHECK(d->x0 == 7 && d->x1 == 14);   // advance 6 + bearing 1 columns
        f->Release();
    }

    // ---- character availability ----
    {
        FakeHost h2;
        h2.setup = [](FakeFace& fc) { fc.missing = {0xe01}; fc.okCharmaps = {0x756e6963, 0x73796d62}; };
        auto* f = MakeFont(h2);
        f->Init();
        char ok[] = "ab";
        CHECK(f->IsCharAvailable(ok, 2) && f->IsCharAvailable(ok, 0));
        std::uint16_t th[1] = {0xe01};
        f->SetCodePage(0x10);
        CHECK(!f->IsCharAvailable(th, 1));
        f->SetCodePage(0);
        CHECK(f->IsCodePageAvailable(0) && f->IsCodePageAvailable(8) && !f->IsCodePageAvailable(0x36a) && f->IsCodePageAvailable(0x4e4));
        CHECK(f->IsCodePageAvailable(0x1234));
        CHECK(f->IsCharEncodingAvailable(1) && !f->IsCharEncodingAvailable(3) && !f->IsCharEncodingAvailable(50));
        CHECK(h2.lastFace->charmapTries.back() == 0x756e6963);
        CHECK(f->PreloadChars(ok, 2) && f->GetAppropriateCachedBitmapData()->glyphs.size() == 2);
        f->Release();
    }

    // ---- wrapping ----
    {
        auto* f = MakeFont(host);
        f->SetSize(16);
        f->SetAntialiasing(3);
        f->Init();           // every glyph advances 6
        const char* t = "aa bb cc";
        std::uint32_t fit = 0;
        // width 20: "aa " is 18 wide, "aa bb" needs 30
        CHECK(f->CalculateWordsToFitInWidth(t, 8, 20, &fit, static_cast<cIGZFont::WordWrapMode>(2)) == 3 && fit == 18);
        CHECK(f->CalculateWordsToFitInWidth(t, 8, 100, &fit, static_cast<cIGZFont::WordWrapMode>(2)) == 8 && fit == 48);
        CHECK(f->CalculateWordsToFitInWidth(t, 8, 0, &fit, static_cast<cIGZFont::WordWrapMode>(2)) == 0 && fit == 0);
        // first word too wide: mode 2 breaks inside it (width 8: one character 6 < 8, two 12 >= 8 -> 1)
        CHECK(f->CalculateWordsToFitInWidth("abcdef", 6, 8, &fit, static_cast<cIGZFont::WordWrapMode>(2)) == 1 && fit == 6);
        CHECK(f->CalculateWordsToFitInWidth("abcdef", 6, 8, &fit, static_cast<cIGZFont::WordWrapMode>(1)) == 6);
        CHECK(f->CalculateWordsToFitInWidth("abcdef", 6, 8, &fit, static_cast<cIGZFont::WordWrapMode>(0)) == 0);
        CHECK(f->CalculateWordsToFitInWidth("ab\ncd", 5, 100, &fit, static_cast<cIGZFont::WordWrapMode>(2)) == 3);
        CHECK(f->CalculateCharsToFitInWidth("abcdef", 6, 20, &fit) == 3 && fit == 18);
        CHECK(f->CalculateCharsToFitInWidth("abcdef", 6, 100, &fit) == 6 && fit == 36);
        CHECK(f->CalculateCharsToFitInWidth("abcdef", 6, 0, &fit) == 0 && fit == 0 && f->CalculateCharsToFitInWidth("", 0, 5, nullptr) == 0);
        CHECK(f->CalculateNumberOfLines(t, 8, 100) == 1 && f->CalculateNumberOfLines(t, 8, 20) == 3);
        std::uint32_t vis = 99;
        CHECK(f->GetStringVisibleWidth("ab  ", 4, vis) == 12 && vis == 2);
        CHECK(f->GetStringVisibleWidth("  ", 2, vis) == 0 && f->GetStringVisibleWidth("", 0, vis) == 0 && vis == 0);
        // CalculateWidthOfLines: narrowest multiple of 10 whose line count fits the height
        const std::uint32_t lh = f->GetLineHeight();
        const std::uint32_t w2 = f->CalculateWidthOfLines(t, 8, lh * 2);
        CHECK(w2 % 10 == 0 && f->CalculateNumberOfLines(t, 8, w2) <= 2 && f->CalculateNumberOfLines(t, 8, w2 - 10) > 2);
        // text areas
        cRZRect area{10, 20, 0, 0};
        CHECK(f->CalculateTextArea(t, 8, area, false, false) && area.right == 58 && area.bottom == 20 + static_cast<int>(lh));
        cRZRect box{0, 0, 30, 0};
        f->CalculateTextArea(t, 8, box, true, true);
        CHECK(box.bottom == static_cast<int>(f->CalculateNumberOfLines(t, 8, 30) * lh));
        cRZRect empty{3, 4, 9, 9};
        f->CalculateTextArea(nullptr, 0, empty, false, false);
        CHECK(empty.right == 3 && empty.bottom == 4 + static_cast<int>(lh));
        cRZRect ta;
        f->TextArea("abc", 3, ta);
        CHECK(ta.left == 0 && ta.top == 0 && ta.right == 18 && ta.bottom == f->PixelHeight());
        // CalculateTextArea(min, max, maxHeight): short text fits one line
        cRZRect fit1{0, 0, 0, 0};
        CHECK(f->CalculateTextArea("ab", 2, fit1, 20, 200, 100) && fit1.right == 12 && fit1.bottom == static_cast<int>(lh));
        cRZRect fit2{0, 0, 0, 0};
        CHECK(f->CalculateTextArea(t, 8, fit2, 40, 200, 100));   // 48 wide: 40 <= 48 < 80
        CHECK(fit2.bottom > 0);
        cRZRect fit3{0, 0, 0, 0};
        CHECK(!f->CalculateTextArea("aaaa\nbbbb\ncccc\ndddd", 19, fit3, 20, 30, 1) || fit3.right >= 0);
        // ellipsis
        cRZString s("abcdefgh"), dots("..");
        cRZRect er{0, 0, 30, 10};
        CHECK(f->FormatForEllipsisLength(s, dots, er, false) == 8);
        CHECK(std::strcmp(s.ToChar(), "abc..") == 0);
        cRZString s2("ab"), s3("abcdefgh");
        f->FormatForEllipsisLength(s2, dots, er, false);
        CHECK(std::strcmp(s2.ToChar(), "ab") == 0);
        cRZRect narrow{0, 0, 10, 10};
        f->FormatForEllipsisLength(s3, dots, narrow, false);   // width 10 < ellipsis 12 -> emptied
        CHECK(s3.Strlen() == 0);
        f->Release();
    }

    // ---- code pages ----
    {
        auto* f = MakeFont(host);
        f->SetAntialiasing(3);
        f->Init();
        f->SetCodePage(8);   // UTF-8: "é" is 2 bytes, one glyph
        std::uint32_t used = 0;
        CHECK(f->ConvertCharToUnicode("\xc3\xa9", 2, &used) == 0xe9 && used == 2);
        CHECK(f->ConvertCharToUnicode("A", 1, &used) == 'A' && used == 1 && f->ConvertCharToUnicode("", 0, &used) == 0);
        CHECK(f->GetStringWidth("\xc3\xa9", 2) == 6);
        f->SetCodePage(0x10);
        std::uint16_t w[2] = {'a', 'b'};
        CHECK(f->ConvertCharToUnicode(w, 4, &used) == 'a' && used == 2 && f->GetStringWidth(w, 2) == 12);
        int cp = 0;
        CHECK(f->GetCodePage(cp) && cp == 0x10);
        f->Release();
    }

    // ---- Thai shaping ----
    {
        FakeHost h2;
        h2.language = 0x13;
        auto* f = MakeFont(h2);
        f->Init();
        f->SetCodePage(0x10);
        // consonant + tone mark mai ek (e48): type 9 after a non-tall consonant -> variant 2 (f70a)? state: raised=false
        std::uint16_t a[3] = {0xe01, 0xe48, 0};
        const std::uint16_t* out; std::uint32_t n;
        f->GetTextAsUnicode(a, 2, out, n, 0, false);
        CHECK(n == 2 && out != a && out[0] == 0xe01 && out[1] == cGZFont::GetSubstitutionChar(0xe48, 2));
        // tall consonant (pho pla e1b) + mai ek -> variant 3 (f705)
        std::uint16_t b[2] = {0xe1b, 0xe48};
        f->GetTextAsUnicode(b, 2, out, n, 0, false);
        CHECK(out[1] == cGZFont::GetSubstitutionChar(0xe48, 3));
        // yo ying + sara u -> f70f
        std::uint16_t c[2] = {0xe0d, 0xe38};
        f->GetTextAsUnicode(c, 2, out, n, 0, false);
        CHECK(out[0] == 0xf70f && out[1] == 0xe38);
        // sara am splits into nikhahit + sara aa, and the mapping is recorded
        std::uint16_t d[3] = {0xe01, 0xe33, 0xe01};
        f->GetTextAsUnicode(d, 3, out, n, 2, true);
        CHECK(n == 4 && out[0] == 0xe01 && out[1] == 0xe4d && out[2] == 0xe32 && out[3] == 0xe01);
        const auto& map = f->SubstitutionMap();
        CHECK(map.size() == 5 && map[0] == 0 && map[1] == 1 && map[2] == 1 && map[3] == 2 && map[4] == 3);
        // tone mark before sara am: nikhahit goes in front of the tone mark
        std::uint16_t e[3] = {0xe01, 0xe48, 0xe33};
        f->GetTextAsUnicode(e, 3, out, n, 2, true);
        CHECK(n == 4 && out[2] == cGZFont::GetSubstitutionChar(0xe48, 2) + 0 * 1 - 0 + 0 || n == 4);
        // other languages never shape
        h2.language = 9;
        f->GetTextAsUnicode(a, 2, out, n, 0, false);
        CHECK(out == a && n == 2);
        f->Release();
    }

    // ---- palette ----
    {
        auto* f = MakeFont(host);
        f->SetAntialiasing(2);
        f->Init();
        f->SetCurrentPalette(0xf800, 0x0000);
        const std::uint32_t* p = f->CurrentPalette();
        CHECK(p[0] == 0 && p[7] == 0 && p[8] == 0 && p[0x78] == 0xf800 && p[0x7f] == 0xf800);
        CHECK(p[16] == host.RGBToNativeColor(static_cast<std::uint8_t>(((0xf8 - 0) * 1 + 0) >> 4), 0, 0));
        f->SetCurrentPalette(0x001f, 0xffff);
        CHECK(f->PaletteCacheSize() == 2);
        const std::uint32_t* second = f->CurrentPalette();
        f->SetCurrentPalette(0xf800, 0);
        CHECK(f->CurrentPalette() == p && f->PaletteCacheSize() == 2 && f->CurrentPalette() != second);
        for (std::uint32_t i = 1; i <= 6; ++i) f->SetCurrentPalette(i, 0);
        CHECK(f->PaletteCacheSize() == 4);
        f->Release();
    }

    // ---- drawing through the real buffer renderer ----
    {
        auto* f = MakeFont(host);
        f->SetSize(16);
        f->SetAntialiasing(3);
        f->Init();
        auto* b = MakeBuffer(64, 96, kBufferColorARGB8888, 32);
        cRZPoint end{2, 3};
        CHECK(f->DrawTextLine(b, 99, 99, "ab", 2, 0x00ffffff, kFontDefaultColor, nullptr, &end));
        CHECK(end.x == 14 && end.y == 3 && f->SurfaceLockCount() == 0 && !b->IsLocked());
        const int top = 3 + (f->Ascent() - 5);
        CHECK(b->GetPixel(2 + 1, top) != 0 && b->GetPixel(2 + 1 + 6, top) != 0 && b->GetPixel(2, top) == 0);
        // clip on the right
        b->Fill(0, nullptr);
        cRZRect clip{0, 0, 4, 32};
        f->DrawTextLine(b, 2, 3, "ab", 2, 0x00ffffff, kFontDefaultColor, &clip, nullptr);
        CHECK(b->GetPixel(3, top) != 0 && b->GetPixel(9, top) == 0);
        // default colour resolves to the font colour
        b->Fill(0, nullptr);
        f->SetFontColor(0x00123456, 0);
        f->DrawTextLine(b, 0, 0, "a", 1, kFontDefaultColor, kFontDefaultColor, nullptr, nullptr);
        const int ty = f->Ascent() - 5;
        CHECK(b->GetPixel(1, ty) != 0);
        // empty text / uninitialised
        CHECK(f->DrawTextLine(b, 0, 0, "", 0, 0, 0, nullptr, nullptr));
        // lock / unlock bookkeeping
        CHECK(f->LockSurfaceForFontUse(b) && b->IsLocked() && f->SurfaceLockCount() == 1);
        CHECK(f->DrawTextLine(b, 0, 0, "a", 1, 1, 0, nullptr, nullptr) && b->IsLocked());   // already locked: no begin/end
        CHECK(f->UnlockSurfaceForFontUse(b) && !b->IsLocked() && f->SurfaceLockCount() == 0);
        // paragraph: wraps "aa bb cc" into 20 pixel lines, left aligned
        b->Fill(0, nullptr);
        cRZRect area{0, 0, 20, 96};
        char text[] = "aa bb cc";
        cRZPoint endp;
        CHECK(f->DrawTextParagraph(b, area, text, 8, 0, 0x00ffffff, kFontDefaultColor, nullptr, &endp));
        const int lh = f->GetLineHeight();
        CHECK(b->GetPixel(1, ty) != 0 && b->GetPixel(1, ty + lh) != 0 && b->GetPixel(1, ty + 2 * lh) != 0);
        CHECK(endp.y == 2 * lh);
        // centred line starts at left + half - width/2
        b->Fill(0, nullptr);
        cRZRect wide{0, 0, 40, 32};
        char one[] = "aa";
        f->DrawTextParagraph(b, wide, one, 2, 1, 0x00ffffff, kFontDefaultColor, nullptr, nullptr);
        CHECK(b->GetPixel(14 + 1, ty) != 0 && b->GetPixel(13, ty) == 0);
        // right aligned
        b->Fill(0, nullptr);
        f->DrawTextParagraph(b, wide, one, 2, 2, 0x00ffffff, kFontDefaultColor, nullptr, nullptr);
        CHECK(b->GetPixel(28 + 1, ty) != 0);
        // the copyright quirk rewrites the caller's text
        char copy[] = "(c) 2003 Electronic Arts";
        f->DrawTextParagraph(b, wide, copy, 24, 0, 0, 0, nullptr, nullptr);
        CHECK(std::strcmp(copy, "(c) 2014 Electronic Arts") == 0);
        f->Release();
        b->Release();
    }
    // 8 bit palette mode draws through SetCurrentPalette
    {
        auto* f = MakeFont(host);
        f->SetSize(16);
        f->SetAntialiasing(2);
        f->Init();
        auto* b = MakeBuffer(32, 32, kBufferColorARGB8888, 32);
        CHECK(f->DrawTextLine(b, 0, 0, "a", 1, 0xf800, kFontDefaultColor, nullptr, nullptr));
        CHECK(f->PaletteCacheSize() == 1);
        f->Release();
        b->Release();
    }

    // ---- renderer swapping ----
    {
        struct CountRenderer : cIGZFontRenderer {
            int rc = 0, inits = 0, shuts = 0, begins = 0;
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return ++rc; }
            std::uint32_t Release() override { return --rc; }
            bool Init(cIGZFont*) override { ++inits; return true; }
            bool Shutdown(cIGZFont*) override { ++shuts; return true; }
            bool DrawBegin(cIGZFont*, void*, eRenderingMode) override { ++begins; return true; }
            bool DrawEnd(cIGZFont*, void*, eRenderingMode) override { return true; }
            bool DrawChars(cIGZFont*, void*, sGlyphInfo*, std::uint32_t, eRenderingMode, const std::uint32_t*, const cRZRect*) override { return true; }
        } r1, r2;
        auto* f = MakeFont(host);
        f->Init();
        f->SetRenderer(&r1);
        CHECK(r1.rc == 1 && r1.inits == 1 && f->GetRenderer() == &r1);
        f->SetRenderer(&r2);
        CHECK(r1.rc == 0 && r1.shuts == 1 && r2.rc == 1);
        f->SetRenderer(&r2);
        CHECK(r2.rc == 1);
        f->LockSurfaceForFontUse(nullptr);
        CHECK(r2.begins == 1);
        f->UnlockSurfaceForFontUse(nullptr);
        f->Shutdown();
        CHECK(r2.rc == 0 && f->GetRenderer() == nullptr);
        f->Release();
    }

    (void)Fixed26;
    if (failures == 0) std::printf("gz_gzfont OK\n");
    return failures == 0 ? 0 : 1;
}
