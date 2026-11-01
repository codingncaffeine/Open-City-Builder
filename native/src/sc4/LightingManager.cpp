// cSC4LightingManager — implementation. See LightingManager.h for the layout, the quirks and the seams.
// Every function is a hand port of the exported decompile; the SIMD-heavy ones (SetTimeOfDay's update test,
// GetColor's two directional lobes, GetModelLight's corner normals) were cross-checked against the disassembly
// where the decompiler lost the lane structure. Compile without FMA contraction: results follow the original
// single-precision operation order.
#pragma STDC FP_CONTRACT OFF

#include "sc4/LightingManager.h"

#include <cmath>
#include <cstring>

#include "rz/cRZMessage2Standard.h"
#include "sc4/DrawContext.h"
#include "sc4/Occupant.h"
#include "sc4/RenderProperties.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ViewUtilities.h"
#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"

namespace {

// cSC4RenderProperties property indices the manager reads (see src/sc4/RenderPropertyDefaults.inc; the ids and
// the registration order coincide for these three).
constexpr std::uint32_t kLightingUpdateDeltaPropertyIndex = 6;   // floats: kLightingUpdateDelta
constexpr std::uint32_t kShadowQualityPropertyIndex = 12;        // ints:   kShadowQuality
constexpr std::uint32_t kRenderSnapshotPropertyIndex = 28;       // bools:  kRenderSnapshot

// The object's own vtable layout depends on these three, so pin the neighbour's record layout down.
static_assert(offsetof(sc4::cSC4RenderProperty<bool>, value) == 0x38, "kRenderSnapshot lives at bools + 0x738");
static_assert(sizeof(sc4::cSC4RenderProperty<bool>) == 0x40, "cSC4RenderProperty<bool> stride");
static_assert(offsetof(sc4::cSC4RenderProperty<float>, value) == 0x38, "kLightingUpdateDelta lives at floats + 0x1e8");
static_assert(sizeof(sc4::cSC4RenderProperty<float>) == 0x48, "cSC4RenderProperty<float> stride");

// ---------------------------------------------------------------------------------------------
// The original's float helpers (every one of them is inlined at each call site in the binary).
// ---------------------------------------------------------------------------------------------

std::uint32_t FloatBits(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

// floor(x) as the original computes it: (int)(x - 0.5 + 12582912.0) with one correction step for exact values
// (0x1007783??, 0x100779fe4, 0x10077a334, 0x10077a6e8, ...). The magic add rounds to nearest-even, so the sequence
// is exactly floor for every input the game passes (positive cell coordinates and texture coordinates).
std::int32_t LightingFloor(float v) {
    const std::uint32_t bits = FloatBits(v - 0.5f + 12582912.0f);
    const std::int32_t rounded = static_cast<std::int32_t>(bits) - 0x4b400000;
    const std::int32_t candidate = rounded + 1;
    const float asFloat = static_cast<float>(candidate);
    if (asFloat != v || std::isnan(asFloat) || std::isnan(v)) return rounded;
    return candidate;
}

// ceil(x), the original's second index in the same sequences (used by the colour-map sampler and the shadow rect).
std::int32_t LightingCeil(float v) {
    const std::uint32_t bits = FloatBits(v + 0.5f + 12582912.0f);
    const std::int32_t rounded = static_cast<std::int32_t>(bits) - 0x4b400000;
    const std::int32_t candidate = rounded - 1;
    const float asFloat = static_cast<float>(candidate);
    if (asFloat != v || std::isnan(asFloat) || std::isnan(v)) return rounded;
    return candidate;
}

// The colour -> byte conversion every writer uses (0x10077a292 and friends): the float is biased by 49152.0
// (1.5 * 2^15, whose mantissa resolution is 1/256), reinterpreted as an integer and the bias' bits subtracted;
// i - (i >> 8) then folds the top of the range onto 255. v = 1.0 gives 255, v = 0.5 gives 128.
std::uint8_t LightingByte(float v) {
    const std::uint32_t i = FloatBits(v + 49152.0f) - 0x47400000u;
    return static_cast<std::uint8_t>(i - (i >> 8));
}

// (sin, cos) of one float argument. The original calls ___sincosf_stret (x86_64 returns sin in the low 4 bytes,
// cos in the high 4); every call site here follows that convention.
void SinCos(float angle, float& sinOut, float& cosOut) {
    sinOut = std::sin(angle);
    cosOut = std::cos(angle);
}

// The clamp every terrain query makes: min(value, extent - 0.001) with the extent difference computed in double
// precision, then 0 for negatives (0x1007784c9, 0x100778a90).
float ClampToTerrain(float value, float extent) {
    const float maximum = static_cast<float>(static_cast<double>(extent) - 0.001);
    const float clamped = value <= maximum ? value : maximum;
    return value < 0.0f ? 0.0f : clamped;
}

SC4RectInt ToRectInt(const sc4::SC4RectT<int>& rect) { return SC4RectInt{rect.x0, rect.z0, rect.x1, rect.z1}; }

// ---------------------------------------------------------------------------------------------
// LightingTunables: the original's file-static globals.
// ---------------------------------------------------------------------------------------------
sc4::LightingTunables& TunablesSlot() {
    static sc4::LightingTunables tunables;
    return tunables;
}

// ILightingHost (the original's SL::sp* globals).
sc4::ILightingHost*& HostSlot() {
    static sc4::ILightingHost* host = nullptr;
    return host;
}

// FindHillShadows()::crest — the function-local static vector at 0x1007772f4 (shared by every instance).
std::vector<float>& CrestScratch() {
    static std::vector<float> crest;
    return crest;
}

// GetS3DPropertyValue(const cISCPropertyHolder*, unsigned, int, float*): reads a float vector property and accepts
// it only when its size is exactly `floats`. The vector is a function-local static in the original (0x100b17968),
// so overlapping reads see the same storage.
bool ReadVectorProperty(const sc4::cISCPropertyHolder* holder, std::uint32_t id, std::size_t floats, float* out) {
    static std::vector<float> value;
    if (!sc4::cSCPropertyHelp::GetPropertyValue(holder, id, value)) return false;
    if (value.size() != floats) return false;
    std::memcpy(out, value.data(), floats * sizeof(float));
    return true;
}

// Linspace sample of a vector table at t (the inlined sampler of SetTimeDependentLightingValues, 0x100777a4c):
// x = (size - 1) * t, index = floor(x), lerp toward the next entry with (x - index).
cS3DVector3 SampleVectorTable(const std::vector<cS3DVector3>& table, float t) {
    if (table.empty()) return {};
    const std::int32_t segments = static_cast<std::int32_t>(table.size()) - 1;
    if (segments == 0) return table[0];
    const float x = static_cast<float>(segments) * t;
    std::int32_t index = LightingFloor(x);
    if (index < 0) index = 0;
    if (index > segments) index = segments;
    const float frac = x - static_cast<float>(index);
    if (!(0.0f < frac) || index >= segments) return table[static_cast<std::size_t>(index)];
    const cS3DVector3& current = table[static_cast<std::size_t>(index)];
    const cS3DVector3& next = table[static_cast<std::size_t>(index) + 1];
    const float inverse = 1.0f - frac;
    return cS3DVector3{frac * next.x + inverse * current.x, frac * next.y + inverse * current.y,
                       frac * next.z + inverse * current.z};
}

float SampleFloatTable(const std::vector<float>& table, float t) {
    if (table.empty()) return 0.0f;
    const std::int32_t segments = static_cast<std::int32_t>(table.size()) - 1;
    if (segments == 0) return table[0];
    const float x = static_cast<float>(segments) * t;
    std::int32_t index = LightingFloor(x);
    if (index < 0) index = 0;
    if (index > segments) index = segments;
    const float frac = x - static_cast<float>(index);
    const float value = table[static_cast<std::size_t>(index)];
    if (!(0.0f < frac) || index >= segments) return value;
    const float next = table[static_cast<std::size_t>(index) + 1];
    return value + frac * (next - value);
}

// ---------------------------------------------------------------------------------------------
// MakeTestPattern(cImageARGB8**) @ 0x10077648c: a 0x40 x 0x40 checkerboard used only when the exemplar sets
// kTestEnvironmentMap. Pixels are 0xff00ff00 / 0xff000000 by half and | 0xff on the right half.
// ---------------------------------------------------------------------------------------------
class LightingTestColorMap final : public sc4::ILightingColorMap {
public:
    LightingTestColorMap() : pixels_(0x40u * 0x40u) {
        for (std::uint32_t y = 0; y < 0x40; ++y) {
            const std::uint32_t row = y > 0x20 ? 0xff00ff00u : 0xff000000u;
            for (std::uint32_t x = 0; x < 0x40; ++x)
                pixels_[y * 0x40u + x] = row | (x > 0x20 ? 0xffu : 0u);
        }
    }
    std::uint32_t Width() const override { return 0x40; }
    std::uint32_t Height() const override { return 0x40; }
    std::int32_t Stride() const override { return 0x40; }
    const std::uint32_t* Pixels() const override { return pixels_.data(); }
    std::size_t PixelCount() const override { return pixels_.size(); }
    // The same single-row bilinear-in-x sample the real cImageARGB8 does (0x10077b15a).
    cS3DVector3 SampleColorLerp(float u, float v) const override {
        const float fx = u * 64.0f;
        std::int32_t x0 = LightingFloor(fx);
        std::int32_t x1 = LightingCeil(fx);
        std::int32_t y = LightingFloor(v * 64.0f);
        if (y < 0) y = 0;
        if (y > 63) y = 63;
        const float frac = fx - static_cast<float>(LightingFloor(fx));
        if (x0 < 0) x0 = 0;
        if (x1 < 0) x1 = 0;
        if (x0 > 63) x0 = 63;
        if (x1 > 63) x1 = 63;
        const std::uint32_t* row = pixels_.data() + static_cast<std::size_t>(y) * 64u;
        const std::uint32_t c0 = row[x0];
        const std::uint32_t c1 = row[x1];
        auto channel = [frac](std::uint32_t a, std::uint32_t b, int shift) {
            const float low = static_cast<float>((a >> shift) & 0xffu) * 0.003921569f;
            const float high = static_cast<float>((b >> shift) & 0xffu) * 0.003921569f;
            return low + frac * (high - low);
        };
        return cS3DVector3{channel(c0, c1, 0), channel(c0, c1, 8), channel(c0, c1, 16)};
    }
    void Release() override { delete this; }

private:
    std::vector<std::uint32_t> pixels_;
};

// The renderer's two-call view (src/sc4/DrawContext.h) over the recovered manager. It owns the manager because the
// renderer deletes the view (cSC43DRender::Shutdown).
class LightingManagerView final : public IS4LightingManager {
public:
    // The view holds a reference: the manager is shared with the host that created it.
    explicit LightingManagerView(sc4::cSC4LightingManager* manager) : manager_(manager) { manager_->AddRef(); }
    ~LightingManagerView() override { manager_->Release(); }
    void GetGlobalLight(cS3DVector3& out) override { manager_->GetGlobalLight(&out); }
    bool GetModelLight(s3d::cIS3DModelInstance* instance, cS3DVector3& out) override {
        return manager_->GetModelLight(instance, &out);
    }

private:
    sc4::cSC4LightingManager* manager_;
};

// The message payloads: the original asks the message for its 64-bit data1/data2 and keeps the low 32 bits.
std::uint32_t MessageData1(cIGZMessage2* message) {
    return static_cast<std::uint32_t>(static_cast<cIGZMessage2Standard*>(message)->GetData1());
}

std::uint32_t MessageData2(cIGZMessage2* message) {
    return static_cast<std::uint32_t>(static_cast<cIGZMessage2Standard*>(message)->GetData2());
}

// The network-occupant flags GetModelLight tests (cISC4NetworkOccupant vslot 0x40).
constexpr std::uint32_t kLightingNetworkFlagSkipShadows = 0x40000;
// The occupant groups / types GetModelLight classifies (0x1007781c7..0x10077822d).
constexpr std::uint32_t kLightingOccupantGroupFloraShadows = 0x5004;
constexpr std::uint32_t kLightingOccupantTypeFlora = 0x74758926;
constexpr std::uint32_t kLightingOccupantTypeAutomataA = 0x897f7ec2;
constexpr std::uint32_t kLightingOccupantTypeAutomataB = 0xc7cc75f1;
constexpr std::uint32_t kLightingOccupantTypeAutomataC = 0x496e5783;
constexpr std::uint32_t kLightingOccupantTypeAutomataD = 0x480e6151;

}  // namespace

namespace sc4 {

LightingTunables& LightingManagerTunables() { return TunablesSlot(); }
void ResetLightingManagerTunables() { TunablesSlot() = LightingTunables{}; }

ILightingHost* LightingManagerHost() { return HostSlot(); }
void SetLightingManagerHost(ILightingHost* host) { HostSlot() = host; }

IS4LightingManager* CreateLightingManagerView(cSC4LightingManager* manager) {
    if (!manager) return nullptr;
    return new LightingManagerView(manager);
}

// ===================================================================================== identity

cSC4LightingManager::cSC4LightingManager(ILightingHost* host) : host_(host) {
    // 0x10077563e: +0x30 = 12.0 (noon), +0xc8..+0xd0 = 1, +0xd4..+0xdc = 1, +0xf8 = 1, +0x124.. = (1, -1, -1),
    // +0x1c8..+0x1d0 = 1, +0x1d4..+0x208 = 0, +0x238 = 0, every vector empty and every other field zero. The
    // member initialisers already hold those values; the original leaves +0x39..+0x3f untouched, the port zeroes
    // them so a copy is deterministic.
    if (!host_) host_ = LightingManagerHost();
}

cSC4LightingManager::~cSC4LightingManager() {
    // 0x1007757cc: the terrain (+0x28), then the terrain map (+0x20), then the property holder (+0x18).
    if (terrain_) terrain_->Release();
    if (terrainMap_) terrainMap_->Release();
    if (properties_) properties_->Release();
}

bool cSC4LightingManager::QueryInterface(std::uint32_t iid, void** out) {
    // 0x10077680c: 1 and 0x681bd52a answer the primary object, 0x452294aa the message target (+8); everything
    // else writes nothing. A hit takes one reference through the primary count.
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cISC4LightingManager) {
        *out = static_cast<cISC4LightingManager*>(this);
    } else if (iid == kLightingIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// 0x10077684e: the primary count at +0x14. The original's message-target thunk increments a separate count at
// +0x0c; the port keeps one count (a port deviation: nothing in the recovered code takes a message-target reference
// without going through QueryInterface, which the original also answers through the primary AddRef).
std::uint32_t cSC4LightingManager::AddRef() { return ++refCount_; }

// 0x10077686a: below 2 the original calls its deleting destructor (vslot 0xf0), so the last release frees the
// object. Heap-allocate the manager before releasing it to zero.
std::uint32_t cSC4LightingManager::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

// ===================================================================================== lifecycle

bool cSC4LightingManager::Init() {
    // 0x100775900: idempotent; the first call reads the render properties, the lighting exemplar and the colour
    // maps and registers the seven notifications.
    if (initialized_) return false;
    if (host_) {
        if (cSC4RenderProperties* renderProperties = host_->RenderProperties())
            hillShadows_ = renderProperties->IntValue(kShadowQualityPropertyIndex) > 1;
        cISCPropertyHolder* holder = host_->LightingProperties();
        if (holder != properties_) {
            if (holder) holder->AddRef();
            cISCPropertyHolder* previous = properties_;
            properties_ = holder;
            if (previous) previous->Release();
        }
    }
    ReadProperties();
    const LightingTunables& tunables = LightingManagerTunables();
    if (!tunables.testEnvironmentMap) {
        if (host_) {
            colorMaps_[0] = host_->CreateColorMap(kLightingColorMap0);
            colorMaps_[1] = host_->CreateColorMap(kLightingColorMap1);
            colorMaps_[2] = host_->CreateColorMap(kLightingColorMap2);
            colorMaps_[3] = host_->CreateColorMap(kLightingColorMap3);
        }
        if (ILightingColorMap* map = colorMaps_[2]) {
            // +0x120: the darkest red of the third map, as 0..1 (an empty map keeps 1.0).
            if (map->PixelCount() == 0) {
                environmentFloor_ = 1.0f;
            } else {
                const std::uint32_t* pixels = map->Pixels();
                std::uint8_t floorValue = 0xff;
                for (std::size_t i = 0; i < map->PixelCount(); ++i) {
                    const std::uint8_t red = static_cast<std::uint8_t>(pixels[i] & 0xffu);
                    if (red < floorValue) floorValue = red;
                }
                environmentFloor_ = static_cast<float>(floorValue) * 0.003921569f;
            }
        }
    } else {
        colorMaps_[0] = new LightingTestColorMap();
        colorMaps_[1] = new LightingTestColorMap();
    }
    if (host_) {
        if (cIGZMessageServer2* server = host_->MessageServer()) {
            const std::uint32_t messages[] = {
                kLightingMessageRepaintRequest,     kLightingMessageRenderPropertiesChanged,
                kLightingMessageTerrainChanged,     kLightingMessageZoomRotationChanged,
                kLightingMessageTerrainRegionBegin, kLightingMessageTerrainRegionEnd,
                kLightingMessageTerrainRegionUpdated,
            };
            for (std::uint32_t message : messages)
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
        }
    }
    initialized_ = true;
    return true;
}

bool cSC4LightingManager::Shutdown() {
    // 0x100776674: unregisters the notifications, drops the property holder and the terrain map and deletes the
    // four colour maps. The terrain reference (+0x28) is deliberately left to the destructor (quirk kept).
    if (!initialized_) return false;
    if (host_) {
        if (cIGZMessageServer2* server = host_->MessageServer()) {
            const std::uint32_t messages[] = {
                kLightingMessageRepaintRequest,     kLightingMessageRenderPropertiesChanged,
                kLightingMessageTerrainChanged,     kLightingMessageZoomRotationChanged,
                kLightingMessageTerrainRegionBegin, kLightingMessageTerrainRegionEnd,
                kLightingMessageTerrainRegionUpdated,
            };
            for (std::uint32_t message : messages)
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
        }
    }
    zoomRotationValid_ = false;
    if (properties_) {
        cISCPropertyHolder* previous = properties_;
        properties_ = nullptr;
        previous->Release();
    }
    if (terrainMap_) {
        ILightingTerrain* previous = terrainMap_;
        terrainMap_ = nullptr;
        previous->Release();
    }
    for (ILightingColorMap*& map : colorMaps_) {
        if (map) {
            map->Release();
            map = nullptr;
        }
    }
    initialized_ = false;
    return true;
}

// ===================================================================================== properties

void cSC4LightingManager::ReadProperties() {
    // 0x100775b44: each property is read only when the exemplar carries it, so the file-static defaults survive a
    // partial exemplar. The globals are shared by every instance, as in the original.
    LightingTunables& tunables = LightingManagerTunables();
    if (properties_) {
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropTextureFloor, tunables.textureFloor);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropUseEnvironmentMap, tunables.useEnvironmentMap);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropTestEnvironmentMap, tunables.testEnvironmentMap);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropTerrainShadowAmount,
                                          tunables.terrainShadowAmount);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropTestTerrainShadowing,
                                          tunables.testTerrainShadowing);
        float colour[3] = {0, 0, 0};
        if (ReadVectorProperty(properties_, kLightingPropDaytimeLight, 3, colour))
            tunables.daytimeLight = cS3DVector3{colour[0], colour[1], colour[2]};
        if (ReadVectorProperty(properties_, kLightingPropNighttimeLight, 3, colour))
            tunables.nighttimeLight = cS3DVector3{colour[0], colour[1], colour[2]};
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropFloraShadowAmount,
                                          tunables.floraTerrainShadowAmount);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropModelShadowAmount,
                                          tunables.modelTerrainShadowAmount);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropAutomataShadowAmount,
                                          tunables.automataTerrainShadowAmount);
        if (ReadVectorProperty(properties_, kLightingPropScatterColor, 3, colour))
            tunables.scatterColor = cS3DVector3{colour[0], colour[1], colour[2]};
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropScatterStrength, tunables.scatterStrength);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropUpdateDelta, tunables.lightingUpdateDelta);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropNightThreshold, tunables.nightThreshold);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropShadowStrength, tunables.shadowStrength);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropShadowThreshold, tunables.shadowThreshold);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropShadowTextureID, tunables.shadowTextureID);
        float offset[2] = {0, 0};
        if (ReadVectorProperty(properties_, kLightingPropShadowOffset, 2, offset))
            tunables.shadowOffset = cS3DVector2{offset[0], offset[1]};
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropShadowSize, tunables.shadowSize);
        if (ReadVectorProperty(properties_, kLightingPropShadowColor, 3, colour))
            tunables.shadowColour = cS3DVector3{colour[0], colour[1], colour[2]};
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropSunDirection, tunables.sunDirection);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropSunPitch, tunables.sunPitch);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropHillShadowStrength, tunables.hillShadowStrength);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropHillShadowWarp, tunables.hillShadowWarp);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropHillShadowFadeDepth,
                                          tunables.hillShadowFadeDepth);
        cSCPropertyHelp::GetPropertyValue(properties_, kLightingPropNightShadowFade, tunables.nightShadowFade);
    }
    // ---- apply ---------------------------------------------------------------------------------
    globalLight_ = tunables.daytimeLight;
    sampledLight_ = tunables.daytimeLight;
    floorLight_ = cS3DVector3{tunables.daytimeLight.x * tunables.textureFloor,
                              tunables.daytimeLight.y * tunables.textureFloor,
                              tunables.daytimeLight.z * tunables.textureFloor};
    scatterStrength_ = tunables.scatterStrength;
    dayNightColors_.clear();
    dayNightColors_.push_back(tunables.nighttimeLight);
    dayNightColors_.push_back(tunables.nighttimeLight);
    dayNightColors_.push_back(tunables.daytimeLight);
    dayNightColors_.push_back(tunables.daytimeLight);
    dayNightColors_.push_back(tunables.daytimeLight);
    dayNightColors_.push_back(tunables.daytimeLight);
    dayNightColors_.push_back(tunables.nighttimeLight);
    dayNightColors_.push_back(tunables.nighttimeLight);
    scatterStrengths_.clear();
    scatterStrengths_.push_back(0.0f);
    scatterStrengths_.push_back(tunables.scatterStrength);
    scatterStrengths_.push_back(tunables.scatterStrength);
    scatterStrengths_.push_back(0.0f);
    scatterColors_.clear();
    scatterColors_.push_back(tunables.scatterColor);
    // Sun direction (0x100775b44 tail): the azimuth and pitch are degrees and sincos returns (sin, cos).
    float sinPitch = 0.0f, cosPitch = 1.0f;
    SinCos(tunables.sunPitch * 0.017453292f, sinPitch, cosPitch);
    float sinAzimuth = 0.0f, cosAzimuth = 1.0f;
    SinCos(tunables.sunDirection * 0.017453292f, sinAzimuth, cosAzimuth);
    sunDirectionRaw_ = cS3DVector3{sinAzimuth * cosPitch, -sinPitch, -(cosAzimuth * cosPitch)};
}

// ===================================================================================== messages

bool cSC4LightingManager::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    switch (message->GetType()) {
        case kLightingMessageRepaintRequest: {   // 0xc53d10aa
            ReadProperties();
            return true;
        }
        case kLightingMessageRenderPropertiesChanged: {   // 0xc9f775bb
            if (host_) {
                if (cSC4RenderProperties* renderProperties = host_->RenderProperties()) {
                    LightingManagerTunables().lightingUpdateDelta =
                        renderProperties->FloatValue(kLightingUpdateDeltaPropertyIndex);
                    const bool wasHillShadows = hillShadows_;
                    hillShadows_ = renderProperties->IntValue(kShadowQualityPropertyIndex) > 1;
                    if (wasHillShadows == hillShadows_) return true;
                    int zoom = 0, rotation = 0;
                    if (SC4ViewUtilities* utilities = host_->ViewUtilities()) {
                        zoom = utilities->Zoom();
                        rotation = utilities->Rotation();
                    }
                    DoZoomAndRotationChange(zoom, rotation);
                    cIGZMessageServer2* server = host_->MessageServer();
                    cRZMessage2Standard* changed = new cRZMessage2Standard();
                    changed->AddRef();
                    changed->SetType(kLightingMessageLightingChanged);
                    changed->SetData1(FloatBits(nightFade_));
                    changed->SetData2(FloatBits(nightShadowFade_));
                    if (server) server->MessageSend(changed);
                    host_->ForceFullRedraw();
                    changed->Release();
                }
            }
            return true;
        }
        case kLightingMessageTerrainChanged: {   // 0x2b96b3ea
            if (host_) {
                ILightingTerrain* terrain = host_->Terrain();
                if (terrain != terrain_) {
                    if (terrain) terrain->AddRef();
                    ILightingTerrain* previous = terrain_;
                    terrain_ = terrain;
                    if (previous) previous->Release();
                }
                if (terrain_) accumulating_ = false;
                ILightingTerrain* map = host_->TerrainMap();
                if (map != terrainMap_) {
                    if (map) map->AddRef();
                    ILightingTerrain* previous = terrainMap_;
                    terrainMap_ = map;
                    if (previous) previous->Release();
                }
                if (terrainMap_) {
                    cellSize_ = terrainMap_->CellSize();
                    inverseCellSize_ = 1.0f / cellSize_;
                    cellCountX_ = static_cast<float>(terrainMap_->CellCountX());
                    cellCountZ_ = static_cast<float>(terrainMap_->CellCountZ());
                    cellCountXPlus1_ = cellCountX_ + 1.0f;
                    cellCountZPlus1_ = cellCountZ_ + 1.0f;
                    terrainWidth_ = cellCountX_ * cellSize_;
                    terrainDepth_ = cellSize_ * cellCountZ_;
                    const std::size_t vertices = static_cast<std::size_t>(cellCountZPlus1_ * cellCountXPlus1_);
                    heights_.resize(vertices);
                    crests_.resize(vertices);
                    int zoom = 0, rotation = 0;
                    if (SC4ViewUtilities* utilities = host_->ViewUtilities()) {
                        zoom = utilities->Zoom();
                        rotation = utilities->Rotation();
                    }
                    DoZoomAndRotationChange(zoom, rotation);
                }
            }
            return true;
        }
        case kLightingMessageZoomRotationChanged: {   // 0xa6b79601: two ints in the message's void2
            auto* standard = static_cast<cIGZMessage2Standard*>(message);
            const int* values = static_cast<const int*>(standard->GetVoid2());
            if (values) DoZoomAndRotationChange(values[0], values[1]);
            return true;
        }
        case kLightingMessageTerrainRegionBegin: {   // 0xea3b0af1
            accumulating_ = true;
            accumulateRect_ = SC4RectT<int>{static_cast<int>(cellCountXPlus1_), static_cast<int>(cellCountZPlus1_), 0,
                                            0};
            return true;
        }
        case kLightingMessageTerrainRegionEnd: {   // 0x6a3b0afd
            accumulating_ = false;
            if (!hillShadows_ || !terrain_) return true;
            if (accumulateRect_.x1 < accumulateRect_.x0) return true;
            if (!terrain_->IsInitialized()) return true;
            const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(accumulateRect_.z0) * strideZ_ +
                                          static_cast<std::ptrdiff_t>(accumulateRect_.x0) * strideX_ + heightBase_;
            FillHeights(accumulateRect_, offset);
            FindHillShadows();
            if (hillRect_.z0 < 0) return true;
            int rotation = 0;
            if (SC4ViewUtilities* utilities = host_ ? host_->ViewUtilities() : nullptr) rotation = utilities->Rotation();
            SC4RectInt terrainRect = ToRectInt(accumulateRect_);
            HillToTerrainRect(rotation, static_cast<int>(cellCountXPlus1_), static_cast<int>(cellCountZPlus1_),
                              ToRectInt(hillRect_), terrainRect);
            accumulateRect_ = SC4RectT<int>{terrainRect.left, terrainRect.top, terrainRect.right, terrainRect.bottom};
            terrain_->DoLighting(terrainRect);
            terrain_->RedisplayTerrain(false, false, terrainRect, 0);
            return true;
        }
        case kLightingMessageTerrainRegionUpdated: {   // 0x278e6003: two packed 16-bit cell coordinates
            if (!terrain_ || !hillShadows_) return true;
            const std::uint32_t data1 = MessageData1(message);
            const std::uint32_t data2 = MessageData2(message);
            SC4RectT<int> rect{static_cast<int>(data1 >> 16), static_cast<int>(data2 >> 16),
                               static_cast<int>(data1 & 0xffffu), static_cast<int>(data2 & 0xffffu)};
            if (accumulating_) {
                accumulateRect_.UnionRect(accumulateRect_, rect);
                return true;
            }
            if (!terrain_->IsInitialized()) return true;
            const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(rect.z0) * strideZ_ +
                                          static_cast<std::ptrdiff_t>(rect.x0) * strideX_ + heightBase_;
            FillHeights(rect, offset);
            FindHillShadows();
            if (hillRect_.z0 < 0) return true;
            int rotation = 0;
            if (SC4ViewUtilities* utilities = host_ ? host_->ViewUtilities() : nullptr) rotation = utilities->Rotation();
            SC4RectInt terrainRect = ToRectInt(accumulateRect_);
            HillToTerrainRect(rotation, static_cast<int>(cellCountXPlus1_), static_cast<int>(cellCountZPlus1_),
                              ToRectInt(hillRect_), terrainRect);
            accumulateRect_ = SC4RectT<int>{terrainRect.left, terrainRect.top, terrainRect.right, terrainRect.bottom};
            terrain_->DoLighting(terrainRect);
            return true;
        }
        default:
            return true;
    }
}

// The original passes `heights_.data() + heightBase_` to GetAltitudes; the helper keeps the pointer arithmetic
// (including a potentially negative heightBase_) in one place and bounds-guards the destination.
void cSC4LightingManager::FillHeights(const SC4RectT<int>& rect, std::ptrdiff_t offset) {
    if (!terrain_) return;
    if (heights_.empty()) return;
    float* destination = heights_.data();
    if (offset >= 0 && static_cast<std::size_t>(offset) < heights_.size()) destination += offset;
    terrain_->GetAltitudes(ToRectInt(rect), destination, strideZ_, strideX_);
}

// ===================================================================================== zoom / rotation

void cSC4LightingManager::DoZoomAndRotationChange(int zoom, int rotation) {
    // 0x100776e40: the three light directions are rotated into the view frame, the 2x2 rotation is rebuilt, the
    // hill-frame centre / axis are derived and the six directional light colours are sampled.
    rotatedSun_ = sunDirectionRaw_;
    diffuseDirection_ = cS3DVector3{0.666666687f, 0.333333343f, -0.666666687f};
    sideDirection_ = cS3DVector3{-0.577350259f, 0.577350259f, 0.577350259f};
    SC4ViewUtilities::RotateVector(&sideDirection_.x, rotation);
    SC4ViewUtilities::RotateVector(&diffuseDirection_.x, rotation);
    SC4ViewUtilities::RotateVector(&rotatedSun_.x, rotation);
    // The angle is computed in double precision (rotation * -pi/2) and then rounded to float.
    const float angle = static_cast<float>(static_cast<double>(rotation) * -1.5707963267948966);
    float sinAngle = 0.0f, cosAngle = 1.0f;
    SinCos(angle, sinAngle, cosAngle);
    rotationMatrix_[0] = cosAngle;      // +0xb8
    rotationMatrix_[1] = -sinAngle;     // +0xbc
    rotationMatrix_[2] = sinAngle;      // +0xc0
    rotationMatrix_[3] = cosAngle;      // +0xc4
    hillCentre_.x = terrainWidth_ * -0.5f;
    hillCentre_.y = 0.0f;
    hillCentre_.z = terrainDepth_ * 0.5f;
    hillAxis_.x = 0.0f;
    hillAxis_.y = 0.0f;
    hillAxis_.z = -(scatterStrength_ / (static_cast<float>(1 << (zoom & 0x1f)) * terrainDepth_));
    SC4ViewUtilities::RotateVector(&hillCentre_.x, rotation);
    SC4ViewUtilities::RotateVector(&hillAxis_.x, rotation);
    hillCentre_.x = terrainWidth_ * 0.5f + hillCentre_.x;
    hillCentre_.y = hillCentre_.y + 0.0f;
    hillCentre_.z = terrainDepth_ * 0.5f + hillCentre_.z;
    if (terrainMap_) {
        switch (rotation) {
            case 0:
                strideX_ = static_cast<int>(cellCountZPlus1_);
                strideZ_ = 1;
                heightBase_ = 0;
                break;
            case 1:
                strideX_ = -1;
                strideZ_ = static_cast<int>(cellCountXPlus1_);
                heightBase_ = static_cast<int>(cellCountXPlus1_ + -1.0f);
                break;
            case 2:
                strideX_ = static_cast<int>(-cellCountZPlus1_);
                strideZ_ = -1;
                heightBase_ = static_cast<int>(cellCountZPlus1_ * cellCountXPlus1_ + -1.0f);
                break;
            case 3:
                strideX_ = 1;
                strideZ_ = static_cast<int>(-cellCountXPlus1_);
                heightBase_ = static_cast<int>((cellCountZPlus1_ + -1.0f) * cellCountXPlus1_);
                break;
            default:
                break;
        }
        if (hillShadows_) {
            const SC4RectT<int> rect{0, 0, static_cast<int>(cellCountXPlus1_ + -1.0f),
                                     static_cast<int>(cellCountZPlus1_ + -1.0f)};
            FillHeights(rect, heightBase_);
            FindHillShadows();
        }
    }
    zoomRotationValid_ = true;
    // The six directional light colours. The original's -Y direction is the same (0, 1, 0) as +Y (the compiler
    // reused the constant because GetModelColor only looks at x and z), so directionalLight_[3] duplicates [2].
    directionalLight_[0] = GetModelColor(cS3DVector3{1.0f, 0.0f, 0.0f});
    directionalLight_[1] = GetModelColor(cS3DVector3{-1.0f, 0.0f, 0.0f});
    directionalLight_[2] = GetModelColor(cS3DVector3{0.0f, 1.0f, 0.0f});
    directionalLight_[3] = GetModelColor(cS3DVector3{0.0f, 1.0f, 0.0f});
    directionalLight_[4] = GetModelColor(cS3DVector3{0.0f, 0.0f, 1.0f});
    directionalLight_[5] = GetModelColor(cS3DVector3{0.0f, 0.0f, -1.0f});
}

void cSC4LightingManager::FindHillShadows() {
    // 0x1007772f4: walk every terrain row in the hill order, keep a running crest per column and mark the columns
    // whose crest changed. The crest buffer is the original's function-local static vector; +0x1f0 keeps the
    // running maximum before the slope offset.
    const LightingTunables& tunables = LightingManagerTunables();
    const float sunX = sunDirectionRaw_.x;
    if (!(0.0f <= sunX)) return;
    const float sunY = sunDirectionRaw_.y;
    const float sunZ = sunDirectionRaw_.z;
    const float depthCount = cellCountZPlus1_;    // +0x184
    const float widthCount = cellCountXPlus1_;    // +0x180
    std::vector<float>& crest = CrestScratch();
    const std::int32_t columns = static_cast<std::int32_t>(depthCount);
    crest.resize(static_cast<std::size_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(depthCount))));
    for (float& value : crest) value = 0.0f;
    const float slope = -sunZ / sunX;
    if (!(slope <= 1.0f) || !(-1.0f <= slope)) return;
    std::int32_t minColumn = -1;   // local_34
    std::int32_t maxColumn = 0;    // local_40
    std::int32_t minRow = -1;      // local_38
    std::int32_t maxRow = 0;       // local_3c
    if (static_cast<int>(static_cast<std::int64_t>(widthCount)) == 0) {
        minColumn = -1;
        maxColumn = 0;
        minRow = -1;
        maxRow = 0;
    } else {
        const float warpStep = cellSize_ * tunables.hillShadowWarp;
        const std::int32_t lastColumn = columns - 1;
        std::uint32_t row = 0;
        std::size_t rowBase = 0;
        while (row != static_cast<std::uint32_t>(static_cast<std::int64_t>(widthCount))) {
            std::int32_t candidateMaxColumn = maxColumn;
            std::int32_t candidateMinRow = minRow;
            std::int32_t candidateMinColumn = minColumn;
            if (columns != 0) {
                float* crestRow = crests_.data() + rowBase;
                std::int32_t changedFirst = -1;
                std::int32_t changedLast = 0;
                for (std::int32_t column = 0; column < columns; ++column) {
                    const std::size_t index = rowBase + static_cast<std::size_t>(column);
                    float value = crest[static_cast<std::size_t>(column)];
                    const float height = heights_[index];
                    if (value < height) {
                        crest[static_cast<std::size_t>(column)] = height;
                        value = crest[static_cast<std::size_t>(column)];
                    }
                    if (!(crestRow[column] == value)) {
                        crestRow[column] = crest[static_cast<std::size_t>(column)];
                        if (changedFirst < 0) changedFirst = column;
                        value = crest[static_cast<std::size_t>(column)];
                        changedLast = column;
                    }
                    crest[static_cast<std::size_t>(column)] = value + (sunY * warpStep) / sunX;
                }
                if (changedFirst >= 0) {
                    maxRow = static_cast<std::int32_t>(row);
                    candidateMaxColumn = changedLast;
                    candidateMinRow = maxRow;
                    candidateMinColumn = changedFirst;
                    if (-1 < minRow) {
                        if (changedFirst <= minColumn) minColumn = changedFirst;
                        candidateMinRow = minRow;
                        candidateMinColumn = minColumn;
                        if (candidateMaxColumn <= maxColumn) candidateMaxColumn = maxColumn;
                    }
                }
            }
            maxColumn = candidateMaxColumn;
            minRow = candidateMinRow;
            minColumn = candidateMinColumn;
            if (slope <= 0.0f) {
                for (std::int32_t column = lastColumn; column > 0; --column) {
                    crest[static_cast<std::size_t>(column)] =
                        crest[static_cast<std::size_t>(column)] -
                        (crest[static_cast<std::size_t>(column) - 1] - crest[static_cast<std::size_t>(column)]) *
                            slope;
                }
            } else if (lastColumn != 0) {
                for (std::int32_t column = 0; column != lastColumn; ++column) {
                    const float value = crest[static_cast<std::size_t>(column)];
                    crest[static_cast<std::size_t>(column)] =
                        (crest[static_cast<std::size_t>(column) + 1] - value) * slope + value;
                }
            }
            ++row;
            rowBase += static_cast<std::size_t>(columns);
        }
    }
    hillRect_ = SC4RectT<int>{minColumn, minRow, maxColumn, maxRow};
}

// ===================================================================================== time of day

bool cSC4LightingManager::SetTimeOfDay(float timeOfDay) {
    // 0x10077770a: stores the time; a time that went backwards means a new day, so the season month is re-read.
    // The lighting values are recomputed and, when the global light moved by more than kLightingUpdateDelta, the
    // night/day messages are posted and the previous values are replaced. Returns whether that happened.
    if (timeOfDay < timeOfDay_) {
        if (host_) seasonMonth_ = host_->CurrentSeasonMonth() - 1;
    }
    timeOfDay_ = timeOfDay;
    SetTimeDependentLightingValues();
    const float dx = previousLight_.x - globalLight_.x;
    const float dy = previousLight_.y - globalLight_.y;
    const float dz = previousLight_.z - globalLight_.z;
    const float distance = std::sqrt(dz * dz + dy * dy + dx * dx);
    const LightingTunables& tunables = LightingManagerTunables();
    const float updateDelta = tunables.lightingUpdateDelta;
    const float nightThreshold = tunables.nightThreshold;
    if (!(updateDelta < distance) || !terrainMap_) return updateDelta < distance;
    const float sampled = sampledLight_.x;
    const bool night = sampled < nightThreshold;
    cIGZMessageServer2* server = host_ ? host_->MessageServer() : nullptr;
    if (server) {
        // The night/day flip: +0xec holds the *previous* sampled red.
        const bool wasNight = previousNightValue_ < nightThreshold;
        const bool suppress = host_->RenderProperties() &&
                              host_->RenderProperties()->BoolValue(kRenderSnapshotPropertyIndex);
        if (wasNight != night && !suppress) {
            cRZMessage2Standard* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(nightThreshold <= previousNightValue_ ? kLightingMessageDusk
                                                                   : kLightingMessageDawn);
            server->MessagePost(message, false);
            message->Release();
        }
        const float floorValue = environmentFloor_;
        float denominator = nightThreshold - floorValue;
        if (denominator == 0.0f) denominator = 9.99999975e-05f;
        const float nightFade = (sampled - nightThreshold) / (1.0f - nightThreshold);
        nightFade_ = nightFade < 0.0f ? 0.0f : nightFade;
        const float shadowFade = 1.0f - (sampled - floorValue) / denominator;
        nightShadowFade_ = shadowFade < 0.0f ? 0.0f : shadowFade;
        cRZMessage2Standard* changed = new cRZMessage2Standard();
        changed->AddRef();
        changed->SetType(kLightingMessageLightingChanged);
        changed->SetData1(FloatBits(nightFade_));
        changed->SetData2(FloatBits(nightShadowFade_));
        server->MessageSend(changed);
        changed->Release();
    }
    previousLight_ = globalLight_;
    previousNightValue_ = sampledLight_.x;
    previousSampledY_ = sampledLight_.y;
    previousSampledZ_ = sampledLight_.z;
    night_ = night;
    return updateDelta < distance;
}

void cSC4LightingManager::SetTimeDependentLightingValues() {
    // 0x100777a4c: sample the day/night and scatter tables (or their colour maps) at the current time and month,
    // then apply the tint and derive the "floor" colour.
    LightingTunables& tunables = LightingManagerTunables();
    const float timeOfDay = timeOfDay_ / 24.0f;
    const float season = (static_cast<float>(static_cast<std::uint32_t>(seasonMonth_)) + 0.5f) / 12.0f;
    cS3DVector3 sampled{0, 0, 0};
    if (colorMaps_[2] == nullptr) {
        sampled = SampleVectorTable(dayNightColors_, timeOfDay);
    } else {
        sampled = colorMaps_[2]->SampleColorLerp(timeOfDay, season);
    }
    sampledLight_ = sampled;
    float scatter = 0.0f;
    cS3DVector3 scatterColour{0, 0, 0};
    const bool snapshot = host_ && host_->RenderProperties() &&
                          host_->RenderProperties()->BoolValue(kRenderSnapshotPropertyIndex);
    if (snapshot) {
        scatterColour = cS3DVector3{0, 0, 0};
        scatter = 0.0f;
    } else if (colorMaps_[3] == nullptr) {
        scatterColour = SampleVectorTable(scatterColors_, timeOfDay);
        scatter = SampleFloatTable(scatterStrengths_, timeOfDay);
    } else {
        scatterColour = colorMaps_[3]->SampleColorLerp(timeOfDay, season);
        scatter = SampleFloatTable(scatterStrengths_, timeOfDay);
    }
    scatterColor_ = scatterColour;
    scatterStrength_ = scatter;
    if (!tintEnabled_) {
        globalLight_ = sampledLight_;
    } else {
        globalLight_.x = tintStrength_ * (tintColor_.x - sampledLight_.x) + sampledLight_.x;
        globalLight_.y = tintStrength_ * (tintColor_.y - sampledLight_.y) + sampledLight_.y;
        globalLight_.z = tintStrength_ * (tintColor_.z - sampledLight_.z) + sampledLight_.z;
        scatterColor_.x = tintStrength_ * (tintColor_.x - scatterColour.x) + scatterColour.x;
        scatterColor_.y = tintStrength_ * (tintColor_.y - scatterColour.y) + scatterColour.y;
        scatterColor_.z = tintStrength_ * (tintColor_.z - scatterColour.z) + scatterColour.z;
    }
    floorLight_ = cS3DVector3{tunables.textureFloor * globalLight_.x, tunables.textureFloor * globalLight_.y,
                              tunables.textureFloor * globalLight_.z};
}

void cSC4LightingManager::SetLightingTint(const cS3DVector3& colour, float intensity) {
    // 0x100777fb6: store the tint and recompute; a time-of-day change that crosses the update threshold asks the
    // renderer for a full redraw.
    tintEnabled_ = true;
    tintColor_ = colour;
    tintStrength_ = intensity;
    if (SetTimeOfDay(timeOfDay_)) {
        if (host_) host_->ForceFullRedraw();
    }
}

void cSC4LightingManager::ResetLightingTint() {
    // 0x100778008
    tintEnabled_ = false;
    if (SetTimeOfDay(timeOfDay_)) {
        if (host_) host_->ForceFullRedraw();
    }
}

// ===================================================================================== queries

void cSC4LightingManager::GetGlobalLight(cS3DVector3* out) const {
    // 0x100778042
    if (out) *out = globalLight_;
}

cS3DVector3 cSC4LightingManager::GetShadowDirection() const {
    // 0x100778d48: the rotated sun direction (+0xa0), returned by value in the original.
    return rotatedSun_;
}

void cSC4LightingManager::GetShadowParams(float& strength, float& threshold, cS3DVector3& colour) {
    // 0x100778d6c
    const LightingTunables& tunables = LightingManagerTunables();
    strength = tunables.shadowStrength;
    threshold = tunables.shadowThreshold;
    colour = tunables.shadowColour;
}

float cSC4LightingManager::GetShadowStrengthFromLocation(const cS3DVector2& position) {
    // 0x100778d9c: how much shadow the crest casts at a world position; 0 when there is no crest.
    const LightingTunables& tunables = LightingManagerTunables();
    const std::int32_t column = LightingFloor(position.y * inverseCellSize_);
    const std::int32_t row = LightingFloor(inverseCellSize_ * position.x);
    const std::int32_t index = column * strideZ_ + heightBase_ + row * strideX_;
    if (index < 0 || static_cast<std::size_t>(index) >= crests_.size()) return 0.0f;
    if (static_cast<std::size_t>(index) >= heights_.size()) return 0.0f;
    const float difference = crests_[static_cast<std::size_t>(index)] - heights_[static_cast<std::size_t>(index)];
    float strength = 0.0f;
    if (difference <= tunables.hillShadowFadeDepth) {
        strength = 1.0f - tunables.nightShadowFade * nightShadowFade_;
        if (0.0f < difference) strength = strength * (1.0f - difference / tunables.hillShadowFadeDepth);
    }
    return strength;
}

void cSC4LightingManager::GetDecalShadowParams(cISCPropertyHolder* properties, std::uint32_t& textureID,
                                               float& strength, float& threshold, float& size,
                                               cS3DVector2& offset, cS3DVector3& colour) {
    // 0x100778e8a: the file-static shadow values, then the holder's overrides.
    const LightingTunables& tunables = LightingManagerTunables();
    textureID = tunables.shadowTextureID;
    strength = tunables.shadowStrength;
    threshold = tunables.shadowThreshold;
    size = tunables.shadowSize;
    offset = tunables.shadowOffset;
    colour = tunables.shadowColour;
    if (!properties) return;
    cSCPropertyHelp::GetPropertyValue(properties, kLightingPropShadowTextureID, textureID);
    cSCPropertyHelp::GetPropertyValue(properties, kLightingPropShadowStrength, strength);
    cSCPropertyHelp::GetPropertyValue(properties, kLightingPropShadowThreshold, threshold);
    cSCPropertyHelp::GetPropertyValue(properties, kLightingPropShadowSize, size);
    float vector[3] = {0, 0, 0};
    if (ReadVectorProperty(properties, kLightingPropShadowOffset, 2, vector))
        offset = cS3DVector2{vector[0], vector[1]};
    if (ReadVectorProperty(properties, kLightingPropShadowColor, 3, vector))
        colour = cS3DVector3{vector[0], vector[1], vector[2]};
}

// ===================================================================================== environment colours

cS3DVector3 cSC4LightingManager::GetModelColor(const cS3DVector3& direction) {
    // 0x1007788e6: samples the *second* colour map (+0x108) using the *first* map's dimensions (a quirk), with the
    // texture coordinates floored rather than interpolated. Without a map it falls back to GetColor(direction).
    if (!LightingManagerTunables().useEnvironmentMap || colorMaps_[0] == nullptr || colorMaps_[1] == nullptr)
        return GetColor(direction);
    ILightingColorMap* first = colorMaps_[0];
    ILightingColorMap* second = colorMaps_[1];
    const std::uint32_t width = first->Width();
    const std::uint32_t height = first->Height();
    // p = rotation row 0 . (x, z), q = rotation row 1 . (x, z); the y component is ignored (the original does the
    // same, which is why the -Y directional light equals +Y).
    const float p = rotationMatrix_[1] * direction.z + rotationMatrix_[0] * direction.x;
    const float q = rotationMatrix_[3] * direction.z + rotationMatrix_[2] * direction.x;
    const float fx = static_cast<float>(static_cast<double>(width) * ((static_cast<double>(p) + 1.0) * 0.5));
    const float fy = static_cast<float>(static_cast<double>(height) * ((static_cast<double>(q) + 1.0) * 0.5));
    std::int32_t ix = LightingFloor(fx);
    std::int32_t iy = LightingFloor(fy);
    if (static_cast<std::uint32_t>(ix) >= width) ix = static_cast<std::int32_t>(width) - 1;
    if (static_cast<std::uint32_t>(iy) >= height) iy = static_cast<std::int32_t>(height) - 1;
    if (ix < 0 || iy < 0 || second->PixelCount() == 0) return GetColor(direction);
    const std::size_t index = static_cast<std::size_t>(iy) * static_cast<std::size_t>(second->Stride()) +
                              static_cast<std::size_t>(ix);
    if (index >= second->PixelCount()) return GetColor(direction);
    const std::uint32_t pixel = second->Pixels()[index];
    return cS3DVector3{static_cast<float>(pixel & 0xffu) * 0.003921569f,
                       static_cast<float>((pixel >> 8) & 0xffu) * 0.003921569f,
                       static_cast<float>((pixel >> 16) & 0xffu) * 0.003921569f};
}

cS3DVector3 cSC4LightingManager::GetColor() {
    // 0x100779f6e: the colour of a light pointing straight up — GetColor((0, 1, 0)).
    return GetColor(cS3DVector3{0.0f, 1.0f, 0.0f});
}

cS3DVector3 cSC4LightingManager::GetColor(const cS3DVector3& direction) {
    // 0x100779fe4: rotate the direction into the view frame (x and z only), then either sample the first colour
    // map or combine two directional lobes over a 0.05 ambient base. Everything is clamped to 1.
    const float directionX = direction.x;
    const float directionY = direction.y;
    const float directionZ = direction.z;
    const float rotatedX = rotationMatrix_[1] * directionZ + rotationMatrix_[0] * directionX;
    const float rotatedZ = rotationMatrix_[3] * directionZ + rotationMatrix_[2] * directionX;
    cS3DVector3 result{0, 0, 0};
    if (LightingManagerTunables().useEnvironmentMap && colorMaps_[0] != nullptr &&
        colorMaps_[0]->PixelCount() != 0) {
        ILightingColorMap* map = colorMaps_[0];
        const std::uint32_t width = map->Width();
        const std::uint32_t height = map->Height();
        const float fx = static_cast<float>(static_cast<double>(width) * ((static_cast<double>(rotatedX) + 1.0) * 0.5));
        const float fy = static_cast<float>(static_cast<double>(height) * ((static_cast<double>(rotatedZ) + 1.0) * 0.5));
        std::int32_t ix = LightingFloor(fx);
        std::int32_t iy = LightingFloor(fy);
        if (static_cast<std::uint32_t>(ix) >= width) ix = static_cast<std::int32_t>(width) - 1;
        if (static_cast<std::uint32_t>(iy) >= height) iy = static_cast<std::int32_t>(height) - 1;
        if (ix < 0 || iy < 0) return result;
        const std::size_t index =
            static_cast<std::size_t>(iy) * static_cast<std::size_t>(map->Stride()) + static_cast<std::size_t>(ix);
        if (index >= map->PixelCount()) return result;
        const std::uint32_t pixel = map->Pixels()[index];
        result.x = static_cast<float>(pixel & 0xffu) * 0.003921569f;
        result.y = static_cast<float>((pixel >> 8) & 0xffu) * 0.003921569f;
        result.z = static_cast<float>((pixel >> 16) & 0xffu) * 0.003921569f;
    } else {
        result = cS3DVector3{0.05f, 0.05f, 0.05f};
        // First lobe: the side direction (+0x94, +0x98, +0x9c). r and g get dot + 0.05, b gets 0.8 * dot + 0.05.
        float dot = sideDirection_.y * directionY + sideDirection_.x * rotatedX + sideDirection_.z * rotatedZ;
        float red = 0.05f;
        float green = 0.05f;
        float blue = 0.05f;
        if (0.0f < dot) {
            red = dot + 0.05f;
            green = dot + 0.05f;
            blue = 0.8f * dot + 0.05f;
        }
        // Second lobe: the diffuse direction (+0x88, +0x8c, +0x90); r unchanged, g += 0.1 * dot, b += 0.5 * dot.
        const float diffuseDot =
            diffuseDirection_.y * directionY + diffuseDirection_.x * rotatedX + diffuseDirection_.z * rotatedZ;
        if (0.0f < diffuseDot) {
            green = green + 0.1f * diffuseDot;
            blue = blue + 0.5f * diffuseDot;
        }
        result = cS3DVector3{red, green, blue};
    }
    if (1.0f < result.x) result.x = 1.0f;
    if (1.0f < result.y) result.y = 1.0f;
    if (1.0f < result.z) result.z = 1.0f;
    return result;
}

cS3DVector3 cSC4LightingManager::GetColor(const cS3DVector3& normal, const cS3DVector3& position) {
    // 0x10077a334: with hill shadows on, bend the normal by the crest height above the position; then GetColor.
    if (hillShadows_) {
        const std::int32_t column = LightingFloor(position.x * inverseCellSize_);
        const std::int32_t row = LightingFloor(inverseCellSize_ * position.z);
        const std::int32_t index = row * strideZ_ + heightBase_ + column * strideX_;
        if (index >= 0 && static_cast<std::size_t>(index) < crests_.size()) {
            float height = crests_[static_cast<std::size_t>(index)] - position.y;
            if (0.0f < height) {
                height = static_cast<float>(static_cast<double>(height * LightingManagerTunables().hillShadowStrength) * 0.01);
                const float bentX = normal.x + diffuseDirection_.x * height;
                const float bentY = height * diffuseDirection_.y + normal.y;
                const float bentZ = normal.z + diffuseDirection_.z * height;
                float lengthSquared = bentY * bentY + bentX * bentX;
                lengthSquared = bentZ * bentZ + lengthSquared;
                if (0.0f < lengthSquared) {
                    const float inverse = 1.0f / std::sqrt(lengthSquared);
                    return GetColor(cS3DVector3{bentX * inverse, bentY * inverse, bentZ * inverse});
                }
            }
        }
    }
    return GetColor(normal);
}

void cSC4LightingManager::GetColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    // 0x100779fa8: the byte form of GetColor((0, 1, 0)).
    GetColor(cS3DVector3{0.0f, 1.0f, 0.0f}, r, g, b);
}

void cSC4LightingManager::GetColor(const cS3DVector3& direction, std::uint8_t& r, std::uint8_t& g,
                                   std::uint8_t& b) {
    // 0x10077a292
    const cS3DVector3 colour = GetColor(direction);
    r = LightingByte(colour.x);
    g = LightingByte(colour.y);
    b = LightingByte(colour.z);
}

void cSC4LightingManager::GetColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r,
                                   std::uint8_t& g, std::uint8_t& b) {
    // 0x10077a4d2: the byte form of GetColor(normal, position) (the normal is the direction).
    const cS3DVector3 colour = GetColor(normal, position);
    r = LightingByte(colour.x);
    g = LightingByte(colour.y);
    b = LightingByte(colour.z);
}

void cSC4LightingManager::GetTerrainColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r,
                                          std::uint8_t& g, std::uint8_t& b) {
    // 0x10077a6e8: the hill-shadow-bent colour, faded by the distance to the hill-frame plane.
    const LightingTunables& tunables = LightingManagerTunables();
    cS3DVector3 direction = normal;
    if (hillShadows_) {
        const std::int32_t column = LightingFloor(position.x * inverseCellSize_);
        const std::int32_t row = LightingFloor(inverseCellSize_ * position.z);
        const std::int32_t index = row * strideZ_ + heightBase_ + column * strideX_;
        if (index >= 0 && static_cast<std::size_t>(index) < crests_.size()) {
            float height = crests_[static_cast<std::size_t>(index)] - heights_[static_cast<std::size_t>(index)];
            if (0.0f < height) {
                height = static_cast<float>(static_cast<double>(height * tunables.hillShadowStrength) * 0.01);
                const float bentX = normal.x + diffuseDirection_.x * height;
                const float bentY = height * diffuseDirection_.y + normal.y;
                const float bentZ = normal.z + diffuseDirection_.z * height;
                float lengthSquared = bentY * bentY + bentX * bentX;
                lengthSquared = bentZ * bentZ + lengthSquared;
                if (0.0f < lengthSquared) {
                    const float inverse = 1.0f / std::sqrt(lengthSquared);
                    direction = cS3DVector3{bentX * inverse, bentY * inverse, bentZ * inverse};
                }
            }
        }
    }
    // The plane distance: x and z are swapped against the stored centre / axis (the original's order).
    float distance = (position.z - hillCentre_.z) * hillAxis_.z + (position.y - hillCentre_.y) * hillAxis_.y +
                     (position.x - hillCentre_.x) * hillAxis_.x;
    distance = distance * distance;
    const cS3DVector3 colour = GetColor(direction);
    const float fade = 1.0f - distance;
    r = LightingByte(colour.x * fade);
    g = LightingByte(colour.y * fade);
    b = LightingByte(colour.z * fade);
}

void cSC4LightingManager::GetTerrainColorFloor(const cS3DVector3& position, const cS3DVector3& normal,
                                               std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    // 0x10077a9ca: the floor colour plus the scatter colour faded by the squared plane distance.
    float distance = (position.z - hillCentre_.z) * hillAxis_.z + (position.y - hillCentre_.y) * hillAxis_.y +
                     (position.x - hillCentre_.x) * hillAxis_.x;
    distance = distance * distance;
    const float scatterX = scatterColor_.x * distance;
    const float scatterY = scatterColor_.y * distance;
    const float scatterZ = distance * scatterColor_.z;
    const cS3DVector3 colour = GetColor(normal);
    r = LightingByte(floorLight_.x * colour.x + scatterX);
    g = LightingByte(floorLight_.y * colour.y + scatterY);
    b = LightingByte(floorLight_.z * colour.z + scatterZ);
}

void cSC4LightingManager::GetTerrainVertexColors(int row, int column, int count, const cS3DVector3* normals,
                                                 int normalStride, std::uint8_t* colours, int colourStride) {
    // 0x10077ad3a: one terrain row. `row` is the fixed z, `column` the first x; normals / colours advance by their
    // byte strides. Without hill shadows this is just the light colour and its floor; with them the crest bends the
    // normal and the plane distance fades the shadowed colour.
    const LightingTunables& tunables = LightingManagerTunables();
    if (!hillShadows_) {
        for (int i = 0; i < count; ++i) {
            const cS3DVector3 colour = GetColor(*normals);
            colours[0] = LightingByte(colour.x);
            colours[1] = LightingByte(colour.y);
            colours[2] = LightingByte(colour.z);
            colours[3] = LightingByte(colour.x * floorLight_.x);
            colours[4] = LightingByte(colour.y * floorLight_.y);
            colours[5] = LightingByte(colour.z * floorLight_.z);
            normals = reinterpret_cast<const cS3DVector3*>(reinterpret_cast<const std::uint8_t*>(normals) +
                                                           normalStride);
            colours += colourStride;
        }
        return;
    }
    float z = static_cast<float>(column) * cellSize_;
    for (int i = 0; i < count; ++i) {
        float distance = (static_cast<float>(row) * cellSize_ - hillCentre_.z) * hillAxis_.z +
                         (0.0f - hillCentre_.y) * hillAxis_.y + (z - hillCentre_.x) * hillAxis_.x;
        distance = distance * distance;
        const std::int32_t index = row * strideZ_ + column * strideX_ + heightBase_;
        cS3DVector3 colour{0, 0, 0};
        if (index >= 0 && static_cast<std::size_t>(index) < crests_.size()) {
            float height = crests_[static_cast<std::size_t>(index)] - heights_[static_cast<std::size_t>(index)];
            if (height <= 0.0f) {
                colour = GetColor(*normals);
            } else {
                height = static_cast<float>(static_cast<double>(height * tunables.hillShadowStrength) * 0.01);
                const float bentX = normals->x + diffuseDirection_.x * height;
                const float bentY = height * diffuseDirection_.y + normals->y;
                const float bentZ = normals->z + diffuseDirection_.z * height;
                float lengthSquared = bentZ * bentZ + bentY * bentY + bentX * bentX;
                cS3DVector3 direction{bentX, bentY, bentZ};
                if (0.0f < lengthSquared) {
                    const float inverse = 1.0f / std::sqrt(lengthSquared);
                    direction = cS3DVector3{bentX * inverse, bentY * inverse, bentZ * inverse};
                }
                colour = GetColor(direction);
            }
        }
        const float fade = 1.0f - distance;
        colours[0] = LightingByte(fade * colour.x);
        colours[1] = LightingByte(fade * colour.y);
        colours[2] = LightingByte(fade * colour.z);
        colours[3] = LightingByte(colour.x * floorLight_.x + scatterColor_.x * distance);
        colours[4] = LightingByte(colour.y * floorLight_.y + scatterColor_.y * distance);
        colours[5] = LightingByte(colour.z * floorLight_.z + scatterColor_.z * distance);
        normals = reinterpret_cast<const cS3DVector3*>(reinterpret_cast<const std::uint8_t*>(normals) + normalStride);
        z += cellSize_;
        colours += colourStride;
        ++column;
    }
}


// ===================================================================================== model lighting

bool cSC4LightingManager::GetModelLight(s3d::cIS3DModelInstance* instance, cS3DVector3* out) {
    // 0x100778062: the model's world box and owner decide the shadow amount (a network piece with flag 0x40000 is
    // lit without hill shadows, flora and automata get their own amounts); the shade direction comes from the hill
    // crest or from the terrain normals around the box, and the final colour is
    // (modelColour * (1 - amount) + modelColour) * globalLight.
    if (!instance || !out) return false;
    const LightingTunables& tunables = LightingManagerTunables();
    float shadowAmount = tunables.modelTerrainShadowAmount;
    cS3DBoundingBox box;
    box.min[0] = box.min[1] = box.min[2] = 1.0f;
    box.max[0] = box.max[1] = box.max[2] = 0.0f;
    instance->GetWorldBounds(box);
    cIGZUnknown* owner = instance->GetOwner();
    if (owner) {
        cISC4Occupant* occupant = nullptr;
        void* raw = nullptr;
        if (owner->QueryInterface(kGZIID_cISC4Occupant, &raw)) occupant = static_cast<cISC4Occupant*>(raw);
        if (occupant) {
            const std::uint32_t type = occupant->GetType();
            if (host_ && host_->OccupantHasNetworkFlag(occupant, kLightingNetworkFlagSkipShadows)) {
                const cS3DVector3 model = GetModelColor(cS3DVector3{0.0f, 1.0f, 0.0f});
                const float scale = 1.0f - shadowAmount;
                out->x = ((1.0f - model.x) * scale + model.x) * globalLight_.x;
                out->y = ((1.0f - model.y) * scale + model.y) * globalLight_.y;
                out->z = ((1.0f - model.z) * scale + model.z) * globalLight_.z;
                occupant->Release();
                return true;
            }
            float amount = 0.0f;
            bool matched = false;
            if (type == kLightingOccupantTypeFlora ||
                occupant->IsOccupantGroup(kLightingOccupantGroupFloraShadows)) {
                amount = tunables.floraTerrainShadowAmount;
                matched = true;
            } else if (static_cast<std::int32_t>(type) <= 0x480e6150) {
                if (type == kLightingOccupantTypeAutomataA || type == kLightingOccupantTypeAutomataB) {
                    amount = tunables.automataTerrainShadowAmount;
                    matched = true;
                }
            } else if (type == kLightingOccupantTypeAutomataC || type == kLightingOccupantTypeAutomataD) {
                amount = tunables.automataTerrainShadowAmount;
                matched = true;
            }
            if (matched) shadowAmount = amount;
            occupant->Release();
        }
    }
    cS3DVector3 direction{0.0f, 0.0f, 0.0f};
    if (tunables.testTerrainShadowing) {
        // The debug path: the box centre folded into 0..1 by fmod 256.
        const float averageX = (box.min[0] + box.max[0]) * 0.5f;
        const float averageZ = (box.min[2] + box.max[2]) * 0.5f;
        out->x = std::fmod(averageX, 256.0f) * 0.00390625f;
        out->y = std::fmod(averageZ, 256.0f) * 0.00390625f;
        out->z = 1.0f;
        return true;
    }
    if (hillShadows_) {
        direction = cS3DVector3{0.0f, 1.0f, 0.0f};
        const float averageX = (box.min[0] + box.max[0]) * 0.5f;
        const float averageZ = (box.min[2] + box.max[2]) * 0.5f;
        if (0.0f <= averageX && 0.0f <= averageZ && averageX < terrainWidth_ && averageZ < terrainDepth_) {
            const float averageY = (box.min[1] + box.max[1]) * 0.5f;
            const std::int32_t column = LightingFloor(averageX * inverseCellSize_);
            const std::int32_t row = LightingFloor(averageZ * inverseCellSize_);
            const std::int32_t index = column * strideX_ + heightBase_ + row * strideZ_;
            if (index >= 0 && static_cast<std::size_t>(index) < crests_.size()) {
                float height = crests_[static_cast<std::size_t>(index)] - averageY;
                if (0.0f < height) {
                    height = static_cast<float>(static_cast<double>(height * tunables.hillShadowStrength) * 0.01);
                    const float bentX = diffuseDirection_.x * height;
                    const float bentY = height * diffuseDirection_.y + 1.0f;
                    const float bentZ = diffuseDirection_.z * height;
                    float lengthSquared = bentY * bentY + bentX * bentX;
                    lengthSquared = bentZ * bentZ + lengthSquared;
                    if (0.0f < lengthSquared) {
                        const float inverse = 1.0f / std::sqrt(lengthSquared);
                        direction = cS3DVector3{bentX * inverse, bentY * inverse, bentZ * inverse};
                    } else {
                        direction = cS3DVector3{bentX, bentY, bentZ};
                    }
                }
            }
        }
    } else if (terrainMap_) {
        const float spanX = box.max[0] - box.min[0];
        const float spanZ = box.max[2] - box.min[2];
        const float extent = spanZ > spanX ? spanZ : spanX;
        if (50.0f <= extent) {
            // Four corner normals of the box, averaged.
            cS3DVector3 normal{0, 0, 0};
            terrainMap_->TerrainNormal(ClampToTerrain(box.min[0], terrainWidth_),
                                       ClampToTerrain(box.min[2], terrainDepth_), normal);
            direction = normal;
            terrainMap_->TerrainNormal(ClampToTerrain(box.min[0], terrainWidth_),
                                       ClampToTerrain(box.max[2], terrainDepth_), normal);
            direction.x += normal.x;
            direction.y += normal.y;
            direction.z += normal.z;
            terrainMap_->TerrainNormal(ClampToTerrain(box.max[0], terrainWidth_),
                                       ClampToTerrain(box.min[2], terrainDepth_), normal);
            direction.x += normal.x;
            direction.y += normal.y;
            direction.z += normal.z;
            terrainMap_->TerrainNormal(ClampToTerrain(box.max[0], terrainWidth_),
                                       ClampToTerrain(box.max[2], terrainDepth_), normal);
            direction.x = (normal.x + direction.x) * 0.25f;
            direction.y = (normal.y + direction.y) * 0.25f;
            direction.z = (normal.z + direction.z) * 0.25f;
        } else {
            const float averageX = (box.max[0] + box.min[0]) * 0.5f;
            const float averageZ = (box.max[2] + box.min[2]) * 0.5f;
            terrainMap_->TerrainNormal(ClampToTerrain(averageX, terrainWidth_),
                                       ClampToTerrain(averageZ, terrainDepth_), direction);
        }
    }
    const cS3DVector3 model = GetModelColor(direction);
    const float scale = 1.0f - shadowAmount;
    out->x = ((1.0f - model.x) * scale + model.x) * globalLight_.x;
    out->y = ((1.0f - model.y) * scale + model.y) * globalLight_.y;
    out->z = ((1.0f - model.z) * scale + model.z) * globalLight_.z;
    return true;
}

void cSC4LightingManager::GetTerrainLighting(const cS3DVector3& position, std::uint8_t& r, std::uint8_t& g,
                                             std::uint8_t& b) {
    // 0x100778a90: the terrain normal at the position (or the crest-bent version), lit and faded by
    // kTerrainShadowAmount.
    const LightingTunables& tunables = LightingManagerTunables();
    cS3DVector3 direction{0.0f, 0.0f, 0.0f};
    if (terrainMap_) {
        terrainMap_->TerrainNormal(ClampToTerrain(position.x, terrainWidth_),
                                   ClampToTerrain(position.z, terrainDepth_), direction);
    }
    if (hillShadows_) {
        const std::int32_t column = LightingFloor(position.x * inverseCellSize_);
        const std::int32_t row = LightingFloor(inverseCellSize_ * position.z);
        const std::int32_t index = column * strideX_ + heightBase_ + row * strideZ_;
        if (index >= 0 && static_cast<std::size_t>(index) < crests_.size()) {
            float height = crests_[static_cast<std::size_t>(index)] - position.y;
            if (0.0f < height) {
                height = static_cast<float>(static_cast<double>(height * tunables.hillShadowStrength) * 0.01);
                const float bentX = direction.x + diffuseDirection_.x * height;
                const float bentY = height * diffuseDirection_.y + direction.y;
                const float bentZ = direction.z + diffuseDirection_.z * height;
                float lengthSquared = bentY * bentY + bentX * bentX;
                lengthSquared = bentZ * bentZ + lengthSquared;
                if (0.0f < lengthSquared) {
                    const float inverse = 1.0f / std::sqrt(lengthSquared);
                    direction = cS3DVector3{bentX * inverse, bentY * inverse, bentZ * inverse};
                }
            }
        }
    }
    const cS3DVector3 colour = GetColor(direction);
    const float scale = 1.0f - tunables.terrainShadowAmount;
    r = LightingByte((1.0f - colour.x) * scale + colour.x);
    g = LightingByte((1.0f - colour.y) * scale + colour.y);
    b = LightingByte((1.0f - colour.z) * scale + colour.z);
}

void cSC4LightingManager::GetVertexColors(int count, const cS3DVector3* normals, int normalStride,
                                          std::uint8_t* colours, int colourStride, const nS3D::Matrix3& rotation) {
    // 0x10077ab0a: rotate each normal by the 3x3 matrix and combine the six directional light colours. The byte
    // output is RGB + 0xff, clamped above at 1 only.
    if (count <= 0) return;
    for (int i = 0; i < count; ++i) {
        const float nx = normals->x;
        const float ny = normals->y;
        const float nz = normals->z;
        const float alongX = rotation.m[0] * nx + rotation.m[1] * ny + rotation.m[2] * nz;
        const float alongY = rotation.m[5] * nz + (rotation.m[4] * ny + rotation.m[3] * nx);
        const float alongZ = rotation.m[8] * nz + (rotation.m[7] * ny + rotation.m[6] * nx);
        // The along-X term is the chosen side's colour *times* the (signed) dot; the other two lobes add or
        // subtract their colour times their dot.
        const cS3DVector3& alongXLight = 0.0f <= alongX ? directionalLight_[0] : directionalLight_[1];
        const float alongXSign = 0.0f <= alongX ? 1.0f : -1.0f;
        float red = alongXSign * alongXLight.x * alongX;
        float green = alongXSign * alongXLight.y * alongX;
        float blue = alongXSign * alongXLight.z * alongX;
        if (0.0f <= alongY) {
            red = red + directionalLight_[2].x * alongY;
            green = green + directionalLight_[2].y * alongY;
            blue = blue + directionalLight_[2].z * alongY;
        } else {
            red = red - directionalLight_[3].x * alongY;
            green = green - directionalLight_[3].y * alongY;
            blue = blue - directionalLight_[3].z * alongY;
        }
        if (0.0f <= alongZ) {
            red = red + directionalLight_[4].x * alongZ;
            green = green + directionalLight_[4].y * alongZ;
            blue = blue + directionalLight_[4].z * alongZ;
        } else {
            red = red - directionalLight_[5].x * alongZ;
            green = green - directionalLight_[5].y * alongZ;
            blue = blue - directionalLight_[5].z * alongZ;
        }
        colours[0] = LightingByte(red <= 1.0f ? red : 1.0f);
        colours[1] = LightingByte(green <= 1.0f ? green : 1.0f);
        colours[2] = LightingByte(blue <= 1.0f ? blue : 1.0f);
        colours[3] = 0xff;
        normals = reinterpret_cast<const cS3DVector3*>(reinterpret_cast<const std::uint8_t*>(normals) + normalStride);
        colours += colourStride;
    }
}

// ===================================================================================== shadows

bool cSC4LightingManager::CalcShadowProjection(int count, const cS3DVector3* points, const cS3DVector2* texCoords,
                                               const cS3DTransform& transform, cS3DMatrix4* out) {
    // 0x10077909a. The points are in the model's space. The light direction goes into that space through the transform's inverse, a
    // frame is built whose z axis is the light (AlignDirToZWithYUp), the points are flattened along the light into that frame's (a, b)
    // plane, and (a, b) is fitted to the texture coordinates by least squares: u = ca a + cb b + cc (and v likewise). The fit becomes a
    // model -> texture matrix (its third column passes the model's z through) composed with the transform's inverse, so it takes world
    // points. A fit whose normal-equation determinant is below 0.01 (the points do not spread) is refused and `out` is left alone.
    // Float order checked against the machine code (scripts/machine_oracle/lightingmanager_oracle.cpp).
    const std::uint8_t flags = transform.flags;
    float rotation[9];
    std::memcpy(rotation, transform.rotation, sizeof rotation);
    float inverseRotation[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) inverseRotation[3 * r + c] = (flags & nS3D::kTransformRotation) ? rotation[3 * c + r] : rotation[3 * r + c];
    cS3DTransform inverse;
    inverse.changeCounter = 1;
    inverse.flags = flags;
    std::memcpy(inverse.rotation, inverseRotation, sizeof inverseRotation);
    float tx = -transform.translation[0], ty = -transform.translation[1], tz = -transform.translation[2];
    inverse.scale = transform.scale;
    inverse.translation[0] = tx; inverse.translation[1] = ty; inverse.translation[2] = tz;
    if (flags & nS3D::kTransformRotation) {
        inverse.translation[0] = rotation[6] * tz + (rotation[3] * ty + rotation[0] * tx);
        inverse.translation[1] = rotation[7] * tz + (rotation[4] * ty + rotation[1] * tx);
        inverse.translation[2] = rotation[8] * tz + (rotation[5] * ty + rotation[2] * tx);
    }
    if (flags & nS3D::kTransformScale) {
        const float inverseScale = 1.0f / transform.scale;
        inverse.scale = inverseScale;
        for (float& t : inverse.translation) t = t * inverseScale;
    }

    const cS3DVector3 light = GetShadowDirection();
    const float negLightX = -light.x;
    cS3DVector3 direction;
    direction.x = (inverseRotation[0] * negLightX - inverseRotation[1] * light.y) - inverseRotation[2] * light.z;
    direction.y = (inverseRotation[3] * negLightX - inverseRotation[4] * light.y) - inverseRotation[5] * light.z;
    direction.z = (inverseRotation[6] * negLightX - inverseRotation[7] * light.y) - inverseRotation[8] * light.z;
    nS3D::Matrix3 frame;
    nS3D::AlignDirToZWithYUp(direction, frame);
    const float* m = frame.m;

    float sumA = 0, sumAA = 0, sumAB = 0, sumBB = 0, sumB = 0, count_ = 0;
    float sumAU = 0, sumU = 0, sumBU = 0, sumAV = 0, sumV = 0, sumBV = 0;
    for (int i = 0; i < count; ++i) {
        const cS3DVector3& p = points[i];
        const float a = (p.x * m[0] + m[1] * p.y) + m[2] * p.z;
        const float b = (p.x * m[3] + m[4] * p.y) + m[5] * p.z;
        const float u = texCoords[i].x, v = texCoords[i].y;
        sumA += a;
        sumAA += a * a;
        sumAB += a * b;
        sumBB += b * b;
        sumB += b;
        count_ += 1.0f;
        sumAU += a * u;
        sumU += u;
        sumBU += u * b;
        sumAV += a * v;
        sumV += v;
        sumBV += b * v;
    }
    const float c0 = sumBB * count_ - sumB * sumB;
    const float c1 = sumB * sumA - sumAB * count_;
    const float c2 = sumAB * sumB - sumA * sumBB;
    const float determinant = sumAA * c0 + sumAB * c1 + sumA * c2;
    if (!(0.01 <= static_cast<double>(determinant))) return false;   // the original compares in double
    const float inv = 1.0f / determinant;
    const float i01 = (sumA * sumB - sumAB * count_) * inv;
    const float i02 = (sumB * sumAB - sumBB * sumA) * inv;
    const float i11 = (sumAA * count_ - sumA * sumA) * inv;
    const float i12 = (sumAB * sumA - sumAA * sumB) * inv;
    const float i22 = inv * (sumAA * sumBB - sumAB * sumAB);
    const float k0 = c0 * inv, k1 = c1 * inv, k2 = c2 * inv;
    // each coefficient: (sum of the plain terms) * (inverse entry) added to (the b term added to the a term), right-nested as the machine code does
    const float ca_u = sumU * i02 + (sumBU * i01 + sumAU * k0);
    const float cb_u = sumU * i12 + (sumBU * i11 + sumAU * k1);
    const float cc_u = sumU * i22 + (sumBU * i12 + sumAU * k2);
    const float ca_v = i02 * sumV + (i01 * sumBV + k0 * sumAV);
    const float cb_v = i12 * sumV + (i11 * sumBV + k1 * sumAV);
    const float cc_v = i22 * sumV + (i12 * sumBV + k2 * sumAV);

    cS3DTransform fit;
    fit.changeCounter = 0;
    fit.flags = nS3D::kTransformTranslation | nS3D::kTransformRotation;
    float* r = &fit.rotation[0][0];
    r[0] = m[3] * cb_u + m[0] * ca_u;
    r[1] = m[4] * cb_u + m[1] * ca_u;
    r[2] = m[5] * cb_u + m[2] * ca_u;
    r[3] = m[3] * cb_v + m[0] * ca_v;
    r[4] = m[4] * cb_v + m[1] * ca_v;
    r[5] = cb_v * m[5] + ca_v * m[2];
    r[6] = 0.0f;
    r[7] = 0.0f;
    r[8] = 1.0f;
    fit.translation[0] = cc_u;
    fit.translation[1] = cc_v;
    fit.translation[2] = 0.0f;
    fit.scale = 1.0f;
    nS3D::MultiplyTransform(fit, inverse);

    const float factor = (fit.flags & nS3D::kTransformScale) ? fit.scale : 1.0f;
    const float* g = &fit.rotation[0][0];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) out->m[row][column] = (fit.flags & nS3D::kTransformScale) ? g[3 * column + row] * factor : g[3 * column + row];
        out->m[row][3] = 0.0f;
    }
    out->m[3][0] = fit.translation[0];
    out->m[3][1] = fit.translation[1];
    out->m[3][2] = fit.translation[2];
    out->m[3][3] = 1.0f;
    return true;
}

bool cSC4LightingManager::CalcShadowTerrainRect(int count, const cS3DVector3* points, const cS3DBoundingBox& box,
                                                cRZRect* out) {
    // 0x1007798dc: clamp the box's cell rectangle, then march each of the four edges outwards while the projected
    // height is below the terrain. The sun's rotated x / z decide which edges move; `count` and `points` are unused
    // by the original too. The index guards are port-only (the original trusts the rectangle it clamped).
    (void)count;
    (void)points;
    if (!out) return false;
    const float cell = cellSize_;
    const float minX = box.min[0];
    const float minZ = box.min[2];
    const float maxX = box.max[0];
    const float maxZ = box.max[2];
    float projectY = box.max[1];
    std::int32_t cellX0 = LightingFloor(minX * inverseCellSize_);
    std::int32_t cellZ0 = LightingFloor(minZ * inverseCellSize_);
    std::int32_t cellX1 = LightingCeil(maxX * inverseCellSize_);
    std::int32_t cellZ1 = LightingCeil(maxZ * inverseCellSize_);
    const std::int32_t cellsX = static_cast<std::int32_t>(cellCountX_);
    const std::int32_t cellsZ = static_cast<std::int32_t>(cellCountZ_);
    std::uint32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    if (cellsX < cellX0 || cellX1 < 0 || cellsZ < cellZ0 || cellZ1 < 0) {
        x0 = z0 = x1 = z1 = 0;
    } else {
        x0 = cellX0 < 0 ? 0u : static_cast<std::uint32_t>(cellX0);
        z0 = cellZ0 < 0 ? 0u : static_cast<std::uint32_t>(cellZ0);
        x1 = (cellsX < cellX1) ? static_cast<std::uint32_t>(cellsX) : static_cast<std::uint32_t>(cellX1);
        z1 = (cellZ1 <= cellsZ) ? static_cast<std::uint32_t>(cellZ1) : static_cast<std::uint32_t>(cellsZ);
    }
    const float sunX = rotatedSun_.x;
    const float sunY = rotatedSun_.y;
    const float sunZ = rotatedSun_.z;
    const float slopeYPerX = sunY / sunX;
    const float slopeYPerZ = sunY / sunZ;
    auto readable = [this](std::ptrdiff_t index) {
        return 0 <= index && static_cast<std::size_t>(index) < heights_.size();
    };
    // The two min-edge marches: decrement the edge while the projected height is below the terrain, at most
    // `edge - 1` times (the original's do/while (1 < edge + step)).
    auto marchMinZ = [&](std::uint32_t edge, int otherX, float projected) -> std::uint32_t {
        std::ptrdiff_t index = static_cast<std::ptrdiff_t>(edge) * strideZ_ + heightBase_ +
                               static_cast<std::ptrdiff_t>(otherX) * strideX_;
        int step = 0;
        do {
            if (!readable(index)) break;
            const float height = heights_[static_cast<std::size_t>(index)];
            if (projected < height || projected == height) break;
            projected -= slopeYPerZ * cell;
            const long remaining = static_cast<long>(edge) + step;
            step -= 1;
            index -= strideZ_;
            if (!(1 < remaining)) break;
        } while (true);
        return static_cast<std::uint32_t>(static_cast<int>(edge) + step);
    };
    auto marchMinX = [&](std::uint32_t edge, int otherZ, float projected) -> std::uint32_t {
        std::ptrdiff_t index = static_cast<std::ptrdiff_t>(edge) * strideX_ + heightBase_ +
                               static_cast<std::ptrdiff_t>(otherZ) * strideZ_;
        int step = 0;
        do {
            if (!readable(index)) break;
            const float height = heights_[static_cast<std::size_t>(index)];
            if (projected < height || projected == height) break;
            projected -= slopeYPerX * cell;
            const long remaining = static_cast<long>(edge) + step;
            step -= 1;
            index -= strideX_;
            if (!(1 < remaining)) break;
        } while (true);
        return static_cast<std::uint32_t>(static_cast<int>(edge) + step);
    };
    // The two max-edge marches: increment the edge while it is inside the map and the projected height is below
    // the terrain.
    auto marchMaxZ = [&](std::uint32_t edge, int otherX, float projected) -> std::uint32_t {
        std::ptrdiff_t index = static_cast<std::ptrdiff_t>(edge) * strideZ_ + heightBase_ +
                               static_cast<std::ptrdiff_t>(otherX) * strideX_;
        if (!(static_cast<float>(static_cast<std::int32_t>(edge)) < cellCountZ_)) return edge;
        do {
            if (!readable(index)) break;
            const float height = heights_[static_cast<std::size_t>(index)];
            if (projected < height || projected == height) break;
            edge += 1;
            projected += slopeYPerZ * cell;
            index += strideZ_;
        } while (static_cast<float>(static_cast<std::int32_t>(edge)) < cellCountZ_);
        return edge;
    };
    auto marchMaxX = [&](std::uint32_t edge, int otherZ, float projected) -> std::uint32_t {
        std::ptrdiff_t index = static_cast<std::ptrdiff_t>(edge) * strideX_ + heightBase_ +
                               static_cast<std::ptrdiff_t>(otherZ) * strideZ_;
        if (!(static_cast<float>(static_cast<std::int32_t>(edge)) < cellCountX_)) return edge;
        do {
            if (!readable(index)) break;
            const float height = heights_[static_cast<std::size_t>(index)];
            if (projected < height || projected == height) break;
            edge += 1;
            projected += slopeYPerX * cell;
            index += strideX_;
        } while (static_cast<float>(static_cast<std::int32_t>(edge)) < cellCountX_);
        return edge;
    };
    if (sunX <= 0.0f) {
        if (sunZ <= 0.0f) {
            if (0 < static_cast<std::int32_t>(z0)) {
                float projected = (static_cast<float>(static_cast<std::int32_t>(z0)) * cell - minZ) * slopeYPerZ +
                                  projectY;
                z0 = marchMinZ(z0, static_cast<int>(x1), projected);
                if (0 < static_cast<std::int32_t>(z0))
                    z0 = marchMinZ(z0, static_cast<int>(x0), projected);
            }
            if (0 < static_cast<std::int32_t>(x0)) {
                projectY = projectY + (static_cast<float>(static_cast<std::int32_t>(x0)) * cell - minX) * slopeYPerX;
                x0 = marchMinX(x0, static_cast<int>(z0), projectY);
            }
        } else {
            if (0 < static_cast<std::int32_t>(x0)) {
                float projected = (static_cast<float>(static_cast<std::int32_t>(x0)) * cell - minX) * slopeYPerX +
                                  projectY;
                x0 = marchMinX(x0, static_cast<int>(z0), projected);
                if (0 < static_cast<std::int32_t>(x0))
                    x0 = marchMinX(x0, static_cast<int>(z1), projected);
            }
            if (static_cast<float>(static_cast<std::int32_t>(z1)) < cellCountZ_) {
                projectY = projectY + (static_cast<float>(static_cast<std::int32_t>(z1)) * cell - maxZ) * slopeYPerZ;
                z1 = marchMaxZ(z1, static_cast<int>(x0), projectY);
            }
        }
    } else if (sunZ <= 0.0f) {
        if (static_cast<float>(static_cast<std::int32_t>(x1)) < cellCountX_) {
            float projected = (cell * static_cast<float>(static_cast<std::int32_t>(x1)) - maxX) * slopeYPerX +
                              projectY;
            x1 = marchMaxX(x1, static_cast<int>(z1), projected);
        }
        if (static_cast<float>(static_cast<std::int32_t>(x1)) < cellCountX_) {
            float projected = (cell * static_cast<float>(static_cast<std::int32_t>(x1)) - maxX) * slopeYPerX +
                              projectY;
            x1 = marchMaxX(x1, static_cast<int>(z0), projected);
        }
        if (0 < static_cast<std::int32_t>(z0)) {
            projectY = projectY + (static_cast<float>(static_cast<std::int32_t>(z0)) * cell - minZ) * slopeYPerZ;
            z0 = marchMinZ(z0, static_cast<int>(x1), projectY);
        }
    } else {
        if (static_cast<float>(static_cast<std::int32_t>(z1)) < cellCountZ_) {
            float projected = (cell * static_cast<float>(static_cast<std::int32_t>(z1)) - maxZ) * slopeYPerZ +
                              projectY;
            z1 = marchMaxZ(z1, static_cast<int>(x0), projected);
        }
        if (static_cast<float>(static_cast<std::int32_t>(z1)) < cellCountZ_) {
            float projected = (cell * static_cast<float>(static_cast<std::int32_t>(z1)) - maxZ) * slopeYPerZ +
                              projectY;
            z1 = marchMaxZ(z1, static_cast<int>(x1), projected);
        }
        if (0 < static_cast<std::int32_t>(x1)) {
            projectY = projectY + (static_cast<float>(static_cast<std::int32_t>(x1)) * cell - maxX) * slopeYPerX;
            x1 = marchMaxX(x1, static_cast<int>(z1), projectY);
        }
    }
    out->left = static_cast<std::int32_t>(x0);
    out->top = static_cast<std::int32_t>(z0);
    out->right = static_cast<std::int32_t>(x1);
    out->bottom = static_cast<std::int32_t>(z1);
    return true;
}

}  // namespace sc4
