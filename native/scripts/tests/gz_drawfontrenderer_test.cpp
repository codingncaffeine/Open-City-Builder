// cGZDrawFontRenderer: direct mode (lockable target -> inner buffer renderer) and context mode (glyph atlas + Blt).
#include <cstdio>
#include <cstring>

#include "fake_drawcontext.h"
#include "gz/DrawFontRenderer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct FakeTarget : cIGZBufferDrawTarget {
    gz::cIGZDrawContext* ctx = nullptr;
    int locks = 0, unlocks = 0, refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZBuffer* AsIGZBuffer() override { return nullptr; }
    gz::cIGZDrawContext* GetDrawContext() override { return ctx; }
    gz::cIGZDrawContext* LockContext() override { ++locks; return ctx; }
    void UnlockContext() override { ++unlocks; }
    void Init(gz::cIGZDrawContext*, cIGZBuffer*) override {}
    void ResetSize() override {}
    void SetBlitSource(cIGZBuffer*) override {}
};

// A buffer that also answers the draw target interface.
struct TargetBuffer : cGZBuffer {
    FakeTarget* target = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZBufferDrawTarget && target) { *out = target; target->AddRef(); return true; }
        return cGZBuffer::QueryInterface(iid, out);
    }
};

struct FakeBufferRenderer : cIGZFontRenderer {
    int rc = 1, begins = 0, ends = 0, drawn = 0;
    sGlyphInfo lastFirst;
    cRZRect lastClip{-1, -1, -1, -1};
    bool hadClip = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++rc; }
    std::uint32_t Release() override { return --rc; }
    bool Init(cIGZFont*) override { return true; }
    bool Shutdown(cIGZFont*) override { return true; }
    bool DrawBegin(cIGZFont*, void*, eRenderingMode) override { ++begins; return true; }
    bool DrawEnd(cIGZFont*, void*, eRenderingMode) override { ++ends; return true; }
    bool DrawChars(cIGZFont*, void*, sGlyphInfo* g, std::uint32_t n, eRenderingMode, const std::uint32_t*, const cRZRect* clip) override {
        drawn += n; if (n) lastFirst = g[0]; hadClip = clip != nullptr; if (clip) lastClip = *clip; return true;
    }
};

struct Host : IDrawFontRendererHost {
    FakeBufferRenderer* inner = new FakeBufferRenderer();
    cIGZFontRenderer* CreateBufferFontRenderer() override { return inner; }
    cIGZBuffer* CreateBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
    void NativeColorToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { r = n >> 16; g = n >> 8; b = n; }
};

}  // namespace

int main() {
    Host host;
    auto* r = new cGZDrawFontRenderer(&host);
    r->AddRef();
    cIGZFont* font = reinterpret_cast<cIGZFont*>(0x1000);
    cIGZFont* font2 = reinterpret_cast<cIGZFont*>(0x2000);
    fake_dc::Ctx* heapCtx = nullptr;

    // texture pages: coverage -> white with the alpha nibble
    r->CreateTexturePages();
    CHECK(r->Atlas() && r->Atlas()->Width() == 256 && r->Atlas()->GetColorType().type == kBufferColorARGB4444);
    const std::uint32_t* lut = r->CoverageTable();
    CHECK(lut[0] == 0x0fff && lut[0xff] == 0xffff && lut[0x80] == 0x8fff && lut[0x7f] == 0x7fff && lut[0x1f] == 0x1fff);
    CHECK(r->PrepChar(0, nullptr, nullptr, 0));
    r->SetGlyphCacheDimensionsHint(64, 32);
    CHECK(r->Atlas()->Width() == 64 && r->Atlas()->Height() == 32 && r->PenX() == 0);
    r->SetGlyphCacheDimensionsHint(256, 256);

    // direct mode: plain buffer, glyphs shifted by the origin
    auto* plain = new cGZBuffer();
    plain->AddRef();
    plain->Init(16, 16, {kBufferColorARGB8888, 32});
    CHECK(r->DrawBegin(font, plain, cIGZFontRenderer::kAlpha) && r->DirectMode() && host.inner->begins == 1);
    cIGZFontRenderer::sGlyphInfo g[2];
    std::uint8_t px[4] = {1, 2, 3, 4};
    g[0] = {1, 2, 3, 4, px, 2, 2, 2, 'a'};
    g[1] = {5, 6, 7, 8, px, 2, 2, 2, 'b'};
    std::uint32_t color = 0x112233;
    cRZRect clip{0, 0, 10, 10};
    CHECK(r->DrawChars(font, plain, g, 2, cIGZFontRenderer::kAlpha, &color, &clip));
    CHECK(host.inner->drawn == 2 && host.inner->hadClip && host.inner->lastClip.right == 10 && g[0].left == 1);
    CHECK(r->DrawEnd(font, plain, cIGZFontRenderer::kAlpha) && host.inner->ends == 1);

    // direct mode with a lockable context: the context origin offsets everything
    {
        fake_dc::Ctx ctx;
        ctx.lockResult = true;
        ctx.origin = {10, 20};
        FakeTarget tgt;
        tgt.ctx = &ctx;
        TargetBuffer tb;
        tb.AddRef();
        tb.Init(8, 8, {kBufferColorARGB8888, 32});
        tb.target = &tgt;
        CHECK(r->DrawBegin(font, &tb, cIGZFontRenderer::kAlpha) && r->DirectMode() && r->OriginX() == 10);
        CHECK(tgt.locks == 1 && tgt.unlocks == 1 && tgt.refs == 1);
        g[0] = {1, 2, 3, 4, px, 2, 2, 2, 'a'};
        r->DrawChars(font, &tb, g, 1, cIGZFontRenderer::kAlpha, &color, &clip);
        CHECK(g[0].left == 11 && g[0].top == 22 && g[0].right == 13 && g[0].bottom == 24);
        CHECK(host.inner->lastClip.left == 10 && host.inner->lastClip.bottom == 30);
        r->DrawEnd(font, &tb, cIGZFontRenderer::kAlpha);
    }

    // context mode (the renderer keeps a counted reference to the context: heap allocated)
    {
        fake_dc::Ctx& ctx = *new fake_dc::Ctx;
        heapCtx = &ctx;
        ctx.lockResult = false;
        FakeTarget tgt;
        tgt.ctx = &ctx;
        TargetBuffer tb;
        tb.AddRef();
        tb.Init(8, 8, {kBufferColorARGB8888, 32});
        tb.target = &tgt;
        CHECK(r->DrawBegin(font, &tb, cIGZFontRenderer::kAlpha) && !r->DirectMode());
        CHECK(ctx.refs == 2 && tgt.refs == 1);
        // two glyphs of the same character share one atlas spot; a third is 4x3
        std::uint8_t cov1[6] = {0xff, 0x80, 0x00, 0x10, 0x20, 0x30};
        std::uint8_t cov2[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        cIGZFontRenderer::sGlyphInfo gl[3];
        gl[0] = {10, 10, 12, 13, cov1, 2, 2, 3, 'a'};
        gl[1] = {20, 10, 22, 13, cov1, 2, 2, 3, 'a'};
        gl[2] = {30, 10, 34, 13, cov2, 4, 4, 3, 'b'};
        std::uint32_t rgb = 0x00ff8040;
        ctx.log.clear();
        CHECK(r->DrawChars(font, &tb, gl, 3, cIGZFontRenderer::kAlpha, &rgb, nullptr));
        // the first pass finds nothing, the second creates the glyphs, the third pass draws what was created
        CHECK(ctx.log.size() >= 1 && ctx.log[0] == "color " + std::to_string(0xffff8040u));
        int blts = 0;
        for (auto& l : ctx.log) if (l.rfind("blt", 0) == 0) ++blts;
        CHECK(blts == 3);
        CHECK(r->CachedGlyphCount(font) == 2 && r->PenX() == 2 + 4 && r->PenY() == 0 && r->RowHeight() == 3);
        // atlas pixels: white with the coverage nibble in the alpha
        auto* atlas = r->Atlas();
        CHECK(atlas->GetPixel(0, 0) == 0xffff && atlas->GetPixel(1, 0) == 0x8fff && atlas->GetPixel(0, 1) == 0x0fff);
        CHECK(atlas->GetPixel(2, 0) == 0x0fff && atlas->GetPixel(5, 2) == 0x0fff);
        bool sawBlt = false;
        for (auto& l : ctx.log) if (l == "blt 0,0,2,3>10,10,12,13") sawBlt = true;
        CHECK(sawBlt);
        // a second draw of cached glyphs creates nothing
        ctx.log.clear();
        r->DrawChars(font, &tb, gl, 2, cIGZFontRenderer::kAlpha, &rgb, nullptr);
        CHECK(r->PenX() == 6 && r->CachedGlyphCount(font) == 2);
        // another font gets its own spots
        r->DrawEnd(font, &tb, cIGZFontRenderer::kAlpha);
        CHECK(tgt.unlocks == 1);
        r->DrawBegin(font2, &tb, cIGZFontRenderer::kAlpha);
        r->DrawChars(font2, &tb, gl, 1, cIGZFontRenderer::kAlpha, &rgb, nullptr);
        CHECK(r->CachedGlyphCount(font2) == 1 && r->PenX() == 8);
        r->DrawEnd(font2, &tb, cIGZFontRenderer::kAlpha);
        CHECK(r->Shutdown(font2) && r->CachedGlyphCount(font2) == 0);
        // row wrapping and flush: glyphs 100x100 in a 256x256 atlas
        std::vector<std::uint8_t> big(100 * 100, 0x40);
        cIGZFontRenderer::sGlyphInfo bg{0, 0, 100, 100, big.data(), 100, 100, 100, 'x'};
        r->DrawBegin(font, &tb, cIGZFontRenderer::kAlpha);
        const auto* a = r->CreateCachedGlyphImg(font, bg, false);   // pen (6,0)... 106 fits
        CHECK(a && a->x == 8 && a->y == 0);
        bg.character = 'y';
        const auto* b = r->CreateCachedGlyphImg(font, bg, false);   // 106+100=206 fits
        CHECK(b && b->x == 108 && b->y == 0);
        bg.character = 'z';
        const auto* c = r->CreateCachedGlyphImg(font, bg, false);   // 206+100 > 256: next row at 100
        CHECK(c && c->x == 0 && c->y == 100 && r->RowHeight() == 100);
        bg.character = 'w';
        r->CreateCachedGlyphImg(font, bg, false);
        bg.character = 'v';
        r->CreateCachedGlyphImg(font, bg, false);                    // third row would end at 300: refused
        CHECK(r->CreateCachedGlyphImg(font, bg, false) == nullptr);
        const std::size_t before = r->CachedGlyphCount(font);
        const auto* f2 = r->CreateCachedGlyphImg(font, bg, true);   // flush and start over
        CHECK(before > 1 && f2 && f2->x == 0 && f2->y == 0 && r->CachedGlyphCount(font) == 1);
        r->DrawEnd(font, &tb, cIGZFontRenderer::kAlpha);
    }

    void* q = nullptr;
    CHECK(r->QueryInterface(0xa8bc2a75, &q) && q == r && !r->QueryInterface(2, &q));
    CHECK(r->Release() == 1 && r->Release() == 0);
    plain->Release();
    delete heapCtx;   // the renderer only held a counted reference
    delete host.inner;
    if (failures == 0) std::printf("gz_drawfontrenderer OK\n");
    return failures == 0 ? 0 : 1;
}
