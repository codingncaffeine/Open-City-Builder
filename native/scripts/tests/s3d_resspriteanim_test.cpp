// The sprite animation resource readers (s3d/ResSpriteAnim): the animation header's table instances and zoom scales, the
// table block's size rule, and a read table driving a real sprite instance's direction lookup.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "s3d/ModelInstanceAnimatedSprite.h"
#include "s3d/ResSpriteAnim.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace s3d;

static void Put(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}
static std::vector<std::uint8_t> Anim(std::uint32_t tag, const std::uint32_t keys[5], std::uint32_t frames) {
    std::vector<std::uint8_t> b;
    Put(b, tag);
    for (std::uint32_t h : {0x7ab50e44u, 0x49a593e7u, 0x01600000u, 0x09adcd75u, 0x49a593e7u}) Put(b, h);
    for (int i = 0; i < 5; ++i) Put(b, keys[i]);
    Put(b, frames);
    return b;
}
static std::vector<std::uint8_t> Table(std::uint32_t tag, std::uint32_t directions, std::uint32_t count, std::size_t records) {
    std::vector<std::uint8_t> b;
    Put(b, tag);
    Put(b, directions);
    Put(b, 1);
    Put(b, 1);
    for (int i = 0; i < 4; ++i) Put(b, 0);   // the four angle floats (0)
    Put(b, count);
    for (std::size_t r = 0; r < records; ++r)
        for (std::uint8_t v : {std::uint8_t(r), std::uint8_t(8), std::uint8_t(r * 10), std::uint8_t(0), std::uint8_t(9), std::uint8_t(15), std::uint8_t(4), std::uint8_t(11)})
            b.push_back(v);
    return b;
}

// The sprite instance's host: tables come from the reader, nothing is drawn.
struct Host final : ISpriteInstanceHost {
    cS3DResSpriteAnimTable* table = nullptr;
    struct Binding final : ISpriteTextureBinding {
        std::uint32_t ids[2] = {7, 8};
        const std::uint32_t* TextureIds() override { return ids; }
        void Release() override {}
    } binding;
    ISpriteDrawContext* DrawContext(SC4DrawContext*) override { return nullptr; }
    cIS3DResSpriteAnim* FindSpriteAnim(const cGZPersistResourceKey&) override { return nullptr; }
    sc4::ISpriteAnimTable* FindSpriteAnimTable(std::uint32_t, std::uint32_t, std::uint32_t) override {
        table->AddRef();
        return table;
    }
    ISpriteTextureBinding* CreateTextureBinding(const std::uint32_t*) override { return &binding; }
    bool TrianglePick(cSC4PickContext*, const float*, const float*, const float*) override { return false; }
};

int main() {
    const std::uint32_t I = 0x01600000;
    {   // a pedestrian's header: zooms 0..3 share the own table, zoom 4 has the next one
        const std::uint32_t keys[5] = {0x11111111, 0x22222222, 0, I, I + 1};
        const auto bytes = Anim(0x29a5d1ec, keys, 11);
        SpriteAnimData d{};
        CHECK(ReadSpriteAnim(bytes.data(), bytes.size(), I, d));
        CHECK(d.textureKey[0] == 0x7ab50e44 && d.textureKey[1] == 0x49a593e7 && d.textureKey[2] == 0x01600000);
        CHECK(d.tableType == 0x09adcd75 && d.tableGroup == 0x49a593e7);
        CHECK(d.tableInstance[0] == I && d.tableInstance[1] == I && d.tableInstance[2] == I && d.tableInstance[3] == I);
        CHECK(d.tableInstance[4] == I + 1);
        CHECK(d.zoomScale[0] == 8.0f / 73.0f && d.zoomScale[1] == 16.0f / 73.0f && d.zoomScale[2] == 32.0f / 73.0f);
        CHECK(d.zoomScale[3] == 1.0f && d.zoomScale[4] == 1.0f);
        CHECK(d.frameCount == 11);
    }
    {   // three tables (the other shape the game's data has): zooms 0 and 1 draw zoom 2's pictures scaled down
        const std::uint32_t keys[5] = {0, 0, I, I + 1, I + 2};
        const auto bytes = Anim(0x29a5d1ec, keys, 7);
        SpriteAnimData d{};
        CHECK(ReadSpriteAnim(bytes.data(), bytes.size(), I, d));
        CHECK(d.tableInstance[0] == I && d.tableInstance[1] == I && d.tableInstance[2] == I);
        CHECK(d.tableInstance[3] == I + 1 && d.tableInstance[4] == I + 2);
        CHECK(d.zoomScale[0] == 0.25f && d.zoomScale[1] == 0.5f && d.zoomScale[2] == 1.0f && d.zoomScale[3] == 1.0f && d.zoomScale[4] == 1.0f);
    }
    {   // every zoom on the own table: the downward scales stay
        const std::uint32_t keys[5] = {0, 0, 0, 0, 0};
        const auto bytes = Anim(0x29a5d1ec, keys, 3);
        SpriteAnimData d{};
        CHECK(ReadSpriteAnim(bytes.data(), bytes.size(), I, d));
        for (int z = 0; z < 5; ++z) CHECK(d.tableInstance[z] == I);
        CHECK(d.zoomScale[0] == 8.0f / 146.0f && d.zoomScale[1] == 16.0f / 146.0f && d.zoomScale[2] == 32.0f / 146.0f);
        CHECK(d.zoomScale[3] == 73.0f / 146.0f && d.zoomScale[4] == 1.0f);
    }
    {   // the closest zoom on the own table and another table below it: the upward pass scales that table up
        const std::uint32_t J = I + 0x500;
        const std::uint32_t keys[5] = {9, 9, J, 0, 0};
        const auto bytes = Anim(0x29a5d1ec, keys, 3);
        SpriteAnimData d{};
        CHECK(ReadSpriteAnim(bytes.data(), bytes.size(), I, d));
        CHECK(d.tableInstance[0] == I && d.tableInstance[1] == I);
        CHECK(d.tableInstance[2] == J && d.tableInstance[3] == J && d.tableInstance[4] == J);
        CHECK(d.zoomScale[0] == 0.5f && d.zoomScale[1] == 1.0f && d.zoomScale[2] == 1.0f);
        CHECK(d.zoomScale[3] == 73.0f / 32.0f && d.zoomScale[4] == 146.0f / 32.0f);
    }
    {   // an instance below the own one counts from it
        const std::uint32_t keys[5] = {0, 0, 0, 0, 1};
        const auto bytes = Anim(0x29a5d1ec, keys, 3);
        SpriteAnimData d{};
        CHECK(ReadSpriteAnim(bytes.data(), bytes.size(), I, d));
        CHECK(d.tableInstance[4] == I + 1 && d.tableInstance[3] == I);
    }
    {   // refusals: a wrong tag, a short header, an own instance of 0 (the farthest zooms end on no table)
        const std::uint32_t keys[5] = {0, 0, 0, I, I + 1};
        auto bytes = Anim(0x29a5d1ed, keys, 11);
        SpriteAnimData d{};
        CHECK(!ReadSpriteAnim(bytes.data(), bytes.size(), I, d));
        bytes = Anim(0x29a5d1ec, keys, 11);
        CHECK(!ReadSpriteAnim(bytes.data(), bytes.size() - 1, I, d));
        const std::uint32_t high[5] = {5, 5, 5, 5, 5};
        bytes = Anim(0x29a5d1ec, high, 11);
        CHECK(!ReadSpriteAnim(bytes.data(), bytes.size(), 0, d));
    }
    {   // the table block: 32 + 8 n bytes after the tag
        std::vector<std::uint8_t> block;
        auto bytes = Table(0x09adcd75, 8, 8, 8);
        CHECK(ReadSpriteAnimTable(bytes.data(), bytes.size(), block));
        CHECK(block.size() == 32 + 64);
        bytes = Table(0x09adcd75, 8, 9, 8);
        CHECK(!ReadSpriteAnimTable(bytes.data(), bytes.size(), block) && block.empty());
        bytes = Table(0x09adcd75, 8, 7, 8);
        CHECK(!ReadSpriteAnimTable(bytes.data(), bytes.size(), block));
        bytes = Table(0x09adcd76, 8, 8, 8);
        CHECK(!ReadSpriteAnimTable(bytes.data(), bytes.size(), block));
        bytes = Table(0x09adcd75, 8, 0xe0000000u, 8);   // 32 + 8 n wraps to 32 in 32 bit arithmetic
        CHECK(!ReadSpriteAnimTable(bytes.data(), bytes.size(), block));
        bytes = Table(0x09adcd75, 8, 0, 0);
        CHECK(!ReadSpriteAnimTable(bytes.data(), 4 + 0x1f, block));
    }
    {   // a read table in a real sprite: the eight directions pick the eight records
        std::vector<std::uint8_t> block;
        const auto bytes = Table(0x09adcd75, 8, 8, 8);
        CHECK(ReadSpriteAnimTable(bytes.data(), bytes.size(), block));
        Host host;
        host.table = new cS3DResSpriteAnimTable(block);
        CHECK(host.table->RecordCount() == 8);
        CHECK(host.table->GetData()->countA == 8 && host.table->GetData()->countB == 1 && host.table->GetData()->countC == 1);
        SetSpriteInstanceHost(&host);
        const std::uint32_t keys[5] = {0, 0, 0, I, I + 1};
        const auto animBytes = Anim(0x29a5d1ec, keys, 11);
        SpriteAnimData d{};
        CHECK(ReadSpriteAnim(animBytes.data(), animBytes.size(), I, d));
        auto* anim = new cS3DResSpriteAnim(d);
        auto* sprite = new cS3DModelInstanceAnimatedSprite();
        sprite->AddRef();
        CHECK(sprite->LoadSpriteResource(anim));
        CHECK(sprite->GetFrameCount() == 11);
        CHECK(sprite->ZoomScale(2) == 32.0f / 73.0f);
        const float quarter = 1.5707964f;
        for (int direction = 0; direction < 8; ++direction) {
            const SpriteTextureRecord* record = sprite->GetTextureResourceData(0.0f, float(direction) * quarter * 0.5f, 0.0f, 4, 0);
            CHECK(record && record->texture == direction && record->x == direction * 10);
        }
        const SpriteTextureRecord* turned = sprite->GetTextureResourceData(0.0f, quarter, 0.0f, 4, 1);   // one view rotation back
        CHECK(turned && turned->texture == 0);
        sprite->Release();
        CHECK(anim->Release() == 0);
        SetSpriteInstanceHost(nullptr);
        host.table->Release();
    }
    std::printf(failures ? "FAILED %d\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
