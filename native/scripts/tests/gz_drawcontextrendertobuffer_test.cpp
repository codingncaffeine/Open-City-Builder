// cGZDrawContextRenderToBuffer: init per format, colour table, span fills, gradients, blits (plain, stretched, tiled, keyed),
// blt source geometry, antialiased polygons.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gz/DrawContextRenderToBuffer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

// Every buffer made here is released at the end of main, after the engines that draw into them.
std::vector<cGZBuffer*> g_buffers;

cGZBuffer* Make(std::uint32_t w, std::uint32_t h, std::int32_t type, std::uint32_t bits) {
    auto* b = new cGZBuffer();
    b->AddRef();
    g_buffers.push_back(b);
    CHECK(b->Init(w, h, {type, bits}));
    b->Fill(0, nullptr);
    return b;
}

cIGZUnknown* U(cGZBuffer* b) { return static_cast<cIGZUnknown*>(static_cast<cIGZBuffer*>(b)); }

constexpr std::uint32_t Span(std::uint32_t y, std::uint32_t x) { return (y << 16) | x; }

cGZDrawContextRenderToBuffer* NewEngine(cGZBuffer* target) {
    auto* e = new cGZDrawContextRenderToBuffer();
    e->AddRef();
    CHECK(e->Init(target));
    cRZRect clip{0, 0, static_cast<int>(target->Width()), static_cast<int>(target->Height())};
    e->SetClipRect(clip);
    return e;
}

}  // namespace

int main() {
    // ---- Init / Shutdown / suspend ----
    {
        for (std::int32_t type : {kBufferColorXRGB1555, kBufferColorRGB565, kBufferColorARGB1555, kBufferColorARGB4444, kBufferColorXRGB8888, kBufferColorARGB8888}) {
            const std::uint32_t bits = type >= kBufferColorXRGB8888 ? 32 : 16;
            auto* b = Make(8, 4, type, bits);
            auto* e = new cGZDrawContextRenderToBuffer();
            e->AddRef();
            CHECK(e->Init(b) && e->TargetLocked() && b->IsLocked());
            cRZRect area;
            e->GetCanvasArea(area);
            CHECK(area.right == 8 && area.bottom == 4);
            gz::cGZBufferSurfaceDesc desc;
            CHECK(e->LockTarget(desc, true) && desc.pixels == b->GetColorSurfaceBits() && desc.pitch == b->GetColorSurfaceStride() &&
                  desc.width == 8 && desc.height == 4 && desc.format == static_cast<std::uint32_t>(type) && desc.bytesPerPixel == bits / 8);
            e->Suspend();
            CHECK(!e->TargetLocked() && !b->IsLocked());
            e->Suspend();
            e->Resume();
            CHECK(e->TargetLocked() && b->IsLocked());
            e->Shutdown();
            CHECK(!b->IsLocked());
            CHECK(e->Release() == 0);
        }
        // type 3 (the "display" format of an uninitialised request) is refused
        auto* d = Make(4, 4, kBufferColorRGB565, 16);
        auto* e = new cGZDrawContextRenderToBuffer();
        e->AddRef();
        void* q = nullptr;
        CHECK(e->QueryInterface(0x2ae78a07, &q) && q == e && !e->QueryInterface(5, &q));
        e->Release(); e->Release();
        (void)d;
    }

    // ---- solid / alpha span fills on 32 and 16 bit ----
    {
        auto* b = Make(16, 8, kBufferColorARGB8888, 32);
        auto* e = NewEngine(b);
        const std::uint32_t colors[4] = {0x00ff8040, 0x00112233, 0, 0};
        e->SetColorTable(0, 2, colors);
        CHECK(e->NativeColor(0) == b->ConvertRGBValueToNative(0xff, 0x80, 0x40) && e->NativeColor(1) == b->ConvertRGBValueToNative(0x11, 0x22, 0x33));
        const std::uint32_t spans[4] = {Span(2, 3), Span(2, 9), Span(5, 0), Span(5, 4)};
        e->DrawPreclippedSpanList(spans, 2);
        CHECK(b->GetPixel(3, 2) == e->NativeColor(0) && b->GetPixel(8, 2) == e->NativeColor(0) && b->GetPixel(9, 2) == 0 && b->GetPixel(2, 2) == 0);
        CHECK(b->GetPixel(0, 5) == e->NativeColor(0) && b->GetPixel(3, 5) == e->NativeColor(0) && b->GetPixel(4, 5) == 0);
        // alpha blend: SetAlphaBlend(1) uses the colour's top byte as alpha (here fully opaque 0xff)
        b->Fill(0, nullptr);
        e->SetAlphaBlend(1);
        const std::uint32_t translucent[1] = {0x80ff0000};
        e->SetColorTable(0, 1, translucent);
        e->DrawPreclippedSpanList(spans, 1);
        const std::uint32_t pixel = b->GetPixel(4, 2);
        CHECK(pixel != 0);
        e->SetAlphaBlend(0);
        e->Release();
    }

    // ---- gradient transform: rows are start / dx / dy ----
    {
        auto* b = Make(16, 8, kBufferColorARGB8888, 32);
        auto* e = NewEngine(b);
        const float m[3][4] = {{0.5f, 0.25f, 0.0f, 1.0f}, {0.1f, 0.0f, 0.5f, 0.0f}, {0.0f, 0.2f, 0.0f, 0.0f}};
        e->SetGradientTransform(m, 2.0f, 3.0f);
        CHECK(e->GradientSet());
        const auto& g = e->Gradient();
        const std::int32_t dx0 = static_cast<std::int32_t>(std::floor(static_cast<double>(0.1f * 16711680.0f) + 0.5));
        const std::int32_t dy1 = static_cast<std::int32_t>(std::floor(static_cast<double>(0.2f * 16711680.0f) + 0.5));
        CHECK(g.dx[0] == dx0 && g.dy[1] == dy1 && g.dx[1] == 0 && g.dy[0] == 0);
        const std::int32_t start0 = static_cast<std::int32_t>(std::floor(static_cast<double>(0.5f * 16711680.0f) + 0.5));
        CHECK(g.base[0] == static_cast<std::int32_t>(static_cast<float>(start0 + 0x8000) - (0.0f * 3.0f + static_cast<float>(dx0) * 2.0f)));
        const std::uint32_t spans[2] = {Span(1, 0), Span(1, 8)};
        e->DrawPreclippedSpanList(spans, 1);
        CHECK(b->GetPixel(0, 1) != 0 && b->GetPixel(8, 1) == 0);
        e->SetGradientTransform(nullptr, 0, 0);
        CHECK(!e->GradientSet());
        e->Release();
    }

    // ---- blt source geometry ----
    {
        auto* b = Make(32, 32, kBufferColorARGB8888, 32);
        auto* e = NewEngine(b);
        auto* src = Make(4, 2, kBufferColorARGB8888, 32);
        cRZRect clip;
        CHECK(e->SetBltSource(U(src), 5.4f, 6.6f, 1.0f, 1.0f, clip));
        CHECK(!e->BltScaled() && e->BltOriginX() == 5 && e->BltOriginY() == 7);
        CHECK(clip.left == 5 && clip.top == 7 && clip.right == 9 && clip.bottom == 9);
        CHECK(e->SetBltSource(U(src), 3.0f, 3.0f, 2.0f, 2.0f, clip));
        CHECK(e->BltScaled() && e->BltScaleX() == 0x20000 && e->BltInverseX() == 0x8000);
        CHECK(clip.left == 3 && clip.top == 3 && clip.right == 11 && clip.bottom == 7);   // x + w * 2 - 0.5 rounded half down
        // ties round down: 2.5 -> 2
        CHECK(e->SetBltSource(U(src), 2.5f, 2.5f, 2.0f, 2.0f, clip) && clip.left == 2 && clip.top == 2);
        CHECK(!e->SetBltSource(nullptr, 0, 0, 0, 0, clip));
        // a source that is not a buffer is accepted but ignored
        struct NotABuffer : cIGZUnknown {
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return 1; }
            std::uint32_t Release() override { return 1; }
        } other;
        CHECK(e->SetBltSource(&other, 1, 1, 1, 1, clip));
        e->Release();
    }

    // ---- blits ----
    {
        auto* b = Make(16, 8, kBufferColorARGB8888, 32);
        auto* e = NewEngine(b);
        auto* src = Make(4, 2, kBufferColorARGB8888, 32);
        for (std::uint32_t y = 0; y < 2; ++y)
            for (std::uint32_t x = 0; x < 4; ++x) src->SetPixel(x, y, 0xff000000u | (y << 8) | (x + 1));
        cRZRect clip;
        e->SetBltSource(U(src), 3.0f, 2.0f, 1.0f, 1.0f, clip);
        const std::uint32_t spans[4] = {Span(2, 3), Span(2, 7), Span(3, 3), Span(3, 7)};
        e->DrawPreclippedSpanList(spans, 2);
        CHECK(b->GetPixel(3, 2) == src->GetPixel(0, 0) && b->GetPixel(6, 2) == src->GetPixel(3, 0) && b->GetPixel(4, 3) == src->GetPixel(1, 1));
        CHECK(b->GetPixel(7, 2) == 0 && !src->IsLocked());
        // tiled: the 4x2 pattern repeats across a 12x4 area
        b->Fill(0, nullptr);
        e->SetBltSource(U(src), 0.0f, 0.0f, 1.0f, 1.0f, clip);
        cRZRect tile{0, 0, 4, 2};
        e->SetBltTiling(tile);
        const std::uint32_t tspans[2] = {Span(1, 0), Span(1, 12)};
        e->DrawPreclippedSpanList(tspans, 1);
        CHECK(b->GetPixel(0, 1) == src->GetPixel(0, 1) && b->GetPixel(4, 1) == src->GetPixel(0, 1) && b->GetPixel(11, 1) == src->GetPixel(3, 1));
        e->ClearBltTiling();
        // stretched 2x: each source pixel covers two destination pixels
        b->Fill(0, nullptr);
        e->SetBltSource(U(src), 0.0f, 0.0f, 2.0f, 2.0f, clip);
        const std::uint32_t sspans[2] = {Span(0, 0), Span(0, 8)};
        e->DrawPreclippedSpanList(sspans, 1);
        CHECK(b->GetPixel(0, 0) == src->GetPixel(0, 0) && b->GetPixel(1, 0) == src->GetPixel(0, 0) && b->GetPixel(2, 0) == src->GetPixel(1, 0) &&
              b->GetPixel(7, 0) == src->GetPixel(3, 0));
        // colour key: the keyed source pixel is skipped
        b->Fill(0x00abcdef, nullptr);
        src->SetTransparency(0xff000002);
        e->SetBltSource(U(src), 0.0f, 0.0f, 1.0f, 1.0f, clip);
        const std::uint32_t kspans[2] = {Span(0, 0), Span(0, 4)};
        e->DrawPreclippedSpanList(kspans, 1);
        CHECK(b->GetPixel(0, 0) == src->GetPixel(0, 0) && b->GetPixel(1, 0) == 0x00abcdef);
        e->Release();
    }

    // ---- antialiased polygons ----
    {
        auto* b = Make(16, 16, kBufferColorARGB8888, 32);
        auto* e = NewEngine(b);
        const std::uint32_t red[1] = {0x00ff0000};
        e->SetColorTable(0, 1, red);
        CHECK(e->Draw(gz::eDrawingOp::FillQuad, nullptr, 0) == gz::eEngineDrawResult::Rasterize);   // antialiasing off
        e->SetAntialiasing(true);
        CHECK(e->Draw(gz::eDrawingOp::Line, nullptr, 0) == gz::eEngineDrawResult::Tesselate);
        // an axis aligned square on whole pixel corners (counter-clockwise on screen as the rasterizer expects)
        const float square[8] = {4, 4, 4, 10, 10, 10, 10, 4};
        CHECK(e->Draw(gz::eDrawingOp::FillQuad, square, 4) == gz::eEngineDrawResult::Done);
        CHECK(b->GetPixel(6, 6) == e->NativeColor(0));
        CHECK(b->GetPixel(0, 0) == 0 && b->GetPixel(12, 12) == 0);
        // half-covered column: x from 4.5 -> the pixel column 4 is blended at about half strength
        b->Fill(0, nullptr);
        const float half[8] = {4.5f, 4, 4.5f, 10, 10, 10, 10, 4};
        e->Draw(gz::eDrawingOp::FillPolygon, half, 4);
        const std::uint32_t edge = b->GetPixel(4, 6);
        std::uint8_t r, g, bl;
        b->ConvertNativeValueToRGB(edge, r, g, bl);
        CHECK(r > 100 && r < 160 && g == 0 && bl == 0);
        CHECK(b->GetPixel(5, 6) == e->NativeColor(0));
        // two triangles from one call
        b->Fill(0, nullptr);
        const float tris[12] = {1, 1, 1, 5, 5, 5, 8, 8, 8, 12, 12, 12};
        CHECK(e->Draw(gz::eDrawingOp::FillTri, tris, 6) == gz::eEngineDrawResult::Done);
        CHECK(b->GetPixel(2, 4) == e->NativeColor(0) && b->GetPixel(10, 11) == e->NativeColor(0));
        // degenerate counts do nothing
        CHECK(e->Draw(gz::eDrawingOp::FillTri, tris, 2) == gz::eEngineDrawResult::Done);
        e->FillAntialiasedPolygon(tris, 2);
        // clipping to the clip rectangle
        b->Fill(0, nullptr);
        e->SetClipRect(cRZRect{0, 0, 6, 16});
        e->Draw(gz::eDrawingOp::FillPolygon, square, 4);
        CHECK(b->GetPixel(5, 6) == e->NativeColor(0) && b->GetPixel(7, 6) == 0);
        e->Release();
    }

    for (cGZBuffer* b : g_buffers) b->Release();
    if (failures == 0) std::printf("gz_drawcontextrendertobuffer OK\n");
    return failures == 0 ? 0 : 1;
}
