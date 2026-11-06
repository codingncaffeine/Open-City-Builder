// The sprite animation resources (cS3DResSpriteAnimFactory's two types): the animation header (resource type 0x29a5d1ec,
// interface 0x29a5d1ec) that names a sprite sheet texture and one table per zoom, and the table (resource type 0x09adcd75,
// interface 0x69b6f01c) that holds one 8 byte sheet record per view direction. The pedestrians and every other animated
// sprite (cS3DModelInstanceAnimatedSprite) are drawn from these.
//
// Readers after the reference description of ReadAnim / ReadTable (all fields little-endian u32):
//   animation: tag 0x29a5d1ec, five header words (texture type / group / instance, table type / group), five table
//     instances for zooms 0..4, one last word (the frame count). The instances of zooms 0 and 1 are dropped; an instance
//     below the resource's own counts from it (0 = the own instance). Each zoom then gets a scale from the zoom sizes
//     (8, 16, 32, 73, 146): a zoom that shares its table with a closer or farther zoom draws that table's pictures scaled.
//   table: tag 0x09adcd75, then one block of 32 + 8 n bytes where n is the u32 at block offset 0x1c.
// Port choice: the table reader refuses a block shorter than its header or with a count that does not fit (the original
// compares in wrapping 32 bit arithmetic).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/MiscTypes.h"

namespace s3d {

constexpr std::uint32_t kResourceTypeSpriteAnim = 0x29a5d1ec;
constexpr std::uint32_t kResourceTypeSpriteAnimTable = 0x09adcd75;

// The animation header of a resource whose own instance is `ownInstance`; false leaves `out` unspecified.
bool ReadSpriteAnim(const std::uint8_t* bytes, std::size_t size, std::uint32_t ownInstance, SpriteAnimData& out);
// The table's block (header + records, without the tag); false leaves `block` empty.
bool ReadSpriteAnimTable(const std::uint8_t* bytes, std::size_t size, std::vector<std::uint8_t>& block);

// A read animation header (reference counted; a new object holds one reference).
class cS3DResSpriteAnim final : public cIS3DResSpriteAnim {
public:
    explicit cS3DResSpriteAnim(const SpriteAnimData& data) : data_(data) {}
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override;
    const SpriteAnimData* GetData() override { return &data_; }

private:
    SpriteAnimData data_;
    std::uint32_t refs_ = 1;
};

// A read table (reference counted; a new object holds one reference). GetData answers the block: the header the sprite
// reads, with the records 0x20 bytes later.
class cS3DResSpriteAnimTable final : public sc4::ISpriteAnimTable {
public:
    explicit cS3DResSpriteAnimTable(std::vector<std::uint8_t> block);
    void AddRef() override { ++refs_; }
    void Release() override;
    const sc4::SpriteAnimTableHeader* GetData() override;
    std::uint32_t RecordCount() const;

private:
    std::vector<std::uint32_t> words_;   // the block, kept aligned for the header's floats
    std::uint32_t refs_ = 1;
};

}  // namespace s3d
