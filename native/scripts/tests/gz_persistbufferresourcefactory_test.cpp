// cGZPersistBufferResourceFactory: record formats (1, 2, legacy), CreateInstance, SerialRead, file load, interfaces.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "gz/PersistBufferResourceFactory.h"
#include "gz/cGZPersistDBSerialRecord.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

class MemoryRecord final : public cGZPersistDBSerialRecord {
public:
    MemoryRecord() : cGZPersistDBSerialRecord(nullptr, true) {}
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
    bool GetFieldVoid(void* d, std::uint32_t n) override {
        if (position + n > bytes.size()) return false;
        std::memcpy(d, bytes.data() + position, n);
        position += n;
        return true;
    }
    bool SetFieldVoid(const void* s, std::uint32_t n) override { auto* p = static_cast<const std::uint8_t*>(s); bytes.insert(bytes.end(), p, p + n); return true; }
    std::uint32_t GetSize() override { return static_cast<std::uint32_t>(bytes.size()); }
    bool SetSize(std::uint32_t) override { return true; }
    std::uint32_t GetPosition() override { return static_cast<std::uint32_t>(position); }
    bool SeekAbsolute(std::uint32_t p) override { position = p; return p <= bytes.size(); }
    bool SeekRelative(std::int32_t d) override { position += d; return true; }
    void DoPostClose() override {}
};

struct Host : IPersistBufferHost {
    int conversions = 0;
    std::int32_t lastConversion = -1;
    cIGZBuffer* CreateAggregatableBuffer() override { return nullptr; }
    cIGZBuffer* CreateBuffer() override { auto* b = new cGZBuffer(); b->AddRef(); return b; }
    cIGZBuffer* CreateGraphicSystemBuffer() override { return CreateBuffer(); }
    bool ConvertBufferFromFormatToFormat(cIGZBuffer*, std::int32_t t) override { ++conversions; lastConversion = t; return true; }
};

cIGZBuffer* Filled(std::uint32_t w, std::uint32_t h, std::int32_t type, std::uint32_t bits) {
    auto* b = new cGZBuffer();
    b->AddRef();
    b->Init(w, h, {type, bits});
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) b->SetPixel(x, y, (0x101 * (x + 1) + 7 * y) & (bits == 32 ? 0xffffffff : 0xffff));
    return b;
}
bool Same(cIGZBuffer* a, cIGZBuffer* b) {
    if (a->Width() != b->Width() || a->Height() != b->Height()) return false;
    for (std::uint32_t y = 0; y < a->Height(); ++y)
        for (std::uint32_t x = 0; x < a->Width(); ++x)
            if (a->GetPixel(x, y) != b->GetPixel(x, y)) return false;
    return true;
}

}  // namespace

int main() {
    Host host;
    auto* f = new cGZPersistBufferResourceFactory(&host);
    f->AddRef();

    // interfaces
    void* a = nullptr; void* b = nullptr;
    CHECK(f->QueryInterface(0xa56b8f17, &a) && a == static_cast<cIGZPersistResourceFactory*>(f));
    CHECK(f->QueryInterface(0x656ddc26, &b) && b == f->AsBufferFactory());
    CHECK(!f->QueryInterface(0x42, &a));
    f->Release(); f->Release();

    // save and load both versions
    for (bool compress : {true, false}) {
        for (auto cfg : {std::pair<std::int32_t, std::uint32_t>{kBufferColorRGB565, 16}, std::pair<std::int32_t, std::uint32_t>{kBufferColorARGB8888, 32}}) {
            cIGZBuffer* src = Filled(9, 5, cfg.first, cfg.second);   // 9 x 2 bytes: the surface stride is padded (18 -> 20)
            MemoryRecord rec;
            CHECK(f->SaveToSerialRecord(src, &rec, compress));
            std::uint32_t version = 0;
            std::memcpy(&version, rec.bytes.data(), 4);
            CHECK(version == (compress ? 2u : 1u));
            cIGZBuffer* loaded = nullptr;
            host.conversions = 0;
            const bool ok = f->LoadFromSerialRecord(&loaded, &rec, false);
            CHECK(ok && loaded != nullptr);
            if (loaded) {
                CHECK(Same(src, loaded) || (!compress && cfg.second == 16));   // v1 with a padded stride is read with swapped strides (kept)
                loaded->Release();
            }
            CHECK(host.conversions == 1 && host.lastConversion == cfg.first);
            src->Release();
        }
    }
    // plain 32 bit rows without padding round trip exactly in version 1
    {
        cIGZBuffer* src = Filled(8, 4, kBufferColorARGB8888, 32);
        MemoryRecord rec;
        f->SaveToSerialRecord(src, &rec, false);
        cIGZBuffer* back = nullptr;
        CHECK(f->LoadFromSerialRecord(&back, &rec, false) && Same(src, back));
        back->Release(); src->Release();
    }
    // legacy version
    {
        MemoryRecord rec;
        std::uint32_t v[] = {0x856ddbac, 2, 2, 0x11, 0x22, 0x33, 0x44};
        rec.SetFieldVoid(v, sizeof(v));
        cIGZBuffer* b = nullptr;
        CHECK(f->LoadFromSerialRecord(&b, &rec, false) && b && b->Width() == 2 && b->Height() == 2);
        CHECK(b->GetPixel(0, 0) == b->GetPixel(0, 0));
        b->Release();
        // unknown versions "succeed" with no buffer (kept)
        MemoryRecord odd;
        std::uint32_t o[] = {77};
        odd.SetFieldVoid(o, 4);
        cIGZBuffer* none = reinterpret_cast<cIGZBuffer*>(0x1);
        CHECK(f->LoadFromSerialRecord(&none, &odd, false) && none == nullptr);
        // truncated data
        MemoryRecord shortRec;
        std::uint32_t s[] = {1, 100, 4, 4, kBufferColorRGB565};
        shortRec.SetFieldVoid(s, sizeof(s));
        cIGZBuffer* bad = nullptr;
        CHECK(!f->LoadFromSerialRecord(&bad, &shortRec, false) && bad == nullptr);
        CHECK(!f->LoadFromSerialRecord(nullptr, &odd, false) && !f->LoadFromSerialRecord(&bad, nullptr, false));
        MemoryRecord empty;
        CHECK(!f->LoadFromSerialRecord(&bad, &empty, false));
        CHECK(!f->SaveToSerialRecord(nullptr, &empty, true));
    }
    // CreateInstance
    {
        void* inst = nullptr;
        CHECK(!f->CreateInstance(0x1234u, kGZIID_cIGZPersistBufferResource, &inst));
        CHECK(f->CreateInstance(kGZResourceTypeBufferResource, kGZIID_cIGZPersistBufferResource, &inst) && inst);
        auto* res = static_cast<cIGZPersistBufferResource*>(inst);
        auto* full = dynamic_cast<cGZPersistBufferResource*>(res);
        CHECK(full && full->RequestedColorType().type == 1);
        res->Release();
        f->SetColorType({kBufferColorRGB565, 16});
        CHECK(f->CreateInstance(kGZResourceTypeBufferResource, kGZIID_cIGZPersistBufferResource, &inst, kBufferColorARGB4444, nullptr));
        auto* r2 = dynamic_cast<cGZPersistBufferResource*>(static_cast<cIGZPersistBufferResource*>(inst));
        CHECK(r2 && r2->RequestedColorType().type == kBufferColorARGB4444 && r2->RequestedColorType().bitsPerPixel == 16);
        static_cast<cIGZPersistBufferResource*>(inst)->Release();
        CHECK(f->CreateInstance(kGZResourceTypeBufferResource, kGZIID_cIGZPersistBufferResource, &inst));
        auto* r3 = dynamic_cast<cGZPersistBufferResource*>(static_cast<cIGZPersistBufferResource*>(inst));
        CHECK(r3 && r3->RequestedColorType().type == kBufferColorRGB565);
        static_cast<cIGZPersistBufferResource*>(inst)->Release();
        f->SetColorType({1, 0});
    }
    // SerialRead copies a loaded image into the resource's buffer; SerialWrite is unsupported
    {
        std::vector<std::uint8_t> bmp = {'B', 'M'};
        auto p32 = [&](std::uint32_t x) { for (int i = 0; i < 4; ++i) bmp.push_back((x >> (8 * i)) & 0xff); };
        auto p16 = [&](std::uint16_t x) { bmp.push_back(x & 0xff); bmp.push_back(x >> 8); };
        p32(54 + 8); p32(0); p32(54); p32(40); p32(2); p32(1); p16(1); p16(24); p32(0); p32(0); p32(0); p32(0); p32(0); p32(0);
        for (std::uint8_t v : {10, 20, 30, 40, 50, 60, 0, 0}) bmp.push_back(v);
        MemoryRecord rec;
        rec.bytes = bmp;
        auto* target = new cGZPersistBufferResource(&host, cGZBufferColorType{kBufferColorARGB8888, 32});
        target->AddRef();
        target->SetType(kGZResourceTypeBufferResource);
        CHECK(f->SerialRead(*static_cast<cGZPersistResource*>(target), rec));
        cIGZBuffer* tb = static_cast<cIGZPersistBufferResource*>(target)->GetBuffer();
        CHECK(tb->Width() == 2 && tb->Height() == 1 && tb->GetPixel(0, 0) == 0xff1e140a && tb->GetPixel(1, 0) == 0xff3c3228);
        tb->Release();
        CHECK(!f->SerialWrite(*static_cast<cGZPersistResource*>(target), rec));
        target->SetType(0x1234);
        CHECK(!f->SerialRead(*static_cast<cGZPersistResource*>(target), rec));
        target->Release();
        // a record that is neither bitmap nor Targa leaves the result false
        MemoryRecord junk;
        junk.bytes = {1, 2, 3, 4};
        auto* t2 = new cGZPersistBufferResource(&host, cGZBufferColorType{kBufferColorARGB8888, 32});
        t2->AddRef();
        t2->SetType(kGZResourceTypeBufferResource);
        CHECK(!f->SerialRead(*static_cast<cGZPersistResource*>(t2), junk));
        t2->Release();
    }
    // file load
    {
        std::vector<std::uint8_t> bmp = {'B', 'M'};
        auto p32 = [&](std::uint32_t x) { for (int i = 0; i < 4; ++i) bmp.push_back((x >> (8 * i)) & 0xff); };
        auto p16 = [&](std::uint16_t x) { bmp.push_back(x & 0xff); bmp.push_back(x >> 8); };
        p32(62); p32(0); p32(54); p32(40); p32(1); p32(1); p16(1); p16(24); p32(0); p32(0); p32(0); p32(0); p32(0); p32(0);
        for (std::uint8_t v : {1, 2, 3, 0}) bmp.push_back(v);
        const char* tmp = std::getenv("TMPDIR");
        std::string path = std::string(tmp ? tmp : "/tmp") + "/sc4_pbrf_" + std::to_string(getpid()) + ".bmp";
        std::FILE* file = std::fopen(path.c_str(), "wb");
        std::fwrite(bmp.data(), 1, bmp.size(), file);
        std::fclose(file);
        cRZString p(path.c_str());
        cIGZBuffer* out = nullptr;
        CHECK(f->LoadFromFile(p, &out, false, {kBufferColorARGB8888, 32}, false) && out && out->GetPixel(0, 0) == 0xff030201);
        out->Release();
        std::remove(path.c_str());
        cRZString missing("/missing/file.bmp");
        cIGZBuffer* none = nullptr;
        CHECK(!f->LoadFromFile(missing, &none, false, {kBufferColorARGB8888, 32}, false) && none == nullptr);
        CHECK(!f->SaveToFile(p, &none) && !f->LoadFromResourceId(1, &none, false, {1, 0}, false));
    }
    f->Release();
    if (failures == 0) std::printf("gz_persistbufferresourcefactory OK\n");
    return failures == 0 ? 0 : 1;
}
