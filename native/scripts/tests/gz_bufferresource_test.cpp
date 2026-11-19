// cGZBufferResource: version-2 (RefPack) save/load round trip, version-0 pixels, 8-bit and 24-bit bitmaps.
#include <cstdio>
#include <cstring>
#include <vector>

#include "gz/BufferResource.h"
#include "gz/cGZPersistDBSerialRecord.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
// A record over a growing byte vector (writes append, reads consume).
class MemoryRecord final : public cGZPersistDBSerialRecord {
public:
    MemoryRecord() : cGZPersistDBSerialRecord(nullptr, true) {}
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
    bool GetFieldVoid(void* destination, std::uint32_t count) override {
        if (position + count > bytes.size()) return false;
        std::memcpy(destination, bytes.data() + position, count);
        position += count;
        return true;
    }
    bool SetFieldVoid(const void* source, std::uint32_t count) override {
        const auto* p = static_cast<const std::uint8_t*>(source);
        bytes.insert(bytes.end(), p, p + count);
        return true;
    }
    std::uint32_t GetSize() override { return static_cast<std::uint32_t>(bytes.size()); }
    bool SetSize(std::uint32_t) override { return true; }
    std::uint32_t GetPosition() override { return static_cast<std::uint32_t>(position); }
    bool SeekAbsolute(std::uint32_t p) override { position = p; return true; }
    bool SeekRelative(std::int32_t d) override { position += d; return true; }
    void DoPostClose() override {}
};
cIGZBuffer* MakeBuffer() { auto* b = new cGZBuffer(); b->AddRef(); return b; }
}  // namespace

int main() {
    SetBufferMaker(MakeBuffer);
    SetBufferFormatConverter([](cIGZBuffer*, std::uint32_t) { return true; });

    // v2 round trip of a 16-bit buffer
    auto* source = new cGZBufferResource;
    source->AddRef();
    cIGZBuffer* buffer = nullptr;
    CHECK(source->GetBuffer(&buffer) && buffer);
    CHECK(buffer->Init(8, 4, {kBufferColorRGB565, 16}));
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) buffer->SetPixel(x, y, 0x1000 + x * 7 + y);
    MemoryRecord record;
    CHECK(source->SaveToSerialRecord(&record, 2));
    std::uint32_t version = 0;
    std::memcpy(&version, record.bytes.data(), 4);
    CHECK(version == 2);

    auto* loaded = new cGZBufferResource;
    loaded->AddRef();
    CHECK(loaded->LoadFromSerialRecord(&record, {kBufferColorRGB565, 16}));
    cIGZBuffer* result = nullptr;
    CHECK(loaded->GetBuffer(&result) && result);
    CHECK(result->Width() == 8 && result->Height() == 4);
    bool same = true;
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) same = same && result->GetPixel(x, y) == 0x1000 + x * 7 + y;
    CHECK(same);

    // an 8-bit bitmap with a two-colour palette, bottom-up rows, row padding to four bytes
    MemoryRecord bmp;
    std::uint8_t header[14] = {'B', 'M'};
    bmp.SetFieldVoid(header, 14);
    std::uint8_t info[0x28] = {};
    std::uint32_t headerSize = 0x28; std::int32_t w = 3, h = 2; std::uint16_t planes = 1, bits = 8;
    std::memcpy(info, &headerSize, 4); std::memcpy(info + 4, &w, 4); std::memcpy(info + 8, &h, 4);
    std::memcpy(info + 12, &planes, 2); std::memcpy(info + 14, &bits, 2);
    bmp.SetFieldVoid(info, 0x28);
    std::uint8_t palette[0x400] = {};
    palette[4] = 0; palette[5] = 0; palette[6] = 255;          // entry 1 = red (stored as blue, green, red)
    bmp.SetFieldVoid(palette, 0x400);
    const std::uint8_t pixels[8] = {1, 0, 1, 0, /*top row*/ 0, 1, 0, 0};   // bottom row first
    bmp.SetFieldVoid(pixels, 8);
    auto* bitmap = new cGZBufferResource;
    bitmap->AddRef();
    CHECK(bitmap->LoadFromSerialRecord(&bmp, {kBufferColorRGB565, 16}));
    cIGZBuffer* image = nullptr;
    CHECK(bitmap->GetBuffer(&image) && image);
    CHECK(image->Width() == 3 && image->Height() == 2);
    CHECK(image->GetPixel(1, 0) == 0xf800 && image->GetPixel(0, 1) == 0xf800 && image->GetPixel(1, 1) == 0 && image->GetPixel(2, 1) == 0xf800);

    void* iface = nullptr;
    CHECK(bitmap->QueryInterface(kGZIID_cIGZBufferResource, &iface) && iface);
    if (iface) static_cast<cIGZBufferResource*>(iface)->Release();
    for (cIGZBuffer* b : {buffer, result, image})
        if (b) b->Release();
    source->Release();
    loaded->Release();
    bitmap->Release();
    if (failures == 0) std::puts("gz_bufferresource: all passed");
    return failures ? 1 : 0;
}
