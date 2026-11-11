// cSTEWaterRipple (Mac i386 0x004fad1c..0x004fccd8, constructor 0x004fbcc0, statics 0x00ab1b10): the animated foam ribbons
// along the sea shore. The terrain view creates one in its Init (0x004f74a4: this + 0x70) and hands it to the renderer as
// view object list 5, priority 1100; the animation tick manager ticks it (priority 0xc0).
//
// How it works:
// - DetectShores: the terrain marks land vertices next to the sea (flag 0x10); every chain of them longer than
//   kMinShoreSizeForPlacement becomes a shore: its vertices as points 16 m apart.
// - CreateRipplesForShore walks a uniform cubic B-spline through the shore points; at each step it puts a water-side point
//   (pushed out by the ribbon width, which breathes with a sine) and a land-side point. Points far enough apart extend a
//   triangle strip; a sharp turn, an edge of the city or 1000 quads start a new strip (a "ripple"). The water side is at
//   sea level, the land side kLandVertexHtAboveSeaLevel above it; the texture's u advances per quad.
// - Each ripple fades in and out at its ends (alpha steps), shows one frame of the animated foam texture and moves to the
//   next frame after a random 0.7..1.3 x kMicroSecsPerFrame.
// - The ribbons are rebuilt when the zoom changes, 2 s after the water grew, or after terraforming near them.
// Every tuning value comes from exemplar {0x6534284a, 0x88cd66e9, 2}, properties 0x491c9c00..0x491c9c0d (per zoom vectors).
// cSTEWaterWave (the original's other class here) never draws: it only keeps wave producers, so nothing of it is needed.
#include "app/NativeWaterRipple.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <list>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeModelMaker.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZRandom.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/DrawContext.h"
#include "sc4/Exemplar.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

bool Debug() { static const bool on = std::getenv("SC4_RIPPLEDEBUG") != nullptr; return on; }

constexpr std::uint32_t kMsgAnimationTick = 0x86ad10ee, kMsgTerrainEdited = 0x278e6003, kMsgTerrainChanged = 0x89171496;
constexpr std::uint32_t kTextureGroup = 0x891b0e1a, kTextureInstance = 0xe9a44a88;   // FSH {0x7ab50e44, ...}
constexpr float kCellWidth = 16.0f, kOneOverCellWidth = 1.0f / 16.0f;
constexpr float kEdgeInset = 0.2f;                 // 0x00a707b8
constexpr float kOneMinusMinAlphaFactor = 0.9f;    // __kOneMinusMinAlphaFactor
constexpr float kPhaseStep = 0.1f;                 // 0x00a70714: the sine's phase per spline step
constexpr float kAlphaStartFactor = 0.1f;          // 0x00a70714 again: the ramps start at a tenth of the full alpha
constexpr int kZooms = 5;

// The tuning statics (ReadTuningParameters 0x004fae42; defaults from the data section 0x00abc6e0..).
struct Tuning {
    bool enabled = true;
    std::uint32_t minShoreSize = 10;
    std::vector<float> quadWidthSquared, splineInterval;
    float landHeightAboveSea = 2.0f;
    std::vector<float> textureUPerQuad, waterRibbonWidth, landRibbonWidth, waterRibbonWidthVariation, concavityThreshold;
    std::int32_t microSecsPerFrame = 1000000, minMicroSecsPerFrame = 0, maxMicroSecsPerFrame = 0;
    std::vector<std::uint8_t> landAlpha, waterAlpha;
    std::uint32_t terrainUpdateAreaThreshold = 850;
    std::int32_t alphaIncrementSteps = 5;
    bool complete = false;   // every per-zoom vector has its five values (the original indexes them unchecked)
} g_tuning;

void ReadTuning() {
    static bool read = false;
    if (read) return;
    read = true;
    Tuning& t = g_tuning;
    void* resource = nullptr;
    cGZPersistResourceKey key{0x6534284a, 0x88cd66e9, 2};
    if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) || !resource) {
        if (Debug()) std::fprintf(stderr, "ripple: no tuning exemplar\n");
        return;
    }
    auto* exemplar = static_cast<sc4::cISCResExemplar*>(resource);
    const sc4::cISCPropertyHolder* p = exemplar->AsISCPropertyHolder();
    using H = sc4::cSCPropertyHelp;
    H::GetPropertyValue(p, 0x491c9c00, t.enabled);
    H::GetPropertyValue(p, 0x491c9c01, t.minShoreSize);
    if (t.minShoreSize < 2) t.minShoreSize = 2;
    H::GetPropertyValue(p, 0x491c9c02, t.quadWidthSquared);
    H::GetPropertyValue(p, 0x491c9c03, t.landHeightAboveSea);
    H::GetPropertyValue(p, 0x491c9c04, t.textureUPerQuad);
    H::GetPropertyValue(p, 0x491c9c05, t.waterRibbonWidth);
    H::GetPropertyValue(p, 0x491c9c06, t.landRibbonWidth);
    H::GetPropertyValue(p, 0x491c9c07, t.microSecsPerFrame);
    H::GetPropertyValue(p, 0x491c9c08, t.landAlpha);
    H::GetPropertyValue(p, 0x491c9c09, t.waterAlpha);
    H::GetPropertyValue(p, 0x491c9c0a, t.waterRibbonWidthVariation);
    H::GetPropertyValue(p, 0x491c9c0b, t.concavityThreshold);
    H::GetPropertyValue(p, 0x491c9c0c, t.terrainUpdateAreaThreshold);
    H::GetPropertyValue(p, 0x491c9c0d, t.alphaIncrementSteps);
    exemplar->Release();
    t.complete = t.quadWidthSquared.size() >= kZooms && t.textureUPerQuad.size() >= kZooms && t.waterRibbonWidth.size() >= kZooms &&
                 t.landRibbonWidth.size() >= kZooms && t.landAlpha.size() >= kZooms && t.waterAlpha.size() >= kZooms &&
                 t.waterRibbonWidthVariation.size() >= kZooms && t.concavityThreshold.size() >= kZooms && t.alphaIncrementSteps > 0;
    if (!t.complete) {
        if (Debug()) std::fprintf(stderr, "ripple: tuning exemplar incomplete\n");
        return;
    }
    t.splineInterval.resize(kZooms);
    for (int i = 0; i < kZooms; ++i) {
        t.splineInterval[i] = t.quadWidthSquared[i] * 0.015625f;
        t.quadWidthSquared[i] *= t.quadWidthSquared[i];
        t.concavityThreshold[i] *= 0.0174533f;   // degrees -> radians
    }
    // the 12582912 rounding trick of the original: floor of 0.7 x, ceiling of 1.3 x
    t.minMicroSecsPerFrame = static_cast<std::int32_t>(std::floor(static_cast<float>(t.microSecsPerFrame) * 0.7f));
    t.maxMicroSecsPerFrame = static_cast<std::int32_t>(std::ceil(static_cast<float>(t.microSecsPerFrame) * 1.3f));
    if (Debug())
        std::fprintf(stderr, "ripple: tuning on %d minShore %u landHt %.2f frame %d..%d us steps %d threshold %u\n", int(t.enabled),
                     t.minShoreSize, t.landHeightAboveSea, t.minMicroSecsPerFrame, t.maxMicroSecsPerFrame, t.alphaIncrementSteps,
                     t.terrainUpdateAreaThreshold);
}

// sVF_V3F_C4UB_T2F (vertex format 10, 24 bytes): colour bytes B, G, R, A as the original fills them (+0xc..+0xf).
struct Vertex {
    float x = 0, y = 0, z = 0;
    std::uint8_t color[4] = {0xff, 0xff, 0xff, 0};
    float u = 0, v = 0;
};
static_assert(sizeof(Vertex) == 24);

struct IntRect { std::int32_t l, t, r, b; };
constexpr IntRect kEmptyIntRect{0x7fffffff, 0x7fffffff, std::int32_t(0x80000000u), std::int32_t(0x80000000u)};
constexpr IntRect kNonIntxRect{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff};
bool Intersects(const IntRect& a, const IntRect& b) { return a.l <= b.r && b.l <= a.r && a.t <= b.b && b.t <= a.b; }

struct Ripple {                       // tRipple (0x4c)
    std::vector<Vertex> vertices;     // +0x0
    std::uint32_t frame = 0;          // +0xc
    std::uint32_t elapsed = 0;        // +0x10 (us)
    std::uint32_t duration = 0;       // +0x14 (us)
    IntRect cells = kEmptyIntRect;    // +0x18
    std::vector<IntRect> pieces;      // +0x28 (one per 20 vertices)
    float boxMin[3] = {}, boxMax[3] = {};   // +0x34 / +0x40
};
struct Shore {                        // tShore (0x3c)
    std::vector<std::pair<float, float>> points;   // +0x0
    std::list<Ripple> ripples;        // +0xc
    IntRect cells = kEmptyIntRect;    // +0x14
    float boxMin[3] = {}, boxMax[3] = {};   // +0x24 / +0x30
};

class WaterRipple final : public cISC4ViewObject3D {
public:
    explicit WaterRipple(sc4::cSTETerrain& terrain) : terrain_(terrain), random_(static_cast<std::uint32_t>(std::time(nullptr)) ^ 0x8c09c57au) {
        target_.owner = this;
    }

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }

    // Init (0x004fb6fe).
    void Init(float sizeX, float sizeZ) {
        ReadTuning();
        bounds_[0] = kEdgeInset;
        bounds_[1] = kEdgeInset;
        bounds_[2] = sizeX - kEdgeInset;
        bounds_[3] = sizeZ - kEdgeInset;
        vertsX_ = static_cast<int>(terrain_.CellCountX()) + 1;
        vertsZ_ = static_cast<int>(terrain_.CellCountZ()) + 1;
        water_.y = terrain_.GetSeaLevel();
        water_.v = 1.0f;
        land_.y = water_.y + g_tuning.landHeightAboveSea;
        land_.v = 0.0f;
    }

    cIGZMessageTarget2* Target() { return &target_; }

    // Update (0x004fccd8).
    void DrawViewObject(SC4DrawContext& context) override {
        if (!g_tuning.complete) return;
        if (!built_) {
            built_ = true;
            DetectShores();
            CreateRipples();
        }
        // The original also needs cSC4RenderProperties kRenderWaterEffects (bool 3, on by default); the native application
        // has no global render properties (View3D host GlobalRenderProperties), so the default holds.
        if (shores_.empty()) return;
        if (!stateReady_) {
            state_.PackRenderState(true, false, true, false, true, 1, true, 3, 4, 5, -1, -1.0f, 1, -1);
            stateReady_ = true;
        }
        context.SetRenderState(state_);
        context.ResetModelViewTransform();
        if (context.Zoom() != zoom_) {
            zoom_ = context.Zoom();
            CreateRipples();
        }
        SC4ViewUtilities* view = Renderer() ? Renderer()->GetViewUtilities() : nullptr;
        int drawn = 0;
        for (Shore& shore : shores_) {
            if (shore.ripples.empty() || (view && !view->IsBoundingBoxInViewFrustum(shore.boxMin, shore.boxMax))) continue;
            for (Ripple& ripple : shore.ripples) drawn += DrawRipple(ripple, context, view);
        }
        if (Debug() && drawn != lastDrawn_) {
            std::fprintf(stderr, "ripple: %zu shores, %d ripples drawn at zoom %d\n", shores_.size(), drawn, zoom_);
            lastDrawn_ = drawn;
        }
    }

private:
    struct MessageTarget final : cIGZMessageTarget2 {
        WaterRipple* owner = nullptr;
        bool QueryInterface(std::uint32_t iid, void** out) override {
            if (iid != 0x452294aa && iid != 1) return false;
            *out = static_cast<cIGZMessageTarget2*>(this);
            return true;
        }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
        bool DoMessage(cIGZMessage2* message) override { return owner->DoMessage(message); }
    };

    static sc4::cSC43DRender* Renderer();
    static std::uint64_t Data(cIGZMessage2* message, int which) {
        void* raw = nullptr;
        if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return 0;
        auto* standard = static_cast<cIGZMessage2Standard*>(raw);
        const std::uint64_t value = which == 1 ? standard->GetData1() : standard->GetData2();
        standard->Release();
        return value;
    }

    // DoMessage (0x004fcc02).
    bool DoMessage(cIGZMessage2* message) {
        if (!message || !g_tuning.complete) return true;
        const std::uint32_t type = message->GetType();
        if (type == kMsgAnimationTick) {
            DoAnimationTick(static_cast<std::uint32_t>(Data(message, 2)));
        } else if (type == kMsgTerrainChanged) {
            shores_.clear();
            DetectShores();
            CreateRipples();
        } else if (type == kMsgTerrainEdited) {
            DoOnTerrainEdit(static_cast<std::uint32_t>(Data(message, 1)), static_cast<std::uint32_t>(Data(message, 2)));
        }
        return true;
    }

    // DoAnimationTick (0x004fcb20): microseconds since the last tick.
    void DoAnimationTick(std::uint32_t elapsed) {
        if (!built_) return;
        if (!rebuildPending_) {
            if (g_tuning.minShoreSize * g_tuning.minShoreSize / 10 + waterVertices_ < terrain_.NumberOfWaterVertices()) {
                rebuildPending_ = true;
                rebuildTimer_ = 0;
            }
        } else {
            rebuildTimer_ += elapsed;
            if (rebuildTimer_ > 2000000) {
                rebuildPending_ = false;
                shores_.clear();
                DetectShores();
                CreateRipples();
            }
        }
        for (Shore& shore : shores_)
            for (Ripple& ripple : shore.ripples) TickRipple(ripple, elapsed);
    }

    // TickRipple (0x004fad1c).
    void TickRipple(Ripple& ripple, std::uint32_t elapsed) {
        ripple.elapsed += elapsed;
        if (ripple.elapsed < ripple.duration) return;
        ++ripple.frame;
        ripple.duration = static_cast<std::uint32_t>(random_.RandomSint32RangeUniform(g_tuning.minMicroSecsPerFrame, g_tuning.maxMicroSecsPerFrame));
        ripple.elapsed = 0;
        if (ripple.frame >= Frames().size()) ripple.frame = 0;
    }

    // DoOnTerrainEdit (0x004fbbc2): the packed cell rectangle of a terraform; big enough edits over the ribbons (or near the
    // area edits have accumulated in) rebuild them.
    void DoOnTerrainEdit(std::uint32_t data1, std::uint32_t data2) {
        IntRect r{static_cast<std::int32_t>(data1 >> 16), static_cast<std::int32_t>(data2 >> 16), static_cast<std::int32_t>(data1 & 0xffff),
                  static_cast<std::int32_t>(data2 & 0xffff)};
        const std::int32_t area = (r.r - r.l) * (r.b - r.t);
        if (area <= 0) return;
        editedArea_ += static_cast<std::uint32_t>(area);
        if (editedArea_ <= g_tuning.terrainUpdateAreaThreshold) return;
        editedArea_ = 0;
        if (!IntersectsRipples(r)) {
            if (rebuildPending_) return;
            // Quirk (kept): the accumulated rectangle is never reset.
            dirty_.l = std::min(dirty_.l, r.l);
            dirty_.t = std::min(dirty_.t, r.t);
            dirty_.r = std::max(dirty_.r, r.r);
            dirty_.b = std::max(dirty_.b, r.b);
            if ((dirty_.r - dirty_.l) * (dirty_.b - dirty_.t) < 0x191) return;
        }
        rebuildPending_ = true;
        rebuildTimer_ = 0;
    }

    // IntersectsRipples (0x004fbb0e) / IntersectsRipple (0x004fb670): erases every ripple a piece of which touches `r`.
    bool IntersectsRipples(const IntRect& r) {
        bool any = false;
        for (Shore& shore : shores_) {
            if (!Intersects(r, shore.cells)) continue;
            for (auto it = shore.ripples.begin(); it != shore.ripples.end();) {
                bool hit = false;
                if (Intersects(r, it->cells))
                    for (const IntRect& piece : it->pieces) if (Intersects(r, piece)) { hit = true; break; }
                if (hit) { it = shore.ripples.erase(it); any = true; } else { ++it; }
            }
        }
        return any;
    }

    // DetectShores (0x004fc7d4) + CreateShoreObject (0x004fc70c).
    void DetectShores() {
        if (!g_tuning.enabled) return;
        editedArea_ = 0;
        waterVertices_ = terrain_.NumberOfWaterVertices();
        terrain_.DetectShoreVertices();
        std::deque<std::int32_t> chain;
        for (int z = 0; z < vertsZ_; ++z) {
            for (int x = 0; x < vertsX_; ++x) {
                const std::int32_t index = z * vertsX_ + x;
                const std::uint16_t flags = terrain_.Vertices()[static_cast<std::size_t>(index)].flags;
                if ((flags & 1) || !(flags & sc4::kVertexShore)) continue;
                chain.clear();
                terrain_.GetShoreStartingAtVert(index, chain, 1);
                if (chain.size() <= g_tuning.minShoreSize) continue;
                Shore& shore = shores_.emplace_back();
                for (std::int32_t v : chain)
                    shore.points.emplace_back(static_cast<float>(v % vertsX_) * kCellWidth, static_cast<float>(v / vertsX_) * kCellWidth);
            }
        }
        terrain_.ClearVertexPropertyFlags(1);
        if (Debug()) std::fprintf(stderr, "ripple: %zu shores, %u water vertices\n", shores_.size(), waterVertices_);
        if (Debug())
            for (const Shore& shore : shores_) {
                const auto& mid = shore.points[shore.points.size() / 2];
                std::fprintf(stderr, "ripple: shore of %zu points, middle at %.0f,%.0f\n", shore.points.size(), mid.first, mid.second);
            }
    }

    // CreateRipples (0x004fc92a).
    void CreateRipples() {
        if (rebuildPending_) {
            rebuildPending_ = false;
            shores_.clear();
            DetectShores();
        }
        const int z = Zoom();
        const Tuning& t = g_tuning;
        waterAlphaStart_ = static_cast<std::uint8_t>(static_cast<int>(static_cast<float>(t.waterAlpha[z]) * kAlphaStartFactor));
        landAlphaStart_ = static_cast<std::uint8_t>(static_cast<int>(static_cast<float>(t.landAlpha[z]) * kAlphaStartFactor));
        auto step = [&](std::uint8_t alpha) {
            const auto s = static_cast<std::uint8_t>(static_cast<int>(static_cast<float>(alpha) * kOneMinusMinAlphaFactor / static_cast<float>(t.alphaIncrementSteps)));
            return s ? s : std::uint8_t(1);
        };
        waterAlphaStep_ = step(t.waterAlpha[z]);
        landAlphaStep_ = step(t.landAlpha[z]);
        widthAmplitude_ = t.waterRibbonWidthVariation[z] * t.waterRibbonWidth[z];
        phase_ = 0.0f;
        land_.color[3] = t.landAlpha[z];
        water_.color[3] = t.waterAlpha[z];
        for (Shore& shore : shores_) {
            shore.ripples.clear();
            CreateRipplesForShore(shore);
        }
        ComputeBoundRects();
    }

    // ComputeSplinePoints (0x004fb18e): the uniform cubic B-spline through points i..i+3 at t; the water point is pushed out
    // along the normal by the breathing ribbon width, the land point the other way by the land width.
    void ComputeSplinePoints(const Shore& shore, int i, float t) {
        const float b0 = (((3.0f - t) * t - 3.0f) * t + 1.0f) / 6.0f;
        const float b1 = ((t * 3.0f - 6.0f) * t * t + 4.0f) / 6.0f;
        const float b2 = ((3.0f + (t * -3.0f + 3.0f) * t) * t + 1.0f) / 6.0f;
        const float b3 = (t * t * t) / 6.0f;
        const float d0 = ((2.0f - t) * t - 1.0f) * 0.5f;
        const float d1 = (t * 3.0f - 4.0f) * t * 0.5f;
        const float d2 = (1.0f + t * (t * -3.0f + 2.0f)) * 0.5f;
        const float d3 = t * t * 0.5f;
        const auto& p0 = shore.points[static_cast<std::size_t>(i)];
        const auto& p1 = shore.points[static_cast<std::size_t>(i) + 1];
        const auto& p2 = shore.points[static_cast<std::size_t>(i) + 2];
        const auto& p3 = shore.points[static_cast<std::size_t>(i) + 3];
        const float px = b3 * p3.first + b2 * p2.first + b1 * p1.first + b0 * p0.first;
        const float pz = b0 * p0.second + b1 * p1.second + b2 * p2.second + b3 * p3.second;
        float tx = p3.first * d3 + p2.first * d2 + d0 * p0.first + d1 * p1.first;
        float tz = d0 * p0.second + d1 * p1.second + d2 * p2.second + d3 * p3.second;
        const float length = std::sqrt(tx * tx + tz * tz);   // cS3DVector2::Normalize
        if (length > 0.0f) { tx /= length; tz /= length; }
        const int z = Zoom();
        const float width = g_tuning.waterRibbonWidth[z] + widthAmplitude_ * std::sin(phase_);
        phase_ += kPhaseStep;
        water_.x = width * tz + px;
        water_.z = pz - width * tx;
        land_.x = px - tz * g_tuning.landRibbonWidth[z];
        land_.z = tx * g_tuning.landRibbonWidth[z] + pz;
    }

    bool Inside(float x, float z) const { return !(x < bounds_[0] || bounds_[2] < x || z < bounds_[1] || bounds_[3] < z); }
    static float Angle(float ax, float az, float bx, float bz) { return std::acos(std::clamp(ax * bx + az * bz, -1.0f, 1.0f)); }   // ComputeAngle

    // StartNewRipple (0x004fc1a2): reuse a ripple that got fewer than 8 vertices, else start another.
    Ripple* StartNewRipple(Shore& shore, Ripple* current) {
        if (current && current->vertices.size() <= 7) {
            current->vertices.clear();
            return current;
        }
        shore.ripples.emplace_back();
        return &shore.ripples.back();
    }

    // CreateRipplesForShore (0x004fc28c).
    void CreateRipplesForShore(Shore& shore) {
        const int n = static_cast<int>(shore.points.size());
        const int zoom = Zoom();
        const Tuning& t = g_tuning;
        Ripple* ripple = nullptr;
        bool newRipple = true, waterStep = true;
        int quads = 0, i = 0;
        float along = 0.0f;
        float lastWaterX = 0, lastWaterZ = 0, lastLandX = 0, lastLandZ = 0;
        float waterDirX = 0, waterDirZ = 0, landDirX = 0, landDirZ = 0;
        while (i < n - 3) {
            ComputeSplinePoints(shore, i, along);
            if (!Inside(land_.x, land_.z) || !Inside(water_.x, water_.z)) {
                ripple = StartNewRipple(shore, ripple);
                newRipple = true;
            } else if (newRipple) {
                ripple = StartNewRipple(shore, ripple);
                land_.u = 0.0f;
                water_.u = 0.0f;
                ripple->vertices.push_back(water_);
                ripple->vertices.push_back(land_);
                lastWaterX = water_.x; lastWaterZ = water_.z;
                lastLandX = land_.x; lastLandZ = land_.z;
                newRipple = false;
                waterStep = true;
                quads = 0;
            } else {
                bool tryLand = !waterStep;
                if (waterStep) {
                    const float dx = lastWaterX - water_.x, dz = lastWaterZ - water_.z;
                    if (dx * dx + dz * dz < t.quadWidthSquared[zoom]) {
                        newRipple = false;
                        waterStep = true;
                    } else {
                        float nx = water_.x - lastWaterX, nz = water_.z - lastWaterZ;
                        const float length = std::sqrt(nx * nx + nz * nz);
                        if (length > 0.0f) { nx /= length; nz /= length; }
                        if (quads >= 1 && Angle(waterDirX, waterDirZ, nx, nz) > t.concavityThreshold[zoom]) {
                            newRipple = true;
                            waterStep = true;
                        } else {
                            ripple->vertices.push_back(water_);
                            lastWaterX = water_.x; lastWaterZ = water_.z;
                            waterDirX = nx; waterDirZ = nz;
                            tryLand = true;
                        }
                    }
                }
                if (tryLand) {
                    const float dx = lastLandX - land_.x, dz = lastLandZ - land_.z;
                    if (t.quadWidthSquared[zoom] <= dx * dx + dz * dz) {
                        float nx = land_.x - lastLandX, nz = land_.z - lastLandZ;
                        const float length = std::sqrt(nx * nx + nz * nz);
                        if (length > 0.0f) { nx /= length; nz /= length; }
                        if (quads > 0 && Angle(landDirX, landDirZ, nx, nz) > t.concavityThreshold[zoom]) {
                            newRipple = true;
                            waterStep = false;
                        } else {
                            ripple->vertices.push_back(land_);
                            lastLandX = land_.x; lastLandZ = land_.z;
                            water_.u += t.textureUPerQuad[zoom];
                            land_.u += t.textureUPerQuad[zoom];
                            landDirX = nx; landDirZ = nz;
                            ++quads;
                            newRipple = quads > 1000;
                            waterStep = true;
                        }
                    } else {
                        newRipple = false;
                        waterStep = false;
                    }
                }
            }
            along += t.splineInterval[zoom];
            if (1.0f <= along) {
                const float whole = std::floor(along);
                i += static_cast<int>(whole);
                along -= whole;
            }
        }
        if (!shore.ripples.empty() && shore.ripples.back().vertices.size() < 8) shore.ripples.pop_back();
        for (Ripple& r : shore.ripples) PrepareRippleForDisplay(r);
    }

    // PrepareRippleForDisplay (0x004fb8ae): a random first frame and frame time, an even vertex count, and the alpha ramps
    // that fade both ends of the strip in.
    void PrepareRippleForDisplay(Ripple& ripple) {
        const std::size_t frames = Frames().size();
        ripple.frame = frames ? random_.RandomUint32Uniform(static_cast<std::uint32_t>(frames)) : 0;
        ripple.duration = static_cast<std::uint32_t>(random_.RandomSint32RangeUniform(g_tuning.minMicroSecsPerFrame, g_tuning.maxMicroSecsPerFrame));
        ripple.elapsed = 0;
        int n = static_cast<int>(ripple.vertices.size());
        if (n & 1) {
            ripple.vertices.pop_back();
            --n;
        }
        const int steps = std::min(n / 4, g_tuning.alphaIncrementSteps);
        const int zoom = Zoom();
        const std::uint8_t waterMax = g_tuning.waterAlpha[zoom], landMax = g_tuning.landAlpha[zoom];
        auto ramp = [&](auto&& at) {
            std::uint8_t water = waterAlphaStart_, land = landAlphaStart_;
            for (int k = 0; k < steps; ++k) {
                at(k, water, land);
                water = static_cast<std::uint8_t>(water + waterAlphaStep_);
                if (waterMax < water) water = waterMax;
                land = static_cast<std::uint8_t>(land + landAlphaStep_);
                if (landMax < land) land = landMax;
            }
        };
        ramp([&](int k, std::uint8_t water, std::uint8_t land) {
            ripple.vertices[static_cast<std::size_t>(2 * k)].color[3] = water;
            ripple.vertices[static_cast<std::size_t>(2 * k + 1)].color[3] = land;
        });
        ramp([&](int k, std::uint8_t water, std::uint8_t land) {
            ripple.vertices[static_cast<std::size_t>(n - 1 - 2 * k)].color[3] = land;
            ripple.vertices[static_cast<std::size_t>(n - 2 - 2 * k)].color[3] = water;
        });
    }

    // ComputeBoundRectsForRipple (0x004fbd6c): a cell rectangle per 20 vertices, their union, and the box at sea level.
    void ComputeBoundRectsForRipple(Ripple& ripple) {
        ripple.cells = kEmptyIntRect;
        ripple.pieces.clear();
        const int n = static_cast<int>(ripple.vertices.size());
        const int count = n / 20 + 1 - (n % 20 == 0 ? 1 : 0);
        ripple.pieces.resize(static_cast<std::size_t>(count));
        int at = 0;
        for (int piece = 0; piece < count; ++piece) {
            const int end = std::min(at + 20, n);
            float minX = ripple.vertices[static_cast<std::size_t>(at)].x, maxX = minX;
            float minZ = ripple.vertices[static_cast<std::size_t>(at)].z, maxZ = minZ;
            for (++at; at < end; ++at) {
                const Vertex& v = ripple.vertices[static_cast<std::size_t>(at)];
                minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
                minZ = std::min(minZ, v.z); maxZ = std::max(maxZ, v.z);
            }
            IntRect& r = ripple.pieces[static_cast<std::size_t>(piece)];
            r.l = static_cast<std::int32_t>(std::floor(minX * kOneOverCellWidth));
            r.r = static_cast<std::int32_t>(std::ceil(kOneOverCellWidth * maxX));
            r.t = static_cast<std::int32_t>(std::floor(kOneOverCellWidth * minZ));
            r.b = static_cast<std::int32_t>(std::ceil(kOneOverCellWidth * maxZ));
            ripple.cells.l = std::min(ripple.cells.l, r.l);
            ripple.cells.t = std::min(ripple.cells.t, r.t);
            ripple.cells.r = std::max(ripple.cells.r, r.r);
            ripple.cells.b = std::max(ripple.cells.b, r.b);
        }
        const float sea = terrain_.GetSeaLevel();
        ripple.boxMin[0] = static_cast<float>(ripple.cells.l) * kCellWidth;
        ripple.boxMax[0] = static_cast<float>(ripple.cells.r) * kCellWidth;
        ripple.boxMin[2] = static_cast<float>(ripple.cells.t) * kCellWidth;
        ripple.boxMax[2] = static_cast<float>(ripple.cells.b) * kCellWidth;
        ripple.boxMin[1] = ripple.boxMax[1] = sea;
    }

    // ComputeBoundRects (0x004fc056).
    void ComputeBoundRects() {
        const float sea = terrain_.GetSeaLevel();
        for (Shore& shore : shores_) {
            if (shore.ripples.empty()) {
                shore.cells = kNonIntxRect;
            } else {
                shore.cells = kEmptyIntRect;
                for (Ripple& ripple : shore.ripples) {
                    ComputeBoundRectsForRipple(ripple);
                    shore.cells.l = std::min(shore.cells.l, ripple.cells.l);
                    shore.cells.t = std::min(shore.cells.t, ripple.cells.t);
                    shore.cells.r = std::max(shore.cells.r, ripple.cells.r);
                    shore.cells.b = std::max(shore.cells.b, ripple.cells.b);
                }
            }
            shore.boxMin[0] = static_cast<float>(shore.cells.l) * kCellWidth;
            shore.boxMax[0] = static_cast<float>(shore.cells.r) * kCellWidth;
            shore.boxMin[2] = static_cast<float>(shore.cells.t) * kCellWidth;
            shore.boxMax[2] = static_cast<float>(shore.cells.b) * kCellWidth;
            shore.boxMin[1] = shore.boxMax[1] = sea;
        }
    }

    // DrawRipple (0x004fb5d2): the ripple's current foam frame over its strip (primitive 7, vertex format 10).
    int DrawRipple(const Ripple& ripple, SC4DrawContext& context, SC4ViewUtilities* view) {
        const std::vector<std::uint32_t>& frames = Frames();
        if (frames.empty() || ripple.vertices.empty()) return 0;
        if (view && !view->IsBoundingBoxInViewFrustum(ripple.boxMin, ripple.boxMax)) return 0;
        context.SetTexture(static_cast<int>(frames[ripple.frame % frames.size()]), 0);
        context.DrawPrims(7, 10, static_cast<int>(ripple.vertices.size()), ripple.vertices.data());
        return 1;
    }

    // The animated texture binding (StartNewRipple loads it on first use; the original's binding is a frame table).
    static const std::vector<std::uint32_t>& Frames() { return FshFrameTextureIds(kTextureGroup, kTextureInstance); }
    int Zoom() const { return zoom_ < 0 ? 0 : (zoom_ >= kZooms ? kZooms - 1 : zoom_); }

    sc4::cSTETerrain& terrain_;
    cRZRandom random_;                    // sRandGen
    MessageTarget target_;                // +0xc
    std::uint32_t refs_ = 1;
    bool built_ = false;                  // +0x18
    int vertsX_ = 0, vertsZ_ = 0;         // +0x2c / +0x30
    std::vector<Shore> shores_;           // +0x34
    int zoom_ = 0;                        // +0x40
    Vertex water_, land_;                 // +0x48 / +0x60 (the vertex templates)
    bool rebuildPending_ = false;         // +0x78
    std::uint32_t rebuildTimer_ = 0;      // +0x7c (us)
    std::uint8_t waterAlphaStart_ = 0, landAlphaStart_ = 0, waterAlphaStep_ = 1, landAlphaStep_ = 1;   // +0x80..+0x83
    float widthAmplitude_ = 0.0f;         // +0x84
    float phase_ = 0.0f;                  // +0x88
    IntRect dirty_ = kEmptyIntRect;       // +0x8c
    std::uint32_t editedArea_ = 0;        // +0x9c
    std::uint32_t waterVertices_ = 0;     // +0xa0
    float bounds_[4] = {};                // +0xa4..+0xb0
    cSC4PackedRenderState state_;         // sPackedRenderState
    bool stateReady_ = false;             // sbRenderStateObjectInited
    int lastDrawn_ = -1;
};

sc4::cSC43DRender* g_render = nullptr;
WaterRipple* g_ripple = nullptr;
sc4::cSC43DRender* WaterRipple::Renderer() { return g_render; }

}  // namespace

void AttachWaterRipple(sc4::cSC43DRender& render, sc4::cSTETerrain& terrain, float citySizeX, float citySizeZ) {
    DetachWaterRipple();
    g_render = &render;
    g_ripple = new WaterRipple(terrain);
    g_ripple->Init(citySizeX, citySizeZ);
    render.AddViewObject(g_ripple, 5, 0x44c);
    if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(g_ripple->Target(), 0xc0);
    if (SL::spMS2) {
        SL::spMS2->AddNotification(g_ripple->Target(), kMsgTerrainEdited);
        SL::spMS2->AddNotification(g_ripple->Target(), kMsgTerrainChanged);
    }
}

void DetachWaterRipple() {
    if (!g_ripple) return;
    if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(g_ripple->Target(), 0xc0);
    if (SL::spMS2) {
        SL::spMS2->RemoveNotification(g_ripple->Target(), kMsgTerrainEdited);
        SL::spMS2->RemoveNotification(g_ripple->Target(), kMsgTerrainChanged);
    }
    if (g_render) g_render->RemoveViewObject(g_ripple);
    g_ripple->Release();
    g_ripple = nullptr;
    g_render = nullptr;
}

}  // namespace native
