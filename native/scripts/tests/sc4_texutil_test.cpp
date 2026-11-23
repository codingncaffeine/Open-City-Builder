// nsTexUtil: the power of two, the texture release, and SetupTexture's two paths (a square 32-bit buffer loaded as is with
// its texture size and format; anything else copied into the next square power of two first).
#include <cstdio>
#include <vector>

#include "gz/Buffer.h"
#include "render/gd/GDriverBase.h"
#include "sc4/TexUtil.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct FakeDriver final : gd::cGDriverBase {
    struct Load { std::uint32_t texture, width, height, format, type, rowLength; const void* pixels; };
    std::uint32_t CreateTexture(gd::eGDInternalTextureFormat format, std::uint32_t w, std::uint32_t h, std::uint32_t levels,
                                gd::eGDTextureHintFlags hint) override {
        created.push_back({format, w, h, levels, hint});
        return 40 + static_cast<std::uint32_t>(created.size());
    }
    void LoadTextureLevel(std::uint32_t texture, std::uint32_t level, std::uint32_t, std::uint32_t, std::uint32_t width,
                          std::uint32_t height, gd::eGDTextureFormat format, gd::eGDType type, std::uint32_t rowLength,
                          const void* pixels) override {
        if (level == 0) loads.push_back({texture, width, height, format, type, rowLength, pixels});
    }
    void DeleteTextures(int count, const std::uint32_t* ids) override { for (int i = 0; i < count; ++i) deleted.push_back(ids[i]); }
    std::vector<std::vector<std::uint32_t>> created;
    std::vector<Load> loads;
    std::vector<std::uint32_t> deleted;
};

cGZBuffer* MakeBuffer(std::uint32_t w, std::uint32_t h) {
    auto* b = new cGZBuffer();
    b->AddRef();
    b->Init(w, h, cGZBufferColorType{kBufferColorARGB8888, 32});
    return b;
}
}  // namespace

int main() {
    CHECK(nsTexUtil::GetNearestPowerOfTwo(0) == 1 && nsTexUtil::GetNearestPowerOfTwo(1) == 1);
    CHECK(nsTexUtil::GetNearestPowerOfTwo(33) == 64 && nsTexUtil::GetNearestPowerOfTwo(64) == 64);

    FakeDriver driver;
    nsTexUtil::ReleaseTexture(&driver, 7);
    nsTexUtil::ReleaseTexture(nullptr, 8);
    CHECK(driver.deleted.size() == 1 && driver.deleted[0] == 7);

    // a square 32x32 buffer: loaded directly, internal format 4 (alpha) / 1, one level, hint 2, format 3, type 1
    cGZBuffer* square = MakeBuffer(32, 32);
    std::uint32_t texture = 0, width = 0, size = 0;
    CHECK(nsTexUtil::SetupTexture(square, &driver, texture, true, nullptr, &width, &size));
    CHECK(driver.created.size() == 1 && driver.created[0] == (std::vector<std::uint32_t>{4, 32, 32, 1, 2}));
    CHECK(texture == 41 && width == 32 && size == 32);
    CHECK(driver.loads.size() == 1 && driver.loads[0].width == 32 && driver.loads[0].height == 32 && driver.loads[0].format == 3 &&
          driver.loads[0].type == 1 && driver.loads[0].rowLength == 32 && driver.loads[0].pixels != nullptr);
    CHECK(nsTexUtil::SetupTexture(square, &driver, texture, false, nullptr, nullptr, nullptr));
    CHECK(driver.created.size() == 2 && driver.created[1][0] == 1);
    CHECK(!square->IsLocked());

    // a 20x10 buffer: copied into a 32x32 square first, so the load is 32 wide from a 32-wide copy
    cGZBuffer* odd = MakeBuffer(20, 10);
    CHECK(nsTexUtil::SetupTexture(odd, &driver, texture, true, nullptr, &width, &size));
    CHECK(driver.created.size() == 3 && driver.created[2][1] == 32 && driver.created[2][2] == 32);
    CHECK(width == 32 && size == 32 && driver.loads.back().rowLength == 32 && driver.loads.back().width == 32);
    CHECK(!odd->IsLocked());

    square->Release();
    odd->Release();
    if (failures == 0) std::printf("sc4_texutil: ok\n");
    return failures == 0 ? 0 : 1;
}
