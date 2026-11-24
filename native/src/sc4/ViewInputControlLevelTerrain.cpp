#include "sc4/ViewInputControlLevelTerrain.h"

#include "sc4/cSTETerrain.h"

namespace sc4 {

namespace {
ILevelTerrainHost* g_host = nullptr;
constexpr std::uint32_t kLevelTerrainCursor = 0x071e5703;
}  // namespace
ILevelTerrainHost* LevelTerrainHost() { return g_host; }
void SetLevelTerrainHost(ILevelTerrainHost* host) { g_host = host; }

cSC4ViewInputControlLevelTerrain::cSC4ViewInputControlLevelTerrain() {
    id_ = kLevelTerrainToolId;
    cursorId_ = kLevelTerrainCursor;
}

bool cSC4ViewInputControlLevelTerrain::Init() {
    if (initialized_) return true;
    cSC4ViewInputControl::Init();
    terrain_ = g_host ? g_host->Terrain() : nullptr;
    if (terrain_) cellWidth_ = cSTETerrain::CellWidth();   // terrain vslot 0x18
    return true;
}

bool cSC4ViewInputControlLevelTerrain::Shutdown() {
    if (!initialized_) return true;
    if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
    }
    if (g_host) g_host->ClearSelection();
    terrain_ = nullptr;
    return cSC4ViewInputControl::Shutdown();
}

void cSC4ViewInputControlLevelTerrain::ComputeAndMarkSelectedArea() {
    left_ = startX_ < endX_ ? startX_ : endX_;
    right_ = startX_ < endX_ ? endX_ : startX_;
    top_ = startZ_ < endZ_ ? startZ_ : endZ_;
    bottom_ = startZ_ < endZ_ ? endZ_ : startZ_;
    if (g_host) g_host->MarkSelected(TerrainRect{left_, top_, right_, bottom_});
}

// Every vertex of the rectangle and one past its far edges (clamped to the city's vertex range) to the start vertex's altitude.
void cSC4ViewInputControlLevelTerrain::LevelTerrain() {
    if (!terrain_) return;
    const int cellsX = static_cast<int>(terrain_->CellCountX());   // vslot 0x1c
    const int cellsZ = static_cast<int>(terrain_->CellCountZ());   // vslot 0x20
    const float altitude = terrain_->GetAltitude((cellsX + 1) * startZ_ + startX_);   // vslot 0x34 (vertex index)
    TerrainRect rect;
    rect.left = left_;
    rect.top = top_;
    rect.right = right_ + 1 <= cellsX ? right_ + 1 : cellsX;
    rect.bottom = bottom_ + 1 <= cellsZ ? bottom_ + 1 : cellsZ;
    terrain_->SetAltitude(rect, altitude, true, 1);   // vslot 0xcc
}

bool cSC4ViewInputControlLevelTerrain::OnMouseDownL(int x, int y, std::uint32_t) {
    if (!g_host) return false;
    float hit[3] = {0.0f, 0.0f, 0.0f};
    if (g_host->PickTerrain(x, y, hit)) {
        dragging_ = true;
        SetCapture();
        startX_ = endX_ = static_cast<int>(hit[0] / cellWidth_);
        startZ_ = endZ_ = static_cast<int>(hit[2] / cellWidth_);
        selectionShown_ = g_host->ShowSelection(false);
    }
    return true;
}

bool cSC4ViewInputControlLevelTerrain::OnMouseMove(int x, int y, std::uint32_t) {
    if (!dragging_) return false;
    float hit[3] = {0.0f, 0.0f, 0.0f};
    if (g_host && g_host->PickTerrain(x, y, hit)) {
        const int cx = static_cast<int>(hit[0] / cellWidth_), cz = static_cast<int>(hit[2] / cellWidth_);
        if (cx != endX_ || cz != endZ_) {
            endX_ = cx;
            endZ_ = cz;
            ComputeAndMarkSelectedArea();
        }
    }
    return true;
}

bool cSC4ViewInputControlLevelTerrain::OnMouseUpL(int x, int y, std::uint32_t) {
    if (!dragging_) return false;
    float hit[3] = {0.0f, 0.0f, 0.0f};
    if (g_host && g_host->PickTerrain(x, y, hit)) {
        endX_ = static_cast<int>(hit[0] / cellWidth_);
        endZ_ = static_cast<int>(hit[2] / cellWidth_);
    }
    ComputeAndMarkSelectedArea();
    LevelTerrain();
    if (g_host) {
        g_host->ClearSelection();
        g_host->ShowSelection(selectionShown_);
    }
    ReleaseCapture();
    dragging_ = false;
    return true;
}

}  // namespace sc4
