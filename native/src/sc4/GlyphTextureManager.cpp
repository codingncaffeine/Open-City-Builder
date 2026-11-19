// cSC4GlyphTextureManager (Mac i386 0x004c4320..0x004c5134; spec OpenSC4 glyph-textures.md). See the header.
#include "sc4/GlyphTextureManager.h"

#include <string>

#include "rz/CharLength.h"
#include "rz/cRZMessage2Standard.h"

namespace sc4 {

void GlyphTextureBinding::Release() {
    if (host_ && texture_) host_->DeleteTexture(texture_);
    delete this;
}

// ---- the glyph texture (cSC4GlyphTextureInfo) ---------------------------------------------------------------------------------

cSC4GlyphTextureManager::GlyphInfo::~GlyphInfo() {   // 0x004c4ff8: the manager, then the binding
    SetManager(nullptr);
    if (binding_) {
        GlyphTextureBinding* binding = binding_;
        binding_ = nullptr;
        binding->Release();
    }
}

// 0x004c446e: the new manager is held before the old one is let go; the same pointer does nothing.
void cSC4GlyphTextureManager::GlyphInfo::SetManager(cSC4GlyphTextureManager* manager) {
    if (manager == manager_) return;
    if (manager) manager->AddRef();
    cSC4GlyphTextureManager* previous = manager_;
    manager_ = manager;
    if (previous) previous->Release();
}

// 0x004c4678: at a count of 1 the only holder left is the manager's map: the manager drops the glyph (and releases it, which
// deletes it). Without a manager the glyph is deleted at 1 as well.
std::uint32_t cSC4GlyphTextureManager::GlyphInfo::Release() {
    const std::uint32_t count = --refs_;
    if (count == 1) {
        if (manager_) {
            manager_->SignalUnused(this);   // erases the map's node, then Release() -> 0 -> delete
            return 0;
        }
        delete this;
        return 0;
    }
    if (count == 0) {
        delete this;
        return 0;
    }
    return count;
}

// ---- the string texture (cSC4CachedStringTexture) -----------------------------------------------------------------------------

cSC4GlyphTextureManager::StringTexture::~StringTexture() {
    cSC4GlyphTextureManager* manager = manager_;
    manager_ = nullptr;
    if (manager) manager->Release();
}

// At 1 with a manager the cache's reference keeps the texture: Release answers 0 and nothing is deleted.
std::uint32_t cSC4GlyphTextureManager::StringTexture::Release() {
    const std::uint32_t count = --refs_;
    if (count == 1 && manager_) return 0;
    if (count <= 1) {
        delete this;
        return 0;
    }
    return count;
}

std::uint32_t cSC4GlyphTextureManager::StringTexture::TextureId() {
    return manager_ ? manager_->GetBoundIdForTexture(this) : 0;
}

// ---- the manager --------------------------------------------------------------------------------------------------------------

cSC4GlyphTextureManager::cSC4GlyphTextureManager(IGlyphTextureHost* host) : host_(host) {}

// The destructor neither runs Shutdown nor releases what the map and the cache hold (glyph-textures.md 1.1.1).
cSC4GlyphTextureManager::~cSC4GlyphTextureManager() = default;

bool cSC4GlyphTextureManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZIID_cISC4GlyphTextureManager) return false;   // not even IID 1 (quirk)
    *out = static_cast<cIGZUnknown*>(static_cast<cIGZMessageTarget2*>(this));
    AddRef();
    return true;
}

std::uint32_t cSC4GlyphTextureManager::Release() {
    const std::uint32_t count = --refs_;
    if (count == 0) delete this;
    return count;
}

// 0x004c459c: the cache budget from the render properties; the restore message.
bool cSC4GlyphTextureManager::Init() {
    if (initialized_) return true;
    initialized_ = true;
    budget_ = static_cast<std::int32_t>((host_ ? host_->StringTextureCacheKB() : kStringTextureCacheDefaultKB) * 1024u);
    if (cIGZMessageServer2* server = host_ ? host_->Messages() : nullptr)
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageRestoreTextures);
    return true;
}

// 0x004c4936: every glyph loses its manager link and the map's reference; the string cache stays.
bool cSC4GlyphTextureManager::Shutdown() {
    if (!initialized_) return true;
    std::map<Key, GlyphInfo*> glyphs;
    glyphs.swap(glyphs_);
    for (auto& [key, glyph] : glyphs) {
        glyph->SetManager(nullptr);
        glyph->Release();
    }
    if (cIGZMessageServer2* server = host_ ? host_->Messages() : nullptr)
        server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageRestoreTextures);
    initialized_ = false;
    return true;
}

// 0x004c4792: every 50 ms, while the cache is over its budget, the oldest entries not used since the last sweep go; the used
// ones lose their flag (a second chance). A large excess is cut by a tenth of the budget per sweep.
bool cSC4GlyphTextureManager::OnTick(std::uint32_t timeMs) {
    if (timeMs < lastSweep_) lastSweep_ = timeMs;
    if (timeMs - lastSweep_ <= 50u) return true;
    lastSweep_ = timeMs;
    if (cachedBytes_ <= budget_) return true;
    const float bytes = static_cast<float>(cachedBytes_);
    float target = static_cast<float>(budget_);
    if (1.1f < bytes / target) target = bytes + target * -0.1f;
    const std::int32_t limit = static_cast<std::int32_t>(target);
    for (auto it = cache_.begin(); it != cache_.end() && limit < cachedBytes_;) {
        if (!it->used) {
            it->binding->Release();
            it->texture->Release();
            cachedBytes_ -= it->bytes;
            it = cache_.erase(it);
            --entries_;
        } else {
            it->used = false;
            ++it;
        }
    }
    return true;
}

void cSC4GlyphTextureManager::DropCache() {
    for (CacheEntry& entry : cache_) {
        entry.binding->Release();
        entry.texture->Release();
    }
    cache_.clear();
    cachedBytes_ = 0;
    entries_ = 0;
}

// 0x004c4ba0: the driver lost its textures: the string cache goes and every glyph is rendered again into its slot.
bool cSC4GlyphTextureManager::DoMessage(cIGZMessage2* message) {
    if (!message || message->GetType() != kMessageRestoreTextures) return true;
    DropCache();
    if (!host_) return true;
    for (auto& [key, glyph] : glyphs_) {
        if (GlyphTextureBinding* old = glyph->binding_) {
            glyph->binding_ = nullptr;
            old->Release();
        }
        const std::uint32_t texture = host_->CreateTextureFromText(key.font, key.glyph.c_str(), static_cast<std::uint32_t>(key.glyph.size()), nullptr, nullptr);
        if (texture) glyph->binding_ = new GlyphTextureBinding(host_, texture);
    }
    return true;
}

// 0x004c4d96: the map's glyph with one more reference, or a new one rendered from the character (the map holds one reference,
// the caller another). The UV rectangle is the rendered extents over the texture size.
bool cSC4GlyphTextureManager::GetGlyphTexture(const cRZString& glyph, std::uint32_t fontId, GlyphInfo** out) {
    const Key key{std::string(glyph.ToChar(), glyph.Strlen()), fontId};
    if (auto it = glyphs_.find(key); it != glyphs_.end()) {
        *out = it->second;
        it->second->AddRef();
        return true;
    }
    if (!host_ || !host_->HasFont(fontId)) return false;
    auto* info = new GlyphInfo();
    info->AddRef();
    int size[2] = {0, 0};
    float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const std::uint32_t texture = host_->CreateTextureFromText(fontId, key.glyph.c_str(), static_cast<std::uint32_t>(key.glyph.size()), size, rect);
    if (!texture) {
        info->Release();
        return false;
    }
    info->binding_ = new GlyphTextureBinding(host_, texture);
    info->SetManager(this);
    info->glyph_ = glyph;
    info->textureW_ = static_cast<std::uint16_t>(size[0]);
    info->textureH_ = static_cast<std::uint16_t>(size[1]);
    info->glyphW_ = static_cast<std::uint16_t>(static_cast<int>(rect[0] - rect[2]));
    info->glyphH_ = static_cast<std::uint16_t>(static_cast<int>(rect[1] - rect[3]));
    info->uv_[0] = rect[2] / static_cast<float>(size[0]);
    info->uv_[1] = rect[3] / static_cast<float>(size[1]);
    info->uv_[2] = rect[0] / static_cast<float>(size[0]);
    info->uv_[3] = rect[1] / static_cast<float>(size[1]);
    glyphs_[key] = info;   // the map's reference (the constructor's AddRef)
    *out = info;
    info->AddRef();        // the caller's
    return true;
}

// 0x004c462c / RemoveGlyph: the node holding this glyph (by identity) is erased, then the map's reference released.
void cSC4GlyphTextureManager::SignalUnused(GlyphInfo* glyph) {
    for (auto it = glyphs_.begin(); it != glyphs_.end(); ++it) {
        if (it->second != glyph) continue;
        glyphs_.erase(it);
        glyph->Release();
        return;
    }
}

// 0x004c4a60: one glyph texture per character (GetCharLength in the framework code page, UTF-8 as on the Mac); a character that fails is skipped.
void cSC4GlyphTextureManager::StringToGlyphTextureArray(std::uint32_t fontId, const cIGZString& text,
                                                        std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>>& out) {
    out.clear();
    const char* data = text.Data();
    std::int32_t remaining = static_cast<std::int32_t>(text.Strlen());
    std::int32_t offset = 0;
    while (0 < remaining) {
        int length = GetCharLength(data + offset, static_cast<std::uint32_t>(remaining), -2);
        if (length <= 0) length = 1;   // port: the original repeats forever on a zero length (malformed text)
        cRZString character;
        character.AssignFromChars(data + offset, static_cast<std::uint32_t>(length));
        GlyphInfo* glyph = nullptr;
        if (GetGlyphTexture(character, fontId, &glyph)) {
            out.push_back(cRZAutoRefCount<ISignpostGlyphTextureInfo>(glyph));   // the vector's own reference
            glyph->Release();
        }
        offset += length;
        remaining -= length;
    }
}

// 0x004c5134: a string texture sized P(width + 2) x P(height + 2) of the text in the font; nothing is rendered yet.
bool cSC4GlyphTextureManager::GetCachedStringTexture(std::uint32_t fontId, const cIGZString& text, StringTexture** out) {
    auto* texture = new StringTexture();
    texture->AddRef();
    AddRef();
    texture->manager_ = this;
    texture->text_.AssignFromChars(text.Data(), text.Strlen());
    texture->fontId_ = fontId;
    int width = 0, height = 0;
    if (host_) host_->MeasureText(fontId, text.Data(), text.Strlen(), width, height);
    auto pow2 = [](int value) {   // nS3D::CeilPow2
        int p = 1;
        while (p < value) p <<= 1;
        return p;
    };
    texture->width_ = static_cast<std::uint16_t>(pow2(width + 2));
    texture->height_ = static_cast<std::uint16_t>(pow2(height + 2));
    *out = texture;
    return true;
}

// 0x004c46b8: a cached string texture moves to the back (newest) and is marked used; a miss renders it and the cache keeps it.
std::uint32_t cSC4GlyphTextureManager::GetBoundIdForTexture(StringTexture* texture) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->texture != texture) continue;
        cache_.splice(cache_.end(), cache_, it);
        it->used = true;
        return it->binding->TextureId();
    }
    ++misses_;
    const std::int32_t bytes = static_cast<std::int32_t>(static_cast<std::uint32_t>(texture->width_) * texture->height_ * 4u);
    const std::uint32_t id = host_ ? host_->CreateTextureFromText(texture->fontId_, texture->text_.ToChar(), texture->text_.Strlen(), nullptr, nullptr) : 0;
    if (!id) return 0;
    texture->AddRef();
    cache_.push_back(CacheEntry{new GlyphTextureBinding(host_, id), texture, bytes, true});
    cachedBytes_ += bytes;
    ++entries_;
    return id;
}

}  // namespace sc4
