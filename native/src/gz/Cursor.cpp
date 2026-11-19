#include "gz/Cursor.h"

#include <memory>
#include <vector>

#include "rz/cRZFile.h"

namespace gz {

namespace {
ICursorPlatform* g_platform = nullptr;

std::uint32_t Swap32(std::uint32_t v) {
    return v >> 24 | (v & 0xff0000) >> 8 | (v & 0xff00) << 8 | v << 24;
}
}  // namespace

bool gMacHardwareCursorSet = false;
ICursorPlatform* CursorPlatform() { return g_platform; }
void SetCursorPlatform(ICursorPlatform* platform) { g_platform = platform; }

// ------------------------------------------------------------------------------------------------- cRZCursor

cRZCursor::cRZCursor() = default;

cRZCursor::cRZCursor(const cIGZString& path) { LoadCursorA(path); }

cRZCursor::~cRZCursor() {
    handle_ = nullptr;
    if (buffer_) {
        buffer_->Release();
        buffer_ = nullptr;
    }
}

bool cRZCursor::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cIGZCursor) return false;
    *out = static_cast<cIGZCursor*>(this);
    AddRef();
    return true;
}

std::uint32_t cRZCursor::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cRZCursor::SetCursor(bool apply) {
    if (!apply) return;
    ICursorPlatform* platform = g_platform;
    gMacHardwareCursorSet = false;
    const bool graphics = platform && platform->HasGraphicsSystem();
    if (!supportsCursor_) {
        if (!handle_) {
            if (graphics && buffer_) {
                platform->SetHardwareCursor(buffer_, hotX_, hotY_);
                if (!gMacHardwareCursorSet) return;
                platform->SetHardwareCursor(nullptr, 0, 0);
                return;
            }
        } else if (platform) {
            platform->SetSystemCursor(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)) - 1);
        }
    } else if (platform) {
        platform->SetNamedCursor(name_.ToChar());
    }
    gMacHardwareCursorSet = true;
    if (graphics) platform->SetHardwareCursor(nullptr, 0, 0);
}

void cRZCursor::ShowCursor(bool show) {
    if (!g_platform) return;
    if (show) g_platform->ShowCursor();
    else g_platform->HideCursor();
}

void cRZCursor::SetCursorBuffer(cIGZBuffer* buffer) {
    if (buffer_ == buffer) return;
    if (buffer) buffer->AddRef();
    if (buffer_) buffer_->Release();
    buffer_ = buffer;
}

bool cRZCursor::LoadCursorA(const cIGZString& path) {
    ++sID;
    name_.Sprintf("%d-SC4", sID);
    SetCursorHandle(nullptr);
    cRZFile file(path);
    if (!file.Open(1, 2, 1)) return buffer_ != nullptr;

    auto read16 = [&file] { std::uint16_t v = 0; file.Read(&v, 2); return v; };
    auto read8 = [&file] { std::uint8_t v = 0; file.Read(&v, 1); return v; };
    auto read32 = [&file] { std::uint32_t v = 0; file.Read(&v, 4); return v; };

    if (read16() != 0) return false;
    const std::uint16_t type = read16();
    if (static_cast<std::uint16_t>(type - 1u) >= 2) return false;
    const std::uint16_t imageCount = read16();
    if (imageCount == 0) return false;
    const std::uint8_t width = read8(), height = read8(), colorCount = read8();
    file.SeekToRelativePosition(1);
    const std::uint8_t rotated = static_cast<std::uint8_t>(colorCount >> 1 | colorCount << 7);
    if (width != 0x20 || height != 0x20 || rotated >= 9 || !((0x113u >> rotated) & 1)) return false;

    std::uint16_t hotX = 0, hotY = 0;
    if (type == 2) {
        hotX = read16();
        hotY = read16();
    } else {
        file.SeekToRelativePosition(4);
    }
    hotX_ = static_cast<std::int16_t>(hotX);
    hotY_ = static_cast<std::int16_t>(hotY);
    if (static_cast<std::int16_t>(width) < static_cast<std::int16_t>(hotX)) hotX_ = 0;
    if (static_cast<std::int16_t>(height) < static_cast<std::int16_t>(hotY)) hotY_ = 0;
    file.SeekToRelativePosition(4);
    const std::uint32_t imageOffset = read32();
    file.SeekToPosition(imageOffset);

    if (!g_platform || !g_platform->HasGraphicsSystem()) return false;
    if (buffer_) buffer_->Release();   // a reload replaces the image
    buffer_ = g_platform->CreateBuffer();
    if (!buffer_) return false;
    if (!buffer_->Init(width, height, cGZBufferColorType{kBufferColorARGB8888, 32})) {
        buffer_->Release();
        buffer_ = nullptr;
        return false;
    }
    const std::uint32_t key = buffer_->ConvertRGBValueToNative(0xff, 0, 0xff);
    buffer_->SetTransparency(key);

    std::uint8_t header[0x28];
    file.Read(header, 0x28);
    const std::uint8_t bitCount = header[0x0e];
    std::uint32_t paletteSize = 1u << (bitCount & 0x1f);
    if (colorCount != 0) paletteSize = colorCount;
    paletteSize &= 0xffff;
    std::uint32_t palette[256] = {};
    if (paletteSize - 1u < 0x100) {
        for (std::uint32_t i = 0; i < paletteSize; ++i) {
            std::uint32_t entry = read32();
            palette[i] = buffer_->ConvertRGBValueToNative(static_cast<std::uint8_t>(entry >> 16), static_cast<std::uint8_t>(entry >> 8),
                                                          static_cast<std::uint8_t>(entry));
        }
    }

    bool colorPassDone = false;
    if (paletteSize == 0x10) {
        for (int y = height - 1; y >= 0; --y) {
            std::uint8_t pair = 0;
            for (int x = 0; x < width; ++x) {
                if ((x & 1) == 0) pair = read8();
                buffer_->SetPixel(x, y, palette[(x & 1) == 0 ? pair >> 4 : pair & 15]);
            }
        }
    } else if (paletteSize == 8) {
        // the colour pass is skipped
    } else if (paletteSize == 2) {
        for (int y = height - 1; y >= 0; --y) {
            std::uint32_t bits = Swap32(read32());
            for (int x = width - 1; x >= 0; --x) {
                buffer_->SetPixel(x, y, palette[bits & 1]);
                bits >>= 1;
            }
        }
    } else if (paletteSize == 0x100) {
        for (int y = height - 1; y >= 0; --y)
            for (int x = 0; x < width; ++x) buffer_->SetPixel(x, y, palette[read8()]);
    } else {
        for (int y = height - 1; y >= 0; --y)
            for (int x = 0; x < width; ++x) buffer_->SetPixel(x, y, read32());
    }
    (void)colorPassDone;

    // the AND mask: set bits are transparent
    for (int y = height - 1; y >= 0; --y) {
        if (file.Length() < static_cast<std::uint32_t>(file.Position()) + 4) break;
        std::uint32_t bits = Swap32(read32());
        for (int x = width - 1; x >= 0; --x) {
            if (bits & 1) buffer_->SetPixel(x, y, key);
            bits >>= 1;
        }
    }

    const std::uint32_t stride = buffer_->GetColorSurfaceStride();
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    std::size_t at = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const std::uint32_t pixel = buffer_->GetPixel(x, y);
            pixels[at++] = pixel == key ? (pixel & 0xffffff) : pixel;
        }
    g_platform->AddNamedCursor(name_.ToChar(), pixels.data(), width, height, static_cast<int>(stride), hotX, hotY);
    supportsCursor_ = true;
    return buffer_ != nullptr;
}

// ------------------------------------------------------------------------------------------ cRZCursorManager

cRZCursorManager::~cRZCursorManager() = default;

bool cRZCursorManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cIGZCursorManager) return false;
    *out = static_cast<cIGZCursorManager*>(this);
    AddRef();
    return true;
}

std::uint32_t cRZCursorManager::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cRZCursorManager::Init() {
    if (initialised_) return true;
    struct Stock { std::intptr_t handle; std::uint32_t id; };
    const Stock stock[5] = {{1, 1}, {5, 3}, {8, 0xf}, {1, 0x10}, {0xb, 0x11}};
    for (int i = 0; i < 5; ++i) {
        auto* cursor = new cRZSystemCursorW95(stock[i].handle);
        AddCursor(cursor, stock[i].id);
        if (i == 0) SetCursor(static_cast<cIGZCursor*>(cursor));
    }
    initialised_ = true;
    return true;
}

bool cRZCursorManager::Shutdown() {
    if (initialised_) {
        if (current_) {
            current_->Release();
            current_ = nullptr;
        }
        release_all_cursors();
        initialised_ = false;
    }
    return true;
}

void cRZCursorManager::release_all_cursors() {
    for (auto& entry : cursors_) entry.second->Release();
    cursors_.clear();
}

void cRZCursorManager::HideCursor() {
    if (g_platform) g_platform->HideCursor();
}

void cRZCursorManager::ShowCursor() {
    if (g_platform) g_platform->ShowCursor();
}

void cRZCursorManager::AddCursor(const cIGZString& path, std::uint32_t id) { AddCursor(new cRZCursor(path), id); }

bool cRZCursorManager::AddCursor(cIGZCursor* cursor, std::uint32_t id) {
    if (cursor) cursor->AddRef();
    auto existing = cursors_.find(id);
    if (existing != cursors_.end()) {
        if (existing->second) existing->second->Release();
        cursors_.erase(existing);
    }
    if (cursor) cursors_.emplace(id, cursor);
    return true;
}

bool cRZCursorManager::RemoveCursor(std::uint32_t id) {
    auto it = cursors_.find(id);
    if (it == cursors_.end()) return false;
    it->second->Release();
    cursors_.erase(it);
    return true;
}

cIGZCursor* cRZCursorManager::GetCursorByID(std::uint32_t id) const {
    auto it = cursors_.find(id);
    return it == cursors_.end() ? nullptr : it->second;
}

bool cRZCursorManager::SetCursor(std::uint32_t id) {
    cIGZCursor* cursor = GetCursorByID(id);
    if (!cursor) return false;
    cursor->SetCursor(true);
    if (current_ != cursor) {
        cursor->AddRef();
        if (current_) current_->Release();
        current_ = cursor;
    }
    return true;
}

void cRZCursorManager::SetCursor(cIGZCursor* cursor) {
    if (current_ != cursor) {
        if (cursor) cursor->AddRef();
        if (current_) current_->Release();
        current_ = cursor;
    }
    if (cursor) cursor->SetCursor(true);
}

bool cRZCursorManager::PushAutoCursor(std::uint32_t id) {
    cIGZCursor* saved = current_;
    if (saved) saved->AddRef();
    const bool ok = SetCursor(id);
    if (!ok) {
        if (saved) saved->Release();
        return false;
    }
    autoStack_.push_back(saved);
    return true;
}

bool cRZCursorManager::PopAutoCursor() {
    if (autoStack_.empty()) return false;
    cIGZCursor* cursor = autoStack_.back();
    autoStack_.pop_back();
    SetCursor(cursor);
    if (cursor) cursor->Release();
    return true;
}

}  // namespace gz
