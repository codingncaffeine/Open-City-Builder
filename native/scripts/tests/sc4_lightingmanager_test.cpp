// cSC4LightingManager — unit test. Drives the recovered object through a fake ILightingHost: the identity
// (QueryInterface's three answers, the reference count, the deleting Release), the constructor defaults, Init
// (the render properties' kShadowQuality, the lighting exemplar's property list, the four colour maps and the
// seven notifications), the time-of-day curve (the sampled table, IsNight, the night/day and lighting-changed
// messages, the tint), the zoom/rotation pass (the six directional light colours), the colour math
// (GetColor / GetColor(dir) / GetModelColor's colour-map quirk / GetVertexColors), the terrain path (the
// terrain-changed / region-begin / region-updated / region-end messages, the altitude strides per rotation,
// FindHillShadows, DoLighting and RedisplayTerrain), the shadow getters, CalcShadowTerrainRect on a flat map and
// Shutdown. GetModelLight is driven with a fake model instance and a fake occupant (flora / automata / network).

#include "sc4/LightingManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/Occupant.h"
#include "sc4/RenderProperties.h"
#include "sc4/SCPropertyHelp.h"
#include "s3d/ModelInstance.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (false)

using namespace sc4;

bool Near(float a, float b, float epsilon = 1.0e-6f) { return std::fabs(a - b) <= epsilon; }

// --------------------------------------------------------------------------------------------- fake terrain
struct FakeTerrain final : ILightingTerrain {
    int cellsX = 0;
    int cellsZ = 0;
    float cellSize = 1.0f;
    bool initialized = true;
    std::vector<float> heights;   // (cellsZ + 1) * (cellsX + 1), indexed x * (cellsZ + 1) + z like cSTETerrain
    cS3DVector3 normal{0.0f, 1.0f, 0.0f};
    int refs = 1;
    int lightingCalls = 0;
    SC4RectInt lastLightingRect{};
    int redisplayCalls = 0;
    SC4RectInt lastRedisplayRect{};
    int altitudeCalls = 0;

    FakeTerrain(int x, int z, float size) : cellsX(x), cellsZ(z), cellSize(size) {
        heights.assign(static_cast<std::size_t>(cellsX + 1) * static_cast<std::size_t>(cellsZ + 1), 0.0f);
    }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    bool IsInitialized() override { return initialized; }
    float CellSize() override { return cellSize; }
    int CellCountX() override { return cellsX; }
    int CellCountZ() override { return cellsZ; }
    void GetAltitudes(const SC4RectInt& rect, float* out, std::ptrdiff_t rowStride,
                      std::ptrdiff_t colStride) override {
        ++altitudeCalls;
        for (int z = rect.top; z <= rect.bottom; ++z) {
            float* row = out + (z - rect.top) * rowStride;
            for (int x = rect.left; x <= rect.right; ++x, row += colStride)
                *row = Height(x, z);
        }
    }
    void TerrainNormal(float x, float z, cS3DVector3& out) override {
        (void)x;
        (void)z;
        out = normal;
    }
    void DoLighting(const SC4RectInt& rect) override {
        ++lightingCalls;
        lastLightingRect = rect;
    }
    void RedisplayTerrain(bool, bool, const SC4RectInt& rect, std::uint32_t) override {
        ++redisplayCalls;
        lastRedisplayRect = rect;
    }
    float Height(int x, int z) const {
        const std::size_t index = static_cast<std::size_t>(x) * static_cast<std::size_t>(cellsZ + 1) +
                                  static_cast<std::size_t>(z);
        return index < heights.size() ? heights[index] : 0.0f;
    }
    void SetHeight(int x, int z, float value) {
        const std::size_t index = static_cast<std::size_t>(x) * static_cast<std::size_t>(cellsZ + 1) +
                                  static_cast<std::size_t>(z);
        if (index < heights.size()) heights[index] = value;
    }
};

// --------------------------------------------------------------------------------------------- fake colour map
struct FakeColorMap final : ILightingColorMap {
    std::vector<std::uint32_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t stride = 0;
    bool released = false;
    int sampleCalls = 0;
    cS3DVector3 sampleResult{0.25f, 0.5f, 0.75f};

    FakeColorMap(std::uint32_t w, std::uint32_t h) : pixels(static_cast<std::size_t>(w) * h, 0), width(w), height(h),
                                                     stride(static_cast<std::int32_t>(w)) {}
    std::uint32_t Width() const override { return width; }
    std::uint32_t Height() const override { return height; }
    std::int32_t Stride() const override { return stride; }
    const std::uint32_t* Pixels() const override { return pixels.data(); }
    std::size_t PixelCount() const override { return pixels.size(); }
    cS3DVector3 SampleColorLerp(float, float) const override {
        const_cast<FakeColorMap*>(this)->sampleCalls++;
        return sampleResult;
    }
    void Release() override { released = true; }
};

// --------------------------------------------------------------------------------------------- fake message server
struct SentMessage {
    std::uint32_t type = 0;
    std::uint64_t data1 = 0;
    std::uint64_t data2 = 0;
    bool posted = false;
};

struct FakeMessageServer final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added;
    std::vector<std::uint32_t> removed;
    std::vector<SentMessage> sent;
    std::vector<SentMessage> posted;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back(SentMessage{standard->GetType(), standard->GetData1(), standard->GetData2(), false});
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        posted.push_back(SentMessage{standard->GetType(), standard->GetData1(), standard->GetData2(), true});
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        added.push_back(messageType);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        removed.push_back(messageType);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// --------------------------------------------------------------------------------------------- fake host
struct FakeHost final : ILightingHost {
    FakeMessageServer server;
    FakeTerrain* terrain = nullptr;
    FakeTerrain* terrainMap = nullptr;
    cSC4RenderProperties properties;
    cISCPropertyHolder* holder = nullptr;
    std::vector<FakeColorMap*> maps;   // handed out in order by CreateColorMap
    std::size_t nextMap = 0;
    int redraws = 0;
    int seasonMonth = 3;
    bool networkFlag = false;
    int networkQueries = 0;

    // The host owns what the scenarios plug in; every scenario declares it before the manager, so it outlives the manager.
    ~FakeHost() override {
        if (holder) holder->Release();   // the test's reference from MakeExemplar
        delete terrainMap;
        if (terrain != terrainMap) delete terrain;
        for (FakeColorMap* map : maps) delete map;
    }

    cISCPropertyHolder* LightingProperties() override { return holder; }
    cIGZMessageServer2* MessageServer() override { return &server; }
    ILightingTerrain* TerrainMap() override { return terrainMap; }
    ILightingTerrain* Terrain() override { return terrain; }
    SC4ViewUtilities* ViewUtilities() override { return nullptr; }   // the zoom / rotation tests set the fields directly
    cSC4RenderProperties* RenderProperties() override { return &properties; }
    int CurrentSeasonMonth() override { return seasonMonth; }
    void ForceFullRedraw() override { ++redraws; }
    ILightingColorMap* CreateColorMap(std::uint32_t) override {
        if (nextMap >= maps.size()) return nullptr;
        return maps[nextMap++];
    }
    bool OccupantHasNetworkFlag(cISC4Occupant*, std::uint32_t flags) override {
        ++networkQueries;
        return networkFlag && flags == 0x40000;
    }
};

// --------------------------------------------------------------------------------------------- fake model + occupant
struct FakeModel final : s3d::cIS3DModelInstance {
    cIGZUnknown* owner = nullptr;
    cS3DBoundingBox bounds{};
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    cIGZUnknown* GetOwner() override { return owner; }
    void SetOwner(cIGZUnknown* value) override { owner = value; }
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetWorldBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetTransform(cS3DTransform&) override {}
    void SetTransform(const cS3DTransform&) override {}
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 1; }
    int GetProperties() override { return 2; }
    void SetProperties(int) override {}
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return s3d::SC4DrawAttributes{}; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes&) override {}
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
};

struct FakeOccupant final : cISC4Occupant {
    std::uint32_t type = 0;
    bool floraGroup = false;
    int refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4Occupant) return false;
        if (out) *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { xyz[0] = xyz[1] = xyz[2] = 0.0f; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        minXyz[0] = minXyz[1] = minXyz[2] = 0.0f;
        maxXyz[0] = maxXyz[1] = maxXyz[2] = 0.0f;
    }
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = SC4CellRect{}; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t group) const override { return floraGroup && group == 0x5004; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

// --------------------------------------------------------------------------------------------- helpers
cRZMessage2Standard MakeMessage(std::uint32_t type, std::uint64_t data1 = 0, std::uint64_t data2 = 0) {
    cRZMessage2Standard message;
    message.SetType(type);
    if (data1 || data2) {
        message.SetData1(data1);
        message.SetData2(data2);
    }
    return message;
}

void Send(cSC4LightingManager& manager, std::uint32_t type, std::uint64_t data1 = 0, std::uint64_t data2 = 0) {
    cRZMessage2Standard message = MakeMessage(type, data1, data2);
    manager.DoMessage(&message);
}

// Builds a lighting exemplar with the properties ReadProperties understands.
cSCPropertyHolder* MakeExemplar() {
    auto* holder = new cSCPropertyHolder();
    holder->AddRef();   // the test owns one reference; the manager takes its own
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropTextureFloor, 0.5f);
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropUseEnvironmentMap, false);
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropDaytimeLight, std::vector<float>{1.0f, 1.0f, 1.0f});
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropNighttimeLight, std::vector<float>{0.2f, 0.2f, 0.2f});
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropScatterColor, std::vector<float>{0.25f, 0.5f, 1.0f});
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropScatterStrength, 0.25f);
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropNightThreshold, 0.8f);
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropSunDirection, 0.0f);
    cSCPropertyHelp::SetPropertyValue(holder, kLightingPropSunPitch, 0.0f);
    return holder;
}

// --------------------------------------------------------------------------------------------- scenarios
void TestDefaultsAndIdentity() {
    ResetLightingManagerTunables();
    FakeHost host;
    cSC4LightingManager manager(&host);
    CHECK(!manager.IsInitialized());
    CHECK(manager.GetTimeOfDay() == 12.0f);
    CHECK(!manager.IsNight());
    cS3DVector3 light{9, 9, 9};
    manager.GetGlobalLight(&light);
    CHECK(light.x == 1.0f && light.y == 1.0f && light.z == 1.0f);
    CHECK(manager.DayNightColors().empty());
    CHECK(manager.ScatterStrengths().empty());
    CHECK(manager.ScatterColors().empty());
    CHECK(manager.Heights().empty());
    CHECK(!manager.HillShadowsEnabled());
    CHECK(manager.EnvironmentFloor() == 0.0f);
    // The rotation matrix and the two light directions are still zero, so the ambient lobe is the 0.05 base.
    const cS3DVector3 ambient = manager.GetColor();
    CHECK(Near(ambient.x, 0.05f) && Near(ambient.y, 0.05f) && Near(ambient.z, 0.05f));
    CHECK(manager.GetShadowDirection().x == 0.0f);   // the rotated sun is only filled by a zoom/rotation pass
    float strength = 0.0f, threshold = 0.0f;
    cS3DVector3 shadowColour{9, 9, 9};
    manager.GetShadowParams(strength, threshold, shadowColour);
    CHECK(strength == 0.5f);
    CHECK(threshold == 0.0f);
    CHECK(shadowColour.x == 0.0f && shadowColour.y == 0.0f && shadowColour.z == 0.0f);
    CHECK(manager.GetShadowStrengthFromLocation(cS3DVector2{0, 0}) == 0.0f);
    const LightingTunables& tunables = LightingManagerTunables();
    CHECK(Near(tunables.lightingUpdateDelta, 9.99999975e-05f));
    CHECK(Near(tunables.nightThreshold, 0.8f));
    CHECK(Near(tunables.textureFloor, 0.1f));
    CHECK(Near(tunables.sunDirection, 90.0f) && Near(tunables.sunPitch, 45.0f));
    CHECK(tunables.useEnvironmentMap && !tunables.testEnvironmentMap && !tunables.testTerrainShadowing);
    CHECK(tunables.shadowTextureID == 0x0917660e);

    // QueryInterface: 1 and 0x681bd52a answer the primary, 0x452294aa the message-target view; anything else fails.
    void* out = nullptr;
    CHECK(manager.QueryInterface(1, &out) && out == static_cast<cISC4LightingManager*>(&manager));
    CHECK(manager.QueryInterface(0x681bd52a, &out) && out == static_cast<cISC4LightingManager*>(&manager));
    CHECK(manager.QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(&manager));
    out = reinterpret_cast<void*>(0x1);
    CHECK(!manager.QueryInterface(0x12345678, &out));
    CHECK(out == reinterpret_cast<void*>(0x1));   // a failed query writes nothing
}

void TestInitAndProperties() {
    ResetLightingManagerTunables();
    FakeHost host;
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    CHECK(manager.IsInitialized());
    CHECK(manager.HillShadowsEnabled());   // kShadowQuality defaults to 2 (> 1)
    CHECK(host.server.added.size() == 7);
    const std::uint32_t expected[] = {kLightingMessageRepaintRequest, kLightingMessageRenderPropertiesChanged,
                                      kLightingMessageTerrainChanged, kLightingMessageZoomRotationChanged,
                                      kLightingMessageTerrainRegionBegin, kLightingMessageTerrainRegionEnd,
                                      kLightingMessageTerrainRegionUpdated};
    for (std::size_t i = 0; i < 7; ++i) CHECK(host.server.added[i] == expected[i]);
    CHECK(!manager.Init());   // idempotent
    CHECK(host.server.added.size() == 7);

    // kShadowQuality = 1 switches the hill shadows off before Init.
    host.properties.SetIntValue(12, 1);
    cSC4LightingManager other(&host);
    CHECK(other.Init() && !other.HillShadowsEnabled());

    // The exemplar's properties land in the shared tunables and the tables.
    ResetLightingManagerTunables();
    FakeHost host2;
    host2.holder = MakeExemplar();
    cSC4LightingManager withProperties(&host2);
    CHECK(withProperties.Init());
    CHECK(Near(LightingManagerTunables().textureFloor, 0.5f));
    CHECK(!LightingManagerTunables().useEnvironmentMap);
    CHECK(withProperties.DayNightColors().size() == 8);
    const std::vector<cS3DVector3>& table = withProperties.DayNightColors();
    CHECK(Near(table[0].x, 0.2f) && Near(table[1].x, 0.2f));
    CHECK(Near(table[2].x, 1.0f) && Near(table[5].x, 1.0f));
    CHECK(Near(table[6].x, 0.2f) && Near(table[7].x, 0.2f));
    CHECK(withProperties.ScatterStrengths().size() == 4);
    CHECK(withProperties.ScatterStrengths()[0] == 0.0f && Near(withProperties.ScatterStrengths()[1], 0.25f));
    CHECK(withProperties.ScatterColors().size() == 1 && Near(withProperties.ScatterColors()[0].y, 0.5f));
    CHECK(Near(withProperties.FloorLight().x, 0.5f));   // day colour * kTextureFloor
    // sun direction with azimuth 0 and pitch 0: (sin 0 * cos 0, -sin 0, -cos 0 * cos 0) = (0, 0, -1).
    CHECK(Near(withProperties.SunDirection().x, 0.0f));
    CHECK(Near(withProperties.SunDirection().y, 0.0f));
    CHECK(Near(withProperties.SunDirection().z, -1.0f));
    withProperties.ResetLightingTint();
}

void TestTimeOfDayCurve() {
    ResetLightingManagerTunables();
    FakeHost host;
    host.holder = MakeExemplar();
    auto* terrain = new FakeTerrain(2, 2, 8.0f);   // the update test requires a terrain map
    host.terrainMap = terrain;
    host.terrain = terrain;
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    CHECK(!manager.IsNight());
    // The original only learns about the terrain from the terrain-changed notification, and SetTimeOfDay's
    // night / message block runs only when a terrain map is attached.
    Send(manager, kLightingMessageTerrainChanged);

    // Midnight: the table is [night, night, day, day, day, day, night, night] at x = 7 * 0 = 0 -> the night colour.
    CHECK(manager.SetTimeOfDay(0.0f));
    CHECK(manager.IsNight());
    CHECK(Near(manager.SampledLight().x, 0.2f));
    cS3DVector3 global{9, 9, 9};
    manager.GetGlobalLight(&global);
    CHECK(Near(global.x, 0.2f) && Near(global.y, 0.2f) && Near(global.z, 0.2f));
    // nightFade = (0.2 - 0.8) / 0.2 clamped to 0; shadowFade = 1 - (0.2 - 0) / 0.8 = 0.75.
    CHECK(manager.NightFade() == 0.0f);
    CHECK(Near(manager.NightShadowFade(), 0.75f));
    CHECK(host.server.sent.size() == 1);
    if (host.server.sent.size() == 1) {
        CHECK(host.server.sent[0].type == kLightingMessageLightingChanged);
        CHECK(host.server.sent[0].data1 == 0);
        CHECK(host.server.sent[0].data2 == 0x3f400000u);   // 0.75f
    }
    // 6:00 samples to exactly 0.8 (0.75 * day + 0.25 * night); 0.8 < 0.8 is false, so the night state flips and
    // the original posts 0xc9da96e8 (dawn: the previous sample was dark).
    CHECK(manager.SetTimeOfDay(24.0f * 0.25f));
    CHECK(Near(manager.SampledLight().x, 0.8f));
    CHECK(!manager.IsNight());
    bool foundDawn = false;
    for (const SentMessage& message : host.server.posted)
        if (message.type == kLightingMessageDawn) foundDawn = true;
    CHECK(foundDawn);
    // Noon is the day colour.
    CHECK(manager.SetTimeOfDay(12.0f));
    CHECK(Near(manager.SampledLight().x, 1.0f));
    CHECK(!manager.IsNight());
    CHECK(Near(manager.NightFade(), 1.0f));

    // A time that goes backwards re-reads the weather simulator's season month (vslot 0x78 -> month - 1).
    host.seasonMonth = 7;
    CHECK(manager.SetTimeOfDay(3.0f));
    // The season only feeds the colour-map path; the table path ignores it. It must not corrupt anything:
    CHECK(manager.GetTimeOfDay() == 3.0f);

    // Tint: the global light becomes the tint colour, the floor light follows, and the redraw is requested when
    // the light moved by more than kLightingUpdateDelta.
    const int redrawsBefore = host.redraws;
    manager.SetLightingTint(cS3DVector3{0.5f, 0.4f, 0.3f}, 1.0f);
    manager.GetGlobalLight(&global);
    CHECK(Near(global.x, 0.5f) && Near(global.y, 0.4f) && Near(global.z, 0.3f));
    CHECK(Near(manager.FloorLight().x, 0.25f));   // 0.5 * textureFloor
    CHECK(host.redraws > redrawsBefore);
    manager.ResetLightingTint();
    manager.GetGlobalLight(&global);
    CHECK(Near(global.x, 0.2f));   // back to the sampled 3:00 value
    // Quirk kept: Shutdown drops the terrain-map reference but not the terrain one (that is the destructor's), so
    // the terrain fixtures must outlive the manager.
    CHECK(manager.Shutdown());
}

void TestZoomRotationAndColours() {
    ResetLightingManagerTunables();
    FakeHost host;
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    manager.DoZoomAndRotationChange(0, 0);
    // The rotated sun is the exemplar's default direction (azimuth 90, pitch 45).
    const cS3DVector3 sun = manager.GetShadowDirection();
    CHECK(Near(sun.x, 0.70710678f, 1.0e-6f));
    CHECK(Near(sun.y, -0.70710678f, 1.0e-6f));
    CHECK(Near(sun.z, 0.0f, 1.0e-6f));   // azimuth 90 -> cos(90) * cos(45) ~ 0
    // The six directional colours: +X, -X, +Y, +Y (the original's duplicate), +Z, -Z.
    CHECK(Near(manager.DirectionalLight(0).x, 0.05f));
    CHECK(Near(manager.DirectionalLight(0).y, 0.11666667f));
    CHECK(Near(manager.DirectionalLight(0).z, 0.38333334f));
    CHECK(Near(manager.DirectionalLight(1).x, 0.62735027f));
    CHECK(Near(manager.DirectionalLight(1).z, 0.51188022f));
    CHECK(Near(manager.DirectionalLight(2).x, 0.62735027f));
    CHECK(Near(manager.DirectionalLight(2).y, 0.66068363f));
    CHECK(Near(manager.DirectionalLight(2).z, 0.67854691f));
    CHECK(Near(manager.DirectionalLight(3).x, manager.DirectionalLight(2).x));   // quirk: -Y == +Y
    CHECK(Near(manager.DirectionalLight(4).x, 0.62735027f));
    CHECK(Near(manager.DirectionalLight(4).z, 0.51188022f));
    // GetColor() is GetColor((0, 1, 0)), and the byte form uses the round(v * 256) fold.
    const cS3DVector3 up = manager.GetColor();
    CHECK(Near(up.x, 0.62735027f) && Near(up.y, 0.66068363f) && Near(up.z, 0.67854691f));
    std::uint8_t r = 0, g = 0, b = 0;
    manager.GetColor(r, g, b);
    CHECK(r == 161 && g == 169 && b == 174);
    manager.GetColor(cS3DVector3{1.0f, 0.0f, 0.0f}, r, g, b);
    CHECK(r == 13 && g == 30 && b == 98);
    // GetColor(normal, position) without hill shadows is GetColor(normal).
    const cS3DVector3 positioned = manager.GetColor(cS3DVector3{0.0f, 1.0f, 0.0f}, cS3DVector3{1.0f, 2.0f, 3.0f});
    CHECK(Near(positioned.x, up.x) && Near(positioned.y, up.y) && Near(positioned.z, up.z));

    // GetVertexColors: an identity rotation feeds the directional colours straight through.
    nS3D::Matrix3 identity;
    cS3DVector3 normals[1] = {{1.0f, 0.0f, 0.0f}};
    std::uint8_t colours[4] = {0, 0, 0, 0};
    manager.GetVertexColors(1, normals, static_cast<int>(sizeof(cS3DVector3)), colours, 4, identity);
    CHECK(colours[0] == 13 && colours[1] == 30 && colours[2] == 98 && colours[3] == 0xff);
    // alongX is 0, so only the +Y lobe contributes: the +Y directional colour.
    normals[0] = cS3DVector3{0.0f, 1.0f, 0.0f};
    manager.GetVertexColors(1, normals, static_cast<int>(sizeof(cS3DVector3)), colours, 4, identity);
    CHECK(colours[0] == 161 && colours[1] == 169 && colours[2] == 174 && colours[3] == 0xff);
    // A matrix row that flips the sign selects the -X colour.
    nS3D::Matrix3 flipped = identity;
    flipped.m[0] = -1.0f;
    normals[0] = cS3DVector3{1.0f, 0.0f, 0.0f};
    manager.GetVertexColors(1, normals, static_cast<int>(sizeof(cS3DVector3)), colours, 4, flipped);
    CHECK(colours[0] == 161 && colours[1] == 161 && colours[2] == 131);   // -X colour (-1,0,0) -> 0.62735/0.62735/0.51188
}

void TestEnvironmentMapQuirk() {
    ResetLightingManagerTunables();
    FakeHost host;
    auto* first = new FakeColorMap(2, 2);    // supplies the dimensions
    auto* second = new FakeColorMap(4, 4);   // supplies the pixels and the stride
    for (std::size_t i = 0; i < second->pixels.size(); ++i) second->pixels[i] = static_cast<std::uint32_t>(i);
    host.maps = {first, second};
    host.holder = MakeExemplar();
    // kUseEnvironmentMap must stay on for the map path; MakeExemplar turns it off, so re-enable it.
    cSCPropertyHelp::SetPropertyValue(host.holder, kLightingPropUseEnvironmentMap, true);
    host.terrainMap = host.terrain = new FakeTerrain(2, 2, 8.0f);
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    CHECK(manager.ColorMap(0) == first && manager.ColorMap(1) == second);
    manager.DoZoomAndRotationChange(0, 0);
    // Direction +X: p = 1, q = 0 -> fx = 2 * 1 = 2 -> clamped to width - 1 = 1; fy = 2 * 0.5 = 1.
    // The quirk: the pixel is read from map +0x108 (the second) with the first map's dimensions.
    const cS3DVector3 colour = manager.GetModelColor(cS3DVector3{1.0f, 0.0f, 0.0f});
    const std::uint32_t pixel = second->pixels[1 * 4 + 1];   // = 5
    CHECK(Near(colour.x, static_cast<float>(pixel & 0xffu) * 0.003921569f));
    CHECK(Near(colour.y, static_cast<float>((pixel >> 8) & 0xffu) * 0.003921569f));
    CHECK(Near(colour.z, static_cast<float>((pixel >> 16) & 0xffu) * 0.003921569f));
    // GetColor samples the first map instead (same transform).
    const cS3DVector3 sampled = manager.GetColor(cS3DVector3{1.0f, 0.0f, 0.0f});
    const std::uint32_t firstPixel = first->pixels[1 * 2 + 1];
    CHECK(Near(sampled.x, static_cast<float>(firstPixel & 0xffu) * 0.003921569f));
    CHECK(!first->released && !second->released);
    // Shutdown releases every map through ILightingColorMap::Release.
    CHECK(manager.Shutdown());
    CHECK(first->released && second->released);
    // The fixtures stay alive until the process exits: the manager's destructor releases the terrain and map
    // references after Shutdown already dropped them.
}

void TestTerrainPath() {
    ResetLightingManagerTunables();
    FakeHost host;
    host.holder = MakeExemplar();
    auto* terrain = new FakeTerrain(2, 2, 8.0f);   // 3 x 3 vertices, 16 m x 16 m
    terrain->SetHeight(0, 0, 1.0f);
    terrain->SetHeight(1, 0, 2.0f);
    terrain->SetHeight(2, 0, 3.0f);
    terrain->SetHeight(0, 1, 4.0f);
    terrain->SetHeight(1, 1, 5.0f);
    terrain->SetHeight(2, 1, 6.0f);
    terrain->SetHeight(0, 2, 7.0f);
    terrain->SetHeight(1, 2, 8.0f);
    terrain->SetHeight(2, 2, 9.0f);
    host.terrainMap = terrain;
    host.terrain = terrain;
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    Send(manager, kLightingMessageTerrainChanged);
    CHECK(manager.CellSize() == 8.0f);
    CHECK(manager.TerrainWidth() == 16.0f && manager.TerrainDepth() == 16.0f);
    CHECK(manager.Heights().size() == 9);
    CHECK(manager.Crests().size() == 9);
    // Rotation 0 of a 2 x 2 map: index = x * 3 + z.
    Send(manager, kLightingMessageTerrainRegionBegin);
    CHECK(manager.AccumulateRect().x0 == 3 && manager.AccumulateRect().z0 == 3);
    Send(manager, kLightingMessageTerrainRegionUpdated, (0u << 16) | 2u, (0u << 16) | 2u);
    // Region begin accumulated (3,3,0,0), the update unioned (0,0,2,2) into it.
    CHECK(manager.AccumulateRect().x0 == 0 && manager.AccumulateRect().z0 == 0);
    CHECK(manager.AccumulateRect().x1 == 2 && manager.AccumulateRect().z1 == 2);
    Send(manager, kLightingMessageTerrainRegionEnd);
    CHECK(terrain->lightingCalls == 1);
    CHECK(terrain->redisplayCalls == 1);
    // The heights were filled from the terrain in hill order (x * 3 + z for rotation 0).
    CHECK(manager.Heights().size() == 9);
    CHECK(Near(manager.Heights()[0], 1.0f));
    CHECK(Near(manager.Heights()[1], 4.0f));   // x = 0, z = 1 -> index 1
    CHECK(Near(manager.Heights()[3], 2.0f));   // x = 1, z = 0 -> index 3
    CHECK(Near(manager.Heights()[8], 9.0f));
    // The hill rect covers at least one vertex and DoLighting got a rect inside the map.
    CHECK(manager.HillRect().x0 >= 0 && manager.HillRect().z0 >= 0);
    CHECK(manager.HillRect().x1 <= 2 && manager.HillRect().z1 <= 2);
    CHECK(terrain->lastLightingRect.left >= 0 && terrain->lastLightingRect.right <= 2);
    // A flat-light terrain query (hill shadows on, the crest is at most the height here).
    std::uint8_t r = 0, g = 0, b = 0;
    manager.GetTerrainLighting(cS3DVector3{4.0f, 5.0f, 4.0f}, r, g, b);
    CHECK(r <= 255 && g <= 255 && b <= 255);
    cS3DVector3 normals[2] = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    std::uint8_t colours[12] = {};
    manager.GetTerrainVertexColors(0, 0, 2, normals, static_cast<int>(sizeof(cS3DVector3)), colours, 6);
    CHECK(colours[3] != 0 || colours[4] != 0 || colours[5] != 0);   // the floor colour half is written
    // Rotation 3 of the same map: index = x * 1 + z * (-3) + 6.
    Send(manager, kLightingMessageTerrainChanged);   // the same terrain, so this is a no-op except the zoom pass
    manager.DoZoomAndRotationChange(0, 3);
    Send(manager, kLightingMessageTerrainRegionEnd);   // nothing accumulated: still the previous rect
    CHECK(terrain->lightingCalls <= 3);
}

void TestTerrainColours() {
    ResetLightingManagerTunables();
    FakeHost host;
    host.holder = MakeExemplar();
    // Hill shadows off so the terrain normal is used unchanged.
    host.properties.SetIntValue(12, 1);
    auto* terrain = new FakeTerrain(2, 2, 8.0f);
    terrain->normal = cS3DVector3{0.0f, 1.0f, 0.0f};
    host.terrainMap = terrain;
    host.terrain = terrain;
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    CHECK(!manager.HillShadowsEnabled());
    Send(manager, kLightingMessageTerrainChanged);
    manager.DoZoomAndRotationChange(0, 0);
    // GetColor((0,1,0)) = (0.62735, 0.66068, 0.67855); kTerrainShadowAmount = 0.6 gives
    // (1 - c) * 0.4 + c = (0.77641, 0.79641, 0.80713) -> bytes 199 / 204 / 207.
    std::uint8_t r = 0, g = 0, b = 0;
    manager.GetTerrainLighting(cS3DVector3{4.0f, 5.0f, 4.0f}, r, g, b);
    CHECK(r == 199 && g == 204 && b == 207);
    // The byte forms of the position/normal queries.
    manager.GetColor(cS3DVector3{4.0f, 5.0f, 4.0f}, cS3DVector3{0.0f, 1.0f, 0.0f}, r, g, b);
    CHECK(r == 161 && g == 169 && b == 174);
    manager.GetTerrainColor(cS3DVector3{4.0f, 5.0f, 4.0f}, cS3DVector3{0.0f, 1.0f, 0.0f}, r, g, b);
    CHECK(r <= 255 && g <= 255 && b <= 255);
    manager.GetTerrainColorFloor(cS3DVector3{4.0f, 5.0f, 4.0f}, cS3DVector3{0.0f, 1.0f, 0.0f}, r, g, b);
    CHECK(r <= 255 && g <= 255 && b <= 255);
    // The terrain-vertex colours write both the shadowed and the floor triples.
    cS3DVector3 normals[2] = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    std::uint8_t colours[12] = {};
    manager.GetTerrainVertexColors(0, 0, 2, normals, static_cast<int>(sizeof(cS3DVector3)), colours, 6);
    CHECK(colours[0] == 161 && colours[1] == 169 && colours[2] == 174);
    // The floor half is colour * kTextureFloor (0.5 from the exemplar): 0.3137 / 0.3303 / 0.3393 -> 80 / 85 / 87.
    CHECK(colours[3] == 80 && colours[4] == 85 && colours[5] == 87);
    CHECK(colours[6] == 161 && colours[9] == 80);
    CHECK(manager.Shutdown());
}

void TestShadowRectMarch() {
    ResetLightingManagerTunables();
    FakeHost host;
    // Azimuth 45, pitch 30: the rotated sun is (0.612, -0.5, -0.612), so the +X and -Z edges march.
    host.holder = MakeExemplar();
    cSCPropertyHelp::SetPropertyValue(host.holder, kLightingPropSunDirection, 45.0f);
    cSCPropertyHelp::SetPropertyValue(host.holder, kLightingPropSunPitch, 30.0f);
    auto* terrain = new FakeTerrain(2, 2, 8.0f);
    for (float& height : terrain->heights) height = -2.0f;   // below the projection, so the march keeps going
    host.terrainMap = terrain;
    host.terrain = terrain;
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    Send(manager, kLightingMessageTerrainChanged);
    manager.DoZoomAndRotationChange(0, 0);
    cS3DBoundingBox box;
    box.min[0] = 0.0f; box.min[1] = 0.0f; box.min[2] = 8.0f;
    box.max[0] = 8.0f; box.max[1] = 0.0f; box.max[2] = 16.0f;
    cRZRect rect{};
    CHECK(manager.CalcShadowTerrainRect(0, nullptr, box, &rect));
    // The clamped cell rectangle is (0, 1) .. (1, 2). With a flat 0 m map nothing would march and the result would
    // stay (0, 1, 1, 2); with the map 2 m below the projection both the +X edge and the -Z edge move one cell, so
    // the result is (0, 0, 2, 2).
    CHECK(rect.left == 0 && rect.top == 0);
    CHECK(rect.right == 2 && rect.bottom == 2);
    CHECK(manager.Shutdown());
}

void TestShadowsAndModelLight() {
    ResetLightingManagerTunables();
    FakeHost host;
    host.holder = MakeExemplar();
    host.terrainMap = host.terrain = new FakeTerrain(2, 2, 8.0f);
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    Send(manager, kLightingMessageTerrainChanged);
    manager.DoZoomAndRotationChange(0, 0);
    // CalcShadowTerrainRect: a flat map, so nothing marches and the clamped cell rectangle comes back.
    cS3DBoundingBox box;
    box.min[0] = 0.0f; box.min[1] = 0.0f; box.min[2] = 0.0f;
    box.max[0] = 16.0f; box.max[1] = 0.0f; box.max[2] = 16.0f;
    cRZRect rect{};
    CHECK(manager.CalcShadowTerrainRect(0, nullptr, box, &rect));
    CHECK(rect.left == 0 && rect.top == 0);
    CHECK(rect.right == 2 && rect.bottom == 2);
    // CalcShadowProjection is a documented PARTIAL.
    cS3DTransform transform;
    cS3DMatrix4 matrix;
    const cS3DVector3 points[1] = {{0, 0, 0}};
    const cS3DVector2 texCoords[1] = {{0, 0}};
    CHECK(!manager.CalcShadowProjection(1, points, texCoords, transform, &matrix));

    // GetModelLight: no owner -> the terrain-normal path, all components finite and in range.
    FakeModel model;
    cS3DVector3 light{0, 0, 0};
    CHECK(manager.GetModelLight(&model, &light));
    CHECK(std::isfinite(light.x) && std::isfinite(light.y) && std::isfinite(light.z));
    CHECK(light.x >= 0.0f && light.y >= 0.0f && light.z >= 0.0f);

    // A flora occupant takes the flora shadow amount; the result only depends on the colour tables, so just check
    // the call chain and the network-flag short circuit.
    FakeOccupant flora;
    flora.type = 0x74758926;
    model.owner = static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(&flora));
    CHECK(manager.GetModelLight(&model, &light));
    CHECK(host.networkQueries == 1 && !host.networkFlag);
    FakeOccupant automata;
    automata.type = 0x496e5783;
    model.owner = static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(&automata));
    CHECK(manager.GetModelLight(&model, &light));
    host.networkFlag = true;
    model.owner = static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(&automata));
    CHECK(manager.GetModelLight(&model, &light));
    CHECK(host.networkQueries == 3);
    host.networkFlag = false;
}

void TestMessagesAndShutdown() {
    ResetLightingManagerTunables();
    FakeHost host;
    host.holder = MakeExemplar();
    cSC4LightingManager manager(&host);
    CHECK(manager.Init());
    manager.DoZoomAndRotationChange(0, 0);
    // The render-properties message reloads kLightingUpdateDelta, flips the hill shadows and asks for a redraw.
    host.properties.SetIntValue(12, 0);
    host.properties.SetFloatValue(6, 0.25f);
    const int redrawsBefore = host.redraws;
    const std::size_t sentBefore = host.server.sent.size();
    Send(manager, kLightingMessageRenderPropertiesChanged);
    CHECK(!manager.HillShadowsEnabled());
    CHECK(Near(LightingManagerTunables().lightingUpdateDelta, 0.25f));
    CHECK(host.server.sent.size() == sentBefore + 1);
    CHECK(host.server.sent.back().type == kLightingMessageLightingChanged);
    CHECK(host.redraws == redrawsBefore + 1);
    // Sending it again without a change does nothing.
    Send(manager, kLightingMessageRenderPropertiesChanged);
    CHECK(host.server.sent.size() == sentBefore + 1);
    // The zoom/rotation message carries two ints in the object payload.
    int zoomAndRotation[2] = {2, 1};
    cRZMessage2Standard zoomMessage;
    zoomMessage.SetType(kLightingMessageZoomRotationChanged);
    zoomMessage.SetVoid2(zoomAndRotation);
    manager.DoMessage(&zoomMessage);
    // A repaint request re-reads the exemplar (which puts kShadowQuality back — the property is not in the
    // exemplar — and the day colour).
    Send(manager, kLightingMessageRepaintRequest);
    cS3DVector3 global{9, 9, 9};
    manager.GetGlobalLight(&global);
    CHECK(Near(global.x, 1.0f));   // the exemplar's day colour
    // Shutdown removes the seven notifications and releases the map.
    CHECK(manager.Shutdown());
    CHECK(!manager.IsInitialized());
    CHECK(host.server.removed.size() == 7);
    CHECK(!manager.Shutdown());   // already shut down
}

void TestExemplarRelease() {
    ResetLightingManagerTunables();
    FakeHost host;
    cSCPropertyHolder* holder = MakeExemplar();
    host.holder = holder;
    {
        cSC4LightingManager manager(&host);
        CHECK(manager.Init());          // takes one reference
        CHECK(holder->RefCount() == 2);
        CHECK(manager.Shutdown());      // drops it
        CHECK(holder->RefCount() == 1);
    }
    holder->Release();                  // the test's reference deletes it
    host.holder = nullptr;              // already released here
}

}  // namespace

int main() {
    TestDefaultsAndIdentity();
    TestInitAndProperties();
    TestTimeOfDayCurve();
    TestZoomRotationAndColours();
    TestEnvironmentMapQuirk();
    TestTerrainPath();
    TestTerrainColours();
    TestShadowRectMarch();
    TestShadowsAndModelLight();
    TestMessagesAndShutdown();
    TestExemplarRelease();
    if (g_failures == 0) {
        std::printf("sc4_lightingmanager ok\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
