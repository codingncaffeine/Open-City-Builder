#include "sc4/SLBuffer.h"

namespace {
ISLBufferHost* g_host = nullptr;
// DAT_100a15830: bits per pixel for colour types 2..9, stored in the upper half of the 64-bit colour type.
constexpr std::uint32_t kBitsForType[8] = {8, 16, 16, 16, 16, 16, 32, 32};
}  // namespace

ISLBufferHost* SLBufferHost() { return g_host; }
void SetSLBufferHost(ISLBufferHost* host) { g_host = host; }

cSLBuffer::cSLBuffer(bool alwaysReload) : colorType_(1), alwaysReload_(alwaysReload) {}

cSLBuffer::cSLBuffer(const cGZPersistResourceKey& key, std::int32_t colorType, bool alwaysReload)
    : key_(key), colorType_(colorType), alwaysReload_(alwaysReload) {
    Load();
}

cSLBuffer::cSLBuffer(std::uint32_t type, std::uint32_t group, std::uint32_t instance, std::int32_t colorType, bool alwaysReload)
    : key_{type, group, instance}, colorType_(colorType), alwaysReload_(alwaysReload) {
    Load();
}

cSLBuffer::cSLBuffer(const cRZString& path, std::int32_t colorType, bool alwaysReload)
    : key_{}, path_(path.ToChar(), path.Strlen()), colorType_(colorType), alwaysReload_(alwaysReload) {
    Load();
}

bool cSLBuffer::Load() {
    if (buffer_ && !alwaysReload_) return true;
    ISLBufferHost* host = g_host;
    if (!host) return buffer_.get() != nullptr;

    if (!path_.empty()) {
        cIGZBuffer* loaded = nullptr;
        host->LoadBufferFromFile(path_, colorType_, &loaded);
        cRZAutoRefCount<cIGZBuffer> hold;
        hold = loaded;
        if (loaded) loaded->Release();   // the host handed over a counted reference
        buffer_ = hold;
        return buffer_.get() != nullptr;
    }

    cGZPersistResourceKey key = key_;
    if (key.type == 0) key.type = kSLBitmapResourceType;
    if (host->LowQualityTextures()) {
        key.group = 1;
        if (!host->HasResource(key)) key.group = key_.group;
    }
    cIGZBuffer* loaded = nullptr;
    if (host->GetBufferResource(key, colorType_, alwaysReload_, &loaded)) buffer_ = loaded;
    if (loaded) loaded->Release();

    if (buffer_ && colorType_ != 1 && colorType_ != 3) {
        cRZAutoRefCount<cIGZBuffer> converted;
        cIGZBuffer* fresh = host->CreateBuffer();
        if (fresh) {   // the host's counted reference is the one released below
            cGZBufferColorType type;
            type.type = colorType_;
            const std::uint32_t index = static_cast<std::uint32_t>(colorType_) - 2;
            type.bitsPerPixel = index < 8 ? kBitsForType[index] : 0;
            if (fresh->Init(buffer_->Width(), buffer_->Height(), type)) {
                std::uint32_t colorKey = 0;
                const bool keyed = buffer_->GetTransparentColor(colorKey);
                if (keyed) buffer_->RemoveTransparency();
                if (fresh->Lock(cIGZBuffer::eLockFlags(0x8010))) {
                    cRZRect source, destination;
                    buffer_->GetBufferArea(source);
                    fresh->GetBufferArea(destination);
                    fresh->Blt(buffer_.get(), source, destination, nullptr);
                    fresh->Unlock(cIGZBuffer::eLockFlags(0x8010));
                    converted = fresh;
                }
                if (keyed) buffer_->SetTransparency(colorKey);   // only the original gets its colour key back
            }
            fresh->Release();
        }
        buffer_ = converted;   // null when the conversion failed
    }
    return buffer_.get() != nullptr;
}
