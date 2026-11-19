#include <cstdio>

#include "sc4/SLBuffer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Host final : ISLBufferHost {
    int resourceCalls = 0, fileCalls = 0;
    cGZPersistResourceKey lastKey{};
    bool lowQuality = false, hasLowGroup = false, uncached = false, resourceOk = true;
    std::int32_t lastType = 0;
    std::string lastPath;
    bool GetBufferResource(const cGZPersistResourceKey& key, std::int32_t type, bool bypass, cIGZBuffer** out) override {
        ++resourceCalls; lastKey = key; lastType = type; uncached = bypass;
        if (!resourceOk) return false;
        auto* b = new cGZBuffer();
        b->AddRef();
        b->Init(8, 4, {kBufferColorXRGB8888, 32});
        b->SetTransparency(0x123456);
        *out = b;
        return true;
    }
    bool HasResource(const cGZPersistResourceKey&) override { return hasLowGroup; }
    bool LowQualityTextures() override { return lowQuality; }
    bool LoadBufferFromFile(const std::string& path, std::int32_t type, cIGZBuffer** out) override {
        ++fileCalls; lastPath = path; lastType = type;
        auto* b = new cGZBuffer(); b->AddRef(); b->Init(2, 2, {kBufferColorRGB565, 16}); *out = b; return true;
    }
    cIGZBuffer* CreateBuffer() override { auto* buffer = new cGZBuffer(); buffer->AddRef(); return buffer; }   // counted, as the hosts do
};
}  // namespace

int main() {
    Host host;
    SetSLBufferHost(&host);
    {
        cSLBuffer plain(false);
        CHECK(!plain.GetBuffer() && !plain.Load() || host.resourceCalls == 1);
    }
    host.resourceCalls = 0;
    {   // colour type 3 keeps the loaded picture as is
        cSLBuffer b(cGZPersistResourceKey{0, 7, 9}, kBufferColorDefault, false);
        CHECK(b.GetBuffer() && host.lastKey.type == kSLBitmapResourceType && host.lastKey.group == 7 && host.lastKey.instance == 9 && !host.uncached);
        CHECK(b.GetBuffer()->GetColorType().type == kBufferColorXRGB8888 && b.Load() && host.resourceCalls == 1);   // cached
        cGZPersistResourceKey k;
        b.GetResKey(k);
        CHECK(k.group == 7 && k.type == 0);
        b.SetResKey(1, 2, 3); b.GetResKey(k);
        CHECK(k.type == 1 && k.group == 2 && k.instance == 3);
        b.Unload();
        CHECK(!b.GetBuffer());
    }
    {   // conversion: a fresh RGB565 buffer without the colour key
        host.resourceCalls = 0;
        cSLBuffer b(1u, 2u, 3u, kBufferColorRGB565, false);
        CHECK(b.GetBuffer() && b.GetBuffer()->GetColorType().type == kBufferColorRGB565 && b.GetBuffer()->Width() == 8 && b.GetBuffer()->Height() == 4);
        CHECK(!b.GetBuffer()->GetTransparency());
    }
    {   // low quality textures try group 1 first
        host.lowQuality = true; host.hasLowGroup = true;
        cSLBuffer b(cGZPersistResourceKey{5, 8, 9}, kBufferColorDefault, true);
        CHECK(host.lastKey.group == 1 && host.uncached);
        host.hasLowGroup = false;
        cSLBuffer c(cGZPersistResourceKey{5, 8, 9}, kBufferColorDefault, false);
        CHECK(host.lastKey.group == 8);
        host.lowQuality = false;
    }
    {   // always-reload loads again
        cSLBuffer b(cGZPersistResourceKey{5, 8, 9}, kBufferColorDefault, true);
        const int calls = host.resourceCalls;
        CHECK(b.Load() && host.resourceCalls == calls + 1);
    }
    {   // file path
        cSLBuffer b(cRZString("art/x.bmp"), kBufferColorDefault, false);
        CHECK(b.GetBuffer() && host.fileCalls == 1 && host.lastPath == "art/x.bmp");
        b.SetPath(cRZString("art/y.bmp")); b.SetColorType(kBufferColorDefault);
        b.Unload();
        CHECK(b.Load() && host.lastPath == "art/y.bmp");
    }
    {   // failed resource
        host.resourceOk = false;
        cSLBuffer b(cGZPersistResourceKey{5, 8, 9}, kBufferColorDefault, false);
        CHECK(!b.GetBuffer() && !b.Load());
    }
    SetSLBufferHost(nullptr);
    std::printf(failures ? "FAILED\n" : "sc4_slbuffer ok\n");
    return failures != 0;
}
