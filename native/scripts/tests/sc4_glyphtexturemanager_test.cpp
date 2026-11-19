// cSC4GlyphTextureManager against OpenSC4 glyph-textures.md section 7 (sharing, size, lazy render, budget, second chance,
// sweep rate, restore) and the signpost's use of it (one glyph per character).
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "rz/cRZMessage2Standard.h"
#include "sc4/GlyphTextureManager.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

// A font whose characters are 10 x 20 pixels (font 7) or that does not exist (font 9); texture ids count up.
struct Host final : IGlyphTextureHost {
    std::uint32_t next = 1;
    std::set<std::uint32_t> live;
    int renders = 0;
    int width = 10, height = 20;   // per character
    bool HasFont(std::uint32_t font) override { return font == 7; }
    void MeasureText(std::uint32_t, const char*, std::uint32_t length, int& w, int& h) override {
        w = width * static_cast<int>(length);
        h = height;
    }
    std::uint32_t CreateTextureFromText(std::uint32_t font, const char*, std::uint32_t length, int* size, float* rect) override {
        if (font != 7) return 0;
        ++renders;
        const int w = width * static_cast<int>(length), h = height;
        int W = 1, H = 1;
        while (W < w + 2) W <<= 1;
        while (H < h + 2) H <<= 1;
        const int x0 = (W - w) / 2 + 1, y0 = (H - h) / 2 - 1;
        if (size) { size[0] = W; size[1] = H; }
        if (rect) { rect[0] = float(x0 + w); rect[1] = float(y0 + h); rect[2] = float(x0); rect[3] = float(y0); }
        live.insert(next);
        return next++;
    }
    void DeleteTexture(std::uint32_t texture) override { live.erase(texture); }
};

cRZString Str(const char* s) { return cRZString(s); }

}  // namespace

int main() {
    Host host;
    auto* manager = new cSC4GlyphTextureManager(&host);
    manager->AddRef();
    CHECK(manager->Init());
    CHECK(manager->Budget() == 1048576);
    void* raw = nullptr;
    CHECK(!manager->QueryInterface(1, &raw));   // only 0x0b54c519
    CHECK(manager->QueryInterface(kGZIID_cISC4GlyphTextureManager, &raw) && raw);
    if (raw) manager->Release();

    // ---- sharing (7.1): the same (character, font) is one object; after every caller lets go the next ask renders again
    cSC4GlyphTextureManager::GlyphInfo *a = nullptr, *b = nullptr;
    CHECK(manager->GetGlyphTexture(Str("A"), 7, &a) && a);
    CHECK(manager->GetGlyphTexture(Str("A"), 7, &b) && b);
    CHECK(a == b);
    CHECK(host.renders == 1 && manager->GlyphCount() == 1);
    // the glyph's measures: a 10 x 20 character in a 16 x 32 texture at (4, 5); UVs = extents / size
    int wh[2] = {};
    a->GetTextureSize(wh);
    CHECK(wh[0] == 16 && wh[1] == 32);
    a->GetGlyphSize(wh);
    CHECK(wh[0] == 10 && wh[1] == 20);
    float uv[4] = {};
    a->GetGlyphTextureCoordinates(uv);
    CHECK(uv[0] == 4.0f / 16 && uv[1] == 5.0f / 32 && uv[2] == 14.0f / 16 && uv[3] == 25.0f / 32);
    const std::uint32_t firstTexture = a->GetTextureBinding()->TextureId();
    CHECK(host.live.count(firstTexture) == 1);
    a->Release();
    CHECK(manager->GlyphCount() == 1);   // b still holds it
    b->Release();                        // count 1 (the map's): the manager drops it and the texture goes
    CHECK(manager->GlyphCount() == 0);
    CHECK(host.live.count(firstTexture) == 0);
    CHECK(manager->GetGlyphTexture(Str("A"), 7, &a) && a);
    CHECK(host.renders == 2);
    a->Release();
    // a font that does not exist: false, no glyph
    cSC4GlyphTextureManager::GlyphInfo* none = nullptr;
    CHECK(!manager->GetGlyphTexture(Str("A"), 9, &none) && none == nullptr);
    CHECK(manager->GlyphCount() == 0);

    // ---- the signpost's array: one glyph per character, shared between repeats ("ABA" = 2 textures, 3 entries)
    {
        std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>> glyphs;
        cRZString text("ABA");
        manager->StringToGlyphTextureArray(7, text, glyphs);
        CHECK(glyphs.size() == 3);
        CHECK(manager->GlyphCount() == 2);
        CHECK(glyphs.size() == 3 && glyphs[0].get() == glyphs[2].get() && glyphs[0].get() != glyphs[1].get());
        // multi-byte UTF-8 stays one character
        cRZString accented("\xc3\xa9t");
        manager->StringToGlyphTextureArray(7, accented, glyphs);   // clears the old list first
        CHECK(glyphs.size() == 2);
        CHECK(manager->GlyphCount() == 2);   // "A" and "B" went with the old list
    }
    CHECK(manager->GlyphCount() == 0);

    // ---- string texture size (7.2): 61 x 13 -> 64 x 16; 63 x 15 -> 128 x 32 (text width = 1 per character here)
    host.width = 1;
    host.height = 13;
    cSC4GlyphTextureManager::StringTexture* s = nullptr;
    std::string sixtyOne(61, 'x');
    cRZString text61(sixtyOne.c_str());
    CHECK(manager->GetCachedStringTexture(7, text61, &s) && s);
    std::int32_t size[2] = {};
    s->GetSize(size);
    CHECK(size[0] == 64 && size[1] == 16);
    s->Release();
    host.height = 15;
    std::string sixtyThree(63, 'x');
    cRZString text63(sixtyThree.c_str());
    CHECK(manager->GetCachedStringTexture(7, text63, &s) && s);
    s->GetSize(size);
    CHECK(size[0] == 128 && size[1] == 32);

    // ---- lazy render (7.3): nothing rendered until the id is asked; the second ask hits the cache
    const int before = host.renders;
    CHECK(host.renders == before);
    const std::uint32_t id = s->TextureId();
    CHECK(id != 0 && host.renders == before + 1);
    CHECK(manager->CacheEntries() == 1 && manager->CachedBytes() == 128 * 32 * 4);
    CHECK(s->TextureId() == id && host.renders == before + 1);
    CHECK(manager->CacheMisses() == 1);
    // the overlay manager's view of it: same size and texture
    std::int32_t ow = 0, oh = 0;
    s->AsOverlayTexture()->GetSize(ow, oh);
    CHECK(ow == 128 && oh == 32 && s->AsOverlayTexture()->GlTextureId() == static_cast<int>(id));
    // the caller lets go: the cache's reference keeps it (Release answers 0)
    CHECK(s->Release() == 0);
    CHECK(manager->CacheEntries() == 1);

    // ---- second chance (7.5) and sweep rate (7.6): over budget, the first sweep only clears the used flags
    manager->SetBudget(1);
    CHECK(manager->OnTick(1000));   // 1000 - 0 > 50: a sweep; the entry was used: it stays, unmarked
    CHECK(manager->CacheEntries() == 1);
    CHECK(manager->OnTick(1030));   // 30 ms later: no sweep
    CHECK(manager->CacheEntries() == 1);
    CHECK(manager->OnTick(1060));   // the next sweep drops it (unused since): binding and texture go
    CHECK(manager->CacheEntries() == 0 && manager->CachedBytes() == 0);
    CHECK(host.live.count(id) == 0);
    // time going backwards resets the last sweep time (a tick at 40 after 100 does not sweep at 80)
    CHECK(manager->OnTick(40));
    manager->SetBudget(1048576);

    // ---- budget (7.4): 1,200,000 bytes over 1,048,576 trims to 1,095,142 (a tenth of the budget per sweep)
    {
        host.width = 1;
        host.height = 1;
        std::vector<cSC4GlyphTextureManager::StringTexture*> textures;
        // entries of 64 x 4 x 4 = 1024 bytes each (a 62-character string, height 1 -> 64 x 4)
        std::string chars(62, 'x');
        for (int i = 0; i < 1172; ++i) {   // 1,200,128 bytes
            cSC4GlyphTextureManager::StringTexture* t = nullptr;
            cRZString text((chars + std::to_string(i)).substr(0, 62).c_str());
            manager->GetCachedStringTexture(7, text, &t);
            t->TextureId();
            textures.push_back(t);
        }
        CHECK(manager->CachedBytes() == 1172 * 1024);
        manager->OnTick(10000);   // every entry used: flags cleared, nothing dropped
        CHECK(manager->CachedBytes() == 1172 * 1024);
        manager->OnTick(20000);   // target = B - 0.1 L = 1,095,270 (float): drop the oldest until at or below it
        const std::int32_t after = manager->CachedBytes();
        CHECK(after <= 1095271 && after > 1095271 - 1024);
        for (auto* t : textures) t->Release();
    }

    // ---- restore (7.7): the cache empties; glyphs are rendered again into their slots
    {
        cSC4GlyphTextureManager::GlyphInfo* g = nullptr;
        CHECK(manager->GetGlyphTexture(Str("Q"), 7, &g) && g);
        const std::uint32_t old = g->GetTextureBinding()->TextureId();
        cRZMessage2Standard restore;
        restore.SetType(kMessageRestoreTextures);
        manager->DoMessage(&restore);
        CHECK(manager->CacheEntries() == 0 && manager->CachedBytes() == 0);
        CHECK(g->GetTextureBinding() && g->GetTextureBinding()->TextureId() != old);
        CHECK(host.live.count(old) == 0);
        g->Release();
    }

    // the cache's string textures hold the manager: the restore message empties it, so nothing is left behind
    cRZMessage2Standard drop;
    drop.SetType(kMessageRestoreTextures);
    manager->DoMessage(&drop);
    CHECK(manager->Shutdown());
    CHECK(manager->GlyphCount() == 0);
    manager->Release();
    if (failures == 0) std::printf("sc4_glyphtexturemanager: all passed\n");
    return failures == 0 ? 0 : 1;
}
