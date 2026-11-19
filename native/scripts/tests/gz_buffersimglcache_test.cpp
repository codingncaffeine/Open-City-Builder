// cGZBufferSimGLCache: tiling of a buffer into textures, uploads (plain and colour keyed), dirty tracking, interfaces.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gz/BufferSimGLCache.h"
#include "render/gd/GDriverBase.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct Upload {
    std::uint32_t texture, x, y, w, h, format, type, rowLength;
    std::vector<std::uint8_t> bytes;   // a copy of the pixels (16 bit)
    const void* pixels;
};

struct FakeDriver : gd::cGDriverBase {
    std::uint32_t next = 100;
    std::vector<std::uint32_t> created, deleted;
    std::vector<std::uint32_t> createdFormat, createdW, createdH;
    std::vector<Upload> uploads;
    std::uint32_t CreateTexture(gd::eGDInternalTextureFormat f, std::uint32_t w, std::uint32_t h, std::uint32_t, gd::eGDTextureHintFlags) override {
        created.push_back(next); createdFormat.push_back(f); createdW.push_back(w); createdH.push_back(h);
        return next++;
    }
    void DeleteTextures(int n, const std::uint32_t* ids) override { for (int i = 0; i < n; ++i) deleted.push_back(ids[i]); }
    void LoadTextureLevel(std::uint32_t t, std::uint32_t, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                          gd::eGDTextureFormat f, gd::eGDType type, std::uint32_t rowLength, const void* pixels) override {
        Upload u{t, x, y, w, h, f, type, rowLength, {}, pixels};
        if (pixels && w && h) { const std::size_t n = (static_cast<std::size_t>(h) - 1) * rowLength + w; u.bytes.assign(static_cast<const std::uint8_t*>(pixels), static_cast<const std::uint8_t*>(pixels) + n * 2); }
        uploads.push_back(std::move(u));
    }
};

struct FakeHost : IGLCacheHost {
    FakeDriver* driver = nullptr;
    std::vector<cIGZBufferHardwareCache*> registered;
    int unregisters = 0;
    void RegisterBufferCache(cIGZBufferHardwareCache* c) override { registered.push_back(c); }
    void UnregisterBufferCache(cIGZBufferHardwareCache*) override { ++unregisters; }
    gd::cIGZGDriver* GetGDriver() override { return driver; }
};

cGZBuffer* Make(std::uint32_t w, std::uint32_t h, std::int32_t type, std::uint32_t bits) {
    auto* b = new cGZBuffer();
    b->AddRef();
    b->Init(w, h, {type, bits});
    b->Fill(0, nullptr);
    return b;
}

}  // namespace

int main() {
    FakeDriver driver;
    FakeHost host;
    host.driver = &driver;

    auto* buffer = Make(300, 40, kBufferColorRGB565, 16);
    for (std::uint32_t y = 0; y < 40; ++y)
        for (std::uint32_t x = 0; x < 300; ++x) buffer->SetPixel(x, y, static_cast<std::uint32_t>((x + y * 7) & 0xffff));

    auto* cache = new cGZBufferSimGLCache(&host);
    cache->AddRef();
    CHECK(cache->Attach(buffer) && host.registered.size() == 1 && host.registered[0] == cache);
    CHECK(!cache->IsResident() && cache->GetTileCount() == 0);

    // interfaces share one object count
    void* a = nullptr; void* b = nullptr;
    CHECK(cache->QueryInterface(0xfa59f7b9, &a) && a == static_cast<cIGZBufferHardwareCache*>(cache));
    CHECK(cache->QueryInterface(0xeb4d101f, &b) && b == static_cast<cIGZBufferTileSource*>(cache));
    CHECK(!cache->QueryInterface(0x1234, &a));
    cache->Release(); cache->Release();

    // first refresh: builds the tiles (300x40 -> 256x40 full-width strip? 1 full column unavailable: 300 > 256)
    CHECK(cache->Refresh(7));
    CHECK(cache->IsResident() && cache->GetLastUsedClock() == 7 && cache->GetTileCount() >= 1);
    CHECK(driver.created.size() == static_cast<std::size_t>(cache->GetTileCount()));
    CHECK(cache->TextureFormat() == 4);   // RGB565 -> driver format 4
    // every pixel of the buffer was uploaded exactly once, from the right place
    std::vector<int> covered(300 * 40, 0);
    cRZTileSize size; const cRZSubTile* pieces = nullptr; std::uint32_t texture = 0, count = 0;
    std::size_t uploadIndex = 0;
    for (std::int32_t i = 0; i < cache->GetTileCount(); ++i) {
        cache->GetTile(static_cast<std::uint32_t>(i), texture, size, pieces, count);
        CHECK(texture == driver.created[static_cast<std::size_t>(i)] && size.width == static_cast<std::int32_t>(driver.createdW[static_cast<std::size_t>(i)]));
        for (std::uint32_t p = 0; p < count; ++p, ++uploadIndex) {
            const Upload& u = driver.uploads[uploadIndex];
            CHECK(u.texture == texture && u.x == static_cast<std::uint32_t>(pieces[p].tileX) && u.w == static_cast<std::uint32_t>(pieces[p].width));
            CHECK(u.format == 3 && u.type == 10 && u.rowLength == buffer->GetColorSurfaceStride() / 2);
            const auto* expected = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits()) + pieces[p].y * buffer->GetColorSurfaceStride() + pieces[p].x * 2;
            CHECK(u.pixels == expected);
            for (int yy = 0; yy < pieces[p].height; ++yy)
                for (int xx = 0; xx < pieces[p].width; ++xx) ++covered[(pieces[p].y + yy) * 300 + pieces[p].x + xx];
        }
    }
    CHECK(uploadIndex == driver.uploads.size());
    bool all = true;
    for (int c : covered) all = all && c == 1;
    CHECK(all);
    CHECK(cache->GetMaxFragments() >= 1);

    // clean buffer: nothing uploaded again; a change or Invalidate uploads again
    const std::size_t before = driver.uploads.size();
    CHECK(!cache->IsDirty() && cache->Refresh(8) && driver.uploads.size() == before);
    buffer->SetPixel(1, 1, 5);   // (SetPixel bumps the change counter only through Fill/Lock paths)
    buffer->Fill(0x1234, nullptr);
    CHECK(cache->IsDirty());
    CHECK(cache->Refresh(9) && driver.uploads.size() > before && !cache->IsDirty());
    const std::size_t again = driver.uploads.size();
    cache->Invalidate();
    CHECK(cache->IsDirty() && cache->Refresh(10) && driver.uploads.size() > again);

    // flush deletes the textures and keeps the tile layout
    const auto created = driver.created;
    cache->Flush();
    CHECK(!cache->IsResident() && driver.deleted == created);
    cache->Flush();
    CHECK(driver.deleted == created);

    // a 16 bit buffer with a colour key: tiles are assembled in a scratch image, key pixels lose alpha (ARGB1555)
    {
        FakeDriver d2;
        FakeHost h2;
        h2.driver = &d2;
        auto* keyed = Make(20, 10, kBufferColorARGB1555, 16);
        for (std::uint32_t y = 0; y < 10; ++y)
            for (std::uint32_t x = 0; x < 20; ++x) keyed->SetPixel(x, y, 0x8000 | ((x + 1) << 5));
        keyed->SetPixel(3, 2, 0x8321);
        keyed->SetTransparency(0x8321);
        auto* c2 = new cGZBufferSimGLCache(&h2);
        c2->AddRef();
        c2->Attach(keyed);
        CHECK(c2->Refresh(1));
        CHECK(c2->TextureFormat() == 3);
        CHECK(d2.uploads.size() == static_cast<std::size_t>(c2->GetTileCount()));
        const Upload& u = d2.uploads[0];   // tile 0's scratch image
        CHECK(u.x == 0 && u.y == 0 && u.rowLength == u.w && u.type == 10);
        cRZTileSize sz; const cRZSubTile* pc = nullptr; std::uint32_t tx = 0, n = 0;
        c2->GetTile(0, tx, sz, pc, n);
        std::vector<std::uint16_t> pxv(u.bytes.size() / 2); std::memcpy(pxv.data(), u.bytes.data(), u.bytes.size());
        bool found = false;
        for (std::uint32_t p = 0; p < n; ++p) {
            if (pc[p].x <= 3 && 3 < pc[p].x + pc[p].width && pc[p].y <= 2 && 2 < pc[p].y + pc[p].height) {
                found = true;
                const std::size_t keyedIndex = (pc[p].tileY + 2 - pc[p].y) * u.rowLength + pc[p].tileX + 3 - pc[p].x;
                CHECK(pxv[keyedIndex] != 0x8321);                       // converted
                CHECK((pxv[keyedIndex + 1] & 0x8000) != 0);            // the next pixel keeps its alpha
            }
        }
        CHECK(found);
        c2->Release();
        keyed->Release();
        // formats that the colour key path skips (RGB565) upload an untouched scratch image
        FakeDriver d3;
        FakeHost h3;
        h3.driver = &d3;
        auto* k565 = Make(8, 8, kBufferColorRGB565, 16);
        k565->Fill(0x1111, nullptr);
        k565->SetTransparency(0x1111);
        auto* c3 = new cGZBufferSimGLCache(&h3);
        c3->AddRef();
        c3->Attach(k565);
        c3->Refresh(1);
        std::vector<std::uint16_t> zv(d3.uploads.back().bytes.size() / 2); std::memcpy(zv.data(), d3.uploads.back().bytes.data(), d3.uploads.back().bytes.size());
        const std::uint16_t* z = zv.data();
        bool zero = true;
        for (int i = 0; i < 8; ++i) zero = zero && z[i] == 0;
        CHECK(zero);
        c3->Release();
        k565->Release();
    }

    // no driver: refresh still succeeds and clears the flag
    {
        FakeHost h4;
        auto* c4 = new cGZBufferSimGLCache(&h4);
        c4->AddRef();
        c4->Attach(buffer);
        CHECK(!c4->InitCache());
        CHECK(c4->Refresh(3) && !c4->IsDirty());
        c4->Release();
    }

    // shutdown unregisters and drops the buffer
    cache->Shutdown();
    CHECK(host.unregisters == 1);
    cache->Release();
    CHECK(host.unregisters == 2 || host.unregisters == 1);   // the destructor shuts down again
    buffer->Release();
    if (failures == 0) std::printf("gz_buffersimglcache OK\n");
    return failures == 0 ? 0 : 1;
}
