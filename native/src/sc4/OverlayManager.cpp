// cSTEOverlayManager — see src/sc4/OverlayManager.h for the layout, the vtable order and the seam map.
// Recovered from 0x100727300..0x10072c4d5 (SimCity 4 Deluxe Editionsub, Aspyr macOS x86_64 v1.2.1).
//
// Floating point: the original's rounding helpers add 1.5 * 2^23 (12582912.0f) to a float, truncate to int and
// subtract the constant back. That is a round-to-nearest that is exact for the coordinate ranges the overlays use;
// FloorEdge/CeilEdge below keep the same constant and the same "an exact integer stays put" fix-up the SSE blend
// performs in UpdateDecal (0x1007286ac).
#pragma STDC FP_CONTRACT OFF
#include "sc4/OverlayManager.h"

#include <bit>
#include <cmath>
#include <cstdio>

#include "s3d/S3DMath.h"

namespace sc4 {
namespace {

constexpr float kRoundMagic = 12582912.0f;      // 1.5 * 2^23
constexpr std::int32_t kRoundMagicBits = 0x4b400000;

IOverlayHost* g_host = nullptr;

// The original moves the float's *bit pattern* into an integer register (movd) and subtracts 0x4b400000, which
// works because every value the overlays produce lands in [2^23, 2^24), where one mantissa step is exactly 1.
inline std::int32_t MagicBits(float v) { return std::bit_cast<std::int32_t>(v + kRoundMagic); }
inline int RoundToInt(float v) { return MagicBits(v) - kRoundMagicBits; }

// floor(v) as the original computes it: rint(v - 0.5 + magic) - magic, then the SSE fix-up replaces the result with
// v + 1 when v is an exact integer and the rounded value equals v (the blendvps in UpdateDecal).
inline int FloorEdge(float v) {
    const int rounded = MagicBits(v - 0.5f) - kRoundMagicBits;
    const int fixup = rounded + 1;
    return (static_cast<float>(fixup) == v) ? fixup : rounded;
}

// ceil(v): rint(v + 0.5 + magic) - magic, with the same exact-integer fix-up (the original keeps v).
inline int CeilEdge(float v) {
    const int rounded = MagicBits(v + 0.5f) - kRoundMagicBits;
    const int fixup = rounded - 1;
    return (static_cast<float>(fixup) == v) ? fixup : rounded;
}

inline bool IsZeroOrNan(float v) { return v == 0.0f; }   // NaN never satisfies either comparison, as in the original

// cS3DMatrix4 helpers ----------------------------------------------------------------------------------------
// The inverse of an affine 4x4 matrix stored with its translation in the last row (the overlay convention: linear
// 0xc is x translation). The original reaches the same matrix by dividing cofactors() @0x10072c5fc by the
// homogeneous w it computes from the first row of the model-view matrix; see OverlayCofactors.
cS3DMatrix4 InverseAffine(const cS3DMatrix4& src) {
    const float a = src.m[0][0], b = src.m[0][1], c = src.m[0][2];
    const float d = src.m[1][0], e = src.m[1][1], f = src.m[1][2];
    const float g = src.m[2][0], h = src.m[2][1], i = src.m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    cS3DMatrix4 out{};
    if (det == 0.0f) return out;   // the original divides by zero (inf/NaN matrix) - kept as a zero matrix
    const float inv = 1.0f / det;
    out.m[0][0] = (e * i - f * h) * inv;
    out.m[0][1] = (c * h - b * i) * inv;
    out.m[0][2] = (b * f - c * e) * inv;
    out.m[1][0] = (f * g - d * i) * inv;
    out.m[1][1] = (a * i - c * g) * inv;
    out.m[1][2] = (c * d - a * f) * inv;
    out.m[2][0] = (d * h - e * g) * inv;
    out.m[2][1] = (b * g - a * h) * inv;
    out.m[2][2] = (a * e - b * d) * inv;
    const float tx = src.m[3][0], ty = src.m[3][1], tz = src.m[3][2];
    out.m[3][0] = -(tx * out.m[0][0] + ty * out.m[1][0] + tz * out.m[2][0]);
    out.m[3][1] = -(tx * out.m[0][1] + ty * out.m[1][1] + tz * out.m[2][1]);
    out.m[3][2] = -(tx * out.m[0][2] + ty * out.m[1][2] + tz * out.m[2][2]);
    out.m[3][3] = 1.0f;
    return out;
}

}  // namespace

IOverlayHost* OverlayHost() { return g_host; }
void SetOverlayHost(IOverlayHost* host) { g_host = host; }

// ===============================================================================================================
// cOverlay / records
// ===============================================================================================================
cOverlay::cOverlay() = default;   // 0x100727300: flags = 1, alpha = 1, white, identity matrices

cOverlay::cOverlay(const cOverlay& other) :   // 0x10072cf78
    freeLink(other.freeLink), reserved4(other.reserved4), flags(other.flags), drawMode(other.drawMode),
    frameStamp(other.frameStamp), rect(other.rect), texMatrix(other.texMatrix), shadowMatrix(other.shadowMatrix),
    alpha(other.alpha), texture(other.texture), stringTexture(other.stringTexture) {
    color[0] = other.color[0];
    color[1] = other.color[1];
    color[2] = other.color[2];
    if (texture) texture->AddRef();
    if (stringTexture) stringTexture->AddRef();
}

cOverlay& cOverlay::operator=(const cOverlay& other) {   // 0x10072ccc8
    if (this == &other) return *this;
    freeLink = other.freeLink;
    reserved4 = other.reserved4;
    flags = other.flags;
    drawMode = other.drawMode;
    frameStamp = other.frameStamp;
    rect = other.rect;
    texMatrix = other.texMatrix;
    shadowMatrix = other.shadowMatrix;
    alpha = other.alpha;
    color[0] = other.color[0];
    color[1] = other.color[1];
    color[2] = other.color[2];
    // cRZAutoRefCount assignment: add the new reference before dropping the old one.
    IOverlayTexture* oldTexture = texture;
    if (other.texture != oldTexture) {
        if (other.texture) other.texture->AddRef();
        texture = other.texture;
        if (oldTexture) oldTexture->Release();
    }
    IOverlayStringTexture* oldString = stringTexture;
    if (other.stringTexture != oldString) {
        if (other.stringTexture) other.stringTexture->AddRef();
        stringTexture = other.stringTexture;
        if (oldString) oldString->Release();
    }
    return *this;
}

cOverlay::~cOverlay() {   // the loop inside cSC4SlotArray<cOverlay>::Clear @0x100727cac
    if (stringTexture) stringTexture->Release();
    if (texture) texture->Release();
}

// ===============================================================================================================
// cSTEOverlayManager: life cycle
// ===============================================================================================================
cSTEOverlayManager::cSTEOverlayManager(IOverlayHost* host) : host_(host ? host : g_host) {
    // 0x1007273c6. The three combiners start as kSC4CtorTexCombiner {0x01030001, 0x01030001, 0} (the ctor writes the
    // three 12-byte records at +0x58 / +0x64 / +0x70); InitDrawStates patches the low u16 words.
    roughShadowCombiner_ = gd::cGDCombiner{0x01030001u, 0x01030001u, 0u};
    shadowCombiner0_ = gd::cGDCombiner{0x01030001u, 0x01030001u, 0u};
    shadowCombiner1_ = gd::cGDCombiner{0x01030001u, 0x01030001u, 0u};
}

cSTEOverlayManager::~cSTEOverlayManager() {
    // 0x10072755e. The slot arrays release their cOverlay textures in their own destructors; the three service
    // pointers are dropped here in the original's order (shadow texture, factory, view, terrain).
    if (shadowTexture_) {
        shadowTexture_->Release();
        shadowTexture_ = nullptr;
    }
    decals_.Clear();
    ringDecals_.Clear();
    overlays_.Clear();
    if (textureFactory_) {
        textureFactory_->Release();
        textureFactory_ = nullptr;
    }
}

bool cSTEOverlayManager::QueryInterface(std::uint32_t iid, void** out) {   // 0x100727d2c
    if (iid == kOverlayIID) {
        *out = this;
        AddRef();
        return true;
    }
    // cRZUnknown::QueryInterface on the cSC4ViewObject3D sub-object (@0x10003c8f8) fails for every other id.
    return false;
}

std::uint32_t cSTEOverlayManager::AddRef() { return ++refCount_; }   // 0x100727dc0

std::uint32_t cSTEOverlayManager::Release() {   // 0x100727df4, destroys at 0 (the original calls the deleting dtor)
    const std::uint32_t count = --refCount_;
    if (count == 0) delete this;
    return count;
}

void cSTEOverlayManager::Init(cSTETerrainView3D* view, bool dynamic, bool drawWaterPass) {   // 0x100727792
    IOverlayHost* host = host_ ? host_ : g_host;
    host_ = host;
    // The original keeps the view at +0xb0 with a reference; the port stores the pointer only (the view owns the
    // manager, so a reference would be a cycle: everything the manager calls goes through IOverlayHost).
    (void)view;
    dynamic_ = dynamic;
    drawWaterPass_ = drawWaterPass;
    inited_ = true;

    // +0xb8 = the texture-binding factory (framework service 0x1ac0e11a / 0xfac0e219)
    if (host) {
        IOverlayTextureFactory* factory = host->TextureFactory();
        if (factory != textureFactory_) {
            if (factory) factory->AddRef();
            if (textureFactory_) textureFactory_->Release();
            textureFactory_ = factory;
        }
    }

    // Terrain extents (+0x7c .. +0x94)
    if (host) {
        cellCountX_ = host->TerrainCellCountX();
        cellCountZ_ = host->TerrainCellCountZ();
        cellSize_ = host->TerrainCellSize();
        invCellSize_ = 1.0f / cellSize_;
        extentX_ = static_cast<float>(cellCountX_) * cellSize_;
        extentX2_ = extentX_;
        extentZ_ = static_cast<float>(cellCountZ_) * cellSize_;
    }
    visible_ = cRZRect{0, 0, cellCountX_, cellCountZ_};
    decalsDrawn_ = 0;
    shadowsDrawn_ = 0;
    vertexBufferExt_ = nullptr;

    // The shadow texture (+0x128), factory slot 0x28 with the fixed instance 0xaa592b77.
    if (textureFactory_) {
        if (shadowTexture_) {
            shadowTexture_->Release();
            shadowTexture_ = nullptr;
        }
        shadowTexture_ = textureFactory_->GetTexture(kOverlayTextureShadow);
    }
    InitDrawStates();

    if (!dynamic_) {
        // Spatial index: a shift of 0 and one bucket per cell (width = cells x, height = cells z).
        sdsShift_ = 0;
        sdsWidth_ = cellCountX_;
        sdsHeight_ = cellCountZ_;
        spatialIndex_.clear();
        spatialIndex_.resize(static_cast<std::size_t>(cellCountZ_ * cellCountX_));
        frameStamp_ = 0;
    }
    if (host) {
        if (cIGZMessageServer2* messages = host->Messages())
            messages->AddNotification(this, kOverlayMessageRenderProperties);
        quality_ = host->RenderQuality();
        secondStage_ = host->RenderSecondTextureStage();
    }
}

void cSTEOverlayManager::InitDrawStates() {   // 0x100727a22
    // Five packed render states, one per tDrawMode. The literals are the original's argument lists.
    renderStates_.assign(5, cSC4PackedRenderState{});
    renderStates_[0].PackRenderState(true, false, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);
    renderStates_[1].PackRenderState(true, false, true, false, true, 1, false, 3, 4, 1, -1, -1.0f, 1, -1);
    renderStates_[2].PackRenderState(true, false, true, false, true, 1, false, 3, 0, 2, -1, -1.0f, 1, -1);
    renderStates_[3].PackRenderState(true, false, true, false, true, 1, false, 4, 4, 5, -1, -1.0f, 1, -1);
    renderStates_[4].PackRenderState(true, true, true, true, true, 1, false, 1, 4, 5, 4, 0.5f, -1, -1);
    // The three combiners: only the low u16 of the first two words is patched.
    roughShadowCombiner_ = gd::cGDCombiner{0x01030200u, 0x01020001u, 0u};   // +0x58: u16 {0x200, 1}, byte 0x5e = 2
    shadowCombiner0_ = gd::cGDCombiner{0x01030100u, 0x01010001u, 0u};       // +0x64: u16 {0x100, 1}, byte 0x6a = 1
    shadowCombiner1_ = gd::cGDCombiner{0x01030000u, 0x01030000u, 0u};       // +0x70: u16 {0, 0}
}

void cSTEOverlayManager::Shutdown() {   // 0x100727bfe
    IOverlayHost* host = host_ ? host_ : g_host;
    if (host) {
        if (cIGZMessageServer2* messages = host->Messages())
            messages->RemoveNotification(this, kOverlayMessageRenderProperties);
    }
    if (textureFactory_) {
        textureFactory_->Release();
        textureFactory_ = nullptr;
    }
    overlays_.Clear();
    decals_.Clear();
    // Quirk kept: the original leaves the cRingDecal slots alone here (only the overlay and cDecal arrays are
    // cleared); the ring records are released by the destructor's slot-array teardown.
    inited_ = false;
}

// ===============================================================================================================
// Messages / picking
// ===============================================================================================================
bool cSTEOverlayManager::DoMessage(cIGZMessage2* message) {   // 0x100727e28
    if (message->GetType() == kOverlayMessageRenderProperties) {
        IOverlayHost* host = host_ ? host_ : g_host;
        if (host) {
            quality_ = host->RenderQuality();
            secondStage_ = host->RenderSecondTextureStage();
            spatialIndexValid_ = host->RenderSpatialIndexValid();
        }
    }
    return true;   // the original always returns 1
}

bool cSTEOverlayManager::Update(IOverlayDrawContext& dc) {   // 0x100727ed0
    DrawOverlays(dc);
    return true;
}

bool cSTEOverlayManager::Pick(const cS3DVector3&, const cS3DVector3&, IOverlayDrawContext*, float*) {   // 0x100728292
    return false;   // 8-byte stub in the binary: `xor eax, eax; ret`
}

// ===============================================================================================================
// Decals
// ===============================================================================================================
std::uint32_t cSTEOverlayManager::AddDecal(std::uint32_t instance, const cS3DVector2& position, float width,
                                           float turns) {   // 0x10072832c
    IOverlayTexture* texture = textureFactory_ ? textureFactory_->GetTexture(instance) : nullptr;
    const std::uint32_t id = AddDecal(nullptr, texture, position, width, turns);
    if (texture) texture->Release();
    return id;
}

std::uint32_t cSTEOverlayManager::AddDecal(const cGZPersistResourceKey& key, const cS3DVector2& position,
                                           float width, float turns) {   // 0x1007282a2
    IOverlayTexture* texture = textureFactory_ ? textureFactory_->GetTexture(key) : nullptr;
    const std::uint32_t id = AddDecal(nullptr, texture, position, width, turns);
    if (texture) texture->Release();
    return id;
}

std::uint32_t cSTEOverlayManager::AddDecal(const IOverlayTexture* texture, const cS3DVector2& position, float width,
                                           float turns) {   // 0x1007283b6
    return AddDecal(nullptr, texture, position, width, turns);
}

std::uint32_t cSTEOverlayManager::AddDecal(const IOverlayStringTexture* text, const cS3DVector2& position,
                                           float width, float turns) {   // 0x1007285a6
    return AddDecal(text, nullptr, position, width, turns);
}

std::uint32_t cSTEOverlayManager::AddDecal(const IOverlayStringTexture* text, const IOverlayTexture* texture,
                                           const cS3DVector2& position, float width, float turns) {   // 0x1007283ce
    if (!text && !texture) return 0xffffffffu;

    cDecal decal;
    decal.x = position.x;
    decal.z = position.y;
    decal.width = width;
    decal.turns = turns;
    decal.aspectAdjust = 1.0f;
    decal.uRepeat = 1.0f;
    decal.vRepeat = 1.0f;
    decal.uvOffset = 0.0f;
    decal.userValue = 0.0f;
    if (texture) {
        if (textureFactory_) {
            std::int32_t texW = 0, texH = 0;
            textureFactory_->GetTextureSize(texture, texW, texH);
            decal.aspect = static_cast<float>(texH) / static_cast<float>(texW);
        }
    } else {
        // cISC4CachedStringTexture::GetSize @ vslot 0x18 fills {width, height}.
        std::int32_t size[2] = {0, 0};
        text->GetSize(size[0], size[1]);
        decal.aspect = static_cast<float>(size[1]) / static_cast<float>(size[0]);
    }

    cOverlay overlay;
    overlay.texture = const_cast<IOverlayTexture*>(texture);   // cRZAutoRefCount::operator= (AddRef)
    if (overlay.texture) overlay.texture->AddRef();
    overlay.stringTexture = const_cast<IOverlayStringTexture*>(text);
    if (overlay.stringTexture) overlay.stringTexture->AddRef();
    overlay.flags |= kOverlayFlagDecal | kOverlayFlagSmoothDecal;   // 0x90

    const std::uint32_t overlaySlot = overlays_.AddSlot(overlay);
    decal.overlay = static_cast<std::int32_t>(overlaySlot);
    UpdateDecal(decal, false);
    AddOverlay(overlaySlot);
    const std::uint32_t decalSlot = decals_.AddSlot(decal);
    return decalSlot | 0x80000000u;
}

std::uint32_t cSTEOverlayManager::AddRingDecal(std::uint32_t instance, const cS3DVector2& position, float width,
                                               float turns) {   // 0x100728f32
    IOverlayTexture* texture = textureFactory_ ? textureFactory_->GetTexture(instance) : nullptr;
    if (!texture) return 0xffffffffu;

    cDecal decal;
    decal.x = position.x;
    decal.z = position.y;
    decal.width = width;
    decal.turns = turns;
    decal.aspectAdjust = 1.0f;
    decal.uRepeat = 1.0f;
    decal.vRepeat = 1.0f;
    decal.uvOffset = 0.0f;
    decal.userValue = 0.0f;
    if (textureFactory_) {
        std::int32_t texW = 0, texH = 0;
        textureFactory_->GetTextureSize(texture, texW, texH);
        decal.aspect = static_cast<float>(texH) / static_cast<float>(texW);
    }

    cRingDecal ring;
    for (int i = 0; i < 8; ++i) {
        cOverlay overlay;
        overlay.texture = texture;
        texture->AddRef();
        overlay.flags |= kOverlayFlagDecal;   // 0x10
        ring.overlay[i] = overlays_.AddSlot(overlay);
    }
    const std::uint32_t ringSlot = ringDecals_.AddSlot(ring);
    decal.overlay = static_cast<std::int32_t>(ringSlot) | static_cast<std::int32_t>(0x80000000u);
    UpdateDecal(decal, false);
    for (int i = 0; i < 8; ++i) AddOverlay(ring.overlay[i]);
    const std::uint32_t decalSlot = decals_.AddSlot(decal);
    texture->Release();
    return decalSlot | 0x80000000u;
}

std::uint32_t cSTEOverlayManager::AddShadowDecal(std::uint32_t instance, const cS3DVector2& position, float width,
                                                 float turns, bool mirror) {   // 0x10072924e
    if (quality_ == 0) return 0xffffffffu;
    cGZPersistResourceKey key;
    key.type = kOverlayShadowKeyType;
    key.group = textureFactory_ ? textureFactory_->ShadowTextureGroup() : 0;
    key.instance = instance;
    return AddShadowDecal(key, position, width, turns, mirror);
}

std::uint32_t cSTEOverlayManager::AddShadowDecal(const cGZPersistResourceKey& key, const cS3DVector2& position,
                                                 float width, float turns, bool mirror) {   // 0x1007292c6
    if (quality_ == 0) return 0xffffffffu;
    IOverlayTexture* texture = textureFactory_ ? textureFactory_->GetTexture(key) : nullptr;
    if (!texture) return 0xffffffffu;

    cDecal decal;
    decal.x = position.x;
    decal.z = position.y;
    decal.width = width;
    decal.turns = turns;
    decal.aspectAdjust = 1.0f;
    decal.uRepeat = 1.0f;
    decal.vRepeat = 1.0f;
    decal.uvOffset = 0.0f;
    decal.userValue = 0.0f;
    {
        std::int32_t texW = 0, texH = 0;
        textureFactory_->GetTextureSize(texture, texW, texH);
        decal.aspect = static_cast<float>(texH) / static_cast<float>(texW);
    }

    cOverlay overlay;
    overlay.texture = texture;
    texture->AddRef();
    overlay.flags |= kOverlayFlagShadow;   // 8
    const std::uint32_t overlaySlot = overlays_.AddSlot(overlay);
    decal.overlay = static_cast<std::int32_t>(overlaySlot);
    UpdateDecal(decal, false);

    // Mirror in V: the original reflects the second and fourth columns of the texture transform about the third /
    // first one: m[r][1] = m[r][3] - m[r][1] for every row (offsets 0x28 <- 0x30, 0x38 <- 0x40, ...).
    cOverlay& stored = overlays_[overlaySlot];
    if (mirror) {
        for (int r = 0; r < 4; ++r) stored.texMatrix.m[r][1] = stored.texMatrix.m[r][3] - stored.texMatrix.m[r][1];
    }

    // The shadow projection matrix (a fixed swizzle of the decal transform, built from the literals at
    // 0x1007298xx): row0 {0.5, 0.5, 0.5, 0}, row1 {0, 0.5, 0, 0}, row2 {0, 0, 1, 0}, row3 {0.25, 0.25, 0, 1}.
    cS3DMatrix4 shadowMatrix{};
    shadowMatrix.m[0][0] = 0.5f; shadowMatrix.m[0][1] = 0.5f; shadowMatrix.m[0][2] = 0.5f;
    shadowMatrix.m[1][1] = 0.5f;
    shadowMatrix.m[2][2] = 1.0f;
    shadowMatrix.m[3][0] = 0.25f; shadowMatrix.m[3][1] = 0.25f;
    shadowMatrix.m[3][3] = 1.0f;
    stored.shadowMatrix = shadowMatrix * stored.texMatrix;

    AddOverlay(overlaySlot);
    const std::uint32_t decalSlot = decals_.AddSlot(decal);
    texture->Release();
    return decalSlot | 0x80000000u;
}

std::uint32_t cSTEOverlayManager::AddHeightMap(std::uint32_t instance) {   // 0x1007295c8
    // An identity matrix (cS3DMatrix4(S3DIdentityTag)) with column 0 cleared (SetColumn(0, 0)) and m[5] = 2 / cells x: u is 0,
    // v follows the altitude (i386 0x004d7d40).
    cS3DMatrix4 matrix{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) matrix.m[r][c] = (r == c && c != 0) ? 1.0f : 0.0f;
    matrix.m[1][1] = 2.0f / static_cast<float>(cellCountX_);
    const cRZRect rect{0, 0, cellCountX_, cellCountZ_};
    return AddOverlay(instance, rect, matrix, 0x12u);   // bitset<8>{0,1,0,0,1,0,0,0}
}

std::uint32_t cSTEOverlayManager::AddShadow(const IOverlayTexture* texture, int pointCount,
                                            const cS3DVector3* points, const cS3DVector2* texCoords,
                                            const cS3DBoundingBox& box, const cS3DTransform& transform) {   // 0x100729916
    if (quality_ == 0) return 0xffffffffu;
    IOverlayHost* host = host_ ? host_ : g_host;
    IOverlayShadowService* shadow = host ? host->ShadowService() : nullptr;
    if (!shadow || !texture) return 0xffffffffu;

    cS3DMatrix4 project{};
    if (!shadow->ProjectShadow(pointCount, points, texCoords, transform, project)) return 0xffffffffu;

    cRZRect rect{};
    if (quality_ < 5) {
        cS3DVector3 lightDir;
        shadow->GetShadowDirection(lightDir);
        CalcShadowTerrainRect(lightDir, pointCount, points, box, &rect);
    } else if (!shadow->CalcShadowRect(pointCount, points, box, rect)) {
        return 0xffffffffu;
    }

    const std::uint32_t slot = AddOverlay(texture, rect, project, 8u);   // bitset<8>{0,0,0,1,0,0,0,0}
    if (slot == 0xffffffffu) return 0xffffffffu;

    // The UV bounds of the projected points, then a transform mapping them into the square [0, 1] * 2.
    float minU = texCoords[0].x, maxU = minU, minV = texCoords[0].y, maxV = minV;
    for (int i = 1; i < pointCount; ++i) {
        const float u = texCoords[i].x, v = texCoords[i].y;
        if (u < minU) minU = u;
        if (maxU < u) maxU = u;
        if (v < minV) minV = v;
        if (maxV < v) maxV = v;
    }
    const float du = (maxU - minU) + (maxU - minU);
    const float dv = (maxV - minV) + (maxV - minV);
    cS3DMatrix4 uv{};
    uv.m[0][0] = 1.0f / du;
    uv.m[2][2] = 1.0f;
    uv.m[1][1] = 1.0f / dv;
    uv.m[3][0] = 0.25f - minU / du;
    uv.m[3][1] = 0.25f - minV / dv;
    uv.m[3][3] = 1.0f;
    overlays_[slot].shadowMatrix = project * uv;
    return slot;
}

void cSTEOverlayManager::MoveDecal(std::uint32_t id, const cS3DVector2& position) {   // 0x100729d16
    if (static_cast<std::int32_t>(id) >= -1) return;
    cDecal& decal = decals_[id & 0x7fffffffu];
    const float dx = position.x - decal.x;
    const float dz = position.y - decal.z;
    // 0.0001 <= |dx| || 0.0001 <= |dz|
    if (0.0001f <= std::fabs(dx) || 0.0001f <= std::fabs(dz)) {
        decal.x = position.x;
        decal.z = position.y;
        UpdateDecal(decal, !dynamic_);
    }
}

void cSTEOverlayManager::UpdateDecalInfo(std::uint32_t id, const cDecalInfo& info) {   // 0x100729dca
    if (static_cast<std::int32_t>(id) >= -1) return;
    cDecal& decal = decals_[id & 0x7fffffffu];
    decal.x = info.x;
    decal.z = info.z;
    decal.userValue = info.userValue;
    decal.uvOffset = info.uvOffset;
    decal.vRepeat = info.vRepeat;
    decal.uRepeat = info.uRepeat;
    decal.aspectAdjust = info.aspectAdjust;
    decal.turns = info.turns;
    decal.width = info.width;
    UpdateDecal(decal, !dynamic_);
}

void cSTEOverlayManager::DecalInfo(std::uint32_t id, cDecalInfo* out) const {   // 0x100729e30
    if (static_cast<std::int32_t>(id) >= -1 || !out) return;
    const cDecal& decal = decals_[id & 0x7fffffffu];
    out->x = decal.x;
    out->z = decal.z;
    out->userValue = decal.userValue;
    out->uvOffset = decal.uvOffset;
    out->vRepeat = decal.vRepeat;
    out->uRepeat = decal.uRepeat;
    out->aspectAdjust = decal.aspectAdjust;
    out->turns = decal.turns;
    out->width = decal.width;
}

// ===============================================================================================================
// The texture transform of one decal (0x1007286ac)
// ===============================================================================================================
void cSTEOverlayManager::UpdateDecal(const cDecal& decal, bool staticOnly) {
    // The 2-D quad lives in the XZ plane; the matrix is stored with its translation in the last row, so linear 0 is
    // the U scale, linear 9 the V scale, linear 0xc / 0x10 the U / V translation (see the cS3DMatrix4 convention).
    const float width = decal.width;
    const float invWidth = 1.0f / width;
    const float vScale = decal.aspect * decal.aspectAdjust;   // +0x28 * +0x14
    const float halfWidth = width * 0.5f;
    const float halfHeight = width * vScale * 0.5f;

    cS3DMatrix4 m{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.m[r][c] = 0.0f;
    m.m[0][0] = invWidth * decal.uRepeat;
    m.m[2][1] = invWidth / vScale * decal.vRepeat;
    m.m[2][2] = 1.0f;
    m.m[3][0] = (halfWidth - decal.x) * invWidth * decal.uRepeat + decal.uvOffset;
    m.m[3][1] = (halfHeight - decal.z) * (invWidth / vScale) * decal.vRepeat + decal.uvOffset;
    m.m[3][3] = 1.0f;

    float halfW = halfWidth;
    float halfH = halfHeight;
    if (decal.turns != 0.0f) {
        // Rotation about Y by turns * 2pi, in the same transposed convention: cos/sin on the m[0]/m[2] columns and
        // the translation that keeps the decal centre at (x, z).
        const float angle = decal.turns * 6.2831855f;
        const float s = std::sin(angle);
        const float c = std::cos(angle);
        cS3DMatrix4 rot{};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) rot.m[r][k] = 0.0f;
        rot.m[0][0] = c;
        rot.m[0][2] = s;
        rot.m[1][1] = 1.0f;
        rot.m[2][0] = -s;
        rot.m[2][2] = c;
        rot.m[3][0] = decal.z * s + decal.x * (1.0f - c);
        rot.m[3][2] = decal.z * (1.0f - c) - decal.x * s;
        rot.m[3][3] = 1.0f;
        // Mac i386 0x004d97a0: operator*(result, rot, m): the point turns about the decal center first, then maps to the
        // texture (row vectors: p * rot * m). The other order put the turn's world-space translation into the texture
        // coordinates and every turned decal sampled far outside its texture.
        m = rot * m;
        if (decal.uRepeat == 1.0f && decal.vRepeat == 1.0f) {
            // Axis-aligned bounds of the rotated quad: |cos| and |cos| + |sin| times the two half extents.
            const float absCos = std::fabs(c);
            const float absCosSin = std::fabs(c) + std::fabs(s);
            halfW = halfWidth * (absCosSin * vScale + absCos);
            halfH = halfHeight * (absCosSin / vScale + absCos);
        }
    }

    // Outward-rounded integer rect in cell space.
    const float scale = invCellSize_;
    const float left = scale * (decal.x - halfW);
    const float top = scale * (decal.z - halfH);
    const float right = scale * (decal.x + halfW);
    const float bottom = scale * (decal.z + halfH);
    const cRZRect decalRect{FloorEdge(left), FloorEdge(top), CeilEdge(right), CeilEdge(bottom)};

    // Bit 1: the transform is rotated or non-uniformly scaled.
    const bool transformed = !(decal.uRepeat == 1.0f && decal.vRepeat == 1.0f);

    const std::uint32_t id = static_cast<std::uint32_t>(decal.overlay);
    if (static_cast<std::int32_t>(id) < 0) {
        cRingDecal& ring = ringDecals_[id & 0x7fffffffu];
        for (int i = 0; i < 8; ++i) {
            cOverlay& overlay = overlays_[ring.overlay[i]];
            overlay.texMatrix = m;
            if (transformed) overlay.flags |= kOverlayFlagTransformed;
            else overlay.flags &= ~static_cast<std::uint64_t>(kOverlayFlagTransformed);
        }
        const std::array<float, 4> ringRect{static_cast<float>(decalRect.left), static_cast<float>(decalRect.top),
                                            static_cast<float>(decalRect.right), static_cast<float>(decalRect.bottom)};
        UpdateRingDecalOverlays(ring, ringRect, staticOnly);
        return;
    }

    cOverlay& overlay = overlays_[id];
    overlay.texMatrix = m;
    if (transformed) overlay.flags |= kOverlayFlagTransformed;
    else overlay.flags &= ~static_cast<std::uint64_t>(kOverlayFlagTransformed);

    if (staticOnly) {
        UpdateStaticOverlayRect(id, decalRect);
        return;
    }
    // Clip to the visible terrain rect; a decal that misses it gets the empty rect.
    if (decalRect.left <= visible_.right && visible_.left <= decalRect.right && decalRect.top <= visible_.bottom &&
        visible_.top <= decalRect.bottom) {
        overlay.rect.left = decalRect.left < visible_.left ? visible_.left : decalRect.left;
        overlay.rect.top = decalRect.top < visible_.top ? visible_.top : decalRect.top;
        overlay.rect.right = visible_.right < decalRect.right ? visible_.right : decalRect.right;
        overlay.rect.bottom = visible_.bottom < decalRect.bottom ? visible_.bottom : decalRect.bottom;
    } else {
        overlay.rect = cRZRect{0, 0, 0, 0};
    }
}

// ===============================================================================================================
// Spatial index (SDS = the culling acceleration structure of the original)
// ===============================================================================================================
void cSTEOverlayManager::AddOverlay(std::uint32_t slot) {   // 0x100728dda
    if (slot == 0xffffffffu || dynamic_) return;
    SDSAdd(slot, overlays_[slot].rect);
    RedisplayStaticOverlay(slot);
}

void cSTEOverlayManager::SDSAdd(std::uint32_t slot, const cRZRect& rect) {   // 0x10072a852
    const int shift = sdsShift_;
    const int mask = (1 << shift) - 1;
    const int left = rect.left >> shift;
    const int top = rect.top >> shift;
    const int right = (rect.right + mask) >> shift;
    const int bottom = (rect.bottom + mask) >> shift;
    for (int z = top; z < bottom; ++z)
        for (int x = left; x < right; ++x) {
            const std::size_t index = static_cast<std::size_t>(sdsWidth_ * z + x);
            if (index < spatialIndex_.size()) spatialIndex_[index].push_front(slot);
        }
}

void cSTEOverlayManager::SDSRemove(std::uint32_t slot, const cRZRect& rect) {   // 0x10072a034
    const int shift = sdsShift_;
    const int mask = (1 << shift) - 1;
    const int left = rect.left >> shift;
    const int top = rect.top >> shift;
    const int right = (rect.right + mask) >> shift;
    const int bottom = (rect.bottom + mask) >> shift;
    for (int z = top; z < bottom; ++z)
        for (int x = left; x < right; ++x) {
            const std::size_t index = static_cast<std::size_t>(sdsWidth_ * z + x);
            if (index < spatialIndex_.size()) spatialIndex_[index].remove(slot);
        }
}

void cSTEOverlayManager::SDSFindOverlaysToDraw(std::vector<std::uint32_t>& out) {   // 0x10072a90e
    IOverlayHost* host = host_ ? host_ : g_host;
    out.clear();
    if (!host) return;
    int rowBegin = 0, rowEnd = 0;
    const std::int32_t* colStart = nullptr;
    const std::int32_t* colEnd = nullptr;
    host->GetTerrainCullingInfo(rowBegin, rowEnd, colStart, colEnd);
    if (!colStart || !colEnd) return;
    const unsigned shift = static_cast<unsigned>(sdsShift_);
    const int firstRow = rowBegin >> shift;
    const int lastRow = (rowEnd - 2 + (1 << shift)) >> shift;
    ++frameStamp_;   // u16, wraps
    for (int z = firstRow; z < lastRow; ++z) {
        const int firstCol = colStart[z] >> shift;
        const int lastCol = (colEnd[z] + ((1 << shift) - 2)) >> shift;
        for (int x = firstCol; x < lastCol; ++x) {
            const std::size_t index = static_cast<std::size_t>(sdsWidth_ * z + x);
            if (index >= spatialIndex_.size()) continue;
            for (std::uint32_t slot : spatialIndex_[index]) {
                cOverlay& overlay = overlays_[slot];
                if (overlay.frameStamp != frameStamp_) {
                    overlay.frameStamp = frameStamp_;
                    out.push_back(slot);
                }
            }
        }
    }
}

void cSTEOverlayManager::RedisplayStaticOverlay(std::uint32_t slot) {   // 0x10072a0cc
    IOverlayHost* host = host_ ? host_ : g_host;
    if (!host || slot >= overlays_.size()) return;
    const cRZRect& rect = overlays_[slot].rect;
    if (rect.left < rect.right && rect.top < rect.bottom) host->RedisplayTerrain(drawWaterPass_ ? 1 : 0, rect);
}

// ===============================================================================================================
// Overlay setters / getters
// ===============================================================================================================
std::uint32_t cSTEOverlayManager::ResolveOverlay(std::uint32_t id) const {
    // A decal id resolves to its overlay slot; a ring decal id to the first of its eight overlays. The original
    // indexes the slot arrays without a bounds check; the port returns 0xffffffff ("no overlay") for an id it cannot
    // resolve, which is what every caller already tests for.
    if (static_cast<std::int32_t>(id) < 0) {
        const std::uint32_t decalId = id & 0x7fffffffu;
        if (decalId >= decals_.size()) return 0xffffffffu;
        id = static_cast<std::uint32_t>(decals_[decalId].overlay);
        if (static_cast<std::int32_t>(id) < 0) {
            const std::uint32_t ringId = id & 0x7fffffffu;
            if (ringId >= ringDecals_.size()) return 0xffffffffu;
            id = ringDecals_[ringId].overlay[0];
        }
    }
    return id < overlays_.size() ? id : 0xffffffffu;
}

void cSTEOverlayManager::RemoveOverlay(std::uint32_t id) {   // 0x100729e80
    if (id == 0xffffffffu) return;
    if (static_cast<std::int32_t>(id) < 0) {
        const std::uint32_t decalId = id & 0x7fffffffu;
        cDecal& decal = decals_[decalId];
        id = static_cast<std::uint32_t>(decal.overlay);
        if (decal.freeLink == -1) {
            decal.freeLink = 0;
            if (decals_.freeListTail_ == 0xffffffffu) decals_.freeListHead_ = decalId;
            else decals_[decals_.freeListTail_].freeLink = static_cast<std::int32_t>(decalId);
            decals_.freeListTail_ = decalId;
        }
        if (static_cast<std::int32_t>(id) < 0) {
            const std::uint32_t ringId = id & 0x7fffffffu;
            cRingDecal& ring = ringDecals_[ringId];
            for (int i = 0; i < 8; ++i) RemoveOverlay(ring.overlay[i]);
            if (ring.freeLink != -1) return;
            ring.freeLink = 0;
            if (ringDecals_.freeListTail_ == 0xffffffffu) ringDecals_.freeListHead_ = ringId;
            else ringDecals_[ringDecals_.freeListTail_].freeLink = static_cast<std::int32_t>(ringId);
            ringDecals_.freeListTail_ = ringId;
            return;
        }
    }
    if (!dynamic_) {
        SDSRemove(id, overlays_[id].rect);
        RedisplayStaticOverlay(id);
    }
    cOverlay& overlay = overlays_[id];
    if (overlay.freeLink == -1) {
        overlay.freeLink = 0;
        if (overlays_.freeListTail_ == 0xffffffffu) overlays_.freeListHead_ = id;
        else overlays_[overlays_.freeListTail_].freeLink = static_cast<std::int32_t>(id);
        overlays_.freeListTail_ = id;
    }
}

void cSTEOverlayManager::SetOverlayEnabled(std::uint32_t id, bool enabled) {   // 0x10072a136
    if (id == 0xffffffffu) return;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return;
    if (static_cast<std::int32_t>(id) < 0 && static_cast<std::int32_t>(decals_[id & 0x7fffffffu].overlay) < 0) {
        // A ring decal forwards to all eight overlays.
        const std::uint32_t ringId = static_cast<std::uint32_t>(decals_[id & 0x7fffffffu].overlay) & 0x7fffffffu;
        for (int i = 0; i < 8; ++i) SetOverlayEnabled(ringDecals_[ringId].overlay[i], enabled);
        return;
    }
    cOverlay& overlay = overlays_[resolved];
    if (enabled) overlay.flags |= kOverlayFlagEnabled;
    else overlay.flags &= ~static_cast<std::uint64_t>(kOverlayFlagEnabled);
    if (!dynamic_) RedisplayStaticOverlay(resolved);
}

void cSTEOverlayManager::SetOverlayAlpha(std::uint32_t id, float alpha) {   // 0x10072a1f6
    if (id == 0xffffffffu) return;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return;
    if (static_cast<std::int32_t>(id) < 0 && static_cast<std::int32_t>(decals_[id & 0x7fffffffu].overlay) < 0) {
        const std::uint32_t ringId = static_cast<std::uint32_t>(decals_[id & 0x7fffffffu].overlay) & 0x7fffffffu;
        for (int i = 0; i < 8; ++i) SetOverlayAlpha(ringDecals_[ringId].overlay[i], alpha);
        return;
    }
    float& stored = overlays_[resolved].alpha;
    if (!dynamic_) {
        // Only redisplays when the change is visible: 0 or 1 always does, otherwise a jump above 0.05.
        if ((stored != alpha || IsZeroOrNan(stored) || IsZeroOrNan(alpha)) &&
            (alpha == 0.0f || alpha == 1.0f || 0.05f < std::fabs(alpha - stored))) {
            stored = alpha;
            RedisplayStaticOverlay(resolved);
        }
    } else {
        stored = alpha;
    }
}

void cSTEOverlayManager::SetOverlayColor(std::uint32_t id, const cS3DVector3& rgb) {   // 0x10072a2f2
    if (id == 0xffffffffu) return;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return;
    if (static_cast<std::int32_t>(id) < 0 && static_cast<std::int32_t>(decals_[id & 0x7fffffffu].overlay) < 0) {
        const std::uint32_t ringId = static_cast<std::uint32_t>(decals_[id & 0x7fffffffu].overlay) & 0x7fffffffu;
        for (int i = 0; i < 8; ++i) SetOverlayColor(ringDecals_[ringId].overlay[i], rgb);
        return;
    }
    cOverlay& overlay = overlays_[resolved];
    overlay.color[0] = rgb.x;
    overlay.color[1] = rgb.y;
    overlay.color[2] = rgb.z;
    if (!dynamic_) RedisplayStaticOverlay(resolved);
}

void cSTEOverlayManager::SetOverlayDrawMode(std::uint32_t id, tOverlayDrawMode mode) {   // 0x10072a3b4
    if (id == 0xffffffffu) return;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return;
    if (static_cast<std::int32_t>(id) < 0 && static_cast<std::int32_t>(decals_[id & 0x7fffffffu].overlay) < 0) {
        const std::uint32_t ringId = static_cast<std::uint32_t>(decals_[id & 0x7fffffffu].overlay) & 0x7fffffffu;
        for (int i = 0; i < 8; ++i) SetOverlayDrawMode(ringDecals_[ringId].overlay[i], mode);
        return;
    }
    overlays_[resolved].drawMode = static_cast<std::uint8_t>(mode);
    if (!dynamic_) RedisplayStaticOverlay(resolved);
}

void cSTEOverlayManager::SetOverlayFlags(std::uint32_t id, std::uint32_t flags) {   // 0x10072a45c
    if (id == 0xffffffffu) return;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return;
    if (static_cast<std::int32_t>(id) < 0 && static_cast<std::int32_t>(decals_[id & 0x7fffffffu].overlay) < 0) {
        const std::uint32_t ringId = static_cast<std::uint32_t>(decals_[id & 0x7fffffffu].overlay) & 0x7fffffffu;
        for (int i = 0; i < 8; ++i) SetOverlayFlags(ringDecals_[ringId].overlay[i], flags);
        return;
    }
    if (flags & 1u) overlays_[resolved].flags |= kOverlayFlagSelfLit;   // only bit 0 is honoured
}

void cSTEOverlayManager::SetHeightMapParams(std::uint32_t id, float scale, float offset) {   // 0x10072a4f2
    if (id == 0xffffffffu) return;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return;
    cOverlay& overlay = overlays_[resolved];
    const float inv = 1.0f / scale;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) overlay.texMatrix.m[r][c] = 0.0f;
    overlay.texMatrix.m[0][0] = inv;
    overlay.texMatrix.m[1][1] = -inv;
    overlay.texMatrix.m[2][0] = inv;      // +0x44: the original writes 1/scale here, not 0
    overlay.texMatrix.m[2][2] = 1.0f;
    overlay.texMatrix.m[3][1] = offset * inv + 1.0f;
    overlay.texMatrix.m[3][3] = 1.0f;
    overlay.flags |= kOverlayFlagHeightMap;
    if (!dynamic_) RedisplayStaticOverlay(resolved);
}

bool cSTEOverlayManager::OverlayIsEnabled(std::uint32_t id) const {   // 0x10072a62c
    if (id == 0xffffffffu) return false;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return false;
    return (overlays_[resolved].flags & kOverlayFlagEnabled) != 0;
}

float cSTEOverlayManager::OverlayAlpha(std::uint32_t id) const {   // 0x10072a688
    if (id == 0xffffffffu) return 0.0f;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return 0.0f;
    return overlays_[resolved].alpha;
}

void cSTEOverlayManager::OverlayColor(std::uint32_t id, cS3DVector3* out) const {   // 0x10072a6e8
    if (!out) return;
    if (id == 0xffffffffu) {
        *out = cS3DVector3{};
        return;
    }
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) {
        *out = cS3DVector3{};
        return;
    }
    const cOverlay& overlay = overlays_[resolved];
    out->x = overlay.color[0];
    out->y = overlay.color[1];
    out->z = overlay.color[2];
}

tOverlayDrawMode cSTEOverlayManager::OverlayDrawMode(std::uint32_t id) const {   // 0x10072a76a
    if (id == 0xffffffffu) return kOverlayDrawMode0;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return kOverlayDrawMode0;
    return static_cast<tOverlayDrawMode>(overlays_[resolved].drawMode);
}

bool cSTEOverlayManager::OverlayFlags(std::uint32_t id) const {   // 0x10072a7c4
    if (id == 0xffffffffu) return false;
    const std::uint32_t resolved = ResolveOverlay(id);
    if (resolved == 0xffffffffu) return false;
    return ((overlays_[resolved].flags >> 6) & 1) != 0;
}

void cSTEOverlayManager::GetStatsString(cRZString& out) {   // 0x10072a824
    out.Sprintf("%d decals, %d shadows", decalsDrawn_, shadowsDrawn_);
}

// ===============================================================================================================
// The draw/paint entry (0x100727ede)
// ===============================================================================================================
void cSTEOverlayManager::DrawOverlays(IOverlayDrawContext& dc) {
    // The whole body is skipped for a dynamic manager while the render properties flag at +0x738 is set.
    if (dynamic_ && dc.SmoothOverlays()) return;

    dc.ResetModelViewTransform();
    cS3DMatrix4 modelView;
    float modelViewFlat[16] = {};
    dc.GetModelViewMatrix(modelViewFlat);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) modelView.m[r][c] = modelViewFlat[r * 4 + c];
    // The original runs cofactors() on the model-view matrix and divides by w; for the affine transforms the
    // renderer hands in, that is the inverse (see OverlayCofactors).
    cS3DMatrix4 inverseView = InverseAffine(modelView);

    const int depthOffset = dc.DepthOffset();

    // The static overlay list the original keeps in DrawOverlays' function-local static (DAT_100b90358..368).
    static std::vector<std::uint32_t> sOverlaysToDraw;
    if (!dynamic_ && spatialIndexValid_) {
        SDSFindOverlaysToDraw(sOverlaysToDraw);
    } else {
        sOverlaysToDraw.clear();
        for (std::uint32_t i = 0; i < overlays_.size(); ++i)
            if (overlays_[i].freeLink == -1) sOverlaysToDraw.push_back(i);
    }

    vertexBufferExt_ = dc.GetVertexBufferExtension();
    if (!sOverlaysToDraw.empty()) {
        if (dc.DecalPassEnabled()) DrawDecals(inverseView, dc, sOverlaysToDraw);
        if (!sOverlaysToDraw.empty() && dc.ShadowPassEnabled()) {
            if (quality_ < 3 || !dc.ShadowProjectionAccurate()) DrawShadowsRough(inverseView, dc, sOverlaysToDraw);
            else DrawShadows(inverseView, dc, sOverlaysToDraw);
        }
    }
    dc.ClearTexTransform(0);
    dc.SetDepthOffset(depthOffset);
    if (vertexBufferExt_) {
        dc.ReleaseVertexBufferExtension();
        vertexBufferExt_ = nullptr;
    }
}

void cSTEOverlayManager::DrawDecals(const cS3DMatrix4& inverseView, IOverlayDrawContext& dc,
                                    const std::vector<std::uint32_t>& ids) {   // 0x10072aa56
    if (ids.empty()) return;
    dc.SetDepthOffset(2);
    const std::uint64_t mask = dc.SmoothOverlays() ? 0x91u : 0x11u;   // render properties +0x738
    decalsDrawn_ = 0;
    for (std::uint32_t id : ids) {
        cOverlay& overlay = overlays_[id];
        if (overlay.freeLink != -1 || ((overlay.flags ^ 0x91u) & mask) != 0) continue;

        dc.SetTransparency(overlay.alpha);
        const bool selfLit = (overlay.flags & kOverlayFlagSelfLit) != 0;
        if (selfLit) dc.SetSelfLitShade(overlay.color);
        else dc.SetShade(overlay.color);
        dc.DriverSetState(selfLit ? 0 : 1, 0);
        dc.SetRenderState(renderStates_[overlay.drawMode]);
        int textureId = 0;
        if (overlay.texture) textureId = overlay.texture->GlTextureId();
        else if (overlay.stringTexture) textureId = overlay.stringTexture->GlTextureId();
        dc.SetTexture(textureId, 0);
        dc.SetTexFiltering(1, 7, 0);
        int wrapU = 2, wrapV = 2;
        if (overlay.flags & kOverlayFlagHeightMap) {
            wrapU = 3;
            wrapV = 2;
        } else if (overlay.flags & kOverlayFlagTransformed) {
            wrapU = 3;
            wrapV = 3;
        }
        dc.SetTexWrapModes(wrapU, wrapV, 0);
        const cS3DMatrix4 transform = inverseView * overlay.texMatrix;
        dc.SetTexTransform4(transform, 0);
        DrawRect(dc, overlay.rect);
        ++decalsDrawn_;
    }
    dc.ResetTransparency();
    dc.ResetShade();
    dc.DriverSetState(1, 1);
}

void cSTEOverlayManager::DrawShadowsRough(const cS3DMatrix4& inverseView, IOverlayDrawContext& dc,
                                          const std::vector<std::uint32_t>& ids) {   // 0x10072acae
    if (ids.empty()) return;
    IOverlayHost* host = host_ ? host_ : g_host;
    IOverlayShadowService* shadow = host ? host->ShadowService() : nullptr;

    dc.SetRenderState(renderStates_[kOverlayDrawModeShadow]);
    const bool previous = dc.DriverStage7Begin();
    dc.DriverStage7Apply();
    cS3DVector4 color{0, 0, 0, 0};
    float scale = 0.0f, intensity = 0.0f;
    if (shadow) shadow->GetShadowColor(color, scale, intensity);
    dc.SetTexColor(color);
    if (intensity <= 0.0f) {
        dc.EnableAlphaTestFlag(false);
    } else {
        dc.EnableAlphaTestFlag(true);
        dc.SetAlphaFunc(6, scale * intensity);
    }
    dc.SetTexture(shadowTexture_ ? shadowTexture_->GlTextureId() : 0, 0);
    dc.SetTexFiltering(1, 1, 0);
    dc.SetTexWrapModes(2, 2, 0);
    dc.SetTexCombiner(roughShadowCombiner_, 0);
    dc.SetDepthOffset(3);
    shadowsDrawn_ = 0;
    for (std::uint32_t id : ids) {
        cOverlay& overlay = overlays_[id];
        if (overlay.freeLink != -1 || (overlay.flags & 9u) != 9u) continue;
        const cS3DMatrix4 transform = inverseView * overlay.texMatrix;
        dc.SetTexTransform4(transform, 0);
        DrawRect(dc, overlay.rect);
        ++shadowsDrawn_;
    }
    if (!previous) dc.DriverStage7End();
}

void cSTEOverlayManager::DrawShadows(const cS3DMatrix4& inverseView, IOverlayDrawContext& dc,
                                     const std::vector<std::uint32_t>& ids) {   // 0x10072aee0
    if (ids.empty()) return;
    IOverlayHost* host = host_ ? host_ : g_host;
    IOverlayShadowService* shadow = host ? host->ShadowService() : nullptr;

    dc.SetRenderState(renderStates_[kOverlayDrawModeShadow]);
    cS3DVector4 color{0, 0, 0, 0};
    float scale = 0.0f, intensity = 0.0f;
    if (shadow) shadow->GetShadowColor(color, scale, intensity);
    dc.SetTexColor(color);
    if (intensity <= 0.0f) {
        dc.EnableAlphaTestFlag(false);
    } else {
        dc.EnableAlphaTestFlag(true);
        dc.SetAlphaFunc(6, intensity * scale);
    }
    dc.SetTexFiltering(1, 1);
    dc.SetTexCombiner(shadowCombiner0_, 0);
    if (shadowTexture_) {
        const int stage = secondStage_ != 4 ? 1 : 0;
        dc.EnableTextureStateFlag(true, stage);
        dc.SetTexture(shadowTexture_->GlTextureId(), stage);
        dc.SetTexFiltering(0, 0, stage);
        dc.SetTexWrapModes(2, 2, stage);   // 0x10072b046: the mask's stage (ecx = r13), not stage 0
        dc.SetTexCombiner(secondStage_ == 4 ? shadowCombiner1_ : shadowCombiner0_, stage);
    }
    dc.SetDepthOffset(3);
    shadowsDrawn_ = 0;
    for (std::uint32_t id : ids) {
        cOverlay& overlay = overlays_[id];
        if (overlay.freeLink != -1 || (overlay.flags & 9u) != 9u) continue;
        if (secondStage_ == 4) {
            // one stage: the mask alone on stage 0, through the overlay's shadowMatrix (+0x64, 0x10072b0d3)
            const cS3DMatrix4 transform = inverseView * overlay.shadowMatrix;
            dc.SetTexTransform4(transform, 0);
        } else {
            dc.SetTexture(overlay.texture ? overlay.texture->GlTextureId() : 0, 0);
            const int wrap = (overlay.flags & 10u) != 0 ? 3 : 2;
            dc.SetTexWrapModes(wrap, wrap, 0);
            const cS3DMatrix4 transform = inverseView * overlay.texMatrix;
            dc.SetTexTransform4(transform, 0);
            const cS3DMatrix4 shadowTransform = inverseView * overlay.shadowMatrix;
            dc.SetTexTransform4(shadowTransform, 1);
            const cS3DVector2 centre{cellSize_ * 0.5f * static_cast<float>(overlay.rect.left + overlay.rect.right),
                                     cellSize_ * 0.5f * static_cast<float>(overlay.rect.top + overlay.rect.bottom)};
            float alpha = scale;
            if (!drawWaterPass_ && shadow) alpha *= shadow->ShadowFalloff(centre);
            if (static_cast<double>(alpha) < 0.05) continue;
            if (0.0f < intensity) dc.SetAlphaFunc(6, alpha * intensity);
            // the faded strength is stored over the colour's alpha (-0x54 is color.w) before SetTexColor (0x10072b25e); the
            // colour stays modified for the overlays after this one
            color.w = alpha;
            dc.SetTexColor(color);
        }
        if (!drawWaterPass_) {
            DrawRect(dc, overlay.rect);
        } else {
            const int cells = host ? host->TerrainCellCountInRect(overlay.rect) : 0;
            const int covered = (overlay.rect.right + 1 - overlay.rect.left) * (overlay.rect.bottom + 1 - overlay.rect.top);
            if (covered <= cells) DrawRect(dc, overlay.rect);
            else DrawMixedWaterAndLandRect(dc, overlay.rect);
        }
        ++shadowsDrawn_;
    }
    dc.EnableTextureStateFlag(false, 1);
}

void cSTEOverlayManager::DrawRect(IOverlayDrawContext& dc, const cRZRect& rect) {   // 0x10072b342
    (void)dc;   // the original only reads the draw context's vertex-buffer extension, cached in vertexBufferExt_
    IOverlayHost* host = host_ ? host_ : g_host;
    if (!host) return;
    // The original adds 1 to the stored right / bottom (an exclusive rect becomes inclusive) and hands the rect to
    // the view's mesh-subset draws.
    const cRZRect inclusive{rect.left, rect.top, rect.right + 1, rect.bottom + 1};
    // dynamic_ == 0 selects the draw frustum, a dynamic manager the view frustum (the original tests +0x29).
    if (drawWaterPass_) {
        host->DrawWaterSurfaceSubset(dynamic_, inclusive);
        return;
    }
    if (vertexBufferExt_) {
        host->DrawTerrainMeshSubset(dynamic_, inclusive, vertexBufferExt_);
        return;
    }
    host->DrawTerrainMeshSubset(dynamic_, inclusive, nullptr);
}

void cSTEOverlayManager::DrawMixedWaterAndLandRect(IOverlayDrawContext& dc, const cRZRect& rect) {   // 0x10072b3ee
    (void)dc;
    IOverlayHost* host = host_ ? host_ : g_host;
    if (!host) return;
    const cRZRect inclusive{rect.left, rect.top, rect.right + 1, rect.bottom + 1};
    host->DrawMixedMeshSubset(dynamic_, inclusive);
}

// ===============================================================================================================
// Shadow rects (0x1007296f0)
// ===============================================================================================================
void cSTEOverlayManager::CalcShadowTerrainRect(const cS3DVector3& lightDir, int, const cS3DVector3*,
                                               const cS3DBoundingBox& box, cRZRect* out) {
    // box min/max are (x, y, z); the shadow is the box's ground footprint widened by the light direction, shrunk by
    // 0.07 cell on every side and padded by 1/16 of the box height.
    const float scale = invCellSize_;
    int left = FloorEdge((box.min[0] + 0.07f) * scale);
    int top = FloorEdge((box.min[2] + 0.07f) * scale);
    int right = CeilEdge((box.max[0] - 0.07f) * scale);
    int bottom = CeilEdge((box.max[2] - 0.07f) * scale);
    const int pad = CeilEdge((box.max[1] - box.min[1]) * 0.0625f);
    if (lightDir.x <= 0.0f) left -= pad;
    else right += pad;
    if (lightDir.z <= 0.0f) top -= pad;
    else bottom += pad;

    if (left <= visible_.right && visible_.left <= right && top <= visible_.bottom && visible_.top <= bottom) {
        left = left < visible_.left ? visible_.left : left;
        top = top < visible_.top ? visible_.top : top;
        right = visible_.right < right ? visible_.right : right;
        bottom = visible_.bottom < bottom ? visible_.bottom : bottom;
    } else {
        left = top = right = bottom = 0;
    }
    *out = cRZRect{left, top, right, bottom};
}

// ===============================================================================================================
// The eight overlays of a ring decal (0x10072b442)
// ===============================================================================================================
void cSTEOverlayManager::UpdateRingDecalOverlays(const cRingDecal& ring, const std::array<float, 4>& rect,
                                                 bool staticOnly) {
    // The ring is the annulus between the decal's bounding ellipse of radius r = (right - left) / 2 and 0.9 r,
    // cut into the eight tiles of a 3x3 grid: three row bands (top 10 %, middle 80 %, bottom 10 %) and three
    // columns (left cap, middle, right cap). Each tile is clipped against both ellipses at its own rows.
    const float left = rect[0], top = rect[1], right = rect[2], bottom = rect[3];
    const int il = FloorEdge(left), it = FloorEdge(top), ir = CeilEdge(right), ib = CeilEdge(bottom);
    const float cx = (left + right) * 0.5f;
    const float cz = (top + bottom) * 0.5f;
    const float radius = (right - left) * 0.5f;
    const float outer2 = radius * radius;
    const float inner2 = (radius * 0.9f) * (radius * 0.9f);
    const int top10 = it + RoundToInt((bottom - top) * 0.1f);
    const int top20 = it + RoundToInt((bottom - top) * 0.2f);
    const int bottom20 = ib - RoundToInt((bottom - top) * 0.2f);
    const int bottom10 = ib - RoundToInt((bottom - top) * 0.1f);

    struct Span { int outerLo, outerHi, innerLo, innerHi; };
    const auto spanAt = [&](int z) {
        const float dz = cz - static_cast<float>(z);
        const float dz2 = dz * dz;
        const float outer = outer2 > dz2 ? outer2 - dz2 : 0.0f;
        const float inner = inner2 > dz2 ? inner2 - dz2 : 0.0f;
        const float sqrtOuter = std::sqrt(outer);
        const float sqrtInner = std::sqrt(inner);
        Span s;
        s.outerLo = FloorEdge(cx - sqrtOuter);
        s.outerHi = CeilEdge(cx + sqrtOuter);
        s.innerLo = CeilEdge(cx - sqrtInner);
        s.innerHi = FloorEdge(cx + sqrtInner);
        return s;
    };

    const Span atTop10 = spanAt(top10);
    const Span atTop20 = spanAt(top20);
    const Span atBottom20 = spanAt(bottom20);
    const Span atBottom10 = spanAt(bottom10);

    // 0 top cap, 1 bottom cap, 2 left middle band, 3 right middle band, 4 left upper corner, 5 left lower corner,
    // 6 right upper corner, 7 right lower corner. (The original's sixteen SIMD blocks assign the same tiles; the
    // slot -> rectangle mapping is the part its listing does not resolve - see the PARTIAL note.)
    const cRZRect tiles[8] = {
        cRZRect{atTop10.outerLo, it, atTop10.outerHi, top10},
        cRZRect{atBottom10.outerLo, bottom10, atBottom10.outerHi, ib},
        cRZRect{il, top10, atTop20.innerLo, bottom20},
        cRZRect{atBottom20.innerHi, top10, ir, bottom20},
        cRZRect{il, top10, atTop20.innerLo, top20},
        cRZRect{il, bottom20, atBottom20.innerLo, bottom10},
        cRZRect{atTop20.innerHi, top10, ir, top20},
        cRZRect{atBottom20.innerHi, bottom20, ir, bottom10},
    };

    for (int i = 0; i < 8; ++i) {
        const std::uint32_t slot = ring.overlay[i];
        if (slot == 0xffffffffu || slot >= overlays_.size()) continue;
        if (staticOnly) UpdateStaticOverlayRect(slot, tiles[i]);
        else {
            cOverlay& overlay = overlays_[slot];
            const cRZRect& tile = tiles[i];
            if (tile.left <= visible_.right && visible_.left <= tile.right && tile.top <= visible_.bottom &&
                visible_.top <= tile.bottom) {
                overlay.rect.left = tile.left < visible_.left ? visible_.left : tile.left;
                overlay.rect.top = tile.top < visible_.top ? visible_.top : tile.top;
                overlay.rect.right = visible_.right < tile.right ? visible_.right : tile.right;
                overlay.rect.bottom = visible_.bottom < tile.bottom ? visible_.bottom : tile.bottom;
            } else {
                overlay.rect = cRZRect{0, 0, 0, 0};
            }
        }
    }
}

// ===============================================================================================================
// One static overlay rect changed (0x10072c246)
// ===============================================================================================================
void cSTEOverlayManager::UpdateStaticOverlayRect(std::uint32_t slot, const cRZRect& rect) {
    cRZRect clipped{0, 0, 0, 0};
    if (visible_.left <= rect.right && rect.left <= visible_.right && visible_.top <= rect.bottom &&
        rect.top <= visible_.bottom) {
        clipped.left = visible_.left < rect.left ? rect.left : visible_.left;
        clipped.top = visible_.top < rect.top ? rect.top : visible_.top;
        clipped.right = rect.right < visible_.right ? rect.right : visible_.right;
        clipped.bottom = rect.bottom < visible_.bottom ? rect.bottom : visible_.bottom;
    }
    cOverlay& overlay = overlays_[slot];
    if (clipped.left != overlay.rect.left || clipped.top != overlay.rect.top ||
        clipped.right != overlay.rect.right || clipped.bottom != overlay.rect.bottom) {
        SDSRemove(slot, overlay.rect);
        RedisplayStaticOverlay(slot);
        overlay.rect = clipped;
        SDSAdd(slot, overlay.rect);
    }
    RedisplayStaticOverlay(slot);
}

// ===============================================================================================================
// Overlay creation helpers
// ===============================================================================================================
std::uint32_t cSTEOverlayManager::AddOverlay(const IOverlayTexture* texture, const cRZRect& rect,
                                             const cS3DMatrix4& matrix, std::uint64_t flags) {   // 0x100729bd6
    if (!texture) return 0xffffffffu;
    cOverlay overlay;
    overlay.texture = const_cast<IOverlayTexture*>(texture);
    overlay.texture->AddRef();
    overlay.texMatrix = matrix;
    overlay.rect = rect;
    overlay.flags |= flags;
    const std::uint32_t slot = overlays_.AddSlot(overlay);
    AddOverlay(slot);
    return slot;
}

std::uint32_t cSTEOverlayManager::AddOverlay(std::uint32_t instance, const cRZRect& rect, const cS3DMatrix4& matrix,
                                             std::uint64_t flags) {   // 0x100729668
    IOverlayTexture* texture = textureFactory_ ? textureFactory_->GetTexture(instance) : nullptr;
    const std::uint32_t slot = AddOverlay(texture, rect, matrix, flags);
    if (texture) texture->Release();
    return slot;
}

// ===============================================================================================================
// cofactors @0x10072c5fc
// ===============================================================================================================
cS3DMatrix4 OverlayCofactors(const cS3DMatrix4& m) {
    // The adjugate (transposed cofactor matrix) of the 4x4, which is what the original's free function computes: it
    // zeroes its output and fills all sixteen entries with the 2x2 / 3x3 cofactor products. The overlay manager then
    // divides it by the homogeneous w it derives from the model-view matrix's first row, giving the inverse; the port
    // uses InverseAffine directly (see DrawOverlays).
    cS3DMatrix4 out{};
    const float a00 = m.m[0][0], a01 = m.m[0][1], a02 = m.m[0][2], a03 = m.m[0][3];
    const float a10 = m.m[1][0], a11 = m.m[1][1], a12 = m.m[1][2], a13 = m.m[1][3];
    const float a20 = m.m[2][0], a21 = m.m[2][1], a22 = m.m[2][2], a23 = m.m[2][3];
    const float a30 = m.m[3][0], a31 = m.m[3][1], a32 = m.m[3][2], a33 = m.m[3][3];
    const float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10;
    const float b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;
    const float b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;
    const float b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31;
    const float b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    out.m[0][0] = a11 * b11 - a12 * b10 + a13 * b09;
    out.m[0][1] = a02 * b10 - a01 * b11 - a03 * b09;
    out.m[0][2] = a31 * b05 - a32 * b04 + a33 * b03;
    out.m[0][3] = a22 * b04 - a21 * b05 - a23 * b03;
    out.m[1][0] = a12 * b08 - a10 * b11 - a13 * b07;
    out.m[1][1] = a00 * b11 - a02 * b08 + a03 * b07;
    out.m[1][2] = a32 * b02 - a30 * b05 - a33 * b01;
    out.m[1][3] = a20 * b05 - a22 * b02 + a23 * b01;
    out.m[2][0] = a10 * b10 - a11 * b08 + a13 * b06;
    out.m[2][1] = a01 * b08 - a00 * b10 - a03 * b06;
    out.m[2][2] = a30 * b04 - a31 * b02 + a33 * b00;
    out.m[2][3] = a21 * b02 - a20 * b04 - a23 * b00;
    out.m[3][0] = a11 * b07 - a10 * b09 - a12 * b06;
    out.m[3][1] = a00 * b09 - a01 * b07 + a02 * b06;
    out.m[3][2] = a31 * b01 - a30 * b03 - a32 * b00;
    out.m[3][3] = a20 * b03 - a21 * b01 + a22 * b00;
    return out;
}

}  // namespace sc4
