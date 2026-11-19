// The port's image codecs through cGZGimexFactory: a 32-bit buffer saved as PNG (alpha kept for A8R8G8B8, dropped for
// X8R8G8B8) and as BMP, then loaded back; the 16-bit conversions both ways (gimex.md sections 5 and 6).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/NativeGimex.h"
#include "gz/Buffer.h"
#include "gz/GimexFactory.h"
#include "gz/GimexReaderWriter.h"
#include "rz/COMDirectors.h"
#include "rz/cRZString.h"

namespace gz { void RegisterDirectorMaker(const char*, DirectorMaker) {} }   // the test makes its readers itself

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
cGZBuffer* Make(int w, int h, std::int32_t type, std::uint32_t bits) {
    auto* b = new cGZBuffer;
    b->AddRef();
    b->Init(w, h, cGZBufferColorType{type, bits});
    return b;
}
std::uint32_t Pixel32(cIGZBuffer* b, int x, int y) {
    std::uint32_t v;
    std::memcpy(&v, static_cast<std::uint8_t*>(b->GetColorSurfaceBits()) + y * b->GetColorSurfaceStride() + x * 4, 4);
    return v;
}
std::uint16_t Pixel16(cIGZBuffer* b, int x, int y) {
    std::uint16_t v;
    std::memcpy(&v, static_cast<std::uint8_t*>(b->GetColorSurfaceBits()) + y * b->GetColorSurfaceStride() + x * 2, 2);
    return v;
}
void Set32(cIGZBuffer* b, int x, int y, std::uint32_t v) {
    std::memcpy(static_cast<std::uint8_t*>(b->GetColorSurfaceBits()) + y * b->GetColorSurfaceStride() + x * 4, &v, 4);
}
void Set16(cIGZBuffer* b, int x, int y, std::uint16_t v) {
    std::memcpy(static_cast<std::uint8_t*>(b->GetColorSurfaceBits()) + y * b->GetColorSurfaceStride() + x * 2, &v, 2);
}
}  // namespace

int main() {
    const char* dir = std::getenv("TMPDIR");
    const std::string base = std::string(dir && *dir ? dir : "/tmp") + "/nativegimex_test";
    native::InstallImageCodecs();
    cGZGimexFactory factory;
    factory.AddRef();
    factory.makeReader = [] { return static_cast<cIGZGimexReader*>(new cGZGimexReader()); };
    factory.makeWriter = [] { return static_cast<cIGZGimexWriter*>(new cGZGimexWriter()); };
    factory.makeBuffer = [] { return static_cast<cIGZBuffer*>(new cGZBuffer()); };

    // A8R8G8B8 through PNG: every pixel and its alpha come back.
    cGZBuffer* source = Make(5, 3, 9, 32);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) Set32(source, x, y, 0x80000000u | static_cast<std::uint32_t>(x * 40) << 16 | static_cast<std::uint32_t>(y * 90) << 8 | 0x11u * (x + y));
    const cRZString png((base + "_a.png").c_str());
    CHECK(factory.SaveToFile(png, source, 100));
    cGZBuffer* back = Make(1, 1, 9, 32);
    CHECK(factory.LoadFromFile(png, back, 9));
    CHECK(back->Width() == 5 && back->Height() == 3);
    bool same = true;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) same = same && Pixel32(back, x, y) == Pixel32(source, x, y);
    CHECK(same);

    // X8R8G8B8: the PNG has no alpha; a load reads it opaque.
    cGZBuffer* opaque = Make(2, 2, 8, 32);
    Set32(opaque, 0, 0, 0x00123456u); Set32(opaque, 1, 0, 0x00abcdefu); Set32(opaque, 0, 1, 0x00010203u); Set32(opaque, 1, 1, 0x00fffefdu);
    const cRZString rgb((base + "_b.png").c_str());
    CHECK(factory.SaveToFile(rgb, opaque, 100));
    CHECK(factory.LoadFromFile(rgb, back, 9));
    CHECK(Pixel32(back, 0, 0) == 0xff123456u && Pixel32(back, 1, 1) == 0xfffffefdu);

    // BMP keeps the colors (24-bit) and reads opaque.
    const cRZString bmp((base + "_c.bmp").c_str());
    CHECK(factory.SaveToFile(bmp, opaque, 100));
    CHECK(factory.LoadFromFile(bmp, back, 9));
    CHECK(back->Width() == 2 && Pixel32(back, 1, 0) == 0xffabcdefu && Pixel32(back, 0, 1) == 0xff010203u);

    // R5G6B5 saved: 5- and 6-bit channels widen as c * 255 / 31 (63); loaded into R5G6B5: the green quirk (halved).
    cGZBuffer* rgb565 = Make(1, 1, 5, 16);
    Set16(rgb565, 0, 0, static_cast<std::uint16_t>(31 << 11 | 63 << 5 | 16));
    const cRZString p565((base + "_d.png").c_str());
    CHECK(factory.SaveToFile(p565, rgb565, 100));
    CHECK(factory.LoadFromFile(p565, back, 9));
    CHECK(Pixel32(back, 0, 0) == (0xff000000u | 255u << 16 | 255u << 8 | (16u * 255 / 31)));
    cGZBuffer* load565 = Make(1, 1, 5, 16);
    CHECK(factory.LoadFromFile(p565, load565, 5));
    CHECK(Pixel16(load565, 0, 0) == static_cast<std::uint16_t>(0xf800 | 0x3e0 | ((16u * 255 / 31) >> 3)));

    // A1R5G5B5 keeps its alpha bit through a load; X1R5G5B5 sets it.
    cGZBuffer* load1555 = Make(1, 1, 6, 16);
    CHECK(factory.LoadFromFile(png, load1555, 6));
    CHECK((Pixel16(load1555, 0, 0) & 0x8000) != 0);   // alpha 0x80 has the top bit
    cGZBuffer* load555 = Make(1, 1, 4, 16);
    CHECK(factory.LoadFromFile(rgb, load555, 4));
    CHECK(Pixel16(load555, 0, 0) == static_cast<std::uint16_t>(0x8000 | (0x12 >> 3) << 10 | (0x34 >> 3) << 5 | 0x56 >> 3));

    // Not an image: the load fails.
    {
        std::FILE* f = std::fopen((base + "_e.png").c_str(), "wb");
        std::fputs("not a picture", f);
        std::fclose(f);
        CHECK(!factory.LoadFromFile(cRZString((base + "_e.png").c_str()), back, 9));
    }

    for (cGZBuffer* b : {source, back, opaque, rgb565, load565, load1555, load555}) b->Release();
    for (const char* s : {"_a.png", "_b.png", "_c.bmp", "_d.png", "_e.png"}) std::remove((base + s).c_str());
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("app_nativegimex: all checks passed\n");
    return failures ? 1 : 0;
}
