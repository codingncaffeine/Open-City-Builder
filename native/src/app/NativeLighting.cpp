#include "app/NativeLighting.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "app/NativeUi.h"
#include "rz/cRZMessage2Standard.h"
#include "gz/Buffer.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/Exemplar.h"
#include "sc4/LightingManager.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/Render3D.h"
#include "sc4/RenderProperties.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

sc4::cSTETerrain* g_terrain = nullptr;
sc4::cSC43DRender* g_render = nullptr;
sc4::cSC4Simulator* g_simulator = nullptr;
sc4::cSC4LightingManager* g_manager = nullptr;
bool g_inited = false;
bool g_relight = false;
float g_lastHour = -1.0f;
sc4::cSC424HourClock* g_clock = nullptr;

class ClockHost final : public sc4::IClockHost {
public:
    void PostMessage(std::uint32_t, std::uint32_t, std::uint32_t) override {}   // nobody listens to the hour messages natively yet
};

// The hour the sun is at: the clock's visual time (hours, fractional), else the simulator's hour and minute counters.
float SunHour() {
    if (g_clock) return g_clock->GetVisualTimePercentage() * 24.0f;
    if (g_simulator) return static_cast<float>(g_simulator->Counter(3) % 24) + static_cast<float>(g_simulator->Counter(2) % 60) / 60.0f;
    return 12.0f;
}

class ColorMap final : public ILightingColorMap {
public:
    explicit ColorMap(cGZBuffer* buffer) {
        w_ = buffer->Width();
        h_ = buffer->Height();
        pixels_.resize(static_cast<std::size_t>(w_) * h_);
        const auto* bits = static_cast<const std::uint8_t*>(buffer->GetColorSurfaceBits());
        const std::uint32_t stride = buffer->GetColorSurfaceStride();
        for (std::uint32_t y = 0; y < h_; ++y) {
            const std::uint8_t* row = bits + static_cast<std::size_t>(y) * stride;
            for (std::uint32_t x = 0; x < w_; ++x, row += 4)   // memory is B, G, R, A; the manager reads red from the low byte
                pixels_[static_cast<std::size_t>(y) * w_ + x] = row[2] | (row[1] << 8) | (row[0] << 16) | (row[3] << 24);
        }
    }
    std::uint32_t Width() const override { return w_; }
    std::uint32_t Height() const override { return h_; }
    std::int32_t Stride() const override { return static_cast<std::int32_t>(w_); }
    const std::uint32_t* Pixels() const override { return pixels_.data(); }
    std::size_t PixelCount() const override { return pixels_.size(); }
    // cImageARGB8::SampleColorLerp @ 0x10077b15a: bilinear in x only, x = floor(u * w), y = floor(v * h).
    cS3DVector3 SampleColorLerp(float u, float v) const override {
        if (pixels_.empty()) return cS3DVector3{1, 1, 1};
        const float fx = u * static_cast<float>(w_);
        int x0 = static_cast<int>(std::floor(fx)), x1 = static_cast<int>(std::ceil(fx));
        int y = static_cast<int>(std::floor(v * static_cast<float>(h_)));
        const float frac = fx - std::floor(fx);
        const int maxX = static_cast<int>(w_) - 1, maxY = static_cast<int>(h_) - 1;
        x0 = x0 < 0 ? 0 : (x0 > maxX ? maxX : x0);
        x1 = x1 < 0 ? 0 : (x1 > maxX ? maxX : x1);
        y = y < 0 ? 0 : (y > maxY ? maxY : y);
        const std::uint32_t* row = pixels_.data() + static_cast<std::size_t>(y) * w_;
        auto channel = [&](int shift) {
            const float a = static_cast<float>((row[x0] >> shift) & 0xffu) * 0.003921569f;
            const float b = static_cast<float>((row[x1] >> shift) & 0xffu) * 0.003921569f;
            return a + frac * (b - a);
        };
        return cS3DVector3{channel(0), channel(8), channel(16)};
    }
    void Release() override { delete this; }
private:
    std::uint32_t w_ = 0, h_ = 0;
    std::vector<std::uint32_t> pixels_;
};

class TerrainAdapter final : public ILightingTerrain {
public:
    void AddRef() override {}
    void Release() override {}
    bool IsInitialized() override { return g_terrain && g_terrain->IsInitialized(); }
    float CellSize() override { return kTerrainCellWidth; }
    int CellCountX() override { return g_terrain ? static_cast<int>(g_terrain->CellCountX()) : 0; }
    int CellCountZ() override { return g_terrain ? static_cast<int>(g_terrain->CellCountZ()) : 0; }
    void GetAltitudes(const SC4RectInt& rect, float* out, std::ptrdiff_t rowStride, std::ptrdiff_t colStride) override {
        if (g_terrain) g_terrain->GetAltitudes(Rect(rect), out, rowStride, colStride);
    }
    void TerrainNormal(float x, float z, cS3DVector3& out) override {
        TerrainVector3 n{};
        if (g_terrain) g_terrain->GetLerpedNormal(x, z, n);
        out = cS3DVector3{n.x, n.y, n.z};
    }
    void DoLighting(const SC4RectInt& rect) override { if (g_terrain) g_terrain->DoLighting(Rect(rect)); }
    void RedisplayTerrain(bool a, bool b, const SC4RectInt& rect, std::uint32_t flags) override {
        if (g_terrain) g_terrain->RedisplayTerrain(a, b, Rect(rect), static_cast<std::int32_t>(flags));
    }
private:
    static TerrainRect Rect(const SC4RectInt& r) { return TerrainRect{r.left, r.top, r.right, r.bottom}; }
};

class Host final : public ILightingHost {
public:
    cISCPropertyHolder* LightingProperties() override {
        void* resource = nullptr;
        const cGZPersistResourceKey key{kLightingExemplarType, kLightingExemplarGroup, kLightingExemplarInstance};
        if (!SL::spRM || !SL::spRM->GetResource(key, kLightingExemplarFilter, &resource, 0, nullptr) || !resource) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(resource);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        if (holder) holder->AddRef();
        exemplar->Release();
        return holder;
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    ILightingTerrain* TerrainMap() override { return &terrain_; }
    ILightingTerrain* Terrain() override { return &terrain_; }
    SC4ViewUtilities* ViewUtilities() override { return g_render ? g_render->GetViewUtilities() : nullptr; }
    cSC4RenderProperties* RenderProperties() override {
        static cSC4RenderProperties* properties = [] { auto* p = new cSC4RenderProperties(); p->AddRef(); return p; }();
        return properties;
    }
    int CurrentSeasonMonth() override {
        int year = 0, month = 1, day = 0, dayOfYear = 0, weekDay = 0;
        if (g_simulator) g_simulator->GetSimDate(&year, &month, &day, &dayOfYear, &weekDay);
        return month < 1 ? 0 : (month > 12 ? 11 : month - 1);
    }
    void ForceFullRedraw() override { g_relight = true; }
    ILightingColorMap* CreateColorMap(std::uint32_t resourceKey) override {
        PngDecoder decode = DecodeFunction();
        std::vector<std::uint8_t> bytes;
        if (!decode || !LoadResourceBytes(0x856ddbac, 0xa9179251, resourceKey, bytes)) return nullptr;
        cGZBuffer* buffer = decode(bytes);
        if (!buffer) return nullptr;
        auto* map = new ColorMap(buffer);
        buffer->Release();
        return map;
    }
    bool OccupantHasNetworkFlag(cISC4Occupant*, std::uint32_t) override { return false; }
private:
    TerrainAdapter terrain_;
};

Host& TheHost() { static Host host; return host; }

}  // namespace

IS4LightingManager* CreateNativeLightingManager() {
    sc4::SetLightingManagerHost(&TheHost());
    g_manager = new sc4::cSC4LightingManager(&TheHost());
    g_manager->AddRef();
    return CreateLightingManagerView(g_manager);
}

// SL::sp24HourClock vslot 0xe8(1) EnableDayNightTransitionNotifications, vslot 0x28 SetVisualTimeMode; the original also stores
// GetVisualTimeMode (vslot 0x30) in the preference byte SL::spSC4 vslot 0x130 returns (the native preferences are not kept).
void SetCityDayNightMode(int mode) {
    if (!g_clock) return;
    g_clock->EnableDayNightTransitionNotifications(true);
    if (mode >= 0) g_clock->SetVisualTimeMode(static_cast<sc4::cSC424HourClock::VisualTimeMode>(mode));
}

void SetLightingClock(sc4::cSC424HourClock* clock) {
    g_clock = clock;
    if (clock && std::getenv("SC4_DEBUG")) std::fprintf(stderr, "clock: game %u:%02u visual %u:%02u mode %d paused %d\n", clock->GetHour(), clock->GetMinute(), clock->GetVisualHour(), clock->GetVisualMinute(), int(clock->GetVisualTimeMode()), clock->IsPaused());
}

cSC4LightingManager* LightingManager() { return g_inited ? g_manager : nullptr; }

void InitCityLighting(sc4::cSTETerrain* terrain, sc4::cSC43DRender* render, sc4::cSC4Simulator* simulator) {
    g_terrain = terrain;
    g_render = render;
    g_simulator = simulator;
    if (!g_manager || !terrain || g_inited) return;
    g_inited = true;
    g_manager->Init();
    if (!g_clock) {   // the city carries no clock component natively: run the recovered 24-hour clock ourselves, starting at the HUD's 10:00
        static ClockHost clockHost;
        static sc4::cSC424HourClock ownClock(&clockHost);
        ownClock.Init();
        ownClock.SetHour(10);
        ownClock.SetMinute(0);
        ownClock.SetVisualTimeMode(sc4::cSC424HourClock::kVisualNoon);   // pin then release: the visual time takes the game time at once
        ownClock.SetVisualTimeMode(sc4::cSC424HourClock::kVisualReal);
        g_clock = &ownClock;
    }
    {   // the terrain-changed message (posted to the city, so it would arrive after the models are made): the manager needs the terrain's size and heights now
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(sc4::kLightingMessageTerrainChanged);
        g_manager->DoMessage(message);
        message->Release();
    }
    SL::spLightingManager = static_cast<cIGZUnknown*>(static_cast<sc4::cISC4LightingManager*>(g_manager));   // the renderer publishes it (cSC43DRender::Init)
    std::fprintf(stderr, "[Runtime Wire] Sub-system SL::spLightingManager successfully bound to Memory Address %p\n", static_cast<void*>(g_manager));
    float hour = 12.0f;
    if (const char* env = std::getenv("SC4_TIMEOFDAY")) hour = static_cast<float>(std::atof(env));
    else hour = SunHour();
    g_manager->SetTimeOfDay(hour);
    g_lastHour = hour;
    TerrainRect all{0, 0, static_cast<std::int32_t>(terrain->CellCountX()), static_cast<std::int32_t>(terrain->CellCountZ())};
    terrain->DoLighting(all);
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "lighting: manager ready, time %.1f night=%d\n", g_manager->GetTimeOfDay(), g_manager->IsNight());
}

void ShutdownCityLighting() {
    g_terrain = nullptr;
    g_render = nullptr;
    g_simulator = nullptr;
    g_inited = false;
    if (g_manager && SL::spLightingManager == static_cast<cIGZUnknown*>(static_cast<sc4::cISC4LightingManager*>(g_manager))) SL::spLightingManager = nullptr;
    if (g_manager) {
        g_manager->Release();
        g_manager = nullptr;
    }
}

void TickLighting() {
    if (!g_inited || !g_manager || !g_terrain) return;
    static std::time_t lastCheck = 0;
    const std::time_t now = std::time(nullptr);
    if (now == lastCheck) return;
    lastCheck = now;
    if (g_clock) g_clock->OnIdle(0);   // the game clock advances with real time at its magnification
    float hour = g_lastHour;
    if (!std::getenv("SC4_TIMEOFDAY")) {   // the day cycle follows the game clock: the simulator's hour and minute counters (the HUD clock)
        hour = SunHour();
    }
    if (hour != g_lastHour) {
        g_lastHour = hour;
        if (g_manager->SetTimeOfDay(hour)) g_relight = true;
    }
    if (g_relight) {
        g_relight = false;
        g_terrain->RedisplayTerrain(false, false, TerrainRect{0, 0, static_cast<std::int32_t>(g_terrain->CellCountX()), static_cast<std::int32_t>(g_terrain->CellCountZ())}, 0);
    }
}

}  // namespace native
