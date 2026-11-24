// The region screen's city picture preparation (sc4/RegionCityImages): resample shifts, span lists, mask to alpha, anchor.
#include <cstdio>
#include <vector>

#include "gz/Buffer.h"
#include "sc4/RegionCityImages.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
cGZBuffer* Make(int w, int h, const std::vector<std::uint32_t>& pixels) {
    auto* b = new cGZBuffer;
    b->AddRef();
    b->Init(w, h, cGZBufferColorType{9, 32});
    b->Lock(cIGZBuffer::kLockWrite);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) b->SetPixel(x, y, pixels[std::size_t(y) * w + x]);
    b->Unlock(cIGZBuffer::kLockWrite);
    return b;
}
std::uint32_t Pixel(cIGZBuffer* b, int x, int y) { return b->GetPixel(std::uint32_t(x), std::uint32_t(y)); }
}  // namespace

int main() {
    // ---- a whole-pixel map position (fraction 1): the kernel is (0, 1), the picture sits at 0,0 of a 2-larger buffer
    cGZBuffer* src = Make(2, 1, {0xff102030u, 0x80405060u});
    cIGZBuffer* whole = sc4::RegionResample(src, 1.0, 1.0);
    CHECK(whole && whole->Width() == 4 && whole->Height() == 3);
    if (whole) {
        CHECK(Pixel(whole, 0, 0) == 0xff102030u);
        CHECK(Pixel(whole, 1, 0) == 0x80405060u);
        CHECK(Pixel(whole, 2, 0) == 0u && Pixel(whole, 0, 1) == 0u);
        whole->Release();
    }
    // ---- half a pixel each way: (0.5, 0.5) taps, each output the mean of its pixel and the one up/left of it
    cIGZBuffer* half = sc4::RegionResample(src, 0.5, 0.5);
    if (half) {
        // row 0 = half of (0 row, source row 0); x = 0: half of (0, p0) -> p0 / 4
        CHECK(Pixel(half, 0, 0) == 0x4004080cu);   // each channel a quarter, rounded at both passes (a 0x40, r 4, g 8, b 0x0c)
        half->Release();
    }
    // ---- span lists: blue runs (no weights) and green weights, rows in the high word, end exclusive
    cGZBuffer* mask = Make(4, 2, {0x00000000u, 0x000011ffu, 0x000022ffu, 0x00000000u,
                                  0x000033ffu, 0x00000000u, 0x00000000u, 0x00000001u});
    std::vector<std::uint32_t> hit, green;
    sc4::SC4CreateSpanList(mask, 0, hit, false);
    CHECK((hit == std::vector<std::uint32_t>{1, 3, 0x10000, 0x10001, 0x10003, 0x10004}));
    sc4::SC4CreateSpanList(mask, 8, green, true);
    CHECK((green == std::vector<std::uint32_t>{1, 3, 0x11, 0x22, 0x10000, 0x10001, 0x33}));
    // ---- the mask's blue becomes the picture's alpha
    cGZBuffer* picture = Make(4, 2, std::vector<std::uint32_t>(8, 0x00abcdefu));
    sc4::SC4MoveMaskToAlpha(picture, mask);
    CHECK(Pixel(picture, 1, 0) == 0xffabcdefu);
    CHECK(Pixel(picture, 0, 0) == 0x00abcdefu);
    CHECK(Pixel(picture, 3, 1) == 0x01abcdefu);
    // ---- the whole chain on a whole-pixel position: spans of the resampled mask, anchor from its bytes
    sc4::RegionCityImages out;
    sc4::SC4RecalculateRegionCityImages(picture, mask, nullptr, nullptr, 3.0f, 7.0f, out);
    CHECK(out.image && out.image->Width() == 6 && out.image->Height() == 4);
    CHECK((out.hitSpans == hit));                // whole pixels: the same runs
    CHECK(out.altImage == nullptr);
    // anchor: weights = byte 1 + 2x of each row (x < 6): row 0 bytes 1,3,5,7,9,11 of pixels 0..2 = g0 a0 g1 a1 g2 a2
    //   = 0, 0, 0x11, 0, 0x22, 0 -> x sum 2*0x11 + 4*0x22 = 0xaa, total 0x33; row 1: g0 = 0x33 at x 0 -> y sum 0x33
    //   total 0x66: centerX = (0xaa + 0x33) / 0x66 = 2, centerY = (0x33 + 0x33) / 0x66 = 1
    CHECK(out.centerX == 2 && out.centerY == 1);
    if (out.image) out.image->Release();
    picture->Release();
    mask->Release();
    src->Release();
    std::printf(failures ? "regioncityimages: %d failures\n" : "regioncityimages: all passed\n", failures);
    return failures ? 1 : 0;
}
