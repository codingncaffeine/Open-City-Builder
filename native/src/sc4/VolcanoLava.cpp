// cSTEVolcanoLava — see sc4/VolcanoLava.h. The float expressions of the flow follow the disassembly (Ghidra regroups
// them), including the minss/ucomiss operand order, which decides what happens with NaN and with ties.
#pragma STDC FP_CONTRACT OFF
#include "sc4/VolcanoLava.h"

#include <cmath>
#include <cstring>

namespace sc4 {

namespace {
// minss dst, src: dst when dst < src, otherwise src.
inline float MinSS(float dst, float src) { return dst < src ? dst : src; }

constexpr float kTextureScalePerMeter = 0.00109375f;                  // 0x00a70c1c
constexpr float kTextureScale[kLavaZoomCount] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};   // 0x00850a8c
constexpr float kMaxSteamGap = 2.0f;                                  // 0x00a7072c
bool g_staticsInitialized = false;                                    // sbInited
}  // namespace

cSTEVolcanoLava::Tuning& cSTEVolcanoLava::TuningValues() {
    static Tuning tuning;
    return tuning;
}

void cSTEVolcanoLava::ReadTuningParameters() {
    if (!host_) return;
    const cISCPropertyHolder* holder = host_->LoadTuningExemplar();
    if (!holder) return;
    Tuning& t = TuningValues();
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e60, t.lavaColorGradientMapFactor);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e61, t.maxFlowRate);
    t.maxFlowRate = t.maxFlowRate / 1e+06f;
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e62, t.heightDiffForMaxFlowRate);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e63, t.liquidToSolidConversionFactor);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e64, t.minAlpha);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e65, t.minAlphaDepth);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e66, t.maxAlphaDepth);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e67, t.scorchedTerrainDecalSize);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e68, t.underWaterSolidificationMultiple);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e69, t.lavaHeightBurnThreshold);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e6a, t.lavaHeightDeepFryThreshold);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e6b, t.animationFrameDuration);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e6c, t.minAmountForDepositToHappen);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e6d, t.scorchDecalLifeTime);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e6e, t.scorchDecalFadeInTime);
    cSCPropertyHelp::GetPropertyValue(holder, 0x69987e6f, t.scorchDecalFadeOutTime);
}

void cSTEVolcanoLava::ResetStatics() { g_staticsInitialized = false; }

// The tuning is read once (StaticInit @ 0x004f9012; the textures and the gradient it loads there are the host's).
bool cSTEVolcanoLava::Init() {
    if (!host_) return false;
    if (!g_staticsInitialized) {
        g_staticsInitialized = true;
        ReadTuningParameters();
    }
    if (active_) return true;
    active_ = true;
    cellsX_ = host_->CityCellCountX();
    cellsZ_ = host_->CityCellCountZ();
    width_ = cellsX_ + 1;
    height_ = cellsZ_ + 1;
    steamCellsX_ = cellsX_ / 4;
    steamCellsZ_ = cellsZ_ / 4;
    steamMaxX_ = steamCellsX_ - 1;
    steamMaxZ_ = steamCellsZ_ - 1;
    cellSize_ = host_->CellWidth();
    InitSurfacePoints();
    host_->ListenForMessages(true);
    for (LavaSideVertex& vertex : side_.vertices) {
        vertex.color[0] = 0xff;
        vertex.color[1] = 0;
        vertex.color[2] = 0;
        vertex.color[3] = 0xff;
    }
    return true;
}

void cSTEVolcanoLava::InitSurfacePoints() {
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    vertices_.assign(count, LavaVertex{});
    lava_.assign(count, 0.0f);
    for (std::int32_t z = 0; z < height_; ++z) {
        for (std::int32_t x = 0; x < width_; ++x) {
            LavaVertex& vertex = vertices_[static_cast<std::size_t>(z * width_ + x)];
            vertex.x = static_cast<float>(x) * cellSize_;
            vertex.z = static_cast<float>(z) * cellSize_;
        }
    }
    textureZoom_ = host_ ? host_->CurrentZoom() : 0;
    AssignSurfaceTextureCoordinates(textureZoom_);
}

void cSTEVolcanoLava::AssignSurfaceTextureCoordinates(int zoom) {
    const float scale = kTextureScalePerMeter * kTextureScale[zoom < 0 ? 0 : (zoom >= kLavaZoomCount ? kLavaZoomCount - 1 : zoom)];
    for (LavaVertex& vertex : vertices_) {
        vertex.u = scale * vertex.x;
        vertex.v = scale * vertex.z;
    }
}

void cSTEVolcanoLava::SetDrawZoom(int zoom) {
    if (zoom == textureZoom_) return;
    AssignSurfaceTextureCoordinates(zoom);
    textureZoom_ = zoom;
}

bool cSTEVolcanoLava::Shutdown() {
    if (!active_) return true;
    active_ = false;
    if (host_) host_->TerrainPeriodicRefresh();
    RemoveDecals();
    RemoveSteamEffects();
    if (host_) {
        host_->ListenForMessages(false);
        if (registered_) host_->UnregisterFromRenderer();
        if (ticking_) host_->StopTicks();
    }
    registered_ = false;
    ticking_ = false;
    lava_.clear();
    vertices_.clear();
    return true;
}

void cSTEVolcanoLava::SetGrid(std::int32_t width, std::int32_t height, float cellSize) {
    width_ = width;
    height_ = height;
    cellsX_ = width - 1;
    cellsZ_ = height - 1;
    steamCellsX_ = cellsX_ / 4;
    steamCellsZ_ = cellsZ_ / 4;
    steamMaxX_ = steamCellsX_ - 1;
    steamMaxZ_ = steamCellsZ_ - 1;
    cellSize_ = cellSize;
    InitSurfacePoints();
}

float cSTEVolcanoLava::GetLavaHeightByCityCell(int x, int z) {
    if (lava_.empty()) return 0.0f;
    const int index = z * width_ + x;
    // the original allows index == width * height, which reads one float past the grid; that reads 0 here
    if (-1 < index && index <= width_ * height_)
        return static_cast<std::size_t>(index) < lava_.size() ? lava_[static_cast<std::size_t>(index)] : 0.0f;
    return 0.0f;
}

int cSTEVolcanoLava::GetNeighborWithLowestLavaLevel(int index) {
    const std::uint32_t mask = host_->GetNeighbors(index % width_, index / width_);
    const long base = index;
    const float here = lava_[static_cast<std::size_t>(base)] + host_->TerrainAltitude(static_cast<std::int32_t>(base));
    static const float kRoot2 = 1.4142135f;
    float best = 0.0f;
    int pick = -1;
    for (int direction = 0; direction <= 6; direction += 2) {                  // the straight neighbors
        if (((mask & 0xff) >> direction) & 1) {
            const long other = host_->NeighborOffset(direction) + base;
            const float drop = here - (lava_[static_cast<std::size_t>(other)] + host_->TerrainAltitude(static_cast<std::int32_t>(other)));
            if (best < drop) {
                pick = direction;
                best = drop;
            }
        }
    }
    best = best * kRoot2;                                                       // a diagonal has to beat that
    for (int direction = 1; direction <= 7; direction += 2) {
        if (((mask & 0xff) >> direction) & 1) {
            const long other = host_->NeighborOffset(direction) + base;
            const float drop = here - (lava_[static_cast<std::size_t>(other)] + host_->TerrainAltitude(static_cast<std::int32_t>(other)));
            if (best < drop) {
                pick = direction;
                best = drop;
            }
        }
    }
    return pick;
}

void cSTEVolcanoLava::StartLavaFlow(float x, float z, float radius, bool withTicks) {
    // round-to-nearest-even through the 1.5 * 2^23 trick, then the tie fix: this is a floor for finite values
    auto cell = [](float position) {
        const float scaled = position * 0.0625f;
        const float shifted = scaled + -0.5f + 12582912.0f;
        const std::int32_t bits = std::bit_cast<std::int32_t>(shifted);
        const std::int32_t low = bits - 0x4b400000;
        std::int32_t result = bits - 0x4b3fffff;
        const float candidate = static_cast<float>(result);
        if (scaled != candidate) result = low;
        if (std::isnan(scaled) || std::isnan(candidate)) result = low;
        return result;
    };
    const std::int32_t cellX = cell(x);
    const std::int32_t cellZ = cell(z);
    host_->CreateVertRectAroundVertex(startRect_, cellX, cellZ, static_cast<int>(radius / cellSize_));
    bounds_ = startRect_;
    flowEpoch_ = 0;
    CreateBurnAroundRegion(cellX, cellZ);
    if (!registered_) registered_ = host_->RegisterWithRenderer();
    if (withTicks && !ticking_) ticking_ = host_->StartTicks();
}

// The cells around the vent that catch fire while the vent produces lava, visited in a random order.
void cSTEVolcanoLava::CreateBurnAroundRegion(int x, int z) {
    const std::int32_t lastZ = cellsZ_ - 1;
    std::int32_t lastX = cellsX_ - 1;
    std::int32_t left = x - burnRadius_, top = 0, bottom = z + burnRadius_;
    const std::int32_t firstZ = z - burnRadius_, right = x + burnRadius_;
    if (right < 0 || lastX < left || bottom < 0 || lastZ < firstZ) {
        bottom = 0;
        lastX = 0;
        top = 0;
        left = 0;
    } else {
        if (left < 1) left = 0;
        top = 0;
        if (0 < firstZ) top = firstZ;
        if (right < lastX) lastX = right;
        if (lastZ <= bottom) bottom = lastZ;
    }
    if (burnCells_.IsModular()) burnCells_.SetModular(false);
    burnCells_.SetRect(left, top, lastX, bottom);
    burnCells_.Reset(true);
}

void cSTEVolcanoLava::BurnSurroundingStuff() {
    for (int i = 0; i < (burnRadius_ * burnRadius_) / 6; ++i) {
        int x = 0, z = 0;
        if (!burnCells_.Next(x, z)) {
            burnCells_.Reset(false);
        } else if (host_->IsCellFlammable(x, z)) {
            host_->IgniteCell(x, z);
        }
    }
}

// Lava reaching a vertex: when it crosses the burn threshold the two by two cells before the vertex catch fire; when it
// crosses the deep-fry threshold they burn down at once.
void cSTEVolcanoLava::SetThingsOnFire(int index, float lavaDepth, float amount) {
    const Tuning& t = TuningValues();
    bool ignite = false, deepFry = false;
    if (t.lavaHeightBurnThreshold <= lavaDepth || lavaDepth + amount < t.lavaHeightBurnThreshold) {
        if (t.lavaHeightDeepFryThreshold <= lavaDepth) return;
        if (lavaDepth + amount < t.lavaHeightDeepFryThreshold) return;
        deepFry = true;
    } else {
        ignite = true;
    }
    const int row = index / width_;
    const int column = index - width_ * row;
    for (int x = column - 1; x <= column; ++x) {
        for (int z = row - 1; z <= row; ++z) {
            if (!host_->IsCellFlammable(x, z)) continue;
            if (ignite) host_->IgniteCell(x, z);
            else if (deepFry) host_->QuickBurnCell(x, z);
            host_->SetFireCause(kLavaFireCause);
        }
    }
}

void cSTEVolcanoLava::DoLavaFlow(std::uint32_t deltaTime) {
    float production = productionRate_;
    LavaRect dirty{0x7fffffff, 0x7fffffff, static_cast<std::int32_t>(0x80000000), static_cast<std::int32_t>(0x80000000)};
    if (!active_) return;
    const Tuning& t = TuningValues();
    const float dt = static_cast<float>(deltaTime);
    const float solid = solidificationRate_ * dt;
    const float underwater = t.underWaterSolidificationMultiple * solid;

    // new lava at the vent
    if (startRect_.top <= startRect_.bottom) {
        production = production * dt;
        for (std::int32_t z = startRect_.top; z <= startRect_.bottom; ++z) {
            for (std::int32_t x = startRect_.left; x <= startRect_.right; ++x) {
                float& cellDepth = lava_[static_cast<std::size_t>(width_ * z + x)];
                cellDepth = cellDepth + production;
                if (x < bounds_.left) bounds_.left = x;
                if (bounds_.right < x) bounds_.right = x;
                if (z < bounds_.top) bounds_.top = z;
                if (bounds_.bottom < z) bounds_.bottom = z;
            }
        }
    }

    // flow, solidification and deposit over the active area
    if (bounds_.top <= bounds_.bottom) {
        const float cap = MinSS(1.0f, solid);
        const float capUnderwater = MinSS(1.0f, underwater);
        const float edgeLoss = 1.0000000116860974e-07f * dt;                    // 0x33d6bf95
        const std::int32_t stride = host_->TerrainStride();
        for (std::int32_t z = bounds_.top; z <= bounds_.bottom; ++z) {
            if (bounds_.left > bounds_.right) continue;
            for (std::int32_t x = bounds_.left; x <= bounds_.right; ++x) {
                const std::int32_t index = width_ * z + x;
                const std::size_t slot = static_cast<std::size_t>(index);
                float depth = lava_[slot];
                if (0.0f >= depth) continue;
                const std::uint32_t row = static_cast<std::uint32_t>(index) / static_cast<std::uint32_t>(stride);
                const std::int32_t rowStart = static_cast<std::int32_t>(row) * stride;
                float flow;
                int target = -1;
                bool measured = true;
                if (index - rowStart == 0 || static_cast<std::uint32_t>(index) < static_cast<std::uint32_t>(stride) ||
                    index - rowStart == host_->TerrainMaxX() || static_cast<std::int32_t>(row) == host_->TerrainMaxZ()) {
                    flow = depth * edgeLoss;                                    // lava at the rim of the terrain drains away
                } else {
                    const int direction = GetNeighborWithLowestLavaLevel(index);
                    if (direction < 0) {
                        measured = false;
                        flow = 0.0f;
                    } else {
                        target = index + host_->NeighborOffset(direction);
                        const std::size_t other = static_cast<std::size_t>(target);
                        float drop = (host_->TerrainAltitude(index) + depth) - (lava_[other] + host_->TerrainAltitude(target));
                        drop = drop * 0.5f;
                        float rate = t.maxFlowRate;
                        if (!(drop >= t.heightDiffForMaxFlowRate)) rate = (rate * drop) / t.heightDiffForMaxFlowRate;
                        const float movable = MinSS(depth, drop);
                        const float wanted = (rate * movable) * dt;
                        flow = MinSS(movable, wanted);
                    }
                }
                if (measured && flow > 0.0f) {
                    if (target >= 0) {
                        const std::size_t other = static_cast<std::size_t>(target);
                        SetThingsOnFire(target, lava_[other], flow);
                        lava_[other] = lava_[other] + flow;
                    }
                    lava_[slot] = lava_[slot] - flow;
                }
                if (solid > 0.0f) {
                    const float remaining = lava_[slot];
                    if (t.minAmountForDepositToHappen <= remaining || std::isnan(remaining)) {
                        const bool water = host_->IsWaterVertex(index);
                        const float settled = (water ? capUnderwater : cap) * lava_[slot];
                        lava_[slot] = lava_[slot] - settled;
                        AddDecal(x, z);
                        host_->RaiseVertex(static_cast<std::uint32_t>(index), settled * t.liquidToSolidConversionFactor);
                        if (x < dirty.left) dirty.left = x;
                        if (dirty.right < x) dirty.right = x;
                        if (z < dirty.top) dirty.top = z;
                        if (dirty.bottom < z) dirty.bottom = z;
                    } else {
                        lava_[slot] = (remaining < 0.1f) ? 0.0f : remaining * 0.9f;
                    }
                }
            }
        }
    }
    host_->PadRectangle(bounds_, 1);
    PrepareForDisplay();
    if (dirty.left <= dirty.right && dirty.top <= dirty.bottom) {
        host_->TerrainRegionChanged(dirty);
        const std::int32_t counter = frameCounter_;
        frameCounter_ = counter + 1;
        if (counter >= 99) {
            host_->TerrainPeriodicRefresh();
            frameCounter_ = 0;
        }
        host_->TerrainRefresh();
    }
}

void cSTEVolcanoLava::GetColorForLavaDepth(float depth, std::uint8_t& red, std::uint8_t& green, std::uint8_t& blue) {
    std::int32_t row = static_cast<std::int32_t>(depth * TuningValues().lavaColorGradientMapFactor);
    const std::int32_t last = host_->ColorGradientLastRow();
    if (row < 0) row = 0;
    else if (last < row) row = last;
    const std::uint32_t pixel = host_->ColorGradientPixel(row);
    red = static_cast<std::uint8_t>(pixel >> 16);
    green = static_cast<std::uint8_t>(pixel >> 8);
    blue = static_cast<std::uint8_t>(pixel);
}

// The surface over the active rectangle (one vertex wider): its height, color and transparency by depth, the steam
// where it stands in water; the active rectangle shrinks to the vertices that hold lava.
void cSTEVolcanoLava::PrepareForDisplay() {
    UpdateSteamEffects();
    if (host_->ColorGradientLastRow() < 0) return;
    const Tuning& t = TuningValues();
    const float alphaSpan = t.maxAlphaDepth - t.minAlphaDepth;
    const std::uint8_t alphaRange = static_cast<std::uint8_t>(~t.minAlpha);
    LavaRect rect = bounds_;
    host_->PadRectangle(rect, 1);
    LavaRect held{0x7fffffff, 0x7fffffff, static_cast<std::int32_t>(0x80000000), static_cast<std::int32_t>(0x80000000)};
    for (std::int32_t z = rect.top; z <= rect.bottom; ++z) {
        for (std::int32_t x = rect.left; x <= rect.right; ++x) {
            const std::int32_t index = z * width_ + x;
            const std::size_t slot = static_cast<std::size_t>(index);
            if (0.0f < lava_[slot]) {
                if (x < held.left) held.left = x;
                if (held.right < x) held.right = x;
                if (z < held.top) held.top = z;
                if (held.bottom < z) held.bottom = z;
            }
            LavaVertex& vertex = vertices_[slot];
            vertex.y = lava_[slot] + host_->TerrainAltitude(index);
            if (IsGoodVertexForPlacingSteam(index)) AddSteamEffect(x, z);
            GetColorForLavaDepth(lava_[slot], vertex.color[0], vertex.color[1], vertex.color[2]);
            const float depth = lava_[slot];
            if (0.0f < depth || std::isnan(depth)) {
                if (t.minAlphaDepth < depth) {
                    if (t.maxAlphaDepth <= depth) {
                        vertex.color[3] = 0xff;
                    } else {
                        vertex.color[3] = static_cast<std::uint8_t>(
                            static_cast<std::uint8_t>(static_cast<std::int32_t>(((depth - t.minAlphaDepth) * static_cast<float>(alphaRange)) / alphaSpan)) +
                            t.minAlpha);
                    }
                } else {
                    vertex.color[3] = t.minAlpha;
                }
            } else {
                vertex.color[3] = 0;
            }
        }
    }
    bounds_ = held;
}

bool cSTEVolcanoLava::IsGoodVertexForPlacingSteam(int index) {
    const std::size_t slot = static_cast<std::size_t>(index);
    if (TuningValues().minAmountForDepositToHappen < lava_[slot] && host_->IsWaterVertex(index)) {
        const float gap = host_->WaterLevel(index) - vertices_[slot].y;
        if (0.0f <= gap && gap < kMaxSteamGap) return true;
    }
    return false;
}

// Steam and scorch decals are kept one per block of four by four cells.
std::int32_t cSTEVolcanoLava::SteamKey(int x, int z) const {
    std::int32_t column = steamMaxX_;
    if (x / 4 <= steamMaxX_) column = x / 4;
    std::int32_t row = steamMaxZ_;
    if (z / 4 <= steamMaxZ_) row = z / 4;
    return column + row * steamCellsX_;
}

void cSTEVolcanoLava::AddSteamEffect(int x, int z) {
    const std::int32_t key = SteamKey(x, z);
    if (steam_.find(key) != steam_.end()) return;
    const std::int32_t vertex = x + z * width_;
    const float xyz[3] = {static_cast<float>(x) * cellSize_, host_->WaterLevel(vertex), static_cast<float>(z) * cellSize_};
    void* effect = host_->StartSteam(key, xyz);
    if (!effect) return;
    steam_[key] = SteamEffect{vertex, effect};
}

void cSTEVolcanoLava::UpdateSteamEffects() {
    std::vector<std::int32_t> ended;
    for (auto& entry : steam_) {
        if (IsGoodVertexForPlacingSteam(entry.second.vertex)) continue;
        host_->StopSteam(entry.second.effect, false);
        entry.second.effect = nullptr;
        ended.push_back(entry.first);
    }
    for (std::int32_t key : ended) steam_.erase(key);
}

void cSTEVolcanoLava::RemoveSteamEffects() {
    for (auto& entry : steam_) {
        if (host_) host_->StopSteam(entry.second.effect, true);
        entry.second.effect = nullptr;
    }
    steam_.clear();
}

void cSTEVolcanoLava::MarkSteamEnded(std::uint32_t key) {
    const auto it = steam_.find(static_cast<std::int32_t>(key));
    if (it == steam_.end()) return;
    if (host_ && it->second.effect) host_->ReleaseSteam(it->second.effect);
    steam_.erase(it);
}

void cSTEVolcanoLava::AddDecal(int x, int z) {
    const std::int32_t key = SteamKey(x, z);
    if (decals_.find(key) != decals_.end()) return;
    const Tuning& t = TuningValues();
    const float xyz[3] = {static_cast<float>(x) * cellSize_, host_->TerrainAltitude(x + z * width_), static_cast<float>(z) * cellSize_};
    decals_[key] = DecalInfo{host_->AddScorchDecal(xyz, t.scorchedTerrainDecalSize, t.scorchDecalFadeInTime), 0};
}

void cSTEVolcanoLava::TickDecals(std::uint32_t deltaTime) {
    const Tuning& t = TuningValues();
    std::vector<std::int32_t> expired;
    for (auto& entry : decals_) {
        entry.second.age += deltaTime;
        if (t.scorchDecalLifeTime <= entry.second.age) {
            expired.push_back(entry.first);
            if (entry.second.decal) host_->RetireScorchDecal(entry.second.decal, t.scorchDecalFadeOutTime, kLavaDecalFadedOut);
        }
    }
    for (std::int32_t key : expired) decals_.erase(key);
}

void cSTEVolcanoLava::RemoveDecals() {
    for (auto& entry : decals_)
        if (host_ && entry.second.decal) host_->RetireScorchDecal(entry.second.decal, TuningValues().scorchDecalFadeOutTime, kLavaDecalRemoved);
    decals_.clear();
}

void cSTEVolcanoLava::DoAnimationTick(std::uint32_t deltaTime) {
    const std::uint32_t frames = host_->SurfaceFrameCount(textureZoom_);
    if (frames != 0) {
        frameTime_ += deltaTime;
        if (TuningValues().animationFrameDuration < frameTime_) {
            frameTime_ = 0;
            ++frame_;
            if (frames <= frame_) frame_ = 0;
        }
    }
    DoLavaFlow(deltaTime);
    if (0.0f < productionRate_) BurnSurroundingStuff();
    TickDecals(deltaTime);
}

void cSTEVolcanoLava::DoMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2) {
    if (!active_) return;
    if (type == kLavaMessageSteamEnded) MarkSteamEnded(data1);
    else if (type == kLavaMessageAnimationTick) DoAnimationTick(data2);
    else if (type == kLavaMessageShutdown) Shutdown();
}

// ------------------------------------------------------------------------------------------------- what is drawn

bool cSTEVolcanoLava::ComputeDrawRect(const LavaRect& visible, const LavaRect& terrainVisible) {
    visibleRect_ = visible;
    if (bounds_.right < visible.left || visible.right < bounds_.left || bounds_.bottom < visible.top || visible.bottom < bounds_.top) {
        drawRect_ = LavaRect{};
    } else {
        drawRect_.left = visible.left < bounds_.left ? bounds_.left : visible.left;
        drawRect_.top = visible.top < bounds_.top ? bounds_.top : visible.top;
        drawRect_.right = visible.right <= bounds_.right ? visible.right : bounds_.right;
        drawRect_.bottom = bounds_.bottom < visible.bottom ? bounds_.bottom : visible.bottom;
    }
    if (!(drawRect_.left <= drawRect_.right && drawRect_.top <= drawRect_.bottom)) return false;
    if (drawRect_.right < terrainVisible.left || terrainVisible.right < drawRect_.left || drawRect_.bottom < terrainVisible.top ||
        terrainVisible.bottom < drawRect_.top) {
        drawRect_ = LavaRect{};
    } else {
        if (drawRect_.left < terrainVisible.left) drawRect_.left = terrainVisible.left;
        if (drawRect_.top < terrainVisible.top) drawRect_.top = terrainVisible.top;
        if (terrainVisible.right <= drawRect_.right) drawRect_.right = terrainVisible.right;
        if (terrainVisible.bottom < drawRect_.bottom) drawRect_.bottom = terrainVisible.bottom;
    }
    return drawRect_.left <= drawRect_.right && drawRect_.top <= drawRect_.bottom;
}

void cSTEVolcanoLava::TriangleIndices(std::int32_t cellCountX, std::vector<std::uint16_t>& indices) {
    indices.clear();
    const std::uint16_t nextRow = static_cast<std::uint16_t>(cellCountX + 1);
    for (std::int32_t cell = 0; cell < cellCountX; ++cell) {
        const std::uint16_t here = static_cast<std::uint16_t>(cell), next = static_cast<std::uint16_t>(cell + 1);
        indices.push_back(here);
        indices.push_back(static_cast<std::uint16_t>(nextRow + here));
        indices.push_back(static_cast<std::uint16_t>(nextRow + next));
        indices.push_back(here);
        indices.push_back(static_cast<std::uint16_t>(nextRow + next));
        indices.push_back(next);
    }
}

// A run starts one cell before the first vertex that holds lava (in its row or the next) and ends one cell after the
// last; the run that reaches the rectangle's right edge is cut to the visible rectangle's.
void cSTEVolcanoLava::CollectSurfaceRuns(std::vector<LavaRun>& runs) const {
    runs.clear();
    for (std::int32_t row = drawRect_.top; row < drawRect_.bottom; ++row) {
        std::int32_t x = drawRect_.left;
        std::int32_t index = row * width_ + x;
        std::int32_t start = -1;
        for (; x <= drawRect_.right; ++x, ++index) {
            if (vertices_[static_cast<std::size_t>(index)].color[3] == 0 && vertices_[static_cast<std::size_t>(index + width_)].color[3] == 0) {
                if (-1 < start) {
                    std::int32_t end = x + 1;
                    if (cellsX_ < x + 1) end = cellsX_;
                    if (0 < (end - start) * 2) runs.push_back(LavaRun{row, start, end - start});
                    start = -1;
                }
            } else if (start < 0) {
                start = x - 1;
                if (start < 0) start = 0;
            }
        }
        if (-1 < start) {
            if (visibleRect_.right < x) x = visibleRect_.right;
            if (0 < (x - start) * 2) runs.push_back(LavaRun{row, start, x - start});
        }
    }
}

void cSTEVolcanoLava::AddSideQuad(std::int32_t from, std::int32_t to, bool alongZ, std::vector<LavaSideQuad>& quads) {
    const LavaVertex& a = vertices_[static_cast<std::size_t>(from)];
    const LavaVertex& b = vertices_[static_cast<std::size_t>(to)];
    if (a.color[3] == 0 && b.color[3] == 0) return;
    const float groundA = host_->TerrainAltitude(from), groundB = host_->TerrainAltitude(to);
    const float alongA = alongZ ? a.z : a.x, alongB = alongZ ? b.z : b.x;
    LavaSideVertex* v = side_.vertices;
    v[0].x = a.x; v[0].y = a.y;     v[0].z = a.z; v[0].u = alongA * kTextureScalePerMeter; v[0].v = a.y * kTextureScalePerMeter;
    v[1].x = a.x; v[1].y = groundA; v[1].z = a.z; v[1].u = alongA * kTextureScalePerMeter; v[1].v = groundA * kTextureScalePerMeter;
    v[2].x = b.x; v[2].y = groundB; v[2].z = b.z; v[2].u = alongB * kTextureScalePerMeter; v[2].v = groundB * kTextureScalePerMeter;
    v[3].x = b.x; v[3].y = b.y;     v[3].z = b.z; v[3].u = alongB * kTextureScalePerMeter; v[3].v = b.y * kTextureScalePerMeter;
    quads.push_back(side_);
}

void cSTEVolcanoLava::DrawZSide(int x, int fromZ, int toZ, std::vector<LavaSideQuad>& quads) {
    const std::int32_t step = fromZ < toZ ? width_ : -width_;
    std::int32_t count = toZ - fromZ;
    if (count < 0) count = -count;
    std::int32_t index = fromZ * width_ + x;
    for (std::int32_t n = 0; n != count; ++n) {
        AddSideQuad(index, index + step, true, quads);
        index += step;
    }
}

void cSTEVolcanoLava::DrawXSide(int z, int fromX, int toX, std::vector<LavaSideQuad>& quads) {
    const std::int32_t step = fromX < toX ? 1 : -1;
    std::int32_t count = toX - fromX;
    if (count < 0) count = -count;
    std::int32_t index = fromX + z * width_;
    for (std::int32_t n = 0; n != count; ++n) {
        AddSideQuad(index, index + step, false, quads);
        index += step;
    }
}

void cSTEVolcanoLava::CollectSideQuads(std::vector<LavaSideQuad>& quads) {
    quads.clear();
    if (!(drawRect_.top < 1 || cellsZ_ <= drawRect_.bottom || drawRect_.left < 1 || cellsX_ <= drawRect_.right)) return;
    if (drawRect_.top == 0) DrawXSide(0, drawRect_.right, drawRect_.left, quads);
    if (drawRect_.bottom == cellsZ_) DrawXSide(drawRect_.bottom, drawRect_.left, drawRect_.right, quads);
    if (drawRect_.left == 0) DrawZSide(0, drawRect_.top, drawRect_.bottom, quads);
    if (drawRect_.right == cellsX_) DrawZSide(drawRect_.right, drawRect_.bottom, drawRect_.top, quads);
}

}  // namespace sc4
