// cSC4GlyphTextureManager — the textures text in the 3D view is drawn with (signposts, labels): Mac i386 0x004c4320..0x004c5134,
// spec OpenSC4 glyph-textures.md. Two kinds:
//  * glyph textures: one texture per (character, font), shared by everyone who asks for that pair and freed as soon as nobody
//    else holds it (GetGlyphTexture 0x004c4d96, StringToGlyphTextureArray 0x004c4a60);
//  * string textures: a whole string in one texture, made on first use of its texture id and kept in a cache of at most
//    kStringTextureCacheSize KB, trimmed every 50 ms with a second-chance sweep (GetCachedStringTexture 0x004c5134,
//    GetBoundIdForTexture 0x004c46b8, OnTick 0x004c4792).
// Both are rendered by nS3D::CreateTextureFromText with a font from RZ::WCM(): that pair is the host (IGlyphTextureHost).
//
// Quirks kept: a glyph info's Release at a count of 1 with a manager asks the manager to drop it (the map's reference), so a
// glyph lives exactly as long as someone besides the map holds it; a string texture at a count of 1 with a manager stays alive
// (the cache's reference) and Release answers 0; Shutdown leaves the string cache alone; a large cache excess is cut by only a
// tenth of the budget per sweep.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "sc4/OverlayManager.h"
#include "sc4/SignpostOccupant.h"
#include "sc4/TextDecalOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4GlyphTextureManager = 0x0b54c519;   // the only interface its QueryInterface answers
constexpr std::uint32_t kGlyphTextureManagerServiceId = 0xabeb5e37;
constexpr std::uint32_t kMessageRestoreTextures = 0x8acaa95f;           // kSGLMessage_RestoreTextures
constexpr std::uint32_t kStringTextureCacheDefaultKB = 1024;

// nS3D::CreateTextureFromText and the fonts behind it (RZ::WCM() slot 5), the texture binding factory's textures and the
// framework's message server.
class IGlyphTextureHost {
public:
    virtual ~IGlyphTextureHost() = default;
    virtual bool HasFont(std::uint32_t fontId) = 0;                                           // RZ::WCM() slot 5 not null
    // The text's size in the font: width (font slot 0xc0) and height (font slot 0x94).
    virtual void MeasureText(std::uint32_t fontId, const char* text, std::uint32_t length, int& width, int& height) = 0;
    // CreateTextureFromText(text, binding, font, size, rect, false): a W x H texture (each the smallest power of two >= the
    // size + 2), the text in white centred in it; size receives (W, H), rect (x0 + w, y0 + h, x0, y0). 0 = failed.
    virtual std::uint32_t CreateTextureFromText(std::uint32_t fontId, const char* text, std::uint32_t length, int* size, float* rect) = 0;
    virtual void DeleteTexture(std::uint32_t texture) = 0;
    virtual std::uint32_t StringTextureCacheKB() { return kStringTextureCacheDefaultKB; }   // render property kStringTextureCacheSize
    virtual cIGZMessageServer2* Messages() { return nullptr; }
};

class cSC4GlyphTextureManager;

// cS3DTextureBinding as the glyphs hold it: one driver texture, deleted with the binding.
class GlyphTextureBinding final : public ISignpostTextureBinding {
public:
    GlyphTextureBinding(IGlyphTextureHost* host, std::uint32_t texture) : host_(host), texture_(texture) {}
    std::uint32_t TextureId() const override { return texture_; }
    void Release() override;

private:
    IGlyphTextureHost* host_;
    std::uint32_t texture_;
};

class cSC4GlyphTextureManager : public cIGZMessageTarget2 {   // the original: cIGZUnknown + system service + message target at +0x20
public:
    // cSC4GlyphTextureInfo (0x34 bytes on the Mac): one character's texture.
    class GlyphInfo final : public ISignpostGlyphTextureInfo {
    public:
        bool QueryInterface(std::uint32_t, void**) override { return false; }   // shared body: false, out untouched
        std::uint32_t AddRef() override { return ++refs_; }
        std::uint32_t Release() override;                                      // 0x004c4678
        void GetGlyph(cIGZString& out) override { out.AssignFromChars(glyph_.ToChar(), glyph_.Strlen()); }
        void GetTextureSize(int* wh) override { wh[0] = textureW_; wh[1] = textureH_; }
        void GetGlyphSize(int* wh) override { wh[0] = glyphW_; wh[1] = glyphH_; }
        void GetGlyphTextureCoordinates(float* rect) override { for (int i = 0; i < 4; ++i) rect[i] = uv_[i]; }   // 0x004c4494
        ISignpostTextureBinding* GetTextureBinding() override { return binding_; }   // no reference
        void SetManager(cSC4GlyphTextureManager* manager);                     // 0x004c446e

    private:
        friend class cSC4GlyphTextureManager;
        ~GlyphInfo();
        cRZString glyph_;                                  // +8
        std::uint16_t textureW_ = 0, textureH_ = 0;        // +0x10
        std::uint16_t glyphW_ = 0, glyphH_ = 0;            // +0x14
        float uv_[4] = {};                                 // +0x18 left, top, right, bottom
        GlyphTextureBinding* binding_ = nullptr;           // +0x28
        std::uint32_t refs_ = 0;
        cSC4GlyphTextureManager* manager_ = nullptr;       // counted
    };

    // cSC4CachedStringTexture: a whole string; its texture is made when its id is first asked for. It is the text decal's
    // ITextTexture and, through AsOverlayTexture(), the overlay manager's cISC4CachedStringTexture.
    class StringTexture final : public ITextTexture {
    public:
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return ++refs_; }
        std::uint32_t Release() override;
        void GetSize(std::int32_t* wh) override { wh[0] = width_; wh[1] = height_; }
        std::uint32_t TextureId();                                             // the manager's GetBoundIdForTexture
        IOverlayStringTexture* AsOverlayTexture() { return &overlay_; }
        const cRZString& Text() const { return text_; }
        std::uint32_t FontId() const { return fontId_; }

    private:
        friend class cSC4GlyphTextureManager;
        struct Overlay final : IOverlayStringTexture {
            StringTexture* owner = nullptr;
            void AddRef() override { owner->AddRef(); }
            void Release() override { owner->Release(); }
            void GetSize(std::int32_t& w, std::int32_t& h) const override { w = owner->width_; h = owner->height_; }
            int GlTextureId() const override { return static_cast<int>(owner->TextureId()); }
        };
        StringTexture() { overlay_.owner = this; }
        ~StringTexture();
        std::uint16_t width_ = 0, height_ = 0;             // +4 / +6
        std::uint32_t refs_ = 0;                           // +8
        cSC4GlyphTextureManager* manager_ = nullptr;       // +0xc (counted)
        cRZString text_;                                   // +0x10
        std::uint32_t fontId_ = 0;                         // +0x1c the font (the original holds the font object)
        Overlay overlay_;
    };

    explicit cSC4GlyphTextureManager(IGlyphTextureHost* host);
    virtual ~cSC4GlyphTextureManager();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;                            // 0x004c4ba0: restore

    bool Init();                                                               // 0x004c459c
    bool Shutdown();                                                           // 0x004c4936
    bool OnTick(std::uint32_t timeMs);                                         // 0x004c4792 the sweep

    bool GetGlyphTexture(const cRZString& glyph, std::uint32_t fontId, GlyphInfo** out);   // slot 3, 0x004c4d96
    void StringToGlyphTextureArray(std::uint32_t fontId, const cIGZString& text,
                                   std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>>& out);   // slot 4, 0x004c4a60
    bool GetCachedStringTexture(std::uint32_t fontId, const cIGZString& text, StringTexture** out);  // slot 5, 0x004c5134

    // tests
    std::size_t GlyphCount() const { return glyphs_.size(); }
    std::size_t CacheEntries() const { return entries_; }
    std::int32_t CachedBytes() const { return cachedBytes_; }
    std::int32_t Budget() const { return budget_; }
    void SetBudget(std::int32_t bytes) { budget_ = bytes; }
    std::uint32_t CacheMisses() const { return misses_; }

private:
    struct Key {
        std::string glyph;
        std::uint32_t font;
        bool operator<(const Key& other) const { return glyph != other.glyph ? glyph < other.glyph : font < other.font; }
    };
    struct CacheEntry {
        GlyphTextureBinding* binding;
        StringTexture* texture;   // one reference (the cache's)
        std::int32_t bytes;
        bool used;
    };
    std::uint32_t GetBoundIdForTexture(StringTexture* texture);                // 0x004c46b8
    void SignalUnused(GlyphInfo* glyph);                                       // 0x004c462c (RemoveGlyph)
    void DropCache();

    IGlyphTextureHost* host_;
    std::uint32_t refs_ = 0;
    bool initialized_ = false;
    std::map<Key, GlyphInfo*> glyphs_;            // the map holds one reference per glyph
    std::list<CacheEntry> cache_;                 // oldest first
    std::int32_t budget_ = 1048576;
    std::int32_t cachedBytes_ = 0;
    std::uint32_t lastSweep_ = 0;
    std::uint32_t entries_ = 0;
    std::uint32_t misses_ = 0;                    // gCacheMisses
};

}  // namespace sc4
