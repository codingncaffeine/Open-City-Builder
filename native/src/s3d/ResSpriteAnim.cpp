#include "s3d/ResSpriteAnim.h"

#include <cstring>

namespace s3d {
namespace {

std::uint32_t Word(const std::uint8_t* bytes, std::size_t index) {
    const std::uint8_t* p = bytes + index * 4;
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

constexpr float kZoomSize[5] = {8.0f, 16.0f, 32.0f, 73.0f, 146.0f};

}  // namespace

bool ReadSpriteAnim(const std::uint8_t* bytes, std::size_t size, std::uint32_t ownInstance, SpriteAnimData& out) {
    if (!bytes || size < 12 * 4) return false;
    if (Word(bytes, 0) != kResourceTypeSpriteAnim) return false;
    for (int i = 0; i < 3; ++i) out.textureKey[i] = Word(bytes, 1 + i);
    out.tableType = Word(bytes, 4);
    out.tableGroup = Word(bytes, 5);
    std::uint32_t key[5];
    for (int z = 0; z < 5; ++z) key[z] = Word(bytes, 6 + z);
    key[0] = 0;
    key[1] = 0;
    for (int z = 0; z < 5; ++z)
        if (key[z] < ownInstance) key[z] += ownInstance;   // relative to the own instance (wraps like the original)
    out.frameCount = Word(bytes, 11);

    // Downwards: the closest zoom is its own source; a farther zoom with the same table draws it scaled down, another
    // table becomes the source. The pass must end on a real table.
    float scale[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    int source = 4;
    for (int z = 3; z >= 0; --z) {
        if (key[z] == key[source]) {
            scale[z] = kZoomSize[z] / kZoomSize[source];
        } else {
            source = z;
            scale[z] = 1.0f;
        }
    }
    if (key[source] == 0) return false;
    // Upwards, only when the closest zoom has the own table: a zoom with the own table takes the last other table below
    // it, scaled up; the leading own-table zooms keep what the downward pass gave them.
    if (key[4] == ownInstance) {
        int up = -1;
        for (int z = 0; z < 5; ++z) {
            if (key[z] != ownInstance) {
                up = z;
            } else if (up >= 0 && key[up] != 0) {
                key[z] = key[up];
                scale[z] = kZoomSize[z] / kZoomSize[up];
            }
        }
    }
    for (int z = 0; z < 5; ++z) {
        out.tableInstance[z] = key[z];
        out.zoomScale[z] = scale[z];
    }
    return true;
}

bool ReadSpriteAnimTable(const std::uint8_t* bytes, std::size_t size, std::vector<std::uint8_t>& block) {
    block.clear();
    if (!bytes || size < 4 + 0x20) return false;
    if (Word(bytes, 0) != kResourceTypeSpriteAnimTable) return false;
    const std::size_t rest = size - 4;
    const std::uint32_t count = Word(bytes + 4, 7);   // block offset 0x1c
    if (count > (rest - 0x20) / 8 || rest != 0x20 + std::size_t(count) * 8) return false;
    block.assign(bytes + 4, bytes + size);
    return true;
}

bool cS3DResSpriteAnim::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cIS3DResSpriteAnim) {
        *out = static_cast<cIS3DResSpriteAnim*>(this);
        AddRef();
        return true;
    }
    *out = nullptr;
    return false;
}

std::uint32_t cS3DResSpriteAnim::Release() {
    const std::uint32_t left = --refs_;
    if (left == 0) delete this;
    return left;
}

cS3DResSpriteAnimTable::cS3DResSpriteAnimTable(std::vector<std::uint8_t> block) : words_((block.size() + 3) / 4) {
    if (!block.empty()) std::memcpy(words_.data(), block.data(), block.size());
}

void cS3DResSpriteAnimTable::Release() {
    if (--refs_ == 0) delete this;
}

const sc4::SpriteAnimTableHeader* cS3DResSpriteAnimTable::GetData() {
    return reinterpret_cast<const sc4::SpriteAnimTableHeader*>(words_.data());
}

std::uint32_t cS3DResSpriteAnimTable::RecordCount() const { return words_.size() > 7 ? words_[7] : 0; }

}  // namespace s3d
