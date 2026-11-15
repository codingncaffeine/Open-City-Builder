// cSTEVolcanoLava — the lava of the volcano disaster (Mac i386 0x004f8170..0x004facd8, constructor 0x004f9728; the flow
// core was first recovered from the x86_64 build, 0x100751d20..0x100754c20).
//
// The lava is a depth of liquid on every terrain vertex. Each animation tick the vent's vertices gain lava at the
// production rate, every vertex passes lava to its lowest neighbor, and lava that is deep enough turns into ground at
// the solidification rate (faster under water), leaving a scorch decal. Lava reaching a new vertex sets the cells around
// it on fire, or burns them down at once when it is deep. Where deep lava meets water a steam effect plays. The surface
// is a vertex grid over the terrain: the height is ground + lava, the color comes from a gradient image by depth, and
// thin lava is transparent.
//
// What stays outside (ILavaHost): the terrain, the fire manager, the effects, the scorch decal occupants, the gradient
// image, the renderer and the tick manager. The drawing itself is the application's (it reads Vertices(), DrawRect()
// and the runs and side quads this class works out).
//
// Layout (i386): +0x10 initialized, +0x11 ticking, +0x12 registered with the renderer, +0x14 terrain, +0x18 effects
// manager, +0x1c cell width, +0x20/+0x24 city cells, +0x28/+0x2c steam grid (cells / 4), +0x30/+0x34 its last column and
// row, +0x38/+0x3c vertices per row / rows, +0x44 the surface vertices (0x20 each), +0x48 the lava depths, +0x4c the vent
// rectangle, +0x5c the active rectangle, +0x6c the visible rectangle, +0x7c the drawn rectangle, +0x8c the terrain's
// vertices, +0xa8 the side quad, +0x108 production rate (initially 0x36a7c5ac), +0x10c solidification rate, +0x110 the
// steam effects, +0x128 the scorch decals, +0x140 texture zoom, +0x144 frame time, +0x148 frame, +0x14c the fire manager,
// +0x150 the random cell iterator, +0x180 burn radius (7), +0x184 terrain refresh counter (initially 100).
#pragma once
#include <bit>
#include <cstdint>
#include <map>
#include <vector>

#include "sc4/RandomIterator.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

struct LavaRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

// A surface vertex: vertex format 11 (position, color, two texture coordinate pairs), 0x20 bytes.
struct LavaVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;       // +0x00
    std::uint8_t color[4] = {0, 0, 0, 0};     // +0x0c red, green, blue of the gradient; +0x0f alpha (0 = no lava here)
    float u = 0.0f, v = 0.0f;                 // +0x10
    float u2 = 0.0f, v2 = 0.0f;               // +0x18 (always 0)
};
static_assert(sizeof(LavaVertex) == 0x20);

// A side vertex: vertex format 10 (position, color, one texture coordinate pair), 0x18 bytes.
struct LavaSideVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::uint8_t color[4] = {0xff, 0, 0, 0xff};   // Init's values
    float u = 0.0f, v = 0.0f;
};
static_assert(sizeof(LavaSideVertex) == 0x18);
struct LavaSideQuad {
    LavaSideVertex vertices[4];               // a triangle fan (primitive 6)
};

// A stretch of surface cells in one vertex row: `cellCount` quads from cell `firstCell`, drawn with the row's vertices
// and the next row's.
struct LavaRun {
    std::int32_t row = 0, firstCell = 0, cellCount = 0;
};

constexpr std::uint32_t kLavaMessageSteamEnded = 0x69e65264;
constexpr std::uint32_t kLavaMessageShutdown = 0x26d31ec2;
constexpr std::uint32_t kLavaMessageAnimationTick = 0x86ad10ee;
constexpr std::uint32_t kLavaFireCause = 3;
constexpr int kLavaDecalFadedOut = 1, kLavaDecalRemoved = 2;   // the scorch decal's completion actions
constexpr int kLavaZoomCount = 5;

// The terrain and city services the lava uses.
class ILavaHost {
public:
    virtual ~ILavaHost() = default;
    // the city: vslots 0x174 / 0x178
    virtual std::int32_t CityCellCountX() = 0;
    virtual std::int32_t CityCellCountZ() = 0;
    // terrain
    virtual float CellWidth() = 0;                                                  // cSTETerrain vslot 0x18
    virtual std::uint8_t GetNeighbors(std::int32_t x, std::int32_t z) = 0;          // cSTETerrain::GetNeighbors
    virtual std::int32_t NeighborOffset(int direction) = 0;                         // terrain +0x84 + direction * 4
    virtual std::int32_t TerrainStride() = 0;                                       // terrain +0x38
    virtual std::int32_t TerrainMaxX() = 0;                                         // terrain +0x28
    virtual std::int32_t TerrainMaxZ() = 0;                                         // terrain +0x2c
    virtual float TerrainAltitude(std::int32_t index) = 0;                          // the terrain's vertex (+0x6c, 0x24 each, +0)
    virtual float WaterLevel(std::int32_t index) = 0;                               // vslot 0x78 GetWaterLevel(int)
    virtual bool IsWaterVertex(std::int32_t index) = 0;
    virtual void RaiseVertex(std::uint32_t index, float amount) = 0;
    virtual void PadRectangle(LavaRect& rect, int amount) = 0;
    virtual void CreateVertRectAroundVertex(LavaRect& rect, int x, int z, int radius) = 0;
    virtual void TerrainRegionChanged(const LavaRect& changed) = 0;                 // vslot 0x114 RedisplayTerrain(1, 1, rect, 3)
    virtual void TerrainPeriodicRefresh() = 0;                                      // vslot 0x13c StopAccumulatingTextureUpdates
    virtual void TerrainRefresh() = 0;                                              // vslot 0x138 StartAccumulatingTextureUpdates
    virtual const cISCPropertyHolder* LoadTuningExemplar() = 0;                     // {0x6534284a, 0x88cd66e9, 5}, borrowed
    // The steam where lava meets water: the effect "lava_meets_water" with its finish message kLavaMessageSteamEnded
    // carrying `key`, placed at `xyz` and started. Null when there are no effects.
    virtual void* StartSteam(std::int32_t key, const float xyz[3]) = 0;
    virtual void StopSteam(void* steam, bool hard) = 0;                             // Stop(hard) then Release
    virtual void ReleaseSteam(void* steam) = 0;                                     // port: the original drops an ended effect unreleased
    // The scorch decal: a terrain decal occupant with texture {0x7ab50e44, 0x891b0e1a, 0x0999d1b7}, `size` across,
    // fading in over `fadeIn` seconds, put into the occupant manager at `xyz`.
    virtual void* AddScorchDecal(const float xyz[3], float size, float fadeIn) = 0;
    // SetOpacityFade(fadeOut, action, 1, 0) then Release.
    virtual void RetireScorchDecal(void* decal, float fadeOut, int completionAction) = 0;
    // fire: the flammability simulator's IsCellFlammable, the fire manager's IgniteCell(x, z, false, true, 0),
    // QuickBurnCell(x, z) and SetCause
    virtual bool IsCellFlammable(std::int32_t x, std::int32_t z) = 0;
    virtual void IgniteCell(std::int32_t x, std::int32_t z) = 0;
    virtual void QuickBurnCell(std::int32_t x, std::int32_t z) = 0;
    virtual void SetFireCause(std::uint32_t cause) = 0;
    // The gradient image {0x7ab50e44, 0x891b0e1a, 0xc9909fb8}: its last row (height - 1; below 0 when it did not load) and
    // the pixel of column 0 in a row (0x00RRGGBB).
    virtual std::int32_t ColorGradientLastRow() = 0;
    virtual std::uint32_t ColorGradientPixel(std::int32_t row) = 0;
    // The animated surface texture of a zoom ({0x7ab50e44, 0x891b0e1a, 0x09909b00 + zoom}): its frames, 0 when missing.
    virtual std::uint32_t SurfaceFrameCount(std::int32_t zoom) = 0;
    virtual std::int32_t CurrentZoom() = 0;                                         // the view utilities' zoom (+0xc); 0 without them
    virtual bool RegisterWithRenderer() = 0;                                        // spRenderer vslot 0x80 (this, 5, 1000)
    virtual void UnregisterFromRenderer() = 0;                                      // vslot 0x84
    virtual bool StartTicks() = 0;                                                  // the animation tick manager's vslot 0x14 (0xc0)
    virtual void StopTicks() = 0;                                                   // vslot 0x18
    virtual void ListenForMessages(bool listen) = 0;                                // spMS2: kLavaMessageShutdown, kLavaMessageSteamEnded
};

class cSTEVolcanoLava {
public:
    explicit cSTEVolcanoLava(ILavaHost* host = nullptr) : host_(host) {}

    // Tuning constants (statics in the original; defaults are the values the binary starts with, 0x00abc680..0x00abc6bc).
    struct Tuning {
        float lavaColorGradientMapFactor = 6.4f;
        float maxFlowRate = 8.0000001e-7f;            // stored divided by 1e6 after loading
        float heightDiffForMaxFlowRate = 12.0f;
        float liquidToSolidConversionFactor = 0.8f;
        std::uint8_t minAlpha = 20;                   // a Uint8 property
        float minAlphaDepth = 0.6f, maxAlphaDepth = 2.0f;
        float scorchedTerrainDecalSize = 60.0f;
        float underWaterSolidificationMultiple = 5.0f;
        float lavaHeightBurnThreshold = 0.01f, lavaHeightDeepFryThreshold = 2.0f;
        std::uint32_t animationFrameDuration = 80000;
        float minAmountForDepositToHappen = 2.0f;
        std::uint32_t scorchDecalLifeTime = 20000000;
        float scorchDecalFadeInTime = 2.2f, scorchDecalFadeOutTime = 6.1f;
    };
    static Tuning& TuningValues();
    void ReadTuningParameters();                                  // @ 0x004f8bf0
    static void ResetStatics();                                   // the tuning is read again by the next Init (sbInited)

    bool Init();                                                  // @ 0x004f91b2
    bool Shutdown();                                              // @ 0x004f9cf4
    bool IsInitialized() const { return active_; }

    // The grid the flow runs on, set directly (tests): `width` x `height` vertices, `cellSize` meters per vertex.
    void SetGrid(std::int32_t width, std::int32_t height, float cellSize);
    std::vector<float>& LavaDepth() { return lava_; }

    void SetLavaProductionRate(float rate) { productionRate_ = rate; }          // @ 0x004f8e00
    void SetSolidificationRate(float rate) { solidificationRate_ = rate; }      // @ 0x004f8e12
    float LavaProductionRate() const { return productionRate_; }
    float SolidificationRate() const { return solidificationRate_; }
    void StartLavaFlow(float x, float z, float radius, bool withTicks);          // @ 0x004fa97a
    void DoLavaFlow(std::uint32_t deltaTime);                                    // @ 0x004fa176
    int GetNeighborWithLowestLavaLevel(int index);                               // @ 0x004f894a
    float GetLavaHeightByCityCell(int x, int z);                                 // @ 0x004f8ea2
    void DoAnimationTick(std::uint32_t deltaTime);                               // @ 0x004fabae
    // DoMessage @ 0x004fac48: `data1` for the steam's end, `data2` for the tick.
    void DoMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2);

    void SetThingsOnFire(int index, float lavaDepth, float amount);              // @ 0x004f81d8
    void CreateBurnAroundRegion(int x, int z);                                   // @ 0x004fa8b0
    void BurnSurroundingStuff();                                                 // @ 0x004faad6
    void PrepareForDisplay();                                                    // @ 0x004f9f3c
    bool IsGoodVertexForPlacingSteam(int index);                                 // @ 0x004f8b76
    void AddSteamEffect(int x, int z);                                           // @ 0x004f982c
    void UpdateSteamEffects();                                                   // @ 0x004f9e38
    void RemoveSteamEffects();                                                   // @ 0x004f9c8c
    void MarkSteamEnded(std::uint32_t key);                                      // @ 0x004f96e4
    void AddDecal(int x, int z);                                                 // @ 0x004f9986
    void TickDecals(std::uint32_t deltaTime);                                    // @ 0x004f9b7a
    void RemoveDecals();                                                         // @ 0x004f9b08
    void AssignSurfaceTextureCoordinates(int zoom);                              // @ 0x004f8170
    void GetColorForLavaDepth(float depth, std::uint8_t& red, std::uint8_t& green, std::uint8_t& blue);   // @ 0x004f88ea

    // ---- what the drawing needs (Update @ 0x004fa640 and the DrawLava* family)
    // True when there is lava to draw.
    bool HasLava() const { return bounds_.left <= bounds_.right && bounds_.top <= bounds_.bottom; }
    // The drawn rectangle: the active rectangle cut to the visible cells and to the terrain's visible rectangle; false
    // when nothing of it is on screen. Keeps `visible` (the last run of a row is cut to its right edge).
    bool ComputeDrawRect(const LavaRect& visible, const LavaRect& terrainVisible);
    const LavaRect& DrawRect() const { return drawRect_; }
    // The texture coordinates follow the drawing zoom (DrawLavaSurface @ 0x004f9640).
    void SetDrawZoom(int zoom);
    int TextureZoom() const { return textureZoom_; }
    std::uint32_t Frame() const { return frame_; }
    // DrawLavaSurfaceWithoutVertBufExtension @ 0x004f947e: the cells to draw, row by row.
    void CollectSurfaceRuns(std::vector<LavaRun>& runs) const;
    // The index list of a row of cells (InitTriangleIndices @ 0x004f8a42): six per cell over this row's vertices and the
    // next row's.
    static void TriangleIndices(std::int32_t cellCountX, std::vector<std::uint16_t>& indices);
    // DrawSides @ 0x004f87d0: the walls at the edge of the city, where the lava surface stands above the ground.
    void CollectSideQuads(std::vector<LavaSideQuad>& quads);
    const std::vector<LavaVertex>& Vertices() const { return vertices_; }
    std::int32_t VertsX() const { return width_; }
    std::int32_t VertsZ() const { return height_; }

    bool FlowActive() const { return active_; }
    bool Registered() const { return registered_; }
    bool Ticking() const { return ticking_; }
    const LavaRect& StartRect() const { return startRect_; }
    const LavaRect& Bounds() const { return bounds_; }
    int FrameCounter() const { return frameCounter_; }
    void SetActive(bool active) { active_ = active; }
    std::size_t SteamCount() const { return steam_.size(); }
    std::size_t DecalCount() const { return decals_.size(); }
    int BurnRadius() const { return burnRadius_; }

private:
    struct SteamEffect {               // tSteamEffect
        std::int32_t vertex = 0;
        void* effect = nullptr;
    };
    struct DecalInfo {                 // tDecalInfo
        void* decal = nullptr;
        std::uint32_t age = 0;         // microseconds
    };
    std::int32_t SteamKey(int x, int z) const;
    void InitSurfacePoints();                                                    // @ 0x004f8a98
    void DrawZSide(int x, int fromZ, int toZ, std::vector<LavaSideQuad>& quads); // @ 0x004f846a
    void DrawXSide(int z, int fromX, int toX, std::vector<LavaSideQuad>& quads); // @ 0x004f861e
    void AddSideQuad(std::int32_t from, std::int32_t to, bool alongZ, std::vector<LavaSideQuad>& quads);

    ILavaHost* host_;
    bool active_ = false;                // +0x10
    bool ticking_ = false;               // +0x11
    bool registered_ = false;            // +0x12
    float cellSize_ = 16.0f;             // +0x1c
    std::int32_t cellsX_ = 0, cellsZ_ = 0;             // +0x20, +0x24
    std::int32_t steamCellsX_ = 0, steamCellsZ_ = 0;   // +0x28, +0x2c
    std::int32_t steamMaxX_ = 0, steamMaxZ_ = 0;       // +0x30, +0x34
    std::int32_t width_ = 0, height_ = 0;              // +0x38, +0x3c
    std::vector<LavaVertex> vertices_;   // +0x44
    std::vector<float> lava_;            // +0x48
    LavaRect startRect_;                 // +0x4c
    LavaRect bounds_;                    // +0x5c
    LavaRect visibleRect_;               // +0x6c
    LavaRect drawRect_;                  // +0x7c
    std::int32_t flowEpoch_ = 0;         // +0x90
    LavaSideQuad side_;                  // +0xa8
    float productionRate_ = std::bit_cast<float>(0x36a7c5acu);   // +0x108
    float solidificationRate_ = 0.0f;    // +0x10c
    std::map<std::int32_t, SteamEffect> steam_;    // +0x110
    std::map<std::int32_t, DecalInfo> decals_;     // +0x128
    std::int32_t textureZoom_ = 0;       // +0x140
    std::uint32_t frameTime_ = 0;        // +0x144
    std::uint32_t frame_ = 0;            // +0x148
    RandomIterator burnCells_;           // +0x150
    std::int32_t burnRadius_ = 7;        // +0x180
    std::int32_t frameCounter_ = 100;    // +0x184
};

}  // namespace sc4
