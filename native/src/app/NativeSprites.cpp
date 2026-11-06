// The animated sprites' services (s3d::ISpriteInstanceHost): the sprite animation headers and tables from the game's
// resources (s3d/ResSpriteAnim), the sprite sheets as driver textures (one per frame of the sheet's FSH, what the texture
// binding factory makes), and the draw context as the sprite instance talks to it. The pedestrians are drawn through this.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "app/NativeCityView.h"
#include "app/NativeUi.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "s3d/ResSpriteAnim.h"
#include "sc4/CameraControl.h"
#include "sc4/DrawContext.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"

namespace native {
const std::vector<std::uint32_t>* NativeFshFrameTextures(std::uint32_t group, std::uint32_t instance);   // app/NativeModelMaker.cpp

namespace {
using namespace s3d;

bool Trace() {
    static const bool on = std::getenv("SC4_SPRITEDEBUG") != nullptr;
    return on;
}

// The sprite's view of SC4DrawContext (one at a time: the renderer draws on one thread).
class DrawContextFace final : public ISpriteDrawContext {
public:
    void Bind(SC4DrawContext* context) { context_ = context; }
    int Zoom() override { return context_->Zoom(); }
    int Rotation() override { return context_->ViewRotation(); }
    cS3DCamera* Camera() override { return context_->Camera(); }
    float CameraMagnification() override {
        sc4::cSC43DRender* render = CityRenderer();
        sc4::cSC4CameraControl* control = render ? render->GetCameraControl() : nullptr;
        return control ? control->GetCustomMagnification() : 1.0f;
    }
    bool ViewPixelScale(float& scale) override {   // the view utilities' world units per viewport unit (+0x158)
        sc4::cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
        if (!view) return false;
        scale = view->ViewportUnitsToWorldUnits(1.0f);
        return true;
    }
    std::uint32_t ViewportWidth() override {
        sc4::cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
        const std::int32_t* rect = view ? view->ViewportRect() : nullptr;
        return rect && rect[2] > rect[0] ? std::uint32_t(rect[2] - rect[0]) : 1u;
    }
    void ResetModelViewTransform() override { context_->ResetModelViewTransform(); }
    void SetModelShade(cIS3DModelInstance* instance, const float* shade) override {
        const cS3DVector3 value{shade[0], shade[1], shade[2]};
        context_->SetModelShade(instance, &value);
    }
    void SetSelfLitShade(const float* shade) override { context_->SetSelfLitShade(cS3DVector3{shade[0], shade[1], shade[2]}); }
    void SetRenderState(const cSC4PackedRenderState& state) override { context_->SetRenderState(state); }
    void SetTransparency(float opacity) override { context_->SetTransparency(opacity); }
    void SetAlphaFunc(int function, float reference) override { context_->SetAlphaFunc(function, reference); }
    void SetTexture(std::uint32_t texture, int stage) override { context_->SetTexture(int(texture), stage); }
    void DrawPrims(int primitive, int format, int count, const SpriteVertex* vertices) override {
        context_->DrawPrims(primitive, format, count, vertices);
        ++drawn_;
    }
    void ResetShade() override { context_->ResetShade(); }
    void ResetTransparency() override { context_->ResetTransparency(); }
    long Drawn() const { return drawn_; }

private:
    SC4DrawContext* context_ = nullptr;
    long drawn_ = 0;
};

// A sprite sheet: the driver textures of the sheet's frames. The cache keeps them for the process (as the binding
// factory's key table does), so a sprite's Release has nothing to free.
class SheetBinding final : public ISpriteTextureBinding {
public:
    explicit SheetBinding(const std::vector<std::uint32_t>* ids) : ids_(ids) {}
    const std::uint32_t* TextureIds() override { return ids_->data(); }
    void Release() override {}

private:
    const std::vector<std::uint32_t>* ids_;
};

class Host final : public ISpriteInstanceHost {
public:
    ISpriteDrawContext* DrawContext(SC4DrawContext* context) override {
        face_.Bind(context);
        return &face_;
    }
    cIS3DResSpriteAnim* FindSpriteAnim(const cGZPersistResourceKey& key) override {
        auto& slot = anims_[{key.group, key.instance}];
        if (!slot.tried) {
            slot.tried = true;
            std::vector<std::uint8_t> bytes;
            SpriteAnimData data{};
            if (LoadResourceBytes(kResourceTypeSpriteAnim, key.group, key.instance, bytes) &&
                ReadSpriteAnim(bytes.data(), bytes.size(), key.instance, data))
                slot.anim = new cS3DResSpriteAnim(data);
            if (Trace())
                std::fprintf(stderr, "sprites: animation %08x/%08x -> %s (%zu bytes, %u frames, tables %08x %08x %08x %08x %08x)\n", key.group,
                             key.instance, slot.anim ? "ok" : "none", bytes.size(), data.frameCount, data.tableInstance[0], data.tableInstance[1],
                             data.tableInstance[2], data.tableInstance[3], data.tableInstance[4]);
        }
        if (slot.anim) slot.anim->AddRef();
        return slot.anim;
    }
    sc4::ISpriteAnimTable* FindSpriteAnimTable(std::uint32_t type, std::uint32_t group, std::uint32_t instance) override {
        auto& slot = tables_[{group, instance}];
        if (!slot.tried) {
            slot.tried = true;
            std::vector<std::uint8_t> bytes, block;
            if (LoadResourceBytes(type, group, instance, bytes) && ReadSpriteAnimTable(bytes.data(), bytes.size(), block))
                slot.table = new cS3DResSpriteAnimTable(std::move(block));
            if (Trace())
                std::fprintf(stderr, "sprites: table %08x/%08x/%08x -> %s (%zu bytes, %u records)\n", type, group, instance, slot.table ? "ok" : "none",
                             bytes.size(), slot.table ? slot.table->RecordCount() : 0);
        }
        if (slot.table) slot.table->AddRef();
        return slot.table;
    }
    ISpriteTextureBinding* CreateTextureBinding(const std::uint32_t key[3]) override {
        auto& slot = sheets_[{key[1], key[2]}];   // the binding factory's key table ignores the type
        if (!slot) {
            const std::vector<std::uint32_t>* ids = NativeFshFrameTextures(key[1], key[2]);
            if (Trace()) std::fprintf(stderr, "sprites: sheet %08x/%08x -> %zu textures\n", key[1], key[2], ids ? ids->size() : 0);
            if (!ids || ids->empty()) return nullptr;
            slot = std::make_unique<SheetBinding>(ids);
        }
        return slot.get();
    }
    bool TrianglePick(cSC4PickContext* context, const float* a, const float* b, const float* c) override {
        return SC4ViewUtilities::TrianglePick(context, a, b, c);
    }
    long Drawn() const { return face_.Drawn(); }

private:
    struct AnimSlot { bool tried = false; cS3DResSpriteAnim* anim = nullptr; };
    struct TableSlot { bool tried = false; cS3DResSpriteAnimTable* table = nullptr; };
    using Key = std::pair<std::uint32_t, std::uint32_t>;
    DrawContextFace face_;
    std::map<Key, AnimSlot> anims_;
    std::map<Key, TableSlot> tables_;
    std::map<Key, std::unique_ptr<SheetBinding>> sheets_;
};
Host g_host;

}  // namespace

// A sprite instance showing the animation `key` (type 0x29a5d1ec), with one reference; null when a resource is missing.
s3d::cS3DModelInstanceAnimatedSprite* NativeCreateSpriteInstance(const cGZPersistResourceKey& key) {
    if (!SpriteInstanceHost()) SetSpriteInstanceHost(&g_host);
    auto* sprite = new cS3DModelInstanceAnimatedSprite();
    sprite->AddRef();
    if (!sprite->LoadSpriteResource(key)) {
        sprite->Release();
        return nullptr;
    }
    return sprite;
}

// How many sprite quads have gone to the driver (the kit's report).
long NativeSpritesDrawn() { return g_host.Drawn(); }

}  // namespace native
