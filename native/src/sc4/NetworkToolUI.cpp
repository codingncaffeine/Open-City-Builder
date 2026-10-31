#include "sc4/NetworkToolUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "rz/RZTime.h"
#include "rz/cRZKeyboard.h"
#include "sc4/SC4Utilities.h"

namespace sc4 {

const std::uint32_t kNetworkPlopSound[13] = {0x6a5ec1d8, 0x6a5ec1ae, 0x6a5ec1d8, 0x6a5ec1d8, 0x8a5ec185, 0x0a5ec4f2, 0x6a5ec1d8,
                                             0x0a5ec22a, 0x6a5ec1ae, 0x6a5ec1ae, 0x6a5ec1d8, 0x6a5ec1d8, 0x6a5ec1d8};
const std::uint32_t kNetworkDragErrorSound[13] = {0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86,
                                                  0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86, 0x495a6d86};
const std::uint32_t kNetworkDragSound[13] = {0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e,
                                             0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e, 0x495a6c6e};

namespace {
INetworkToolUIHost* g_host = nullptr;
// SC4_NETDEBUG=1 traces the drag's decisions on stderr (a port aid; the original has no such output).
bool Trace() { static const bool on = std::getenv("SC4_NETDEBUG") != nullptr; return on; }
constexpr std::uint32_t kKeyShift = 0x10;
constexpr std::uint32_t kErrorSound = 0xaa62c9d5;        // OnMouseUpL on a failed drag
constexpr float kPickAltitudeWindow = 50.0f;             // cSC4Helicopter::...::kDeltaAltitude 0x00a7079c
constexpr float kBridgeIndicatorClearance = 30.0f;       // cSC4Tank::sfBarrelMinVelocity 0x00a70894
// Bridge chooser control ids.
constexpr std::uint32_t kButtonPlace = 0x2000, kButtonCancel = 0x2001, kBridgeButtonBase = 0xa1000;
constexpr std::uint32_t kIndicatorAbove = 0x4000, kIndicatorBelow = 0x4001, kCostText = 0x3003, kBridgeName = 0x3002;

void Play(const std::uint32_t* table, std::uint32_t type) {
    if (g_host && type < 13 && table[type] != 0) g_host->PlaySound(table[type]);
}
}  // namespace

INetworkToolUIHost* NetworkToolUIHost() { return g_host; }
void SetNetworkToolUIHost(INetworkToolUIHost* host) { g_host = host; }

cSC4NetworkToolUI::~cSC4NetworkToolUI() {
    Shutdown();
    if (bridgeChooser_) bridgeChooser_->Release();
    if (tool_) tool_->Release();
}

std::uint32_t cSC4NetworkToolUI::Release() {
    if (refCount_ > 1) return --refCount_;
    refCount_ = 0;
    delete this;
    return 0;
}

bool cSC4NetworkToolUI::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == 1 || iid == kGZIID_cISC4NetworkToolUI) {
        *out = static_cast<cISC4NetworkToolUI*>(this);
    } else {
        return false;   // 0x22e85d8e (cIGZWinProc, the bridge chooser's proc) is not ported
    }
    AddRef();
    return true;
}

bool cSC4NetworkToolUI::Init(cISC4NetworkTool* tool, std::uint32_t networkType) {
    inited_ = true;
    if (tool) tool->AddRef();
    if (tool_) tool_->Release();
    tool_ = tool;
    networkType_ = networkType;
    tool->GetPlacementParams(params_);                                   // vslot 0xc0
    multiAnchor_ = params_.unknown24 != 0;                                // +0xa0 = params +0x24
    if (g_host) g_host->StartTick(this);
    return true;
}

bool cSC4NetworkToolUI::Shutdown() {
    if (inited_) {
        if (g_host) g_host->StopTick(this);
        AbortPlacement();
        inited_ = false;
    }
    return true;
}

void cSC4NetworkToolUI::EnterUI() {
    if (const std::uint32_t view = tool_->GetToolViewId(); view != 0 && g_host) g_host->EnterToolView(view);
}

void cSC4NetworkToolUI::LeaveUI() {
    if (const std::uint32_t view = tool_->GetToolViewId(); view != 0 && g_host) g_host->LeaveToolView(view);
}

void cSC4NetworkToolUI::AttachToView(cISC4View3DWin* view) {
    AbortPlacement();
    view_ = view;
}

void cSC4NetworkToolUI::CloseBridgeChooser() {
    if (!bridgeChooser_) return;
    if (g_host) g_host->RemoveWindow(bridgeChooser_);
    bridgeChooser_->Release();
    bridgeChooser_ = nullptr;
}

void cSC4NetworkToolUI::AbortPlacement() {
    if (Trace()) std::fprintf(stderr, "network ui: abort (placing %d)\n", placing_ ? 1 : 0);
    if (!placing_) return;
    placing_ = false;
    mouseDown_ = false;
    previewDirty_ = false;
    CloseBridgeChooser();
    tool_->Reset();
    tool_->ClearCellPreviews();
    tool_->ClearOccupantHighlights();
    if (g_host) {
        g_host->SetGridShown(gridWasShown_);
        g_host->ClearTerrainHighlight();
    }
    if (view_) {
        view_->ClearCostIndicator(false);
        view_->ClearCursorText(kNetworkToolCursorText);
    }
    if (g_host) g_host->EndNetworkDrag();
}

void cSC4NetworkToolUI::DoPlacement() {
    if (!placing_) return;
    CloseBridgeChooser();
    const bool paid = g_host && g_host->SpendFunds(static_cast<std::uint32_t>(tool_->GetCostOfSolution()));
    view_->ClearCostIndicator(paid);
    placing_ = false;
    if (Trace()) std::fprintf(stderr, "network ui: place, cost %d, paid %d\n", tool_->GetCostOfSolution(), paid ? 1 : 0);
    if (paid) {
        // A dry run with neighbour connections on tells whether the drag reaches the city edge.
        tool_->Reset();
        tool_->SetPreferredBridge(preferredBridge_);
        tool_->SetBridgeAltitudeOverride(bridgeAltitudeOverride_);
        tool_->SetAnchorPoints(anchors_);
        tool_->PlaceAllSegments(false, allowDiagonals_, true, networkType_);
        NeighborConnectionInfo info;
        const bool connect = tool_->GetPossibleNeighborConnection(info) && !tool_->GetFailureState() &&
                             g_host->ConfirmNeighborConnection(info);
        tool_->Reset();
        tool_->SetPreferredBridge(preferredBridge_);
        tool_->SetBridgeAltitudeOverride(bridgeAltitudeOverride_);
        tool_->SetAnchorPoints(anchors_);
        tool_->PlaceAllSegments(true, allowDiagonals_, connect, networkType_);
        Play(kNetworkPlopSound, networkType_);
    }
    tool_->ClearOccupantHighlights();
    tool_->ClearCellPreviews();
    if (g_host) g_host->SetGridShown(gridWasShown_);
    view_->ClearCursorText(kNetworkToolCursorText);
    if (paid) tool_->QueryUserForPotentialIntersections();
    tool_->Reset();
    if (g_host) g_host->EndNetworkDrag();
}

bool cSC4NetworkToolUI::TestPlacement(bool updatePreview) {
    int penX = 0, penZ = 0;
    tool_->PenWidth(penX, penZ);
    if (penX > 1) {
        // Wide networks (highways) start one cell back so the pen is centred on the drag.
        SC4PointU& first = anchors_.front();
        const std::uint32_t dx = cell_.x - first.x;
        const int dz = static_cast<int>(cell_.z - first.z);
        first = startCell_;
        if (anchors_.size() == 2) {
            if (first.x != 0 && std::abs(dz) <= static_cast<int>(dx * 2)) --first.x;
            const std::uint32_t adx = dx > 0x7fffffffu ? 0u - dx : dx;
            if (first.z != 0 && static_cast<int>(adx) <= dz * 2) --first.z;
        }
    }
    SC4PointU from = anchors_[anchors_.size() - 2];
    SC4PointU to = cell_;
    tool_->Reset();
    tool_->SetPreferredBridge(preferredBridge_);
    tool_->SetBridgeAltitudeOverride(bridgeAltitudeOverride_);
    tool_->EnablePreviewUpdate(updatePreview);
    tool_->ClearFailureReason();
    if (tool_->DrawNetworkLine(from, to, allowDiagonals_, networkType_)) {
        anchors_.back() = to;
        tool_->SetAnchorPoints(anchors_);
        tool_->PlaceAllSegments(false, allowDiagonals_, false, networkType_);
    }
    tool_->EnablePreviewUpdate(true);
    return !tool_->GetFailureState();
}

bool cSC4NetworkToolUI::UpdatePreview() {
    const bool ok = TestPlacement(true);
    cRZString why;
    tool_->GetFailureReasonString(why);
    if (why.Strlen() != failureText_.Strlen() || std::string(why.Data(), why.Strlen()) != std::string(failureText_.Data(), failureText_.Strlen())) {
        failureText_.Copy(why);
        view_->SetCursorText(kNetworkToolCursorText, 0, nullptr, &failureText_, 1);
    }
    cRZString costText;
    bool affordable = false;
    if (!ok) {
        view_->ClearCostIndicator(false);
        tool_->SetValidStepCount(0);
    } else {
        const std::uint32_t cost = static_cast<std::uint32_t>(tool_->GetCostOfSolution());
        view_->SetCostIndicator(cost);
        const std::int64_t funds = g_host ? g_host->Funds() : 0;
        affordable = !(funds < 0x100000000LL && (funds < 0 || static_cast<std::uint32_t>(funds) < cost));
        SC4MakeMoneyString(static_cast<std::int64_t>(cost), costText);
    }
    if (bridgeChooser_) {
        if (cIGZWin* text = bridgeChooser_->GetChildWindowFromID(kCostText)) text->SetCaption(costText);
    }
    return affordable;
}

void cSC4NetworkToolUI::UpdateIndicators(bool affordable) {
    if (!bridgeChooser_) return;
    float altitude = 0.0f;
    const bool high = tool_->GetMajorBridgeAltitude(altitude) && g_host &&
                      !(altitude < g_host->SeaLevel() + kBridgeIndicatorClearance);
    if (cIGZWin* w = bridgeChooser_->GetChildWindowFromID(kIndicatorAbove)) w->SetFlag(1, !majorLandBridge_ && high);
    if (cIGZWin* w = bridgeChooser_->GetChildWindowFromID(kIndicatorBelow)) w->SetFlag(1, !majorLandBridge_ && !high);
    if (cIGZWin* w = bridgeChooser_->GetChildWindowFromID(kButtonPlace)) w->SetFlag(2, affordable);
}

void cSC4NetworkToolUI::OnButtonActivated(std::uint32_t id) {
    if (id == kButtonPlace) {
        CloseBridgeChooser();
        DoPlacement();
    } else if (id == kButtonCancel) {
        AbortPlacement();
    } else if (bridgeChooser_ && id - kBridgeButtonBase < bridges_.size()) {
        // The bridge rows (AddBridgeEntry) are not ported yet, so bridges_ stays empty and this branch does not run.
        const BridgeEntry& entry = bridges_[id - kBridgeButtonBase];
        if (entry.bridge != preferredBridge_) {
            preferredBridge_ = entry.bridge;
            bridgeAltitude_ = entry.altitude;
            bridgeAltitudeOverride_ = entry.altitude + bridgeAltitudeOffset_;
            if (cIGZWin* name = bridgeChooser_->GetChildWindowFromID(kBridgeName)) {
                cRZString text;
                tool_->GetBridgeName(text, entry.bridge);
                name->SetCaption(text);
            }
            UpdateIndicators(UpdatePreview());
        }
    }
}

bool cSC4NetworkToolUI::DoMessage(cIGZMessage2* message) {
    if (message->GetType() != kNetworkToolTickMessage) return false;
    if (previewDirty_) {
        previewDirty_ = false;
        UpdateIndicators(UpdatePreview());
    }
    return true;
}

bool cSC4NetworkToolUI::ScreenToCell(int x, int y, SC4PointU& cell, float* altitude, bool usePlane, float planeAltitude,
                                     float minAltitude, float maxAltitude) {
    if (!view_ || !g_host) return false;
    float position[3] = {0, 0, 0};
    const bool picked = view_->PickTerrain(x, y, position, tool_->StopPickAtWater());
    if (!picked && !usePlane) return false;
    if (!picked || (usePlane && !(minAltitude <= position[1] && position[1] <= maxAltitude))) {
        // Off the terrain, or on terrain far above / below the drag's start: stay on the start's plane.
        if (!g_host->IntersectViewportWithPlane(x, y, planeAltitude, position)) return false;
    }
    const float cellSize = g_host->CellSize();
    const int cellsX = g_host->CellCountX();
    const int cellsZ = g_host->CellCountZ();
    int cx = static_cast<int>(position[0] / cellSize);
    int cz = static_cast<int>(position[2] / cellSize);
    if (cx < 0) cx = 0;
    if (cx >= cellsX) cx = cellsX - 1;
    if (cz < 0) cz = 0;
    if (cz >= cellsZ) cz = cellsZ - 1;
    cell.x = static_cast<std::uint32_t>(cx);
    cell.z = static_cast<std::uint32_t>(cz);
    if (altitude) *altitude = position[1];
    return true;
}

int cSC4NetworkToolUI::LastDragDirection() {
    std::vector<SC4PointU> cells;
    tool_->GetDraggedCells(cells);
    if (cells.size() > 2) {
        const SC4PointU& last = cells[cells.size() - 1];
        const SC4PointU& before = cells[cells.size() - 2];
        const int dx = static_cast<int>(last.x - before.x);
        const int dz = static_cast<int>(last.z - before.z);
        if (dx != 0 || dz != 0) {
            // (float)(atan2 * 4/pi) + 12582912.0f: the mantissa holds the rounded octant.
            const float rounded = static_cast<float>(std::atan2(static_cast<double>(dx), static_cast<double>(-dz)) * 1.2732395447351628) +
                                  12582912.0f;
            std::uint32_t bits;
            static_assert(sizeof bits == sizeof rounded);
            std::memcpy(&bits, &rounded, sizeof bits);
            return static_cast<int>(bits & 7u);
        }
    }
    return -1;
}

bool cSC4NetworkToolUI::LiesOnSegment(const SC4PointU& cell, const SC4PointU& from, const SC4PointU& to) {
    const std::uint32_t x0 = std::min(from.x, to.x), x1 = std::max(from.x, to.x);
    const std::uint32_t z0 = std::min(from.z, to.z), z1 = std::max(from.z, to.z);
    if (cell.x < x0 || cell.z < z0 || cell.x > x1 || cell.z > z1) return false;
    SC4PointU a = from, b = to;
    tool_->DrawNetworkLine(a, b, allowDiagonals_, networkType_);
    std::vector<SC4PointU> cells;
    tool_->GetDraggedCells(cells);
    return std::find(cells.begin(), cells.end(), cell) != cells.end();
}

bool cSC4NetworkToolUI::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    if (!view_) return false;
    if (placing_) AbortPlacement();
    allowDiagonals_ = !cRZKeyboard::IsKeyDownNowStatic(kKeyShift);
    multiAnchor_ = false;   // see the header: the shipped game never drags with anchors
    SC4PointU cell;
    const bool picked = ScreenToCell(x, y, cell, &startAltitude_, false, 0.0f, 0.0f, 0.0f);
    if (Trace()) std::fprintf(stderr, "network ui: mouse down at %d,%d picked %d cell %u,%u this %p\n", x, y, picked ? 1 : 0, cell.x, cell.z, static_cast<void*>(this));
    if (picked) {
        placing_ = true;
        mouseDown_ = true;
        preferredBridge_ = -1;
        bridgeAltitudeOverride_ = -1.0f;
        bridgeAltitudeOffset_ = 0.0f;
        if (g_host) g_host->BeginNetworkDrag();
        tool_->ForceReload();
        Play(kNetworkDragSound, networkType_);
        cell_ = cell;
        startCell_ = cell;
        gridWasShown_ = g_host && g_host->SetGridShown(false);
        anchors_.clear();
        anchors_.push_back(cell);
        anchors_.push_back(cell);
        tool_->SetAnchorPoints(anchors_);
        anchorTime_ = cRZTime::GetMillisecondSystemTime();
        failureText_.AssignFromChars("", 0);
        cell_ = SC4PointU{0xffffffffu, 0xffffffffu};
        OnMouseMove(x, y, modifiers);
    }
    return true;
}

bool cSC4NetworkToolUI::OnMouseUpL(int, int, std::uint32_t) {
    if (Trace()) std::fprintf(stderr, "network ui: up entry view %p down %d placing %d this %p\n", static_cast<void*>(view_), mouseDown_ ? 1 : 0, placing_ ? 1 : 0, static_cast<void*>(this));
    if (!view_ || !mouseDown_) return false;
    mouseDown_ = false;
    if (Trace()) std::fprintf(stderr, "network ui: mouse up, failure %d, %zu anchors\n", tool_->GetFailureState() ? 1 : 0, anchors_.size());
    if (tool_->GetFailureState()) {
        if (g_host) g_host->PlaySound(kErrorSound);
        AbortPlacement();
        return true;
    }
    std::vector<int> possible;
    bridges_.clear();
    if (!tool_->GetPossibleBridges(possible)) {
        DoPlacement();
        return true;
    }
    // The drag crosses water: the bridge chooser lists every bridge type the drag can take.
    CloseBridgeChooser();
    bridgeChooser_ = g_host ? g_host->CreateBridgeChooser(view_->AsIGZWin()) : nullptr;
    if (!bridgeChooser_) {
        // Port gap (AddBridgeEntry / the chooser's proc not ported): place with the tool's own bridge choice.
        DoPlacement();
        return true;
    }
    majorLandBridge_ = tool_->IsMajorBridgeLandBridge();
    const int major = tool_->GetMajorBridge();
    if (major >= 0) {
        auto it = std::find(possible.begin(), possible.end(), major);
        if (it != possible.end()) possible.erase(it);
        possible.insert(possible.begin(), major);
    }
    for (int bridge : possible) {
        preferredBridge_ = bridge;
        bridgeAltitudeOverride_ = -1.0f;
        if (TestPlacement(false)) {
            float altitude = 0.0f;
            tool_->GetMajorBridgeReferenceAltitude(altitude);
            bridges_.push_back({bridge, altitude});   // AddBridgeEntry(name, cost, 0, bridge) adds the row's button
        }
    }
    preferredBridge_ = -1;
    OnButtonActivated(kBridgeButtonBase);
    return true;
}

void cSC4NetworkToolUI::UpdateAnchorPoint(const SC4PointU& cell) {
    const std::size_t n = anchors_.size();
    const SC4PointU& last = anchors_[n - 1];
    const SC4PointU& before = anchors_[n - 2];
    if (last == before) return;
    const std::uint32_t valid = static_cast<std::uint32_t>(tool_->GetValidStepCount());
    const int zoom = g_host ? g_host->ZoomLevel() : 0;
    const std::uint32_t minSteps = static_cast<std::size_t>(zoom) < params_.list28.size() ? params_.list28[static_cast<std::size_t>(zoom)] : 0;
    if (valid < minSteps) return;
    const int straight = tool_->GetStraightStepCount();
    if (!(2u < static_cast<std::uint32_t>(straight - 1) && 2u < valid - static_cast<std::uint32_t>(straight) - 1u)) return;
    if (!tool_->IsRegularStep(static_cast<int>(valid) - 1)) return;
    SC4PointU point = cell;
    if (static_cast<int>(valid) < tool_->GetTotalStepCount()) tool_->GetLastValidPoint(point);
    anchors_.pop_back();
    if (n - 1 > 1) {
        // Three anchors on one straight line collapse into one segment (AreOnOneSegment).
        const SC4PointU a = anchors_[n - 3], b = anchors_[n - 2];
        const auto sgn = [](std::int64_t v) { return (v > 0) - (v < 0); };
        const std::int64_t abx = static_cast<std::int64_t>(b.x) - a.x, abz = static_cast<std::int64_t>(b.z) - a.z;
        const std::int64_t bpx = static_cast<std::int64_t>(point.x) - b.x, bpz = static_cast<std::int64_t>(point.z) - b.z;
        if (abx * bpz == abz * bpx && sgn(abx) == sgn(bpx) && sgn(abz) == sgn(bpz)) anchors_.pop_back();
    }
    anchors_.push_back(point);
    anchors_.push_back(point);
    anchorTime_ = cRZTime::GetMillisecondSystemTime();
    tool_->SetAnchorPoints(anchors_);
}

bool cSC4NetworkToolUI::OnMouseMove(int x, int y, std::uint32_t) {
    if (!view_ || !mouseDown_) return false;
    SC4PointU cell;
    if (!ScreenToCell(x, y, cell, &altitude_, true, altitude_, startAltitude_ - kPickAltitudeWindow, startAltitude_ + kPickAltitudeWindow))
        return true;
    if (cell == cell_) return true;
    cell_ = cell;
    const SC4PointU lastAnchor = anchors_.back();
    const bool ok = UpdatePreview();
    if (multiAnchor_) {
        if (!ok) {
            anchors_.front() = startCell_;
            anchors_.back() = lastAnchor;
            tool_->SetAnchorPoints(anchors_);
            tool_->PlaceAllSegments(false, allowDiagonals_, false, networkType_);
            SC4PointU from = lastAnchor, to = cell;
            tool_->DrawNetworkLine(from, to, allowDiagonals_, networkType_);
            tool_->SetValidStepCount(0);
        }
        const int n = static_cast<int>(anchors_.size());
        if (n > 2 && !(cell == anchors_[static_cast<std::size_t>(n - 2)])) {
            // Dragging back over an earlier segment drops the anchors after it.
            for (int i = 0; i <= n - 3; ++i) {
                if (!LiesOnSegment(cell, anchors_[static_cast<std::size_t>(i)], anchors_[static_cast<std::size_t>(i + 1)])) continue;
                anchors_.resize(static_cast<std::size_t>(i + 1));
                anchors_.push_back(cell);
                tool_->SetAnchorPoints(anchors_);
                break;
            }
        }
        const std::uint32_t now = cRZTime::GetMillisecondSystemTime();
        const std::uint32_t delay = params_.unknown40[7];   // +0xcc = params +0x50: the anchor delay in ms
        if (now >= anchorTime_ + delay) UpdateAnchorPoint(anchors_.back());
        else if (now < anchorTime_) anchorTime_ = 0;
    }
    Play(ok ? kNetworkDragSound : kNetworkDragErrorSound, networkType_);
    if (g_host) g_host->RefreshCursor();
    return true;
}

}  // namespace sc4
