#include "sc4/ViewInputControlDemolish.h"

#include <algorithm>
#include <cstdlib>

#include "sc4/Lot.h"
#include "sc4/OccupantManager.h"

namespace sc4 {
namespace {
IDemolishToolHost* g_host = nullptr;

constexpr int kSoundPress = static_cast<int>(0xca5c328bu);
constexpr int kSoundRefused = static_cast<int>(0x495a6d86u);
constexpr int kSoundFree = static_cast<int>(0x8a5ec5b8u);
constexpr int kSoundCannot = static_cast<int>(0xaa62c9d5u);
constexpr int kSoundGrow = static_cast<int>(0x0a5c32abu);

DemolitionCellRect LotRect(cSC4Lot* lot) {
    std::int32_t r[4] = {};
    lot->GetBoundingRect(r);   // lot vslot 0x38
    return DemolitionCellRect{r[0], r[1], r[2], r[3]};
}
int Area(const DemolitionRegion& region) {
    return (region.bounds.x1 - region.bounds.x0 + 1) * (region.bounds.z1 - region.bounds.z0 + 1);
}
}  // namespace

IDemolishToolHost* DemolishToolHost() { return g_host; }
void SetDemolishToolHost(IDemolishToolHost* host) { g_host = host; }

cSC4ViewInputControlDemolish::cSC4ViewInputControlDemolish() {   // 0x002e4850
    id_ = kDemolishToolId;
    cursorId_ = kDemolishToolCursor;
}

cSC4ViewInputControlDemolish::~cSC4ViewInputControlDemolish() {
    SetPicked(nullptr);
    if (highlighted_) highlighted_->Release();
}

void cSC4ViewInputControlDemolish::SetPicked(cISC4Occupant* occupant) {
    if (occupant) occupant->AddRef();
    if (picked_) picked_->Release();
    picked_ = occupant;
}

// 0x002e3a8c: the ids, the base, and (once) the marked-cell view; the services and the four colours are the host's.
bool cSC4ViewInputControlDemolish::Init() {
    if (initialized_) return true;
    id_ = kDemolishToolId;
    cursorId_ = kDemolishToolCursor;
    return cSC4ViewInputControl::Init();
}

// 0x002e3724: the input ends, the selection and the marked cells go, the held services are released, the cost indicator clears.
bool cSC4ViewInputControlDemolish::Shutdown() {
    if (!initialized_) return true;
    EndInput();
    ClearSelectedRegion();
    if (g_host) g_host->ClearMarkedCells();
    SetPicked(nullptr);
    SetHighlighted(nullptr);
    if (g_host) g_host->ClearCostIndicator(false);
    return cSC4ViewInputControl::Shutdown();
}

// 0x002e346c: Escape ends a drag.
bool cSC4ViewInputControlDemolish::OnKeyDown(std::uint32_t key, std::uint32_t) {
    if (!IsOnTop() || key != 0x1b || !dragging_) return false;
    EndInput();
    return true;
}

// 0x002e34a6: the renderer's pick with the demolishable filter; a signpost is demolished on its own.
bool cSC4ViewInputControlDemolish::PickDemolishableOccupant(int x, int y) {
    pickedSignpost_ = false;
    bool signpost = false;
    cISC4Occupant* occupant = g_host ? g_host->PickOccupant(x, y, signpost) : nullptr;
    SetPicked(occupant);
    if (occupant) occupant->Release();
    pickedSignpost_ = occupant && signpost;
    return occupant != nullptr;
}

// The highlighted occupant gets back the highlight it had (vslot 0x44 with the remembered value).
void cSC4ViewInputControlDemolish::SetHighlighted(cISC4Occupant* occupant) {
    if (highlighted_) {
        if (g_host) g_host->SetHighlight(highlighted_, oldHighlight_);
        oldHighlight_ = 0;
        highlighted_->Release();
        highlighted_ = nullptr;
    }
    if (!occupant) return;
    occupant->AddRef();
    highlighted_ = occupant;
    if (g_host) {
        oldHighlight_ = g_host->Highlight(occupant);
        g_host->SetHighlight(occupant, 5);
    }
}

// 0x002e3598
void cSC4ViewInputControlDemolish::ClearSelectedRegion() {
    SetHighlighted(nullptr);
    if (g_host) g_host->ClearMarkedCells();
    selection_.reset();
}

// 0x002e3614
void cSC4ViewInputControlDemolish::EndInput() {
    if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
        ClearSelectedRegion();
    }
    if (g_host) g_host->ClearCostIndicator(false);
}

// 0x002e3654
bool cSC4ViewInputControlDemolish::OnMouseExit() {
    if (g_host) g_host->ClearCostIndicator(false);
    ClearSelectedRegion();
    return true;
}

// 0x002e3684: the terrain under the mouse (its water surface when the terrain shows it), as a city cell.
bool cSC4ViewInputControlDemolish::DoCellPick(int x, int y, int& cellX, int& cellZ) {
    float position[3] = {};
    return g_host && g_host->PickCell(x, y, cellX, cellZ, position);
}

// 0x002e386e: what a click there takes: the whole lot when the cell's lot may be demolished, else the picked occupant's
// cells (a power pole its own cell), else the cell.
bool cSC4ViewInputControlDemolish::DoLotPick(int x, int y, DemolitionCellRect& rect) {
    if (!g_host) return false;
    SetPicked(nullptr);
    PickDemolishableOccupant(x, y);
    int cellX = 0, cellZ = 0;
    float position[3] = {};
    if (picked_) {
        g_host->OccupantPosition(picked_, position);   // occupant vslot 0x20
        cellX = static_cast<int>(position[0] / 16.0f);
        cellZ = static_cast<int>(position[2] / 16.0f);
    } else if (!g_host->PickCell(x, y, cellX, cellZ, position)) {
        return false;
    }
    cSC4Lot* lot = g_host->LotAt(cellX, cellZ);
    if (lot && lot->IsInDemolishableState()) {   // lot vslot 0x9c; no view filter natively
        rect = LotRect(lot);
        return true;
    }
    if (!picked_ || !g_host->OccupantCells(picked_, rect)) rect = DemolitionCellRect{cellX, cellZ, cellX, cellZ};
    return true;
}

// 0x002e493c
bool cSC4ViewInputControlDemolish::OnMouseDownL(int x, int y, std::uint32_t) {
    dragging_ = DoCellPick(x, y, pressX_, pressZ_);
    if (dragging_) {
        SetCapture();
        DoLotPick(x, y, clickRect_);
        currentX_ = pressX_;
        currentZ_ = pressZ_;
        UpdateSelectedRegion();
    }
    PlayResultSound(kSoundPress);
    return true;
}

// 0x002e4748: while not dragging, the press cell and the click rectangle follow the mouse (a new lot or occupant under it
// counts as a change); the selection is redone when the cell or the pick changed.
bool cSC4ViewInputControlDemolish::OnMouseMove(int x, int y, std::uint32_t) {
    int cellX = 0, cellZ = 0;
    if (!DoCellPick(x, y, cellX, cellZ)) return true;
    bool changed = false;
    if (!dragging_) {
        changed = cellX != pressX_ || cellZ != pressZ_;
        cISC4Occupant* old = picked_;
        if (old) old->AddRef();
        if (DoLotPick(x, y, clickRect_) && old != picked_) changed = true;
        if (changed) {
            pressX_ = cellX;
            pressZ_ = cellZ;
        }
        if (old) old->Release();
    }
    if (cellX == currentX_ && cellZ == currentZ_) {
        if (!changed) return true;
    } else {
        currentX_ = cellX;
        currentZ_ = cellZ;
    }
    UpdateSelectedRegion();
    return true;
}

// 0x002e3d62: the selection (the dragged rectangle, or the click rectangle), grown by the developed lots that reach out of it,
// priced by a dry run; the clicked lot's building (or the cell's network piece, or the signpost) is highlighted, the cells are
// marked in the verdict's colour and the cost shows.
void cSC4ViewInputControlDemolish::UpdateSelectedRegion() {
    const bool dragged = pressX_ != currentX_ || pressZ_ != currentZ_;
    DemolitionCellRect r = clickRect_;
    if (dragged) r = DemolitionCellRect{std::min(pressX_, currentX_), std::min(pressZ_, currentZ_), std::max(pressX_, currentX_),
                                        std::max(pressZ_, currentZ_)};
    const int oldCells = selection_ ? Area(*selection_) : 0;
    selection_.reset();
    DemolitionCellRect grown = r;
    bool grew = false;
    auto reachesOut = [&](int x, int z, DemolitionCellRect& lotRect) {
        cSC4Lot* lot = g_host ? g_host->LotAt(x, z) : nullptr;
        if (!lot || lot->GetState() <= 1) return false;   // lot vslot 0x84
        lotRect = LotRect(lot);
        return lotRect.x0 < r.x0 || r.x1 < lotRect.x1 || lotRect.z0 < r.z0 || r.z1 < lotRect.z1;
    };
    for (int x = r.x0; x <= r.x1; ++x)
        for (int z = r.z0; z <= r.z1; ++z) {
            DemolitionCellRect lotRect;
            if (!reachesOut(x, z, lotRect)) continue;
            grown.x0 = std::min(grown.x0, lotRect.x0);
            grown.z0 = std::min(grown.z0, lotRect.z0);
            grown.x1 = std::max(grown.x1, lotRect.x1);
            grown.z1 = std::max(grown.z1, lotRect.z1);
            grew = true;
        }
    selection_ = std::make_unique<DemolitionRegion>();
    selection_->bounds = grew ? grown : r;
    if (grew) {
        selection_->mask.assign(static_cast<std::size_t>(grown.x1 - grown.x0 + 1), std::vector<bool>(static_cast<std::size_t>(grown.z1 - grown.z0 + 1), false));
        for (int x = r.x0; x <= r.x1; ++x)
            for (int z = r.z0; z <= r.z1; ++z) selection_->mask[x - grown.x0][z - grown.z0] = true;
        for (int x = grown.x0; x <= grown.x1; ++x)
            for (int z = grown.z0; z <= grown.z1; ++z) {
                DemolitionCellRect lotRect;
                if (reachesOut(x, z, lotRect)) selection_->mask[x - grown.x0][z - grown.z0] = true;
            }
    }
    cost_ = 0;
    if (!g_host) return;
    verdict_ = pickedSignpost_ ? g_host->DemolishOccupant(false, picked_, &cost_) : g_host->DemolishRegion(false, *selection_, &cost_, nullptr, 0, 0);
    SetHighlighted(nullptr);
    if (!dragged) {
        cISC4Occupant* highlight = nullptr;
        if (pickedSignpost_) {
            highlight = picked_;
            if (highlight) highlight->AddRef();
        } else if (cSC4Lot* lot = g_host->LotAt(clickRect_.x0, clickRect_.z0)) {
            highlight = g_host->LotBuilding(lot);
        } else {
            highlight = g_host->NetworkOccupantAt(clickRect_.x0, clickRect_.z0);
        }
        if (highlight) {
            SetHighlighted(highlight);
            highlight->Release();
        }
    }
    g_host->MarkCells(*selection_, verdict_);
    g_host->SetCostIndicator(cost_);
    if (oldCells != Area(*selection_)) {
        if (!verdict_) PlayResultSound(kSoundCannot);
        else if (dragging_) PlayResultSound(kSoundGrow);
    }
}

// 0x002e4564: the release demolishes the selection (the clicked occupant itself when it was a signpost, the clicked occupant
// as the effects' focus when nothing was dragged) after paying the dry run's price.
bool cSC4ViewInputControlDemolish::OnMouseUpL(int x, int y, std::uint32_t) {
    if (!dragging_) return false;
    DoCellPick(x, y, currentX_, currentZ_);
    UpdateSelectedRegion();
    if (!verdict_) {
        if (g_host) g_host->ClearCostIndicator(false);
        PlayResultSound(kSoundRefused);
    } else if (g_host) {
        g_host->PayFromFunds(cost_);
        if (pressX_ == currentX_ && pressZ_ == currentZ_ && picked_) {
            if (pickedSignpost_) g_host->DemolishOccupant(true, picked_, &cost_);
            else g_host->DemolishRegion(true, *selection_, &cost_, picked_, x, y);
        } else {
            g_host->DemolishRegion(true, *selection_, &cost_, nullptr, 0, 0);
        }
        g_host->ClearCostIndicator(true);
        if (cost_ == 0) PlayResultSound(kSoundFree);
    }
    ClearSelectedRegion();
    dragging_ = false;
    ReleaseCapture();
    return true;
}

}  // namespace sc4
