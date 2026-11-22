// cSC4LightingManager — the city's day/night lighting (40 symbols, 0x100775628..0x10077b15a, object size 0x240).
// The renderer allocates it (cSC43DRender::Init, 0x1007602f6), hands it to the draw context (+0x68) and publishes it
// as SL::spLightingManager; the terrain (DoLighting), every model occupant (UpdateLighting), the effects manager
// (tint), the prop developer (IsNight) and the automata ask it what colour the world is at the current time of day.
//
// It is a GZCOM object with two views: the primary cISC4LightingManager (iid 0x681bd52a, also answered for iid 1)
// and a cIGZMessageTarget2 (+8, iid 0x452294aa). DoMessage receives the seven notifications Init registers for
// (the renderer's zoom/rotation, the terrain's region / region-done pair, the render-property change, the terrain
// object change and two repaint requests).
//
// Original layout (0x240 bytes; every field offset below is from the constructor at 0x10077563e):
//   +0x00 primary vptr (cISC4LightingManager)     +0x08 cIGZMessageTarget2 vptr
//   +0x10 bool initialised                        +0x11 bool zoom/rotation values valid
//   +0x14 reference count (primary)               +0x18 cISCPropertyHolder* (the lighting exemplar)
//   +0x1c / +0x24 / +0x2c never written           +0x20 terrain map (SL::spTerrainMap, referenced)
//   +0x28 terrain (SL::spTerrain, referenced)     +0x30 float time of day (0..24, starts at 12)
//   +0x34 int season month index (0..11)          +0x38 bool night
//   +0x40 std::vector<cS3DVector3> day/night colour table (8 entries after ReadProperties)
//   +0x58 std::vector<float> scatter-strength table (4 entries)
//   +0x70 std::vector<cS3DVector3> scatter-colour table (1 entry)
//   +0x88 / +0x94 two light directions (rotated with the view)   +0xa0 the rotated sun direction
//   +0xac the sampled light colour times kTextureFloor            +0xb8 2x2 rotation matrix (cos,-sin,sin,cos)
//   +0xc8 global light colour (what GetGlobalLight answers)       +0xd4 the sampled (untinted) light colour
//   +0xe0 previous global light (for the update-delta test)       +0xec previous sampled x (night flip detection)
//   +0xf0 / +0xf4 previous sampled y / z                          +0xf8 night fade, +0xfc night shadow fade
//   +0x100 / +0x108 / +0x110 / +0x118 the four colour maps (cImageARGB8*)
//   +0x120 the darkest red of map +0x110 (0..1)                   +0x124 the sun direction (from the exemplar)
//   +0x130 six directional light colours (+x, -x, +y, +y, +z, -z) — GetVertexColors combines them
//   +0x178 / +0x17c terrain cells X / Z                           +0x180 / +0x184 cells X / Z + 1
//   +0x188 / +0x18c terrain width / depth (metres)                +0x190 cell size, +0x194 its inverse
//   +0x198 hill-frame centre point                                +0x1a4 hill-frame view axis
//   +0x1b0 scatter strength, +0x1b4 scatter colour                +0x1c4 bool tint, +0x1c8 tint colour,
//   +0x1d4 tint strength
//   +0x1d8 float[] terrain heights (W*D, in the rotation's hill order)
//   +0x1f0 float[] hill-shadow crest per vertex
//   +0x208 bool accumulating a terrain region                     +0x20c SC4Rect<int> the accumulating region
//   +0x21c SC4Rect<int> the hill region FindHillShadows produced   +0x22c / +0x230 / +0x234 height index strides
//   +0x238 bool hill shadows enabled (render property kShadowQuality > 1)
//
// The time-of-day curve: SetTimeDependentLightingValues samples the day/night table at
// (timeOfDay / 24, (seasonMonth + 0.5) / 12) — or the +0x110 colour map when one was loaded — and the scatter
// table at timeOfDay / 24, scales both by the render properties' kLightingUpdateDelta-shaped tolerance and applies
// the optional tint (SetLightingTint). SetTimeOfDay then compares the new global light with the previous one and,
// when it moved by more than kLightingUpdateDelta, posts the "lighting changed" / "night fell" messages.
//
// Quirks kept (with their addresses):
//  * GetModelColor checks colour map +0x100 for null but samples map +0x108 with the *first* map's width, height
//    and the second map's stride (0x1007788e6); it also floors the texture coordinates instead of interpolating.
//  * GetColor samples the environment map with the same floor-and-clamp and a double-precision uv; without a map
//    it returns 0.05 plus two directional lobes (0x100779fe4). Every byte it hands out is
//    (int)(v + 49152.0f) reinterpreted as bits, minus 49152.0's bits, folded as i - (i >> 8) — i.e. round(v * 256)
//    with the top of the range folding onto 255 (0x10077a292 and every other colour writer).
//  * GetShadowDirection / GetShadowParams / GetDecalShadowParams / GetShadowStrengthFromLocation read the file-static
//    tunables, which ReadProperties writes from the exemplar; GetDecalShadowParams reads its own holder's overrides.
//  * Init only loads the colour maps when kTestEnvironmentMap is false; the other branch builds a 0x40 x 0x40 test
//    pattern (MakeTestPattern 0x10077648c). Shutdown deletes the four maps but not the terrain reference, which the
//    destructor releases (0x100776674).
//  * The constructor leaves +0x39..+0x3f untouched (0x10077563e); the port zeroes them so a copy is deterministic.
//  * SetTimeOfDay reads the *sampled* red (+0xd4) against kNightThreshold for IsNight and stores its previous value
//    at +0xec (0x10077770a); the night message is suppressed when render property kRenderSnapshot is set.
//  * FindHillShadows stores (first column, first row, last column, last row) — the reverse of its own scan order —
//    and only runs when the sun's x is non-negative; its crest buffer is a function-local static (0x1007772f4).
//  * The renderer deletes this object through a plain delete (its Release calls the deleting destructor at vslot
//    0xf0); the port keeps the primary reference count at +0x14 and does not model the message-target count at +0x0c.
//
// Seams (unrecovered neighbours; the original call is in each comment):
//  * ILightingHost  — SL::spRM (the lighting exemplar), SL::spMS2, SL::spTerrain / spTerrainMap, SL::spViewUtilities,
//                     SL::spRenderProperties, the weather simulator (the season month), SL::spRenderer (full redraw),
//                     the colour-map loader (CreateColorMap 0x10077651e) and the network-occupant QI of GetModelLight.
//  * ILightingTerrain    — the terrain and terrain-map slots DoMessage / GetModelLight call (0x28, 0x30, 0x38, 0x40,
//                     0x78, 0xd8, 0x218, 0x228).
//  * ILightingColorMap   — cImageARGB8 (the GZ engine image, not recovered): its pixels, dimensions and the
//                     SampleColorLerp sampler at 0x10077b15a.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gz/MiscFunctions.h"
#include "sc4/MiscTypes.h"
#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "s3d/S3DMath.h"

class cIGZMessageServer2;
class cIGZUnknown;
class IS4LightingManager;   // the renderer's two-call view (src/sc4/DrawContext.h)
class SC4ViewUtilities;     // global scope in src/sc4/ViewUtilities.h
namespace s3d {
class cIS3DModelInstance;
}
namespace sc4 {
class cISCPropertyHolder;
class cISC4Occupant;
class cSC4RenderProperties;
}  // namespace sc4

namespace sc4 {

// ---------------------------------------------------------------------------------------------
// Message ids (Init registers for all seven; DoMessage dispatches on them)
// ---------------------------------------------------------------------------------------------
constexpr std::uint32_t kLightingMessageRepaintRequest = 0xc53d10aa;      // re-read the properties
constexpr std::uint32_t kLightingMessageRenderPropertiesChanged = 0xc9f775bb;
constexpr std::uint32_t kLightingMessageTerrainChanged = 0x2b96b3ea;      // new city / terrain object
constexpr std::uint32_t kLightingMessageZoomRotationChanged = 0xa6b79601; // payload: two ints in the message's void2
constexpr std::uint32_t kLightingMessageTerrainRegionBegin = 0xea3b0af1;
constexpr std::uint32_t kLightingMessageTerrainRegionEnd = 0x6a3b0afd;
constexpr std::uint32_t kLightingMessageTerrainRegionUpdated = 0x278e6003;
// The flip messages of SetTimeOfDay: the original posts 0xc9da96e8 when the light left the night state
// (dawn) and 0xc9da96e9 when it entered it (dusk). The ids are the binary's; the names describe when they fire.
constexpr std::uint32_t kLightingMessageDawn = 0xc9da96e8;
constexpr std::uint32_t kLightingMessageDusk = 0xc9da96e9;
constexpr std::uint32_t kLightingMessageLightingChanged = 0xc9da96ea;     // data1 = night fade, data2 = shadow fade

// The lighting exemplar: (type 0x6534284a, group 0xa9189cf0, instance 1), filter 0xa52160f5.
constexpr std::uint32_t kLightingExemplarType = 0x6534284a;
constexpr std::uint32_t kLightingExemplarGroup = 0xa9189cf0;
constexpr std::uint32_t kLightingExemplarInstance = 1;
constexpr std::uint32_t kLightingExemplarFilter = 0xa52160f5;

// Interface ids QueryInterface answers.
constexpr std::uint32_t kLightingIID_cIGZMessageTarget2 = 0x452294aa;   // the +8 message-target view

// Exemplar property ids (ReadProperties 0x100775b44).
constexpr std::uint32_t kLightingPropTextureFloor = 0x29189beb;
constexpr std::uint32_t kLightingPropUseEnvironmentMap = 0x29189bec;
constexpr std::uint32_t kLightingPropTestEnvironmentMap = 0x29189bed;
constexpr std::uint32_t kLightingPropTerrainShadowAmount = 0x29189bee;
constexpr std::uint32_t kLightingPropTestTerrainShadowing = 0x29189bef;
constexpr std::uint32_t kLightingPropDaytimeLight = 0x29189bf0;
constexpr std::uint32_t kLightingPropNighttimeLight = 0x9b6d6ae;
constexpr std::uint32_t kLightingPropFloraShadowAmount = 0x29189bf1;
constexpr std::uint32_t kLightingPropModelShadowAmount = 0x29189bf2;
constexpr std::uint32_t kLightingPropAutomataShadowAmount = 0x29189bf3;
constexpr std::uint32_t kLightingPropScatterColor = 0x29189bf4;
constexpr std::uint32_t kLightingPropScatterStrength = 0x29189bf5;
constexpr std::uint32_t kLightingPropUpdateDelta = 0x29189bf6;
constexpr std::uint32_t kLightingPropNightThreshold = 0x29189bf7;
constexpr std::uint32_t kLightingPropShadowStrength = 0x85489cc;
constexpr std::uint32_t kLightingPropShadowThreshold = 0x85489cd;
constexpr std::uint32_t kLightingPropShadowTextureID = 0x85489c8;
constexpr std::uint32_t kLightingPropShadowOffset = 0x85489c9;
constexpr std::uint32_t kLightingPropShadowSize = 0x85489ca;
constexpr std::uint32_t kLightingPropShadowColor = 0x85489cb;
constexpr std::uint32_t kLightingPropSunDirection = 0x85489ce;
constexpr std::uint32_t kLightingPropSunPitch = 0x85489cf;
constexpr std::uint32_t kLightingPropHillShadowStrength = 0x85489d0;
constexpr std::uint32_t kLightingPropHillShadowWarp = 0x85489d1;
constexpr std::uint32_t kLightingPropHillShadowFadeDepth = 0x85489d2;
constexpr std::uint32_t kLightingPropNightShadowFade = 0x85489d3;

// The colour-map resource keys Init loads (CreateColorMap 0x10077651e).
constexpr std::uint32_t kLightingColorMap0 = 0x0917660c;
constexpr std::uint32_t kLightingColorMap1 = 0x0917660d;
constexpr std::uint32_t kLightingColorMap2 = 0x0917660e;
constexpr std::uint32_t kLightingColorMap3 = 0x0917660f;

// ---------------------------------------------------------------------------------------------
// The file-static tunables ReadProperties writes (0x100b17a00..0x100b17a68 and 0x100b91110..0x100b91128).
// Their initial values are the binary's data section; the exemplar only overwrites those it carries. The class
// keeps them shared between instances exactly like the original's static locals.
// ---------------------------------------------------------------------------------------------
struct LightingTunables {
    cS3DVector3 daytimeLight = {1.0f, 1.0f, 1.0f};      // kDaytimeLight @ 0x100b17a00
    cS3DVector3 nighttimeLight = {1.0f, 1.0f, 1.0f};    // kNighttimeLight @ 0x100b17a0c
    cS3DVector3 scatterColor = {0.0f, 0.5f, 1.0f};      // kScatterColor @ 0x100b17a18
    float lightingUpdateDelta = 9.99999975e-05f;        // kLightingUpdateDelta @ 0x100b17a24 (0x38d1b717)
    float nightThreshold = 0.8f;                        // kNightThreshold @ 0x100b17a28
    float modelTerrainShadowAmount = 0.6f;              // kModelTerrainShadowAmount @ 0x100b17a2c
    float floraTerrainShadowAmount = 0.6f;              // kFloraTerrainShadowAmount @ 0x100b17a30
    float automataTerrainShadowAmount = 0.6f;           // kAutomataTerrainShadowAmount @ 0x100b17a34
    float terrainShadowAmount = 0.6f;                   // kTerrainShadowAmount @ 0x100b17a38
    float shadowStrength = 0.5f;                        // kShadowStrength @ 0x100b17a3c
    float hillShadowFadeDepth = 50.0f;                  // kHillShadowFadeDepth @ 0x100b17a40
    float nightShadowFade = 0.6f;                       // kNightShadowFade @ 0x100b17a44
    std::uint32_t shadowTextureID = 0x0917660e;         // kShadowTextureID @ 0x100b17a48
    float shadowSize = 8.0f;                            // kShadowSize @ 0x100b17a4c
    bool useEnvironmentMap = true;                      // kUseEnvironmentMap @ 0x100b17a50
    float textureFloor = 0.1f;                          // kTextureFloor @ 0x100b17a54
    float scatterStrength = 0.5f;                       // kScatterStrength @ 0x100b17a58
    float sunDirection = 90.0f;                         // kSunDirection @ 0x100b17a5c (degrees)
    float sunPitch = 45.0f;                             // kSunPitch @ 0x100b17a60 (degrees)
    float hillShadowStrength = 1.0f;                    // kHillShadowStrength @ 0x100b17a64
    float hillShadowWarp = 0.7f;                        // kHillShadowWarp @ 0x100b17a68 (0x3f333333)
    cS3DVector2 shadowOffset = {0.0f, 0.0f};            // kShadowOffset @ 0x100b91110
    cS3DVector3 shadowColour = {0.0f, 0.0f, 0.0f};      // kShadowColor @ 0x100b91118
    bool testEnvironmentMap = false;                    // kTestEnvironmentMap @ 0x100b91124
    bool testTerrainShadowing = false;                  // kTestTerrainShadowing @ 0x100b91125
    float shadowThreshold = 0.0f;                       // kShadowThreshold @ 0x100b91128
};
LightingTunables& LightingManagerTunables();   // the constants every instance shares
void ResetLightingManagerTunables();           // restore the binary's initial values (tests / a fresh city)

// ---------------------------------------------------------------------------------------------
// cImageARGB8 (the GZ engine image, unrecovered): the four colour maps the time-of-day curve samples.
// The original stores: +0 pixels (cS3DColorARGB8*, one 32-bit word per texel), +8 pixels end, +0x18 width,
// +0x1c height, +0x20 stride in texels.
// ---------------------------------------------------------------------------------------------
class ILightingColorMap {
public:
    virtual ~ILightingColorMap() = default;
    virtual std::uint32_t Width() const = 0;                 // +0x18
    virtual std::uint32_t Height() const = 0;                // +0x1c
    virtual std::int32_t Stride() const = 0;                 // +0x20
    virtual const std::uint32_t* Pixels() const = 0;         // +0
    virtual std::size_t PixelCount() const = 0;              // (+8 - +0) / 4
    // cImageARGB8::SampleColorLerp(float u, float v) @ 0x10077b15a. u/v are 0..1; the sample is bilinear in x
    // only (the original computes the second row and never uses it), with the x index floor(u * width) and the
    // y index floor(v * height), unclamped like the original.
    virtual cS3DVector3 SampleColorLerp(float u, float v) const = 0;
    // The original Shutdown deletes the pixel buffer and then the image; Release stands for that pair.
    virtual void Release() = 0;
};

// ---------------------------------------------------------------------------------------------
// The terrain (cISTETerrain) and terrain map (SL::spTerrainMap) as DoMessage / GetModelLight call them. Both
// pointers in the original name the same object in this build (ServiceLocator sets spTerrainMap = spTerrain).
// ---------------------------------------------------------------------------------------------
class ILightingTerrain {
public:
    virtual ~ILightingTerrain() = default;
    virtual void AddRef() = 0;                                              // +0x08
    virtual void Release() = 0;                                             // +0x10
    virtual bool IsInitialized() = 0;                                       // +0x28
    virtual float CellSize() = 0;                                           // +0x30
    virtual int CellCountX() = 0;                                           // +0x38
    virtual int CellCountZ() = 0;                                           // +0x40
    // +0x78: fills `out` from the rectangle, stepping rowStride floats per z and colStride per x.
    virtual void GetAltitudes(const SC4RectInt& rect, float* out, std::ptrdiff_t rowStride,
                              std::ptrdiff_t colStride) = 0;
    // +0xd8: the terrain normal at (x, z).
    virtual void TerrainNormal(float x, float z, cS3DVector3& out) = 0;
    // +0x218: light the rows of `rect` (cSTETerrain::DoLighting).
    virtual void DoLighting(const SC4RectInt& rect) = 0;
    // +0x228: redisplay the rows of `rect` (cSTETerrain::RedisplayTerrain(bool, bool, rect, int)).
    virtual void RedisplayTerrain(bool a, bool b, const SC4RectInt& rect, std::uint32_t flags) = 0;
};

// ---------------------------------------------------------------------------------------------
// Everything the manager reaches outside itself. One method per original call.
// ---------------------------------------------------------------------------------------------
class ILightingHost {
public:
    virtual ~ILightingHost() = default;
    // SL::spRM vslot 0x18 GetResource({0x6534284a, 0xa9189cf0, 1}, 0xa52160f5) followed by the exemplar's
    // AsISCPropertyHolder (vslot 0x30). Returns the holder with one reference, or null when the exemplar is missing.
    virtual cISCPropertyHolder* LightingProperties() = 0;
    virtual cIGZMessageServer2* MessageServer() = 0;                        // SL::spMS2
    // SL::spTerrainMap / SL::spTerrain (referenced while the manager holds them).
    virtual ILightingTerrain* TerrainMap() = 0;
    virtual ILightingTerrain* Terrain() = 0;
    virtual SC4ViewUtilities* ViewUtilities() = 0;                          // SL::spViewUtilities
    virtual cSC4RenderProperties* RenderProperties() = 0;                   // SL::spRenderProperties
    virtual int CurrentSeasonMonth() = 0;                                   // weather simulator vslot 0x78
    virtual void ForceFullRedraw() = 0;                                     // SL::spRenderer vslot 0xa0
    // CreateColorMap 0x10077651e: loads the resource and copies it into a new cImageARGB8. Null when missing;
    // the manager deletes it through ILightingColorMap::Release in Shutdown.
    virtual ILightingColorMap* CreateColorMap(std::uint32_t resourceKey) = 0;
    // GetModelLight asks the model's owner for cISC4NetworkOccupant (iid 0xa821ef94) and reads its vslot 0x40 flag
    // word. The interface is not recovered as a class, so the host bridges the one query.
    virtual bool OccupantHasNetworkFlag(cISC4Occupant* occupant, std::uint32_t flags) = 0;
};
ILightingHost* LightingManagerHost();
void SetLightingManagerHost(ILightingHost* host);

// ---------------------------------------------------------------------------------------------
// cISC4LightingManager (iid 0x681bd52a): the primary vtable, slots 3..32 in binary order.
// Pointers/references follow the original ABI: the colour vectors are returned through the caller's pointer and the
// shadow direction is returned by value (the original's hidden sret pointer).
// ---------------------------------------------------------------------------------------------
class cISC4LightingManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                          // [3]  0x18 @ 0x100775900
    virtual bool Shutdown() = 0;                                                      // [4]  0x20 @ 0x100776674
    virtual float GetTimeOfDay() const = 0;                                           // [5]  0x28 @ 0x1007776fe
    // False when the light moved by less than kLightingUpdateDelta (no message posted).
    virtual bool SetTimeOfDay(float timeOfDay) = 0;                                   // [6]  0x30 @ 0x10077770a
    virtual void SetLightingTint(const cS3DVector3& colour, float intensity) = 0;     // [7]  0x38 @ 0x100777fb6
    virtual void ResetLightingTint() = 0;                                             // [8]  0x40 @ 0x100778008
    virtual bool IsNight() const = 0;                                                 // [9]  0x48 @ 0x100778038
    virtual void GetGlobalLight(cS3DVector3* out) const = 0;                          // [10] 0x50 @ 0x100778042
    virtual bool GetModelLight(s3d::cIS3DModelInstance* instance, cS3DVector3* out) = 0;   // [11] 0x58 @ 0x100778062
    virtual void GetTerrainLighting(const cS3DVector3& position, std::uint8_t& r, std::uint8_t& g,
                                    std::uint8_t& b) = 0;                             // [12] 0x60 @ 0x100778a90
    virtual cS3DVector3 GetShadowDirection() const = 0;                               // [13] 0x68 @ 0x100778d48
    virtual void GetShadowParams(float& strength, float& threshold, cS3DVector3& colour) = 0;   // [14] 0x70
    virtual float GetShadowStrengthFromLocation(const cS3DVector2& position) = 0;     // [15] 0x78 @ 0x100778d9c
    virtual void GetDecalShadowParams(cISCPropertyHolder* properties, std::uint32_t& textureID, float& strength,
                                      float& threshold, float& size, cS3DVector2& offset,
                                      cS3DVector3& colour) = 0;                       // [16] 0x80 @ 0x100778e8a
    virtual bool CalcShadowProjection(int count, const cS3DVector3* points, const cS3DVector2* texCoords,
                                      const cS3DTransform& transform, cS3DMatrix4* out) = 0;   // [17] 0x88
    virtual bool CalcShadowTerrainRect(int count, const cS3DVector3* points, const cS3DBoundingBox& box,
                                       cRZRect* out) = 0;                             // [18] 0x90 @ 0x1007798dc
    virtual cS3DVector3 GetColor() = 0;                                               // [19] 0x98 @ 0x100779f6e
    virtual cS3DVector3 GetColor(const cS3DVector3& direction) = 0;                   // [20] 0xa0 @ 0x100779fe4
    // The original passes the normal in the first argument (r/dx) and the position in the second (rcx).
    virtual cS3DVector3 GetColor(const cS3DVector3& normal, const cS3DVector3& position) = 0;   // [21] 0xa8
    virtual void GetColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;     // [22] 0xb0 @ 0x100779fa8
    virtual void GetColor(const cS3DVector3& direction, std::uint8_t& r, std::uint8_t& g,
                          std::uint8_t& b) = 0;                                       // [23] 0xb8 @ 0x10077a292
    virtual void GetColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r,
                          std::uint8_t& g, std::uint8_t& b) = 0;                      // [24] 0xc0 @ 0x10077a4d2
    virtual void GetTerrainColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r,
                                 std::uint8_t& g, std::uint8_t& b) = 0;               // [25] 0xc8 @ 0x10077a6e8
    virtual void GetTerrainColorFloor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r,
                                      std::uint8_t& g, std::uint8_t& b) = 0;          // [26] 0xd0 @ 0x10077a9ca
    virtual void GetVertexColors(int count, const cS3DVector3* normals, int normalStride, std::uint8_t* colours,
                                 int colourStride, const nS3D::Matrix3& rotation) = 0;   // [27] 0xd8 @ 0x10077ab0a
    virtual void GetTerrainVertexColors(int row, int column, int count, const cS3DVector3* normals,
                                        int normalStride, std::uint8_t* colours,
                                        int colourStride) = 0;                        // [28] 0xe0 @ 0x10077ad3a
    virtual ~cISC4LightingManager() = default;   // PORT-ONLY: lets the renderer delete the object (original slots 29/30)
};

// ---------------------------------------------------------------------------------------------
// The object
// ---------------------------------------------------------------------------------------------
class cSC4LightingManager final : public cISC4LightingManager, public cIGZMessageTarget2 {
public:
    static constexpr std::uint32_t kGZIID_cISC4LightingManager = 0x681bd52a;

    // The original constructor takes no arguments and reads the SL::sp* globals; the port takes the one host.
    explicit cSC4LightingManager(ILightingHost* host);
    ~cSC4LightingManager() override;   // @ 0x1007757cc

    // cIGZUnknown
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x10077680c
    std::uint32_t AddRef() override;                               // @ 0x10077684e
    std::uint32_t Release() override;                              // @ 0x10077686a

    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1007768ae

    // cISC4LightingManager
    bool Init() override;                                          // @ 0x100775900
    bool Shutdown() override;                                      // @ 0x100776674
    float GetTimeOfDay() const override { return timeOfDay_; }     // @ 0x1007776fe
    bool SetTimeOfDay(float timeOfDay) override;                   // @ 0x10077770a
    void SetLightingTint(const cS3DVector3& colour, float intensity) override;   // @ 0x100777fb6
    void ResetLightingTint() override;                             // @ 0x100778008
    bool IsNight() const override { return night_; }               // @ 0x100778038
    void GetGlobalLight(cS3DVector3* out) const override;          // @ 0x100778042
    bool GetModelLight(s3d::cIS3DModelInstance* instance, cS3DVector3* out) override;   // @ 0x100778062
    void GetTerrainLighting(const cS3DVector3& position, std::uint8_t& r, std::uint8_t& g,
                            std::uint8_t& b) override;             // @ 0x100778a90
    cS3DVector3 GetShadowDirection() const override;               // @ 0x100778d48
    void GetShadowParams(float& strength, float& threshold, cS3DVector3& colour) override;   // @ 0x100778d6c
    float GetShadowStrengthFromLocation(const cS3DVector2& position) override;   // @ 0x100778d9c
    void GetDecalShadowParams(cISCPropertyHolder* properties, std::uint32_t& textureID, float& strength,
                              float& threshold, float& size, cS3DVector2& offset,
                              cS3DVector3& colour) override;       // @ 0x100778e8a
    bool CalcShadowProjection(int count, const cS3DVector3* points, const cS3DVector2* texCoords,
                              const cS3DTransform& transform, cS3DMatrix4* out) override;   // @ 0x10077909a
    bool CalcShadowTerrainRect(int count, const cS3DVector3* points, const cS3DBoundingBox& box,
                               cRZRect* out) override;             // @ 0x1007798dc
    cS3DVector3 GetColor() override;                               // @ 0x100779f6e
    cS3DVector3 GetColor(const cS3DVector3& direction) override;   // @ 0x100779fe4
    cS3DVector3 GetColor(const cS3DVector3& normal, const cS3DVector3& position) override;   // @ 0x10077a334
    void GetColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override;   // @ 0x100779fa8
    void GetColor(const cS3DVector3& direction, std::uint8_t& r, std::uint8_t& g,
                  std::uint8_t& b) override;                       // @ 0x10077a292
    void GetColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r, std::uint8_t& g,
                  std::uint8_t& b) override;                       // @ 0x10077a4d2
    void GetTerrainColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r, std::uint8_t& g,
                         std::uint8_t& b) override;                // @ 0x10077a6e8
    void GetTerrainColorFloor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r,
                              std::uint8_t& g, std::uint8_t& b) override;   // @ 0x10077a9ca
    void GetVertexColors(int count, const cS3DVector3* normals, int normalStride, std::uint8_t* colours,
                         int colourStride, const nS3D::Matrix3& rotation) override;   // @ 0x10077ab0a
    void GetTerrainVertexColors(int row, int column, int count, const cS3DVector3* normals, int normalStride,
                                std::uint8_t* colours, int colourStride) override;   // @ 0x10077ad3a

    // ---- port wiring / inspection -----------------------------------------------------------------
    ILightingHost* Host() const { return host_; }
    bool IsInitialized() const { return initialized_; }
    const std::vector<float>& Heights() const { return heights_; }         // +0x1d8
    const std::vector<float>& Crests() const { return crests_; }           // +0x1f0
    const SC4RectT<int>& AccumulateRect() const { return accumulateRect_; }   // +0x20c
    const SC4RectT<int>& HillRect() const { return hillRect_; }               // +0x21c
    bool HillShadowsEnabled() const { return hillShadows_; }               // +0x238
    float TerrainWidth() const { return terrainWidth_; }                   // +0x188
    float TerrainDepth() const { return terrainDepth_; }                   // +0x18c
    float CellSize() const { return cellSize_; }                           // +0x190
    const std::vector<cS3DVector3>& DayNightColors() const { return dayNightColors_; }   // +0x40
    const std::vector<float>& ScatterStrengths() const { return scatterStrengths_; }     // +0x58
    const std::vector<cS3DVector3>& ScatterColors() const { return scatterColors_; }     // +0x70
    const cS3DVector3& SampledLight() const { return sampledLight_; }      // +0xd4
    const cS3DVector3& FloorLight() const { return floorLight_; }          // +0xac
    float ScatterStrength() const { return scatterStrength_; }             // +0x1b0
    const cS3DVector3& ScatterColour() const { return scatterColor_; }     // +0x1b4
    float NightFade() const { return nightFade_; }                         // +0xf8
    float NightShadowFade() const { return nightShadowFade_; }             // +0xfc
    const cS3DVector3& SunDirection() const { return sunDirectionRaw_; }   // +0x124
    const cS3DVector3& DirectionalLight(int index) const { return directionalLight_[index]; }   // +0x130..
    float EnvironmentFloor() const { return environmentFloor_; }           // +0x120
    ILightingColorMap* ColorMap(int index) const { return colorMaps_[index]; }   // +0x100..

    // ---- the original's private helpers, kept accessible to the test ---------------------------------
    void ReadProperties();                        // @ 0x100775b44
    void DoZoomAndRotationChange(int zoom, int rotation);   // @ 0x100776e40
    void FindHillShadows();                       // @ 0x1007772f4
    void SetTimeDependentLightingValues();        // @ 0x100777a4c
    cS3DVector3 GetModelColor(const cS3DVector3& direction);   // @ 0x1007788e6

private:
    // Original helper not visible in the decompile's prototype: passes `heights_.data() + offset` to the terrain's
    // GetAltitudes with the current hill strides (see DoMessage / DoZoomAndRotationChange).
    void FillHeights(const SC4RectT<int>& rect, std::ptrdiff_t offset);

    // +0x00 primary vptr, +0x08 message-target vptr (from the bases)
    bool initialized_ = false;                        // +0x10
    bool zoomRotationValid_ = false;                  // +0x11
    [[maybe_unused]] std::uint16_t reserved12_ = 0;                    // +0x12
    std::uint32_t refCount_ = 0;                      // +0x14
    cISCPropertyHolder* properties_ = nullptr;        // +0x18 (referenced)
    [[maybe_unused]] std::uint32_t reserved1c_ = 0;                    // +0x1c
    ILightingTerrain* terrainMap_ = nullptr;          // +0x20 (referenced)
    [[maybe_unused]] std::uint32_t reserved24_ = 0;                    // +0x24
    ILightingTerrain* terrain_ = nullptr;             // +0x28 (referenced)
    [[maybe_unused]] std::uint32_t reserved2c_ = 0;                    // +0x2c
    float timeOfDay_ = 12.0f;                         // +0x30
    std::int32_t seasonMonth_ = 0;                    // +0x34
    bool night_ = false;                              // +0x38
    [[maybe_unused]] std::uint8_t reserved39_[7] = {};                 // +0x39 (the original leaves these bytes untouched)
    std::vector<cS3DVector3> dayNightColors_;         // +0x40 / +0x48 / +0x50
    std::vector<float> scatterStrengths_;             // +0x58 / +0x60 / +0x68
    std::vector<cS3DVector3> scatterColors_;          // +0x70 / +0x78 / +0x80
    cS3DVector3 diffuseDirection_ = {0, 0, 0};        // +0x88
    cS3DVector3 sideDirection_ = {0, 0, 0};           // +0x94
    cS3DVector3 rotatedSun_ = {0, 0, 0};              // +0xa0
    cS3DVector3 floorLight_ = {0, 0, 0};              // +0xac
    float rotationMatrix_[4] = {0, 0, 0, 0};          // +0xb8 (cos, -sin, sin, cos)
    cS3DVector3 globalLight_ = {1, 1, 1};             // +0xc8
    cS3DVector3 sampledLight_ = {1, 1, 1};            // +0xd4
    cS3DVector3 previousLight_ = {0, 0, 0};           // +0xe0
    float previousNightValue_ = 0.0f;                 // +0xec
    float previousSampledY_ = 0.0f;                   // +0xf0
    float previousSampledZ_ = 0.0f;                   // +0xf4
    float nightFade_ = 1.0f;                          // +0xf8
    float nightShadowFade_ = 0.0f;                    // +0xfc
    ILightingColorMap* colorMaps_[4] = {nullptr, nullptr, nullptr, nullptr};   // +0x100 / +0x108 / +0x110 / +0x118
    float environmentFloor_ = 0.0f;                   // +0x120
    cS3DVector3 sunDirectionRaw_ = {1.0f, -1.0f, -1.0f};   // +0x124
    cS3DVector3 directionalLight_[6] = {};            // +0x130..+0x177
    float cellCountX_ = 0.0f;                         // +0x178
    float cellCountZ_ = 0.0f;                         // +0x17c
    float cellCountXPlus1_ = 0.0f;                    // +0x180
    float cellCountZPlus1_ = 0.0f;                    // +0x184
    float terrainWidth_ = 0.0f;                       // +0x188
    float terrainDepth_ = 0.0f;                       // +0x18c
    float cellSize_ = 0.0f;                           // +0x190
    float inverseCellSize_ = 0.0f;                    // +0x194
    cS3DVector3 hillCentre_ = {0, 0, 0};              // +0x198
    cS3DVector3 hillAxis_ = {0, 0, 0};                // +0x1a4
    float scatterStrength_ = 0.0f;                    // +0x1b0
    cS3DVector3 scatterColor_ = {0, 0, 0};            // +0x1b4
    bool tintEnabled_ = false;                        // +0x1c4
    cS3DVector3 tintColor_ = {1, 1, 1};               // +0x1c8
    float tintStrength_ = 1.0f;                       // +0x1d4
    std::vector<float> heights_;                      // +0x1d8 / +0x1e0 / +0x1e8
    std::vector<float> crests_;                       // +0x1f0 / +0x1f8 / +0x200
    bool accumulating_ = false;                       // +0x208
    SC4RectT<int> accumulateRect_;                    // +0x20c
    SC4RectT<int> hillRect_;                          // +0x21c
    std::int32_t strideX_ = 0;                        // +0x22c
    std::int32_t strideZ_ = 0;                        // +0x230
    std::int32_t heightBase_ = 0;                     // +0x234
    bool hillShadows_ = false;                        // +0x238
    ILightingHost* host_ = nullptr;                   // PORT-ONLY (the original read the SL::sp* globals)
};

// ---------------------------------------------------------------------------------------------
// The renderer's two-call view (src/sc4/DrawContext.h IS4LightingManager, DrawContext +0x68 / cSC43DRender +0x178).
// It forwards to the recovered manager and owns it: the renderer deletes the view at Shutdown (0x1007602f6 allocates
// the manager and 0x10075f?dee's shutdown block deletes it). Returns null for a null manager.
IS4LightingManager* CreateLightingManagerView(cSC4LightingManager* manager);

}  // namespace sc4
