#include "sc4/ViewInputControlZone.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>

#include "rz/cRZString.h"

namespace sc4 {

// The binary's tables, read from 0x100a1eae0 / eb20 / eb60 / eba0 / ebe0 (16 entries each).
const std::uint32_t kZoneCursorId[16] = {0x42e434f7, 0x62e4352b, 0x016c4e26, 0x02e43524, 0x82e4350c, 0xa16c4e2a, 0xc2e43505, 0x42e43517,
                                         0xc16c4e30, 0x22e43512, 0x02e43524, 0x42e434ff, 0x62e43531, 0x02e43524, 0x02e4351e, 0x02e43524};
const std::uint32_t kZoneDragSound[16] = {0x0a5c32ab, 0x0a5c32ab, 0x0a5c32ab, 0x0a5c32ab, 0x0a5c32ab, 0x0a5c32ab, 0x0a5c32ab, 0x0a5c32ab,
                                          0x0a5c32ab, 0x0a5c32ab, 0, 0, 0, 0, 0x0a5c32ab, 0};
const std::uint32_t kZoneDragErrorSound[16] = {0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86,
                                               0x495a6d86, 0x495a6d86, 0, 0, 0, 0, 0x495a6d86, 0};
const std::uint32_t kZoneClickSound[16] = {0xca5c328b, 0xca5c328b, 0xca5c328b, 0xca5c328b, 0xca5c328b, 0xca5c328b, 0xca5c328b, 0xca5c328b,
                                           0xca5c328b, 0xca5c328b, 0, 0, 0, 0, 0xca5c328b, 0};
const std::uint32_t kZonePlopSound[16] = {0x8a5ec5b8, 0x8a5c329d, 0x8a5c329d, 0x8a5c329d, 0x8a5c329d, 0x8a5c329d, 0x8a5c329d, 0x8a5c329d,
                                          0x8a5c329d, 0x8a5c329d, 0, 0, 0, 0, 0x8a5c329d, 0};

namespace {
IZoneToolHost* g_host = nullptr;
constexpr std::uint32_t kStringGroup = 0x0a419226;
constexpr std::uint32_t kDemolishCountString = 0xca711a5e;   // "%u buildings will be demolished"
// The sounds only play for the zone types of the mask 0x43ff (types 0..9 and 14).
bool PlaysSound(int type) { return type >= 0 && type < 15 && ((0x43ffu >> type) & 1u) != 0; }
}  // namespace

IZoneToolHost* ZoneToolHost() { return g_host; }
void SetZoneToolHost(IZoneToolHost* host) { g_host = host; }

cSC4ViewInputControlZone::cSC4ViewInputControlZone(int zoneType) : zoneType_(zoneType) {
    id_ = kZoneToolId;                                                   // +0x10 = 0xa766c101
    cursorId_ = kZoneCursorId[static_cast<unsigned>(zoneType) & 15u];    // +0x14
}

cSC4ViewInputControlZone::~cSC4ViewInputControlZone() {
    if (outline_) outline_->Release();
    if (terrainView_) terrainView_->Release();
}

cISC4View3DWin* cSC4ViewInputControlZone::View() const { return g_host ? g_host->View() : nullptr; }

bool cSC4ViewInputControlZone::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kZoneToolMessageTarget) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    if (iid == 1 || iid == kGZIID_cISC4ViewInputControl) {
        *out = static_cast<cSC4ViewInputControl*>(this);
        AddRef();
        return true;
    }
    return false;
}

bool cSC4ViewInputControlZone::Init() {
    if (initialized_) return true;
    cSC4ViewInputControl::Init();
    if (!g_host) return true;
    cellSize_ = g_host->CellSize();                                      // terrain vslot 0x30
    if (!terrainView_) terrainView_ = g_host->TerrainView();             // terrain vslot 0x210
    if (terrainView_) terrainView_->AddRef();
    localGrid_.Init();
    if (cIGZMessageServer2* server = g_host->MessageServer()) server->AddNotification(AsTarget(), kZoneToolMsgGridChanged);
    if (!outline_) outline_ = g_host->CreateTileOutlineEffect();         // "local_tile_outline"
    return true;
}

bool cSC4ViewInputControlZone::Shutdown() {
    if (!initialized_) return true;
    EndInput();                                                          // vslot 0xe0
    if (cISC4View3DWin* view = View()) {
        view->ClearCursorText(kZoneToolCursorText);                      // 0x148
        view->ClearCostIndicator(false);                                 // 0x160
    }
    if (g_host) {
        if (cIGZMessageServer2* server = g_host->MessageServer()) server->RemoveNotification(AsTarget(), kZoneToolMsgGridChanged);
    }
    if (terrainView_) { terrainView_->Release(); terrainView_ = nullptr; }
    localGrid_.Shutdown();
    if (outline_) {
        if (outline_->IsStarted()) outline_->Stop(0);
        outline_->Release();
        outline_ = nullptr;
    }
    return cSC4ViewInputControl::Shutdown();
}

void cSC4ViewInputControlZone::SetOptionsFromModifiers(std::uint32_t m) {
    // swap orientation = control (4), extend as streets = !shift (1), examine boundaries = alt (2)
    if (g_host && g_host->ZoneDeveloper()) g_host->ZoneDeveloper()->SetOptions((m & 4) >> 2 != 0, (m & 1) == 0, (m & 2) >> 1 != 0);
}

bool cSC4ViewInputControlZone::DoMessage(cIGZMessage2* message) {
    if (!initialized_) return false;
    const std::uint32_t type = message->GetType();
    if (type == kZoneToolMsgGridChanged) {
        const bool gridShown = terrainView_ && terrainView_->GridShown();
        localGrid_.ShowEffect(gridShown ? false : !dragging_);
    } else if (type == kZoneToolMsgTick) {
        void* standard = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard) {
            restMs_ += static_cast<float>(static_cast<cIGZMessage2Standard*>(standard)->GetData3());   // vslot 0x98
            static_cast<cIGZMessage2Standard*>(standard)->Release();
        }
        DoCursorUpdate();
    }
    return true;
}

void cSC4ViewInputControlZone::DoCursorUpdate() {
    if (end_.x != start_.x || end_.z != start_.z) {
        restMs_ = 0.0f;
        flood_.reset();
        return;
    }
    if (restMs_ <= kZoneToolFloodDelay) return;
    if (flood_) return;
    flood_ = FloodFillRegion(start_);
    if (!flood_ || zoneType_ == 0) {
        ComputeAndMarkSelectedArea();
        return;
    }
    if (g_host && g_host->ZoneDeveloper()) g_host->ZoneDeveloper()->HighlightParcels(*flood_, zoneType_, nullptr, nullptr);   // vslot 0x28
    flood_.reset();
    ComputeAndMarkSelectedArea();
}

bool cSC4ViewInputControlZone::TestCell(const SC4Point<int>& cell, const SC4Point<int>& origin, int distance,
                                        const std::list<SC4Point<int>>& visited) {
    cSC4ZoneManager* zones = g_host->ZoneManager();
    const int zone = zones->GetZoneType(cell.x, cell.z);                 // vslot 0x60
    const bool seen = std::find(visited.begin(), visited.end(), cell) != visited.end();
    if (zoneType_ == 0) {
        // de-zoning follows zoned cells (not none / airport / seaport / plopped)
        if (static_cast<unsigned>(zone) > 0xf || ((0x9801u >> zone) & 1u) != 0) return false;
        if (seen) return false;
        return std::abs(origin.x - cell.x) < distance && std::abs(origin.z - cell.z) < distance;
    }
    if (zone != 0 || seen) return false;
    if (std::abs(origin.x - cell.x) >= distance || std::abs(origin.z - cell.z) >= distance) return false;
    return g_host->CellIsOpenGround(cell.x, cell.z);
}

std::unique_ptr<SC4CellRegion<int>> cSC4ViewInputControlZone::FloodFillRegion(const SC4Point<int>& start) {
    const int cellsX = g_host->CityCellsX();
    const int cellsZ = g_host->CityCellsZ();
    std::list<SC4Point<int>> cells;
    if (TestCell(start, start, 8, cells)) cells.push_back(start);
    int minX = start.x, minZ = start.z, maxX = start.x, maxZ = start.z;
    for (auto it = cells.begin(); it != cells.end(); ++it) {
        const SC4Point<int> c = *it;
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
        minZ = std::min(minZ, c.z);
        maxZ = std::max(maxZ, c.z);
        const SC4Point<int> neighbours[4] = {{c.x, c.z - 1}, {c.x, c.z + 1}, {c.x - 1, c.z}, {c.x + 1, c.z}};
        const bool inside[4] = {c.z > 0, c.z < cellsZ - 1, c.x > 0, c.x < cellsX - 1};
        for (int i = 0; i < 4; ++i)
            if (inside[i] && TestCell(neighbours[i], start, 8, cells)) cells.push_back(neighbours[i]);
    }
    if (cells.empty()) return nullptr;
    auto region = std::make_unique<SC4CellRegion<int>>(SC4CellRegion<int>::Make({minX, minZ, maxX, maxZ}, false));
    for (const SC4Point<int>& c : cells) region->SetBit(c.x, c.z);
    return region;
}

void cSC4ViewInputControlZone::GetFailureReasonString(int reason, std::string& out) {
    static const std::uint32_t kInstances[6] = {0, 0x6a711976, 0x6a711a47, 0xaa711a4e, 0x8a711a54, 0x6a711a59};
    out.clear();
    if (reason > 0 && reason < 6 && g_host) g_host->LoadString(kStringGroup, kInstances[reason], out);
}

void cSC4ViewInputControlZone::SetCursorTextIfChanged(const std::string& text) {
    if (cursorText_ == text) return;
    cursorText_ = text;
    cRZString string(text.c_str());
    if (cISC4View3DWin* view = View()) view->SetCursorText(kZoneToolCursorText, 0, 0, &string, 1);   // 0x140
}

void cSC4ViewInputControlZone::ClearHighlights() {
    for (cSC4Lot* lot : demolishLots_) g_host->HighlightLot(lot, 0);
    demolishLots_.clear();
    // an empty one-cell region clears the zone developer's parcel highlight
    SC4CellRegion<int> empty = SC4CellRegion<int>::Make({0, 0, 0, 0}, false);
    if (g_host->ZoneDeveloper()) g_host->ZoneDeveloper()->HighlightParcels(empty, zoneType_, nullptr, nullptr);
}

bool cSC4ViewInputControlZone::ComputeAndMarkSelectedArea() {
    selected_.x0 = std::min(start_.x, end_.x);
    selected_.x1 = std::max(start_.x, end_.x);
    selected_.z0 = std::min(start_.z, end_.z);
    selected_.z1 = std::max(start_.z, end_.z);
    ClearHighlights();
    SC4CellRegion<int> local = SC4CellRegion<int>::Make(selected_, true);
    SC4CellRegion<int>* region = flood_ ? flood_.get() : &local;
    std::int64_t cost = 0;
    int reason = 0;
    cSC4ZoneManager* zones = g_host->ZoneManager();
    const bool ok = zones->PlaceZone(*region, zoneType_, false, false, true, flood_ != nullptr, zoneType_ != 0, &cost, &reason,
                                     &demolishLots_);
    static const bool trace = std::getenv("SC4_ZONEDEBUG") != nullptr;
    if (trace) std::fprintf(stderr, "zone tool area %d,%d..%d,%d: %s reason %d cost %lld\n", selected_.x0, selected_.z0, selected_.x1, selected_.z1,
                            ok ? "ok" : "refused", reason, static_cast<long long>(cost));
    int buildings = 0;
    bool emptyCounts = false;
    for (cSC4Lot* lot : demolishLots_) {
        const int answer = g_host->HighlightLot(lot, 5);
        if (answer == 1) ++buildings;
        else if (answer == 2) emptyCounts = true;
    }
    cISC4View3DWin* view = View();
    if (ok) {
        if (PlaysSound(zoneType_)) g_host->PlaySound(kZoneDragSound[zoneType_]);
        if (terrainView_) terrainView_->ClearHighlight();
        if (view) view->SetCostIndicator(static_cast<std::uint32_t>(cost));      // 0x150
        const unsigned count = static_cast<unsigned>(buildings) + ((emptyCounts && buildings == 0) ? 1u : 0u);
        if (count == 0) {
            cursorText_.clear();
            if (view) view->ClearCursorText(kZoneToolCursorText);
        } else {
            std::string format;
            g_host->LoadString(kStringGroup, kDemolishCountString, format);
            char text[256];
            std::snprintf(text, sizeof text, format.c_str(), count);
            SetCursorTextIfChanged(text);
        }
        const std::uint32_t color = zones->GetZoneDragColor(0);
        if (zoneType_ == 14) {                                              // landfill: the cells themselves
            for (int z = selected_.z0; z <= selected_.z1; ++z)
                for (int x = selected_.x0; x <= selected_.x1; ++x)
                    if (region->Test(x, z) && terrainView_) terrainView_->HighlightCell(x, z, color);
        } else if (zoneType_ == 0) {
            const SC4Rect<int> bounds = flood_ ? flood_->bounds : selected_;
            for (int z = bounds.z0; z <= bounds.z1; ++z)
                for (int x = bounds.x0; x <= bounds.x1; ++x)
                    if ((!flood_ || flood_->Test(x, z)) && terrainView_) terrainView_->HighlightCell(x, z, color);
        } else if (g_host->ZoneDeveloper()) {
            g_host->ZoneDeveloper()->HighlightParcels(*region, zoneType_, nullptr, nullptr);
        }
    } else {
        if (PlaysSound(zoneType_)) g_host->PlaySound(kZoneDragErrorSound[zoneType_]);
        if (terrainView_) terrainView_->HighlightRegionError(selected_);
        std::string why;
        GetFailureReasonString(reason, why);
        SetCursorTextIfChanged(why);
        if (view) {
            if (zoneType_ == 0 && reason == 1) view->SetCostIndicator(static_cast<std::uint32_t>(cost), 0);   // 0x158
            else view->ClearCostIndicator(false);
        }
    }
    return ok;
}

bool cSC4ViewInputControlZone::PlaceZone(int zoneType, SC4CellRegion<int>& region, bool setRectangle, bool hasMask,
                                         std::int64_t* cost, int* reason) {
    if (!g_host->TutorialAllowsZone(region, setRectangle)) return false;
    if (setRectangle) {
        start_ = {region.bounds.x0, region.bounds.z0};
        end_ = {region.bounds.x1, region.bounds.z1};
    }
    return g_host->ZoneManager()->PlaceZone(region, zoneType, true, false, true, hasMask, zoneType_ != 0, cost, reason, nullptr);
}

bool cSC4ViewInputControlZone::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (!IsOnTop()) return false;
    if (key == 0x1b) {
        if (dragging_) { EndInput(); return true; }
    } else if (dragging_) {
        SetOptionsFromModifiers(modifiers);
        ComputeAndMarkSelectedArea();
    }
    return false;
}

bool cSC4ViewInputControlZone::OnKeyUp(std::uint32_t, std::uint32_t modifiers) {
    if (IsOnTop() && dragging_) {
        SetOptionsFromModifiers(modifiers);
        ComputeAndMarkSelectedArea();
    }
    return false;
}

void cSC4ViewInputControlZone::EndInput() {
    if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
        if (g_host) g_host->StopTick(AsTarget(), 0xffffffffu);
    }
    flood_.reset();
    if (cISC4View3DWin* view = View()) view->ClearCostIndicator(false);
    if (!cursorText_.empty()) {
        if (cISC4View3DWin* view = View()) view->ClearCursorText(kZoneToolCursorText);
        cursorText_.clear();
    }
    if (g_host) ClearHighlights();
    localGrid_.ShowEffect(false);
    if (terrainView_) terrainView_->ClearHighlight();
}

bool cSC4ViewInputControlZone::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    SetOptionsFromModifiers(modifiers);
    cISC4View3DWin* view = View();
    if (!view) return false;
    if (outline_ && outline_->IsStarted()) outline_->Stop(0);
    float position[3] = {0, 0, 0};
    if (!view->PickTerrain(x, y, position, false)) return true;
    if (PlaysSound(zoneType_)) g_host->PlaySound(kZoneClickSound[zoneType_]);
    dragging_ = true;
    view->KillKeyboardScrolling();                                       // 0x118
    SetCapture();
    start_ = {static_cast<int>(position[0] / cellSize_), static_cast<int>(position[2] / cellSize_)};
    end_ = start_;
    savedGrid_ = terrainView_ ? terrainView_->SetGridShown(false) : false;
    ComputeAndMarkSelectedArea();
    g_host->StartTick(AsTarget(), kZoneToolTickInterval);
    restMs_ = 0.0f;
    flood_.reset();
    localGrid_.ShowEffect(false);
    return true;
}

bool cSC4ViewInputControlZone::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    SetOptionsFromModifiers(modifiers);
    cISC4View3DWin* view = View();
    float position[3] = {0, 0, 0};
    if (!view || !view->PickTerrain(x, y, position, false)) {
        localGrid_.ShowEffect(false);
        return false;
    }
    localGrid_.SetPosition(cS3DVector3{position[0], position[1], position[2]});
    const bool gridShown = terrainView_ && terrainView_->GridShown();
    localGrid_.ShowEffect(gridShown ? false : !dragging_);
    if (outline_) {
        outline_->SetCell(std::floor(position[0] / cellSize_), std::floor(position[2] / cellSize_));
        const bool started = outline_->IsStarted();
        if (!dragging_) { if (!started) outline_->Start(0); }
        else if (started) outline_->Stop(0);
    }
    if (!dragging_) return false;
    if ((modifiers & 0x10) == 0) {                                       // the button went up outside the window
        EndInput();
    } else {
        const SC4Point<int> cell{static_cast<int>(position[0] / cellSize_), static_cast<int>(position[2] / cellSize_)};
        if (end_ == cell) return true;
        end_ = cell;
        DoCursorUpdate();
        ComputeAndMarkSelectedArea();
    }
    return true;
}

bool cSC4ViewInputControlZone::OnMouseUpL(int x, int y, std::uint32_t modifiers) {
    SetOptionsFromModifiers(modifiers);
    if (!dragging_) return false;
    cISC4View3DWin* view = View();
    float position[3] = {0, 0, 0};
    if (view && view->PickTerrain(x, y, position, false)) {
        end_ = {static_cast<int>(position[0] / cellSize_), static_cast<int>(position[2] / cellSize_)};
    }
    const bool ok = ComputeAndMarkSelectedArea();
    static const bool trace = std::getenv("SC4_ZONEDEBUG") != nullptr;
    if (trace) std::fprintf(stderr, "zone tool release at %d,%d: start %d,%d end %d,%d -> area %s\n", x, y, start_.x, start_.z, end_.x, end_.z, ok ? "ok" : "REFUSED");
    if (!ok) {
        if (PlaysSound(zoneType_)) g_host->PlaySound(kZoneDragErrorSound[zoneType_]);
    } else {
        if (PlaysSound(zoneType_)) g_host->PlaySound(kZonePlopSound[zoneType_]);
        bool placed;
        if (!flood_) {
            SC4CellRegion<int> region = SC4CellRegion<int>::Make(selected_, true);
            placed = PlaceZone(zoneType_, region, false, false, nullptr, nullptr);
        } else {
            placed = PlaceZone(zoneType_, *flood_, true, true, nullptr, nullptr);
        }
        if (view) view->ClearCostIndicator(placed);                      // 0x160
    }
    if (view) view->ClearCursorText(kZoneToolCursorText);
    cursorText_.clear();
    ClearHighlights();
    if (outline_ && !outline_->IsStarted()) outline_->Start(0);
    if (terrainView_) {
        terrainView_->ClearHighlight();
        terrainView_->SetGridShown(savedGrid_);                          // 0x50(this[0x38])
    }
    localGrid_.ShowEffect(!(terrainView_ && terrainView_->GridShown()));
    g_host->StopTick(AsTarget(), kZoneToolTickInterval);
    flood_.reset();
    ReleaseCapture();
    dragging_ = false;
    return true;
}

bool cSC4ViewInputControlZone::OnMouseExit() {
    ReleaseCapture();
    if (outline_ && outline_->IsStarted()) outline_->Stop(0);
    localGrid_.ShowEffect(false);
    cursorText_.clear();
    if (cISC4View3DWin* view = View()) {
        view->ClearCursorText(kZoneToolCursorText);
        view->ClearCostIndicator(false);
    }
    return true;
}

}  // namespace sc4
