#include "sc4/TexUtil.h"

#include "gz/Buffer.h"
#include "render/gd/IGZGDriver.h"

namespace nsTexUtil {

namespace {
constexpr std::uint32_t kLockReadTexture = 0x8040;   // Lock / Unlock flags of the source
constexpr std::uint32_t kLockWriteCopy = 0x8010;     // Lock / Unlock flags of the copy
}  // namespace

int GetNearestPowerOfTwo(int value) {
    int power = 1;
    while (power < value) power *= 2;
    return power;
}

void ReleaseTexture(gd::cIGZGDriver* driver, std::uint32_t texture) {
    if (driver) driver->DeleteTextures(1, &texture);   // vslot 0x90
}

bool SetupTexture(cIGZBuffer* buffer, gd::cIGZGDriver* driver, std::uint32_t& texture, bool alpha, const cRZRect* rect,
                  std::uint32_t* outWidth, std::uint32_t* outSize) {
    if (!buffer || !buffer->Lock(static_cast<cIGZBuffer::eLockFlags>(kLockReadTexture))) return true;   // the original answers 1
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());   // vslot 0x88
    const std::uint32_t bufferWidth = buffer->Width();
    std::uint32_t width = bufferWidth;
    std::uint32_t height = buffer->Height();
    const std::uint32_t bitsPerPixel = buffer->GetBitsPerPixel();
    if (rect) {
        width = static_cast<std::uint32_t>(rect->right - rect->left);
        height = static_cast<std::uint32_t>(rect->bottom - rect->top);
    }
    const int sizeW = GetNearestPowerOfTwo(static_cast<int>(width));
    const int sizeH = GetNearestPowerOfTwo(static_cast<int>(height));
    const std::uint32_t size = static_cast<std::uint32_t>(sizeW < sizeH ? sizeH : sizeW);
    if (width != size || width != height || bitsPerPixel != 32) {
        bool ok = false;
        auto* copy = new cGZBuffer();   // RZ::GS() vslot 0x10
        copy->AddRef();
        if (copy->Init(size, size, cGZBufferColorType{kBufferColorARGB8888, 0x20})) {
            if (copy->Lock(static_cast<cIGZBuffer::eLockFlags>(kLockWriteCopy))) {
                copy->Fill(0u, nullptr);
                const cRZRect* area = buffer->GetBufferArea();   // vslot 0x2c, as both the source and the destination
                if (area) copy->Blt(buffer, *area, *area, nullptr);
                copy->Unlock(static_cast<cIGZBuffer::eLockFlags>(kLockWriteCopy));
            }
            ok = SetupTexture(copy, driver, texture, alpha, nullptr, outWidth, outSize);
        }
        copy->Release();
        buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(kLockReadTexture));
        return ok;
    }
    if (outWidth) *outWidth = width;
    if (outSize) *outSize = size;
    if (driver) {
        texture = driver->CreateTexture(alpha ? 4u : 1u, size, size, 1, 2);   // vslot 0xf0
        if (rect) bits += buffer->GetColorSurfaceStride() * static_cast<std::uint32_t>(rect->top) + static_cast<std::uint32_t>(rect->left) * 4;
        // Quirk kept: the height loaded is the width (the buffer is square here unless a rect was given).
        driver->LoadTextureLevel(texture, 0, 0, 0, width, width, 3, 1, bufferWidth, bits);   // vslot 0xf4
    }
    buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(kLockReadTexture));
    return true;
}

}  // namespace nsTexUtil
