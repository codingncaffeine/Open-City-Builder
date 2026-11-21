// cSTEOverlayManager: the record layouts, the slot pool, the render-state packs, the decal / ring / shadow / height-map
// creation paths, the setters and getters with their quirks, the spatial index, the static-overlay clipping and the
// whole draw pass (decal pass, both shadow paths, the water-pass mesh choice) against a recording fake draw context.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sc4/OverlayManager.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool RectEq(const cRZRect& r, int l, int t, int rr, int b) {
    return r.left == l && r.top == t && r.right == rr && r.bottom == b;
}
static bool RectEq(const cRZRect& a, const cRZRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
static std::size_t BucketSize(const std::forward_list<std::uint32_t>& list) {
    std::size_t n = 0;
    for (auto it = list.begin(); it != list.end(); ++it) ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------------------------------------------
struct FakeTexture : IOverlayTexture {
    int id;
    int refs = 1;
    explicit FakeTexture(int id_) : id(id_) { Textures().push_back(this); }
    static std::vector<FakeTexture*>& Textures() { static std::vector<FakeTexture*> v; return v; }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int GlTextureId() const override { return id; }
};

struct FakeStringTexture : IOverlayStringTexture {
    int id;
    std::int32_t width, height;
    int refs = 1;
    FakeStringTexture(int id_, std::int32_t w, std::int32_t h) : id(id_), width(w), height(h) { Strings().push_back(this); }
    static std::vector<FakeStringTexture*>& Strings() { static std::vector<FakeStringTexture*> v; return v; }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void GetSize(std::int32_t& w, std::int32_t& h) const override { w = width; h = height; }
    int GlTextureId() const override { return id; }
};

struct FakeFactory : IOverlayTextureFactory {
    int refs = 1;
    int byKey = 0, byInstance = 0, sizeCalls = 0;
    std::uint32_t lastInstance = 0;
    cGZPersistResourceKey lastKey;
    std::int32_t sizeW = 64, sizeH = 32;
    std::vector<FakeTexture*> made;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    IOverlayTexture* GetTexture(const cGZPersistResourceKey& key) override {
        lastKey = key;
        ++byKey;
        auto* t = new FakeTexture(static_cast<int>(key.instance) + 1000);
        made.push_back(t);
        return t;
    }
    IOverlayTexture* GetTexture(std::uint32_t instance) override {
        lastInstance = instance;
        ++byInstance;
        auto* t = new FakeTexture(static_cast<int>(instance) + 2000);
        made.push_back(t);
        return t;
    }
    void GetTextureSize(const IOverlayTexture*, std::int32_t& w, std::int32_t& h) override {
        ++sizeCalls;
        w = sizeW;
        h = sizeH;
    }
    std::uint32_t ShadowTextureGroup() override { return 0x1234; }
};

struct FakeShadow : IOverlayShadowService {
    bool projectOk = true, calcOk = true;
    cS3DVector3 direction{1.0f, 0.0f, 0.0f};
    cS3DVector4 color{0.5f, 0.25f, 0.125f, 0.0625f};
    float scale = 0.5f, intensity = 2.0f, falloff = 1.0f;
    int projectCalls = 0, calcCalls = 0, colorCalls = 0, directionCalls = 0, falloffCalls = 0;
    cS3DMatrix4 projectOut{};
    cRZRect calcOut{2, 3, 4, 5};
    bool ProjectShadow(int, const cS3DVector3*, const cS3DVector2*, const cS3DTransform&, cS3DMatrix4& out) override {
        ++projectCalls;
        out = projectOut;
        return projectOk;
    }
    void GetShadowDirection(cS3DVector3& out) override {
        ++directionCalls;
        out = direction;
    }
    bool CalcShadowRect(int, const cS3DVector3*, const cS3DBoundingBox&, cRZRect& out) override {
        ++calcCalls;
        out = calcOut;
        return calcOk;
    }
    void GetShadowColor(cS3DVector4& colour, float& s, float& i) override {
        ++colorCalls;
        colour = color;
        s = scale;
        i = intensity;
    }
    float ShadowFalloff(const cS3DVector2&) override {
        ++falloffCalls;
        return falloff;
    }
};

struct FakeMessage : cIGZMessage2 {
    std::uint32_t type = 0;
    FakeMessage(std::uint32_t t) : type(t) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Create(std::uint32_t, void** out) const override { *out = nullptr; return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t t) override { type = t; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
};

struct FakeMessages : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeHost : IOverlayHost {
    FakeMessages messages;
    FakeFactory factory;
    FakeShadow shadow;
    int cellsX = 8, cellsZ = 8;
    float cellSize = 16.0f;
    int quality = 2, secondStage = 0;
    bool spatialIndexValid = true;
    std::vector<std::int32_t> colStart, colEnd;
    int redisplayCalls = 0, lastRedisplayWater = -1;
    cRZRect lastRedisplay;
    int rectCells = 0;
    std::vector<std::string> draws;

    FakeHost() : colStart(16, 0), colEnd(16, 8) {}
    cIGZMessageServer2* Messages() override { return &messages; }
    IOverlayTextureFactory* TextureFactory() override { return &factory; }
    IOverlayShadowService* ShadowService() override { return &shadow; }
    int RenderQuality() override { return quality; }
    int RenderSecondTextureStage() override { return secondStage; }
    bool RenderSpatialIndexValid() override { return spatialIndexValid; }
    int TerrainCellCountX() override { return cellsX; }
    int TerrainCellCountZ() override { return cellsZ; }
    float TerrainCellSize() override { return cellSize; }
    int TerrainCellCountInRect(const cRZRect&) override { return rectCells; }
    void RedisplayTerrain(int water, const cRZRect& rect) override {
        ++redisplayCalls;
        lastRedisplayWater = water;
        lastRedisplay = rect;
    }
    void GetTerrainCullingInfo(int& rowBegin, int& rowEnd, const std::int32_t*& start, const std::int32_t*& end) override {
        rowBegin = 0;
        rowEnd = cellsZ;
        start = colStart.data();
        end = colEnd.data();
    }
    void DrawTerrainMeshSubset(bool viewFrustum, const cRZRect& rect, ITerrainVertBufExtn* vb) override {
        draws.push_back(std::string("terrain:") + (viewFrustum ? "view" : "draw") + (vb ? ":vb" : "") + ":" +
                        std::to_string(rect.left) + "," + std::to_string(rect.top) + "," + std::to_string(rect.right) +
                        "," + std::to_string(rect.bottom));
    }
    void DrawWaterSurfaceSubset(bool viewFrustum, const cRZRect& rect) override {
        draws.push_back(std::string("water:") + (viewFrustum ? "view" : "draw") + ":" + std::to_string(rect.left) + "," +
                        std::to_string(rect.top) + "," + std::to_string(rect.right) + "," + std::to_string(rect.bottom));
    }
    void DrawMixedMeshSubset(bool viewFrustum, const cRZRect& rect) override {
        draws.push_back(std::string("mixed:") + (viewFrustum ? "view" : "draw") + ":" + std::to_string(rect.left) + "," +
                        std::to_string(rect.top) + "," + std::to_string(rect.right) + "," + std::to_string(rect.bottom));
    }
};

struct FakeDC : IOverlayDrawContext {
    std::vector<std::string> log;
    bool smooth = false, decalPass = true, shadowPass = true, accurateShadow = false;
    int depthOffset = 7;
    float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ITerrainVertBufExtn* vertexBuffer = nullptr;
    int releasedVertexBuffer = 0;
    bool SmoothOverlays() const override { return smooth; }
    bool DecalPassEnabled() const override { return decalPass; }
    bool ShadowPassEnabled() const override { return shadowPass; }
    bool ShadowProjectionAccurate() const override { return accurateShadow; }
    int DepthOffset() const override { return depthOffset; }
    void ResetModelViewTransform() override { log.push_back("resetview"); }
    void GetModelViewMatrix(float m[16]) const override { std::memcpy(m, view, sizeof(view)); }
    void ClearTexTransform(int stage) override { log.push_back("cleartex" + std::to_string(stage)); }
    void SetDepthOffset(int offset) override { log.push_back("depth" + std::to_string(offset)); }
    void SetTransparency(float a) override { log.push_back("alpha" + std::to_string(a)); }
    void ResetTransparency() override { log.push_back("resetalpha"); }
    void SetShade(const float* rgb) override { log.push_back("shade" + std::to_string(rgb[0])); }
    void SetSelfLitShade(const float* rgb) override { log.push_back("selflit" + std::to_string(rgb[0])); }
    void ResetShade() override { log.push_back("resetshade"); }
    void SetRenderState(const cSC4PackedRenderState& state) override {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "state%08x", state.value);
        log.push_back(buffer);
    }
    void SetTexture(int texture, int stage) override { log.push_back("tex" + std::to_string(texture) + "/" + std::to_string(stage)); }
    void SetTexFiltering(int minFilter, int magFilter, int stage) override {
        log.push_back("filter" + std::to_string(minFilter) + "/" + std::to_string(magFilter) + "/" + std::to_string(stage));
    }
    void SetTexWrapModes(int u, int v, int stage) override {
        log.push_back("wrap" + std::to_string(u) + "/" + std::to_string(v) + "/" + std::to_string(stage));
    }
    void SetTexTransform4(const cS3DMatrix4& m, int stage) override {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "xform%d/%.3f,%.3f", stage, m.m[0][0], m.m[3][1]);
        log.push_back(buffer);
    }
    void SetTexColor(const cS3DVector4& c) override {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "color%.2f/%.2f", c.x, c.w);
        log.push_back(buffer);
    }
    void SetTexCombiner(const gd::cGDCombiner& c, int stage) override {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "comb%08x/%d", c.word0, stage);
        log.push_back(buffer);
    }
    void EnableAlphaTestFlag(bool on) override { log.push_back(on ? "atest1" : "atest0"); }
    void SetAlphaFunc(int func, float ref) override {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "afunc%d/%.3f", func, ref);
        log.push_back(buffer);
    }
    void EnableTextureStateFlag(bool on, int stage) override {
        log.push_back(std::string("texstate") + (on ? "1" : "0") + "/" + std::to_string(stage));
    }
    void DriverSetState(int a, int b) override { log.push_back("driver" + std::to_string(a) + "/" + std::to_string(b)); }
    bool DriverStage7Begin() override { log.push_back("stage7begin"); return false; }
    void DriverStage7Apply() override { log.push_back("stage7apply"); }
    void DriverStage7End() override { log.push_back("stage7end"); }
    ITerrainVertBufExtn* GetVertexBufferExtension() override { return vertexBuffer; }
    void ReleaseVertexBufferExtension() override { ++releasedVertexBuffer; }
    bool Has(const char* what) const {
        for (const auto& entry : log)
            if (entry.rfind(what, 0) == 0) return true;
        return false;
    }
    int Count(const char* what) const {
        int n = 0;
        for (const auto& entry : log)
            if (entry.rfind(what, 0) == 0) ++n;
        return n;
    }
};

// ---------------------------------------------------------------------------------------------------------------
static void TestRecordsAndSlots() {
    // cOverlay defaults: flags bit 0, alpha 1, white, identity matrices, null textures.
    cOverlay overlay;
    CHECK(overlay.flags == kOverlayFlagEnabled);
    CHECK(overlay.alpha == 1.0f);
    CHECK(overlay.color[0] == 1.0f && overlay.color[1] == 1.0f && overlay.color[2] == 1.0f);
    CHECK(overlay.rect.left == 0 && overlay.texMatrix.m[0][0] == 1.0f && overlay.texMatrix.m[3][3] == 1.0f);
    CHECK(overlay.texture == nullptr && overlay.stringTexture == nullptr);
    CHECK(sizeof(cOverlay) == 0xc8 && sizeof(cDecal) == 0x30 && sizeof(cRingDecal) == 0x24);

    // Reference counting through copies and assignments (the cRZAutoRefCount semantics).
    auto* texture = new FakeTexture(7);
    {
        cOverlay a;
        a.texture = texture;
        texture->AddRef();
        CHECK(texture->refs == 2);
        cOverlay b(a);
        CHECK(texture->refs == 3);
        cOverlay c;
        c = b;
        CHECK(texture->refs == 4);
        c = *&c;   // self assignment keeps the reference
        CHECK(texture->refs == 4);
        // dropping the source releases all but the last
        c.texture = nullptr;
        texture->Release();
        CHECK(texture->refs == 3);
    }
    CHECK(texture->refs == 1);
    // the texture is freed with the others at the end of main

    // The slot pool: append, free-list push (tail chain), reuse in LIFO order, and the chain reset when the last slot
    // is reused. cDecal is the POD instantiation.
    cSC4SlotArray<cDecal> slots;
    cDecal decal;
    decal.freeLink = 0;
    decal.x = 1;
    const std::uint32_t a = slots.AddSlot(decal);
    decal.x = 2;
    const std::uint32_t b = slots.AddSlot(decal);
    CHECK(a == 0 && b == 1 && slots.size() == 2);
    CHECK(slots[a].freeLink == -1 && slots[b].freeLink == -1);
    // free both the way RemoveOverlay does: link a -> b, tail = b
    slots[a].freeLink = 0;
    slots.freeListHead_ = a;
    slots.freeListTail_ = a;
    slots[b].freeLink = 0;
    slots[a].freeLink = static_cast<std::int32_t>(b);
    slots.freeListTail_ = b;
    decal.x = 3;
    const std::uint32_t reused = slots.AddSlot(decal);   // pops a
    CHECK(reused == a && slots.size() == 2);
    CHECK(slots[reused].x == 3);
    const std::uint32_t reused2 = slots.AddSlot(decal);  // a == tail at this point -> chain reset, slot b reused
    CHECK(reused2 == b);
    CHECK(slots.freeListHead_ == 0xffffffffu && slots.freeListTail_ == 0xffffffffu);
    decal.x = 4;
    const std::uint32_t appended = slots.AddSlot(decal);   // chain empty again -> append
    CHECK(appended == 2 && slots.size() == 3);
    slots.Clear();
    CHECK(slots.size() == 0 && slots.freeListHead_ == 0xffffffffu);
}

static void TestLifeCycle(FakeHost& host) {
    // ---- construct / QueryInterface / AddRef / Release -----------------------------------------------------
    {
        auto* manager = new cSTEOverlayManager(&host);
        CHECK(manager->IsInited() == false);
        CHECK(manager->Quality() == 2);              // ctor default, replaced by Init
        CHECK(manager->SecondTextureStage() == 0);
        CHECK(manager->RenderStates().empty());
        CHECK(manager->RoughShadowCombiner().word0 == 0x01030001u);
        CHECK(manager->RoughShadowCombiner().word2 == 0u);
        CHECK(manager->DecalsDrawn() == 0 && manager->ShadowsDrawn() == 0);
        void* out = nullptr;
        CHECK(manager->QueryInterface(0x895d6544, &out) && out == manager);   // QueryInterface AddRefs
        CHECK(manager->AddRef() == 3);
        CHECK(manager->Release() == 2);
        out = reinterpret_cast<void*>(0x1);
        CHECK(!manager->QueryInterface(0x12345678, &out));   // unknown iid: cRZUnknown::QueryInterface fails
        CHECK(manager->Release() == 1);
        CHECK(manager->Release() == 0);                       // deletes at 0 - heap only, as the original
    }

    // ---- Init: terrain extents, the five render states, the combiners, the message registration --------------
    {
        cSTEOverlayManager manager(&host);
        manager.Init(nullptr, false, false);
        CHECK(manager.IsInited() && !manager.IsDynamic() && !manager.DrawsWaterPass());
        CHECK(manager.CellCountX() == 8 && manager.CellCountZ() == 8);
        CHECK(Near(manager.CellSize(), 16.0f) && Near(manager.InvCellSize(), 1.0f / 16.0f));
        CHECK(RectEq(manager.VisibleRect(), 0, 0, 8, 8));
        CHECK(manager.Quality() == host.quality && manager.SecondTextureStage() == host.secondStage);
        CHECK(manager.RenderStates().size() == 5);
        CHECK(manager.RenderStates()[0].value == 0x020a81b5u);
        CHECK(manager.RenderStates()[1].value == 0x020281b5u);
        CHECK(manager.RenderStates()[2].value == 0x020401b5u);
        CHECK(manager.RenderStates()[3].value == 0x020a8235u);
        CHECK(manager.RenderStates()[4].value == 0x010a90bfu);   // the shadow pass: depth write + alpha test
        CHECK(manager.RoughShadowCombiner().word0 == 0x01030200u && manager.RoughShadowCombiner().word1 == 0x01020001u);
        CHECK(manager.ShadowCombiner0().word0 == 0x01030100u && manager.ShadowCombiner0().word1 == 0x01010001u);
        CHECK(manager.ShadowCombiner1().word0 == 0x01030000u && manager.ShadowCombiner1().word1 == 0x01030000u);
        CHECK(manager.ShadowTexture() != nullptr);                     // factory key 0xaa592b77
        CHECK(host.factory.byInstance >= 1 && host.factory.lastInstance == 0xaa592b77u);
        CHECK(!host.messages.added.empty());
        CHECK(host.messages.added.back().first == static_cast<cIGZMessageTarget2*>(&manager));
        CHECK(host.messages.added.back().second == 0xc9f775bbu);
        CHECK(manager.SpatialIndex().size() == 64);                    // cells x * cells z for a static manager

        // A render-properties message refreshes the three cached values (DoMessage slot 0xf0).
        host.quality = 5;
        host.secondStage = 4;
        host.spatialIndexValid = false;
        FakeMessage message(0xc9f775bb);
        CHECK(manager.DoMessage(&message));
        CHECK(manager.Quality() == 5 && manager.SecondTextureStage() == 4);
        FakeMessage unrelated(0x11111111);
        CHECK(manager.DoMessage(&unrelated));                          // always returns true
        CHECK(manager.Quality() == 5);

        // Update always reports success; Pick is the 8-byte stub that returns 0.
        FakeDC dc;
        CHECK(manager.Update(dc));
        CHECK(!manager.Pick(cS3DVector3{}, cS3DVector3{}, &dc, nullptr));

        // Shutdown unregisters and drops the factory; the overlays are gone.
        manager.Shutdown();
        CHECK(!manager.IsInited());
        CHECK(!host.messages.removed.empty() && host.messages.removed.back().second == 0xc9f775bbu);
        CHECK(manager.Overlays().size() == 0 && manager.Decals().size() == 0);
    }

    // A dynamic manager gets no spatial index; Init leaves quality/secondStage from the host.
    {
        cSTEOverlayManager manager(&host);
        host.quality = 1;
        manager.Init(nullptr, true, true);
        CHECK(manager.IsDynamic() && manager.DrawsWaterPass());
        CHECK(manager.Quality() == 1);
        CHECK(manager.SpatialIndex().empty());
        manager.Shutdown();
    }
}

static void TestDecals(FakeHost& host) {
    host.quality = 2;
    host.factory.sizeW = 64;
    host.factory.sizeH = 32;
    cSTEOverlayManager manager(&host);
    manager.Init(nullptr, false, false);

    // ---- AddDecal(texture) -----------------------------------------------------------------------------------
    auto* texture = new FakeTexture(11);   // refs = 1: the caller's reference (the game's factory hands out a +1 binding)
    const std::uint32_t id = manager.AddDecal(texture, cS3DVector2{16.0f, 32.0f}, 16.0f, 0.0f);
    CHECK(id == 0x80000000u);                                  // decal slot 0 with the "decal" bit
    CHECK(manager.Decals().size() == 1 && manager.Overlays().size() == 1);
    const cDecal& decal = manager.Decals()[0];
    CHECK(Near(decal.x, 16.0f) && Near(decal.z, 32.0f));
    CHECK(Near(decal.width, 16.0f) && Near(decal.turns, 0.0f));
    CHECK(Near(decal.aspect, 0.5f));                            // 32 / 64
    CHECK(decal.overlay == 0);
    CHECK(decal.aspectAdjust == 1.0f && decal.uRepeat == 1.0f && decal.vRepeat == 1.0f);
    const cOverlay& overlay = manager.Overlays()[0];
    CHECK(overlay.texture == texture && overlay.stringTexture == nullptr);
    CHECK(overlay.flags == (kOverlayFlagEnabled | kOverlayFlagDecal | kOverlayFlagSmoothDecal));
    CHECK(texture->refs == 2);                                  // caller + the overlay
    // 16 wide, 16 * 0.5 * 2 tall, centred on (16, 32) in 16-unit cells -> x 0.5..1.5, z 1.75..2.25
    CHECK(RectEq(overlay.rect, 0, 1, 2, 3));
    CHECK(Near(overlay.texMatrix.m[0][0], 1.0f / 16.0f));
    CHECK(Near(overlay.texMatrix.m[2][1], 2.0f / 16.0f));        // 1 / width / vScale
    // The transform maps the decal quad (x 8..24, z 28..36) onto u / v 0..1:
    //   u = x / 16 - 0.5, v = z * 0.125 - 3.5
    CHECK(Near(overlay.texMatrix.m[3][0], -0.5f));
    CHECK(Near(overlay.texMatrix.m[3][1], -3.5f));
    CHECK(manager.OverlayIsEnabled(id));   // a decal id resolves to its overlay slot
    CHECK(manager.OverlayIsEnabled(0));
    CHECK(Near(manager.OverlayAlpha(0), 1.0f));
    cS3DVector3 rgb;
    manager.OverlayColor(0, &rgb);
    CHECK(rgb.x == 1.0f && rgb.y == 1.0f && rgb.z == 1.0f);
    CHECK(manager.OverlayDrawMode(0) == kOverlayDrawMode0);
    CHECK(!manager.OverlayFlags(0));
    CHECK(manager.OverlayAlpha(0xffffffffu) == 0.0f);
    manager.OverlayColor(0xffffffffu, &rgb);
    CHECK(rgb.x == 0.0f);
    // Out-of-range ids resolve to "no overlay" instead of indexing the slot array out of bounds.
    CHECK(!manager.OverlayIsEnabled(9999u));
    CHECK(manager.OverlayAlpha(9999u) == 0.0f);
    CHECK(manager.OverlayDrawMode(9999u) == kOverlayDrawMode0);
    CHECK(!manager.OverlayFlags(9999u));
    manager.OverlayColor(9999u, &rgb);
    CHECK(rgb.x == 0.0f);
    manager.SetOverlayEnabled(9999u, true);
    manager.SetOverlayAlpha(9999u, 0.5f);
    manager.SetOverlayColor(9999u, cS3DVector3{1, 2, 3});
    manager.SetOverlayDrawMode(9999u, kOverlayDrawMode1);
    manager.SetOverlayFlags(9999u, 1);
    manager.SetHeightMapParams(9999u, 1.0f, 0.0f);
    manager.RedisplayStaticOverlay(9999u);   // must not read the slot array

    // ---- the setters -----------------------------------------------------------------------------------------
    manager.SetOverlayColor(0, cS3DVector3{0.25f, 0.5f, 0.75f});
    CHECK(manager.Overlays()[0].color[0] == 0.25f && manager.Overlays()[0].color[2] == 0.75f);
    manager.SetOverlayDrawMode(0, kOverlayDrawMode2);
    CHECK(manager.OverlayDrawMode(0) == kOverlayDrawMode2);
    manager.SetOverlayFlags(0, 1);
    CHECK(manager.OverlayFlags(0) && (manager.Overlays()[0].flags & kOverlayFlagSelfLit));
    manager.SetOverlayFlags(0, 0);                                // only bit 0 does anything
    CHECK(manager.OverlayFlags(0));
    manager.SetOverlayEnabled(0, false);
    CHECK(!manager.OverlayIsEnabled(0));
    manager.SetOverlayEnabled(0, true);
    CHECK(manager.OverlayIsEnabled(0));

    // Alpha quirk: with a static manager only 0 / 1 / a jump above 0.05 redisplays.
    const int before = host.redisplayCalls;
    manager.SetOverlayAlpha(0, 0.5f);
    CHECK(Near(manager.OverlayAlpha(0), 0.5f) && host.redisplayCalls == before + 1);
    manager.SetOverlayAlpha(0, 0.51f);                            // delta 0.01: neither stored nor redrawn
    CHECK(Near(manager.OverlayAlpha(0), 0.5f) && host.redisplayCalls == before + 1);
    manager.SetOverlayAlpha(0, 1.0f);                             // 1.0 always redraws
    CHECK(host.redisplayCalls == before + 2);

    // Height map parameters: inv scale on U, -inv scale on V, offset in the V translation.
    manager.SetHeightMapParams(0, 4.0f, 2.0f);
    const cS3DMatrix4& heightMatrix = manager.Overlays()[0].texMatrix;
    CHECK(Near(heightMatrix.m[0][0], 0.25f) && Near(heightMatrix.m[1][1], -0.25f));
    CHECK(Near(heightMatrix.m[2][0], 0.25f) && Near(heightMatrix.m[2][2], 1.0f));
    CHECK(Near(heightMatrix.m[3][1], 1.5f));
    CHECK((manager.Overlays()[0].flags & kOverlayFlagHeightMap) != 0);

    // ---- MoveDecal / DecalInfo / UpdateDecalInfo -------------------------------------------------------------
    const int rectBefore = host.redisplayCalls;
    manager.MoveDecal(id, cS3DVector2{16.00001f, 32.0f});           // below the 1e-4 threshold: ignored
    CHECK(Near(manager.Decals()[0].x, 16.0f) && host.redisplayCalls == rectBefore);
    manager.MoveDecal(id, cS3DVector2{20.0f, 32.0f});
    CHECK(Near(manager.Decals()[0].x, 20.0f));
    manager.MoveDecal(1, cS3DVector2{1.0f, 1.0f});                  // a non-decal id is ignored
    manager.MoveDecal(0, cS3DVector2{99.0f, 99.0f});                // an overlay index is not a decal id
    manager.MoveDecal(static_cast<std::uint32_t>(-1), cS3DVector2{1.0f, 1.0f});
    cDecalInfo info;
    manager.DecalInfo(id, &info);
    CHECK(Near(info.x, 20.0f) && Near(info.width, 16.0f) && Near(info.aspectAdjust, 1.0f));
    cDecalInfo update;
    update.x = 40.0f;
    update.z = 48.0f;
    update.width = 32.0f;
    update.turns = 0.25f;
    update.aspectAdjust = 2.0f;
    update.uRepeat = 3.0f;
    update.vRepeat = 4.0f;
    update.uvOffset = 0.125f;
    update.userValue = 6.0f;
    manager.UpdateDecalInfo(id, update);
    CHECK(Near(manager.Decals()[0].x, 40.0f) && Near(manager.Decals()[0].width, 32.0f));
    CHECK(manager.Decals()[0].userValue == 6.0f);
    manager.DecalInfo(id, &info);
    CHECK(Near(info.uvOffset, 0.125f) && Near(info.userValue, 6.0f) && Near(info.vRepeat, 4.0f));
    manager.DecalInfo(0xffffffffu, &info);                          // no-op
    manager.DecalInfo(id, nullptr);                                 // no-op

    // A rotated decal sets flag 2 and grows its bounds to the rotated AABB.
    manager.UpdateDecalInfo(id, update);
    CHECK((manager.Overlays()[0].flags & kOverlayFlagTransformed) != 0);
    const cRZRect rotated = manager.Overlays()[0].rect;
    CHECK(rotated.right > rotated.left && rotated.bottom > rotated.top);
    cDecalInfo plain = update;
    plain.turns = 0.0f;
    plain.uRepeat = 1.0f;
    plain.vRepeat = 1.0f;
    manager.UpdateDecalInfo(id, plain);
    CHECK((manager.Overlays()[0].flags & kOverlayFlagTransformed) == 0);
    // A turned decal still maps its own square onto the texture (Mac i386 0x004d97a0: the turn about the center comes
    // first, then the texture mapping): the center samples (0.5, 0.5), the four corners of the turned square stay in 0..1.
    {
        cDecalInfo turned = plain;
        turned.turns = 0.125f;   // 45 degrees
        manager.UpdateDecalInfo(id, turned);
        const cS3DMatrix4& m = manager.Overlays()[0].texMatrix;
        const auto uv = [&m](float x, float z, float& u, float& v) {   // row vector (x, 0, z, 1) * m
            u = x * m.m[0][0] + z * m.m[2][0] + m.m[3][0];
            v = x * m.m[0][1] + z * m.m[2][1] + m.m[3][1];
        };
        float u = 0, v = 0;
        uv(turned.x, turned.z, u, v);
        CHECK(Near(u, 0.5f + turned.uvOffset) && Near(v, 0.5f + turned.uvOffset));
        const float half = turned.width * 0.5f * 1.41421356f;   // the turned square's corners lie on the axes
        const float offsets[4][2] = {{half, 0}, {-half, 0}, {0, half}, {0, -half}};
        for (const auto& d : offsets) {
            uv(turned.x + d[0], turned.z + d[1], u, v);
            CHECK(u > -0.01f + turned.uvOffset && u < 1.01f + turned.uvOffset && v > -0.01f + turned.uvOffset &&
                  v < 1.01f + turned.uvOffset);
        }
        manager.UpdateDecalInfo(id, plain);
    }

    // ---- a text decal uses the cached string texture's size --------------------------------------------------
    auto* text = new FakeStringTexture(21, 128, 64);   // refs = 1: the caller's reference
    const std::uint32_t textId = manager.AddDecal(text, cS3DVector2{64.0f, 64.0f}, 32.0f, 0.0f);
    CHECK((textId & 0x80000000u) != 0);
    const cDecal& textDecal = manager.Decals()[1];
    CHECK(Near(textDecal.aspect, 0.5f) && textDecal.overlay >= 0);
    CHECK(manager.Overlays()[static_cast<std::size_t>(textDecal.overlay)].stringTexture == text);
    CHECK(text->refs == 2);
    // 32 wide, 32 tall, centred on (64, 64): x / z 48..80 -> cells 3..5 (the decal overflows the 8-cell city, so
    // the rect is clipped to the visible terrain)
    CHECK(RectEq(manager.Overlays()[static_cast<std::size_t>(textDecal.overlay)].rect, 3, 3, 5, 5));

    // The empty overloads fail with 0xffffffff.
    CHECK(manager.AddDecal(static_cast<const IOverlayTexture*>(nullptr), cS3DVector2{}, 16.0f, 0.0f) == 0xffffffffu);
    CHECK(manager.AddDecal(static_cast<const IOverlayStringTexture*>(nullptr), cS3DVector2{}, 16.0f, 0.0f) == 0xffffffffu);

    // ---- AddDecal(instance) and AddDecal(key) go through the factory -----------------------------------------
    const std::uint32_t instanceId = manager.AddDecal(0x1234u, cS3DVector2{8.0f, 8.0f}, 16.0f, 0.0f);
    CHECK((instanceId & 0x80000000u) != 0);
    CHECK(host.factory.lastInstance == 0x1234u);
    cGZPersistResourceKey key{0x1a2b3c4d, 0x5e6f7081, 0x92a3b4c5};
    const std::uint32_t keyId = manager.AddDecal(key, cS3DVector2{8.0f, 8.0f}, 16.0f, 0.0f);
    CHECK((keyId & 0x80000000u) != 0);
    CHECK(host.factory.lastKey == key);
    // the factory's temporary reference is released: the overlay keeps exactly one
    CHECK(host.factory.made.back()->refs == 1);

    // ---- RemoveOverlay: decal + overlay go back on their free lists ------------------------------------------
    (void)instanceId;
    (void)keyId;
    const std::size_t overlayCount = manager.Overlays().size();
    const std::size_t decalCount = manager.Decals().size();
    manager.RemoveOverlay(id);
    CHECK(manager.Decals()[0].freeLink == 0);
    CHECK(manager.Overlays()[0].freeLink == 0);
    // The freed slot keeps its texture reference until the slot is reused (cOverlay::operator= releases it) or the
    // manager is cleared - the original only rewrites the free-list link here.
    CHECK(texture->refs == 2);
    // the freed slots are reused by the next decal
    const std::uint32_t reAdded = manager.AddDecal(texture, cS3DVector2{16.0f, 16.0f}, 16.0f, 0.0f);
    CHECK(reAdded == 0x80000000u);
    CHECK(manager.Overlays().size() == overlayCount && manager.Decals().size() == decalCount);
    CHECK(manager.Overlays()[0].texture == texture && manager.Overlays()[0].freeLink == -1);
    manager.RemoveOverlay(0xffffffffu);                           // no-op
    texture->Release();
    text->Release();
}

static void TestRingAndShadowDecals(FakeHost& host) {
    host.quality = 2;
    host.factory.sizeW = 64;
    host.factory.sizeH = 64;
    cSTEOverlayManager manager(&host);
    manager.Init(nullptr, false, false);

    // ---- AddRingDecal: eight overlays with flag 0x10 and one ring record ------------------------------------
    const std::uint32_t id = manager.AddRingDecal(0x99u, cS3DVector2{64.0f, 64.0f}, 128.0f, 0.0f);
    CHECK((id & 0x80000000u) != 0);
    CHECK(manager.Overlays().size() == 8 && manager.RingDecals().size() == 1 && manager.Decals().size() == 1);
    const cRingDecal& ring = manager.RingDecals()[0];
    for (int i = 0; i < 8; ++i) {
        CHECK(ring.overlay[i] == static_cast<std::uint32_t>(i));
        CHECK(manager.Overlays()[static_cast<std::size_t>(i)].flags == (kOverlayFlagEnabled | kOverlayFlagDecal));
        CHECK(manager.Overlays()[static_cast<std::size_t>(i)].texture != nullptr);
        // a 128-unit decal centred on cell (4, 4): the ring spans the whole 8-cell city
        const cRZRect& tile = manager.Overlays()[static_cast<std::size_t>(i)].rect;
        CHECK(tile.right > tile.left && tile.bottom > tile.top);
        CHECK(tile.left >= 0 && tile.top >= 0 && tile.right <= 8 && tile.bottom <= 8);
    }
    CHECK(manager.Decals()[0].overlay == static_cast<std::int32_t>(id));
    // the eight tiles cover different parts of the ring
    CHECK(!RectEq(manager.Overlays()[0].rect, manager.Overlays()[1].rect));

    // A ring decal id forwards the setters to all eight overlays.
    manager.SetOverlayEnabled(id, false);
    for (int i = 0; i < 8; ++i) CHECK(!manager.OverlayIsEnabled(ring.overlay[i]));
    manager.SetOverlayEnabled(id, true);
    CHECK(manager.OverlayIsEnabled(id));                            // resolves to the first tile
    manager.SetOverlayAlpha(id, 0.25f);
    for (int i = 0; i < 8; ++i) CHECK(Near(manager.OverlayAlpha(ring.overlay[i]), 0.25f));
    manager.SetOverlayColor(id, cS3DVector3{0.1f, 0.2f, 0.3f});
    CHECK(Near(manager.OverlayAlpha(ring.overlay[7]), 0.25f));
    CHECK(manager.Overlays()[ring.overlay[7]].color[2] == 0.3f);
    manager.SetOverlayDrawMode(id, kOverlayDrawMode3);
    CHECK(manager.OverlayDrawMode(ring.overlay[3]) == kOverlayDrawMode3);
    manager.SetOverlayFlags(id, 1);
    for (int i = 0; i < 8; ++i) CHECK(manager.OverlayFlags(ring.overlay[i]));

    // Removing the ring removes all eight overlays and both slots.
    const std::size_t overlayCount = manager.Overlays().size();
    manager.RemoveOverlay(id);
    for (int i = 0; i < 8; ++i) CHECK(manager.Overlays()[static_cast<std::size_t>(i)].freeLink != -1);
    CHECK(manager.Overlays()[0].freeLink == 1);                   // the chain head links to the next free slot
    CHECK(manager.Overlays()[7].freeLink == 0);                   // the tail links to 0
    CHECK(manager.Overlays().freeListHead_ == 0 && manager.Overlays().freeListTail_ == 7);
    CHECK(manager.RingDecals()[0].freeLink == 0 && manager.Decals()[0].freeLink == 0);
    // the ring record is already free: a second removal returns before touching the overlays again
    manager.RemoveOverlay(id);
    CHECK(manager.RingDecals()[0].freeLink == 0);
    CHECK(manager.Overlays().size() == overlayCount);

    // ---- AddShadowDecal --------------------------------------------------------------------------------------
    // quality 0 refuses every shadow decal
    {
        cSTEOverlayManager lowQuality(&host);
        host.quality = 0;
        lowQuality.Init(nullptr, false, false);
        CHECK(lowQuality.AddShadowDecal(0x55u, cS3DVector2{}, 16.0f, 0.0f, false) == 0xffffffffu);
        CHECK(lowQuality.AddShadowDecal(cGZPersistResourceKey{1, 2, 3}, cS3DVector2{}, 16.0f, 0.0f, false) ==
              0xffffffffu);
        lowQuality.Shutdown();
    }
    host.quality = 2;
    const std::uint32_t shadowId = manager.AddShadowDecal(0x55u, cS3DVector2{64.0f, 64.0f}, 16.0f, 0.0f, false);
    CHECK((shadowId & 0x80000000u) != 0);
    CHECK(host.factory.lastKey.type == 0x7ab50e44u);
    CHECK(host.factory.lastKey.group == 0x1234u);                   // factory slot 0x58
    CHECK(host.factory.lastKey.instance == 0x55u);
    const std::uint32_t shadowSlot = static_cast<std::uint32_t>(manager.Decals()[0].overlay);
    CHECK((manager.Overlays()[shadowSlot].flags & kOverlayFlagShadow) != 0);
    CHECK((manager.Overlays()[shadowSlot].flags & 9u) == 9u);
    // The shadow matrix is the swizzle times the texture transform, so it differs from it.
    CHECK(!Near(manager.Overlays()[shadowSlot].shadowMatrix.m[0][0], manager.Overlays()[shadowSlot].texMatrix.m[0][0]) ||
          !Near(manager.Overlays()[shadowSlot].shadowMatrix.m[3][0],
                manager.Overlays()[shadowSlot].texMatrix.m[3][0]));

    // A mirrored shadow decal reflects V: m[r][1] = m[r][3] - m[r][1].
    host.factory.sizeW = 64;
    host.factory.sizeH = 32;
    const std::uint32_t mirrorId = manager.AddShadowDecal(0x66u, cS3DVector2{64.0f, 64.0f}, 16.0f, 0.0f, true);
    const std::uint32_t mirrorSlot = static_cast<std::uint32_t>(manager.Decals()[1].overlay);
    CHECK((mirrorId & 0x80000000u) != 0);
    const cOverlay& mirrored = manager.Overlays()[mirrorSlot];
    CHECK(Near(mirrored.texMatrix.m[2][1], -0.125f));               // v scale 1/16/0.5 negated
    CHECK(Near(mirrored.texMatrix.m[3][1], 8.5f));                  // 1 - (-7.5)
    manager.RemoveOverlay(shadowId);
    manager.RemoveOverlay(mirrorId);

    // ---- AddHeightMap ----------------------------------------------------------------------------------------
    const std::uint32_t heightId = manager.AddHeightMap(0x77u);
    CHECK(heightId == manager.Decals().size() - 1 || heightId < manager.Overlays().size());
    CHECK(host.factory.lastInstance == 0x77u);
    const cOverlay& height = manager.Overlays()[heightId];
    CHECK(RectEq(height.rect, 0, 0, 8, 8));
    CHECK(Near(height.texMatrix.m[1][1], 0.25f));                   // 2 / cells x
    // identity with column 0 cleared: v follows the altitude only, never the z axis (that drew stripes, not contour lines)
    CHECK(Near(height.texMatrix.m[2][2], 1.0f) && Near(height.texMatrix.m[3][3], 1.0f));
    CHECK(Near(height.texMatrix.m[2][1], 0.0f) && Near(height.texMatrix.m[0][1], 0.0f) && Near(height.texMatrix.m[3][1], 0.0f));
    for (int r = 0; r < 4; ++r) CHECK(Near(height.texMatrix.m[r][0], 0.0f));
    CHECK((height.flags & 0x12u) == 0x12u);          // the bitset 0x12, not the 0x20 height-map bit
    CHECK((height.flags & kOverlayFlagHeightMap) == 0);
    manager.SetHeightMapParams(heightId, 2.0f, 1.0f);
    CHECK((manager.Overlays()[heightId].flags & kOverlayFlagHeightMap) != 0);
    manager.RemoveOverlay(heightId);
}

static void TestAddShadow(FakeHost& host) {
    host.quality = 5;   // quality is cached by Init
    cSTEOverlayManager manager(&host);
    manager.Init(nullptr, false, false);
    cS3DVector3 points[3] = {{0, 0, 0}, {16, 0, 0}, {0, 0, 16}};
    cS3DVector2 uvs[3] = {{0, 0}, {1, 0}, {0, 1}};
    cS3DBoundingBox box{{0, 0, 0}, {16, 4, 16}};
    cS3DTransform transform;
    IOverlayTexture* texture = host.factory.GetTexture(0x321u);

    // A failed projection is refused.
    host.shadow.projectOk = false;
    CHECK(manager.AddShadow(texture, 3, points, uvs, box, transform) == 0xffffffffu);
    host.shadow.projectOk = true;
    CHECK(host.shadow.projectCalls == 1);

    // quality < 5 takes the approximate path: the light direction plus CalcShadowTerrainRect.
    host.quality = 2;
    FakeMessage refresh(0xc9f775bb);
    CHECK(manager.DoMessage(&refresh));
    CHECK(manager.Quality() == 2);
    host.shadow.direction = cS3DVector3{1.0f, 0.0f, 1.0f};   // pad to the right and to the bottom
    const std::uint32_t approximate = manager.AddShadow(texture, 3, points, uvs, box, transform);
    CHECK(approximate != 0xffffffffu);
    CHECK(host.shadow.directionCalls == 1 && host.shadow.calcCalls == 0);
    // box 0..16 in x/z, 0..4 high, pad = ceil(4/16) = 1: shrunk by 0.07 cell then padded on +x / +z
    CHECK(RectEq(manager.Overlays()[approximate].rect, 0, 0, 2, 2));

    // quality 5 takes the exact path (the shadow service's CalcShadowRect) and fails when it fails.
    host.quality = 5;
    CHECK(manager.DoMessage(&refresh));
    CHECK(manager.Quality() == 5);
    host.shadow.calcOk = false;
    CHECK(manager.AddShadow(texture, 3, points, uvs, box, transform) == 0xffffffffu);
    host.shadow.calcOk = true;
    const std::uint32_t exact = manager.AddShadow(texture, 3, points, uvs, box, transform);
    CHECK(exact != 0xffffffffu);
    CHECK(host.shadow.calcCalls == 2);
    CHECK(RectEq(manager.Overlays()[exact].rect, 2, 3, 4, 5));     // the service's rect, clipped to 0..8
    CHECK((manager.Overlays()[exact].flags & 8u) != 0);

    // The UV transform maps the projected UV bounds onto the [0.25, 0.75] square.
    const cS3DMatrix4& shadowMatrix = manager.Overlays()[exact].shadowMatrix;
    // The UV matrix of AddShadow keeps the un-transposed convention (m[1][1] / m[3][1]) - the original's own
    // convention for this matrix differs from the decal texture transform.
    CHECK(Near(shadowMatrix.m[0][0], 0.5f) && Near(shadowMatrix.m[3][0], 0.25f));
    CHECK(Near(shadowMatrix.m[1][1], 0.5f) && Near(shadowMatrix.m[3][1], 0.25f));
    texture->Release();
}

static void TestSpatialIndexAndStaticRects(FakeHost& host) {
    host.quality = 2;
    cSTEOverlayManager manager(&host);
    manager.Init(nullptr, false, false);

    // Two overlays at different cells; one of them is added twice (SDSAdd allows duplicates).
    const IOverlayTexture* texture = host.factory.GetTexture(0x11u);
    const std::uint32_t a = manager.AddOverlay(texture, cRZRect{0, 0, 2, 2}, cS3DMatrix4{}, 0);
    const std::uint32_t b = manager.AddOverlay(texture, cRZRect{6, 6, 8, 8}, cS3DMatrix4{}, 0);
    CHECK(a == 0 && b == 1);
    CHECK(host.redisplayCalls > 0);
    CHECK(RectEq(host.lastRedisplay, 6, 6, 8, 8));                  // the last AddOverlay's rect
    manager.SDSAdd(b, manager.Overlays()[b].rect);
    const auto& buckets = manager.SpatialIndex();
    CHECK(!buckets[0].empty() && !buckets[6 * 8 + 6].empty());
    CHECK(BucketSize(buckets[6 * 8 + 6]) == 2);                          // duplicate kept, de-duplicated at draw time

    // The culling info covers rows 0..8 and all columns: both overlays are found once each.
    std::vector<std::uint32_t> found;
    manager.SDSFindOverlaysToDraw(found);
    CHECK(found.size() == 2);
    CHECK(manager.FrameStamp() == 1);
    CHECK(manager.Overlays()[a].frameStamp == 1 && manager.Overlays()[b].frameStamp == 1);
    manager.SDSFindOverlaysToDraw(found);
    CHECK(manager.FrameStamp() == 2);                               // the next pass re-finds them

    // A narrow culling window misses the second overlay.
    host.colEnd.assign(16, 4);
    manager.SDSFindOverlaysToDraw(found);
    CHECK(found.size() == 1 && found[0] == a);
    host.colEnd.assign(16, 8);

    // SDSRemove unlinks one copy; the duplicate stays.
    manager.SDSRemove(b, manager.Overlays()[b].rect);               // both copies go
    CHECK(BucketSize(buckets[6 * 8 + 6]) == 0);

    // UpdateStaticOverlayRect clips to the visible rect and only rebuilds the index on a change.
    const int redisplayBefore = host.redisplayCalls;
    manager.UpdateStaticOverlayRect(a, cRZRect{-2, -2, 4, 4});
    CHECK(RectEq(manager.Overlays()[a].rect, 0, 0, 4, 4));
    CHECK(host.redisplayCalls >= redisplayBefore + 2);              // once before, once after the change
    const int redisplayAfter = host.redisplayCalls;
    manager.UpdateStaticOverlayRect(a, cRZRect{-2, -2, 4, 4});      // unchanged: no SDS rebuild, one redisplay
    CHECK(host.redisplayCalls == redisplayAfter + 1);
    // A rect entirely outside the visible area becomes empty.
    manager.UpdateStaticOverlayRect(a, cRZRect{20, 20, 30, 30});
    CHECK(RectEq(manager.Overlays()[a].rect, 0, 0, 0, 0));
    // Empty rects never reach the terrain.
    const int emptyRedisplay = host.redisplayCalls;
    manager.RedisplayStaticOverlay(a);
    CHECK(host.redisplayCalls == emptyRedisplay);
    std::vector<std::uint32_t> none;
    manager.SDSFindOverlaysToDraw(none);

    // RedisplayStaticOverlay passes the water flag through.
    manager.SetOverlayAlpha(a, 1.0f);   // forces a redisplay of a non-empty rect? a is empty, so pick b
    manager.UpdateStaticOverlayRect(b, cRZRect{1, 1, 3, 3});
    CHECK(host.lastRedisplayWater == 0);
    manager.Shutdown();
}

static void TestDrawPass(FakeHost& host) {
    host.quality = 2;
    host.secondStage = 0;
    host.factory.sizeW = 64;
    host.factory.sizeH = 32;
    cSTEOverlayManager manager(&host);
    manager.Init(nullptr, false, false);

    auto* texture = new FakeTexture(31);   // refs = 1: the caller's reference
    const std::uint32_t decalId = manager.AddDecal(texture, cS3DVector2{32.0f, 32.0f}, 16.0f, 0.0f);
    CHECK(decalId == 0x80000000u);
    const std::uint32_t slot = static_cast<std::uint32_t>(manager.Decals()[0].overlay);
    manager.SetOverlayColor(slot, cS3DVector3{0.5f, 0.5f, 0.5f});

    // ---- the empty draw pass ---------------------------------------------------------------------------------
    FakeDC dc;
    manager.Update(dc);
    CHECK(dc.Has("resetview") && dc.Has("depth7") && dc.Has("cleartex0"));
    // One decal state and one shadow state: the shadow pass always sets its render state, even with no shadow
    // overlay in the list.
    CHECK(dc.Count("state") == 2);
    CHECK(dc.Has("state020a81b5") && dc.Has("state010a90bf"));

    // ---- the decal pass: a 0x91 decal draws under both masks -----------------------------------------------
    dc.log.clear();
    manager.Update(dc);
    CHECK(dc.Count("state020a81b5") == 1);                            // flags 0x91 XOR 0x91 == 0: always drawn
    CHECK(dc.Has("wrap2/2/0") && dc.Has("filter1/7/0"));
    // With the smooth flag set the decal pass runs: the recorded call order is transparency, shade, driver
    // state, render state, texture, filtering, wrap, transform, draw.
    {
        struct SmoothDC : FakeDC {
            SmoothDC() { smooth = true; }
        } smoothDc;
        manager.Update(smoothDc);
        CHECK(smoothDc.Count("state020a81b5") == 1);
        CHECK(smoothDc.Has("alpha1.000000"));
        CHECK(smoothDc.Has("shade0.500000"));
        CHECK(smoothDc.Has("driver1/0"));
        CHECK(smoothDc.Has("tex31/0"));
        CHECK(smoothDc.Has("filter1/7/0"));
        CHECK(smoothDc.Has("wrap2/2/0"));
        CHECK(smoothDc.Has("xform0/0.062,-3.500"));
        CHECK(smoothDc.Has("resetalpha") && smoothDc.Has("resetshade") && smoothDc.Has("driver1/1"));
        CHECK(smoothDc.Has("cleartex0") && smoothDc.Has("depth7"));
        CHECK(manager.DecalsDrawn() == 1);
        CHECK(!host.draws.empty());
        CHECK(host.draws.back() == "terrain:draw:1,1,4,4");            // rect {1,1,3,3} + 1 on right/bottom
    }

    // ---- the shadow pass: the shadow decal is drawn by both shadow paths ------------------------------------
    const std::uint32_t shadowId = manager.AddShadowDecal(0x44u, cS3DVector2{32.0f, 32.0f}, 16.0f, 0.0f, false);
    CHECK((shadowId & 0x80000000u) != 0);
    const std::uint32_t shadowSlot = static_cast<std::uint32_t>(manager.Decals()[1].overlay);
    manager.SetOverlayDrawMode(shadowSlot, kOverlayDrawModeShadow);
    host.shadow.color = cS3DVector4{0.25f, 0.5f, 0.75f, 1.0f};
    host.shadow.scale = 0.5f;
    host.shadow.intensity = 2.0f;
    host.quality = 2;
    {
        FakeDC rough;
        manager.Update(rough);
        CHECK(rough.Has("stage7begin") && rough.Has("stage7apply") && rough.Has("stage7end"));
        CHECK(rough.Has("color0.25"));
        CHECK(rough.Has("atest1") && rough.Has("afunc6/1.000"));      // intensity 2 * scale 0.5
        CHECK(rough.Has("comb01030200/0"));
        CHECK(rough.Has("depth3"));
        CHECK(manager.ShadowsDrawn() == 1);
    }
    // quality >= 5 and the accurate-projection flag pick DrawShadows: two texture stages and the falloff path.
    host.quality = 5;
    {
        FakeMessage refresh(0xc9f775bb);
        CHECK(manager.DoMessage(&refresh));
        struct AccurateDC : FakeDC {
            AccurateDC() { accurateShadow = true; }
        } accurate;
        manager.Update(accurate);
        CHECK(accurate.Has("filter1/1/0"));
        CHECK(accurate.Has("comb01030100/0"));
        CHECK(accurate.Has("texstate1/1"));
        CHECK(accurate.Has("wrap2/2/1") && accurate.Has("wrap3/3/0"));   // the mask stage gets 2/2, the model texture 3/3
        CHECK(accurate.Has("xform1/0.031,-3.500"));                   // inverseView * shadowMatrix
        CHECK(accurate.Has("afunc6/1.000"));                          // scale * falloff * intensity
        bool fadedAlpha = false;                                      // the colour's alpha carries scale * falloff (0.5)
        for (const std::string& entry : accurate.log) fadedAlpha |= entry.rfind("color", 0) == 0 && entry.size() > 5 && entry.substr(entry.find('/')) == "/0.50";
        CHECK(fadedAlpha);
        CHECK(host.shadow.falloffCalls == 1);
        CHECK(manager.ShadowsDrawn() == 1);
    }
    // A falloff below 0.05 skips the overlay entirely.
    host.shadow.falloff = 0.01f;
    {
        struct AccurateDC : FakeDC {
            AccurateDC() { accurateShadow = true; }
        } accurate;
        manager.Update(accurate);
        CHECK(manager.ShadowsDrawn() == 0);
    }
    host.shadow.falloff = 1.0f;
    // intensity <= 0 turns alpha testing off.
    host.shadow.intensity = 0.0f;
    {
        FakeDC rough;
        manager.Update(rough);
        CHECK(rough.Has("atest0") && !rough.Has("afunc"));
    }
    host.shadow.intensity = 2.0f;

    // ---- the water pass: DrawWaterSurfaceSubset and the mixed-mesh choice -----------------------------------
    {
        cSTEOverlayManager waterManager(&host);
        waterManager.Init(nullptr, false, true);  // water pass
        waterManager.AddShadowDecal(0x45u, cS3DVector2{32.0f, 32.0f}, 16.0f, 0.0f, false);
        std::uint32_t waterId = waterManager.Decals()[0].overlay;
        waterManager.SetOverlayDrawMode(static_cast<std::uint32_t>(waterId), kOverlayDrawModeShadow);
        host.draws.clear();
        host.quality = 2;
        host.rectCells = 100;                     // the whole rect is water: a plain water draw
        struct AccurateWaterDC : FakeDC {
            AccurateWaterDC() { accurateShadow = true; }   // the mixed-rect choice only exists in DrawShadows
        } waterDc;
        waterManager.Update(waterDc);
        CHECK(!host.draws.empty() && host.draws.back().rfind("water:draw:", 0) == 0);
        host.rectCells = 0;                       // not covered: the mixed land/water mesh instead
        host.draws.clear();
        AccurateWaterDC mixedDc;
        waterManager.Update(mixedDc);
        CHECK(!host.draws.empty() && host.draws.back().rfind("mixed:draw:", 0) == 0);
        waterManager.Shutdown();
    }

    // ---- the vertex-buffer fast path ------------------------------------------------------------------------
    {
        cSTEOverlayManager vbManager(&host);
        vbManager.Init(nullptr, false, false);
        vbManager.AddRingDecal(0x46u, cS3DVector2{16.0f, 16.0f}, 16.0f, 0.0f);   // ring overlays draw with mask 0x11
        struct VbDC : FakeDC {
            int token = 0;
            VbDC() { vertexBuffer = reinterpret_cast<ITerrainVertBufExtn*>(&token); }
        } vbDc;
        host.draws.clear();
        vbManager.Update(vbDc);
        CHECK(vbDc.releasedVertexBuffer == 1);
        CHECK(!host.draws.empty() && host.draws.back().find(":vb:") != std::string::npos);
    }

    // ---- a dynamic manager is skipped while the smooth flag is set -----------------------------------------
    {
        cSTEOverlayManager dynamicManager(&host);
        dynamicManager.Init(nullptr, true, false);
        dynamicManager.AddRingDecal(0x47u, cS3DVector2{16.0f, 16.0f}, 16.0f, 0.0f);
        struct SmoothDC : FakeDC {
            SmoothDC() { smooth = true; }
        } smoothDc;
        dynamicManager.Update(smoothDc);
        CHECK(smoothDc.log.empty());              // dynamic_ && SmoothOverlays(): the whole pass is skipped
        dynamicManager.Shutdown();
    }

    // ---- GetStatsString -------------------------------------------------------------------------------------
    {
        cRZString stats;
        FakeDC dc;
        manager.Update(dc);
        manager.GetStatsString(stats);
        CHECK(std::strcmp(stats.ToChar(), "1 decals, 1 shadows") == 0);
    }

    texture->Release();
    manager.Shutdown();
}

static void TestCofactors() {
    // The adjugate of a rotation + translation matrix satisfies M * adj(M) = det(M) * I.
    cS3DMatrix4 m{};
    const float angle = 0.7f;
    const float c = std::cos(angle), s = std::sin(angle);
    m.m[0][0] = c; m.m[0][2] = s;
    m.m[1][1] = 1.0f;
    m.m[2][0] = -s; m.m[2][2] = c;
    m.m[3][0] = 3.0f; m.m[3][1] = 4.0f; m.m[3][2] = 5.0f; m.m[3][3] = 1.0f;
    const cS3DMatrix4 adj = OverlayCofactors(m);
    const cS3DMatrix4 product = m * adj;
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) CHECK(Near(product.m[r][k], r == k ? 1.0f : 0.0f, 1e-4f));

    // A singular matrix yields the zero adjugate.
    cS3DMatrix4 zero{};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) zero.m[r][k] = 0.0f;
    const cS3DMatrix4 zeroAdj = OverlayCofactors(zero);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) CHECK(zeroAdj.m[r][k] == 0.0f);
}

int main() {
    FakeHost host;
    TestRecordsAndSlots();
    TestLifeCycle(host);
    TestDecals(host);
    TestRingAndShadowDecals(host);
    TestAddShadow(host);
    TestSpatialIndexAndStaticRects(host);
    TestDrawPass(host);
    TestCofactors();
    // every fake texture was created by the test or its fake factory; the game only held references
    for (auto* t : FakeTexture::Textures()) delete t;
    for (auto* t : FakeStringTexture::Strings()) delete t;
    std::printf(failures ? "sc4_overlaymanager: %d FAILED\n" : "sc4_overlaymanager: all ok\n", failures);
    return failures ? 1 : 0;
}
