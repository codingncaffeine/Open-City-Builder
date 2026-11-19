// The glyph texture manager on the native application: nS3D::CreateTextureFromText over the game's own fonts (the font source's
// styles, e.g. "Signpost" 0x2c1fd612 and "Label" 0xcc1fd62c) and the GL driver's textures. Replaces the boot's placeholder.
#include "app/NativeGlyphTextures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeGraphics.h"
#include "app/NativeModelMaker.h"
#include "gz/WinFont.h"
#include "sc4/GlyphTextureManager.h"
#include "sc4/ServiceLocator.h"

namespace native {
namespace {

bool Trace() {
    static const bool on = std::getenv("SC4_GLYPHDEBUG") != nullptr;
    return on;
}

int CeilPow2(int value) {   // nS3D::CeilPow2
    int p = 1;
    while (p < value) p <<= 1;
    return p;
}

struct Host final : sc4::IGlyphTextureHost {
    static gz::IWinFont* FontFor(std::uint32_t id) {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        return fonts && fonts->Available() ? fonts->Font(id) : nullptr;
    }
    bool HasFont(std::uint32_t id) override { return FontFor(id) != nullptr; }
    void MeasureText(std::uint32_t id, const char* text, std::uint32_t length, int& width, int& height) override {
        width = height = 0;
        gz::IWinFont* font = FontFor(id);
        cIGZFont* raw = font ? font->AsFont() : nullptr;
        if (!raw) return;
        width = raw->GetStringWidth(text, length);   // font slot 48
        height = raw->CharHeight(text, length);      // font slot 37
    }
    // nS3D::CreateTextureFromText (geometry-utilities.md 6.4), the plain upload: W x H (the smallest powers of two >= the size + 2),
    // the text drawn in white at x0 = (W - w) / 2 + 1, y0 = (H - h) / 2 - 1 on a cleared surface, the 1-pixel border cleared.
    // The native surface is XRGB: white text on black gives the coverage, which becomes the alpha of a white texture (the same
    // pixels the original's ARGB surface holds).
    std::uint32_t CreateTextureFromText(std::uint32_t id, const char* text, std::uint32_t length, int* size, float* rect) override {
        gz::IWinFont* font = FontFor(id);
        cIGZFont* raw = font ? font->AsFont() : nullptr;
        if (!raw) return 0;
        const int w = raw->GetStringWidth(text, length);
        const int h = raw->CharHeight(text, length);
        const int W = CeilPow2(w + 2), H = CeilPow2(h + 2);
        const int x0 = (W - w) / 2 + 1, y0 = (H - h) / 2 - 1;
        if (size) { size[0] = W; size[1] = H; }
        if (rect) { rect[0] = static_cast<float>(x0 + w); rect[1] = static_cast<float>(y0 + h); rect[2] = static_cast<float>(x0); rect[3] = static_cast<float>(y0); }
        Buffer surface(W, H, false);   // ARGB8888, cleared to 0 by its constructor
        font->DrawText(&surface, x0, y0, text, length, 0xffffffffu, gz::kWinUnsetTextColor, nullptr, 0);
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(W) * H * 4);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                std::uint8_t r = 0, g = 0, b = 0;
                if (x != 0 && y != 0 && x != W - 1 && y != H - 1) surface.NativeToRGB(surface.Pixels()->GetPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)), r, g, b);
                std::uint8_t* p = &bgra[(static_cast<std::size_t>(y) * W + x) * 4];
                p[0] = p[1] = p[2] = 0xff;
                p[3] = std::max(r, std::max(g, b));
            }
        }
        const unsigned texture = CreateBgraTexture(W, H, bgra.data());
        if (Trace()) std::fprintf(stderr, "glyph: \"%.*s\" font %08x -> %dx%d (text %dx%d at %d,%d) texture %u\n", static_cast<int>(length), text, id, W, H, w, h, x0, y0, texture);
        return texture;
    }
    void DeleteTexture(std::uint32_t texture) override { DeleteDriverTexture(texture); }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
};

Host g_host;
sc4::cSC4GlyphTextureManager* g_manager = nullptr;

}  // namespace

cIGZUnknown* CreateGlyphTextureManager() {
    if (!g_manager) {
        g_manager = new sc4::cSC4GlyphTextureManager(&g_host);
        g_manager->AddRef();   // this module's own reference: the service lives as long as the process
        g_manager->Init();
    }
    return static_cast<cIGZUnknown*>(static_cast<cIGZMessageTarget2*>(g_manager));
}

sc4::cSC4GlyphTextureManager* GlyphTextureManager() { return g_manager; }

void TickGlyphTextures(std::uint32_t timeMs) {
    if (g_manager) g_manager->OnTick(timeMs);
}

}  // namespace native
