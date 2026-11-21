#include "sc4/ViewInputControlNetworkIntxTool.h"

#include <cmath>
#include <cstring>

#include "sc4/NetworkIntersectionRules.h"
#include "sc4/NetworkTool.h"
#include "sc4/View3DWin.h"

namespace sc4 {

namespace {
INetworkIntxToolHost* g_host = nullptr;
constexpr std::uint32_t kKeyTab = 0x09, kKeyEscape = 0x1b, kKeyEnd = 0x23, kKeyHome = 0x24;
constexpr std::uint32_t kModifierShift = 0x1;
// The intersection placement strings (SC4LoadString group, instance = the rule's PlaceQueryID).
constexpr std::uint32_t kIntersectionStringGroup = 0x2a592fd1;
// OnMouseMove::kSearchPattern (i386 0x00849980): the cell, then its four neighbours.
constexpr int kSearchPattern[5][2] = {{0, 0}, {1, 0}, {0, -1}, {-1, 0}, {0, 1}};
// The position OnKeyDown stores so the re-run move counts as a move.
constexpr std::uint32_t kForceMoveX = 0x501502f9;

// The cell of a world coordinate, rounded as the original does: round-to-nearest of (c / 16 - 0.5) through the 1.5 * 2^23
// float trick, one up when c / 16 is exactly the next integer.
int CellOf(float coordinate) {
    const float f = coordinate * 0.0625f;
    const float rounded = (f - 0.5f) + 12582912.0f;
    std::int32_t bits = 0;
    std::memcpy(&bits, &rounded, sizeof bits);
    const int down = bits - 0x4b400000;
    return f == static_cast<float>(down + 1) ? down + 1 : down;
}

// cS3DMatrix3::RotateY on the identity (this = this * R).
void RotateY(float m[3][3], float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (int row = 0; row < 3; ++row) {
        const float x = m[row][0], z = m[row][2];
        m[row][0] = x * c - z * s;
        m[row][2] = x * s + z * c;
    }
}

ILocalGridEffect* CreateEffect(const char* name) {
    ILocalGridHost* grid = LocalGridHost();
    return grid && grid->Effects() ? grid->Effects()->Create(name) : nullptr;
}
}  // namespace

INetworkIntxToolHost* NetworkIntxToolHost() { return g_host; }
void SetNetworkIntxToolHost(INetworkIntxToolHost* host) { g_host = host; }

cSC4ViewInputControlNetworkIntxTool::cSC4ViewInputControlNetworkIntxTool(std::uint32_t ruleId) : ruleId_(ruleId) {}

cSC4ViewInputControlNetworkIntxTool::~cSC4ViewInputControlNetworkIntxTool() {
    if (preview_) preview_->Release();
    if (outline_) outline_->Release();
    if (terrainView_) terrainView_->Release();
}

cISC4View3DWin* cSC4ViewInputControlNetworkIntxTool::View() const { return g_host ? g_host->View() : nullptr; }

bool cSC4ViewInputControlNetworkIntxTool::Init() {
    if (initialized_) return true;
    cursorId_ = kNetworkIntxToolCursor;
    const bool ok = cSC4ViewInputControl::Init();
    if (g_host) {
        cellSize_ = g_host->CellSize();
        terrainView_ = g_host->TerrainView();
        if (terrainView_) terrainView_->AddRef();
    }
    localGrid_.Init();
    if (!outline_) outline_ = CreateEffect("local_tile_outline");
    initialized_ = true;
    return ok;
}

void cSC4ViewInputControlNetworkIntxTool::StopPreview() {
    if (!preview_) return;
    preview_->Stop(0);
    preview_->Release();
    preview_ = nullptr;
    previewName_.clear();
}

bool cSC4ViewInputControlNetworkIntxTool::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (cISC4View3DWin* view = View()) {
        view->ClearCursorText(kNetworkIntxToolCursorText);
        view->ClearCostIndicator(false);
    }
    localGrid_.Shutdown();
    if (terrainView_) {
        terrainView_->Release();
        terrainView_ = nullptr;
    }
    if (outline_) {
        if (outline_->IsStarted()) outline_->Stop(0);
        outline_->Release();
        outline_ = nullptr;
    }
    StopPreview();
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4ViewInputControlNetworkIntxTool::OnMouseExit() {
    ReleaseCapture();
    if (outline_ && outline_->IsStarted()) outline_->Stop(0);
    StopPreview();
    localGrid_.ShowEffect(false);
    if (cISC4View3DWin* view = View()) {
        view->ClearCursorText(kNetworkIntxToolCursorText);
        view->ClearCostIndicator(false);
    }
    return true;
}

void cSC4ViewInputControlNetworkIntxTool::StepRotationRing(bool forward) {
    if (const nSC4Networks::cIntRule* rule = cSC4NetworkTool::GetIntersectionRule(ruleId_)) ruleId_ = forward ? rule->rotationNext : rule->rotationPrev;
}

void cSC4ViewInputControlNetworkIntxTool::StepTypeRing(bool forward) {
    if (const nSC4Networks::cIntRule* rule = cSC4NetworkTool::GetIntersectionRule(ruleId_)) ruleId_ = forward ? rule->typeNext : rule->typePrev;
}

// The move again at the cursor, with the remembered position spoiled so it counts as a move.
void cSC4ViewInputControlNetworkIntxTool::RerunAtCursor() {
    int x = 0, y = 0;
    if (g_host) g_host->CursorPosition(x, y);
    std::memcpy(&lastPosition_.x, &kForceMoveX, sizeof lastPosition_.x);
    OnMouseMove(x, y, 0);
}

bool cSC4ViewInputControlNetworkIntxTool::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (!IsOnTop()) return false;
    switch (key) {
        case kKeyEscape: EndInput(); return true;
        case kKeyTab: StepTypeRing((modifiers & kModifierShift) == 0); break;
        case kKeyEnd: StepRotationRing(true); break;
        case kKeyHome: StepRotationRing(false); break;
        default: return false;
    }
    RerunAtCursor();
    return true;
}

bool cSC4ViewInputControlNetworkIntxTool::OnMouseDownL(int, int, std::uint32_t) {
    SetCapture();
    if (outline_ && outline_->IsStarted()) outline_->Stop(0);
    PlaceOrRefuse();
    return false;
}

// The network manager's highway tool places the fitting rule for real: its arms first (DoAutoComplete), then the rule;
// the cost the last move showed is paid.
void cSC4ViewInputControlNetworkIntxTool::PlaceOrRefuse() {
    if (!g_host) return;
    if (!fits_) {
        g_host->PlaySound(kNetworkIntxRefuseSound);
        return;
    }
    cSC4NetworkTool* tool = g_host->HighwayTool();
    if (!tool) return;
    tool->Init();
    tool->Reset();
    if (const nSC4Networks::cIntRule* rule = cSC4NetworkTool::GetIntersectionRule(ruleId_)) rule->DoAutoComplete(anchorX_, anchorZ_, *tool);
    tool->InsertIsolatedHighwayIntersection(anchorX_, anchorZ_, ruleId_, true);
    tool->Reset();
    tool->Shutdown();
    g_host->SpendFunds(cost_);
    g_host->PlaySound(kNetworkIntxPlaceSound);
}

bool cSC4ViewInputControlNetworkIntxTool::OnMouseUpL(int, int, std::uint32_t) {
    ReleaseCapture();
    if (outline_ && !outline_->IsStarted()) outline_->Start(0);
    if (!fits_ && g_host) g_host->PlaySound(kNetworkIntxReleaseSound);
    fits_ = false;
    return false;
}

bool cSC4ViewInputControlNetworkIntxTool::OnMouseMove(int x, int y, std::uint32_t) {
    cISC4View3DWin* view = View();
    float position[3] = {lastPosition_.x, lastPosition_.y, lastPosition_.z};
    const bool picked = view && view->PickTerrain(x, y, position, false);
    if (picked) {
        const double cell = cellSize_;
        position[0] = static_cast<float>(std::floor(static_cast<double>(position[0]) / cell) * cell + cell * 0.5);
        position[2] = static_cast<float>(cell * std::floor(static_cast<double>(position[2]) / cell) + cell * 0.5);
    }
    const bool moved = position[0] != lastPosition_.x || position[1] != lastPosition_.y || position[2] != lastPosition_.z;
    lastPosition_ = cS3DVector3{position[0], position[1], position[2]};
    if (!picked || AmCapturing()) {
        StopPreview();
        if (outline_ && outline_->IsStarted()) outline_->Stop(0);
        localGrid_.ShowEffect(false);
    } else if (moved) {
        localGrid_.SetPosition(lastPosition_);
        localGrid_.ShowEffect(!(terrainView_ && terrainView_->GridShown()));
        if (outline_) {
            cS3DTransform transform;
            transform.scale = cellSize_;
            transform.flags = static_cast<std::uint8_t>(transform.scale != 1.0f ? (transform.flags | 2) : (transform.flags & 0xfd));
            transform.translation[0] = position[0];
            transform.translation[1] = position[1];
            transform.translation[2] = position[2];
            transform.flags |= 1;
            transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 2);
            outline_->SetEffectTransform(transform);
            if (!outline_->IsStarted()) outline_->Start(0);
        }
        UpdatePlacement(CellOf(position[0]), CellOf(position[2]));
    }
    if (g_host) g_host->GraphicsSystemUpdate();
    return false;
}

// Every rule of the rotation ring, starting with the current one, is tried at the cell and then at each neighbour (the anchor
// is the cell minus the rule's handle offset) until one fits; the dry run's cost is kept. The ring position stays at the rule
// that fit. Without a fit the cursor text is the last failure of a rule tried twice; with one, its placement string and cost.
// A city that cannot pay does not fit either.
bool cSC4ViewInputControlNetworkIntxTool::UpdatePlacement(int cellX, int cellZ) {
    anchorX_ = cellX;
    anchorZ_ = cellZ;
    const std::uint32_t firstId = ruleId_;
    cRZString failure;
    fits_ = false;
    lastRule_ = nullptr;
    const nSC4Networks::cIntRule* fitting = nullptr;
    const std::uint32_t cityX = g_host ? g_host->CityCellsX() : 0, cityZ = g_host ? g_host->CityCellsZ() : 0;
    cSC4NetworkTool* tool = g_host ? g_host->HighwayTool() : nullptr;
    if (tool) {
        tool->Init();
        for (const auto& offset : kSearchPattern) {
            do {
                const nSC4Networks::cIntRule* rule = cSC4NetworkTool::GetIntersectionRule(ruleId_);
                if (rule) {
                    const int ax = cellX + offset[0] - rule->handleX, az = cellZ + offset[1] - rule->handleZ;
                    if (rule->IsWithinBounds(ax, az, cityX, cityZ)) {
                        tool->Reset();
                        rule->DoAutoComplete(ax, az, *tool);
                        unknown79_ = false;
                        const bool ok = tool->InsertIsolatedHighwayIntersection(ax, az, ruleId_, false);
                        if (!ok) {
                            if (rule == lastRule_ && tool->FailureReason() != 0)
                                tool->GetFailureReasonString(static_cast<std::uint32_t>(tool->FailureReason()), failure);   // vslot 0xd8
                        } else {
                            cost_ = static_cast<std::uint32_t>(tool->GetCostOfSolution());                                // vslot 0x50
                        }
                        if (!lastRule_ || ok) lastRule_ = rule;
                        tool->Reset();
                        if (ok) {
                            fits_ = true;
                            anchorX_ = ax;
                            anchorZ_ = az;
                            fitting = rule;
                            goto searched;
                        }
                    }
                }
                ruleId_ = rule ? rule->rotationNext : ruleId_;
            } while (ruleId_ != firstId);
        }
    searched:
        tool->Shutdown();
    }
    cISC4View3DWin* view = View();
    if (!fits_) {
        if (view) view->ClearCostIndicator(false);
        if (failure.Strlen() == 0) {
            if (view) view->ClearCursorText(kNetworkIntxToolCursorText);
        } else {
            cursorText_.Copy(failure);
            if (view) view->SetCursorText(kNetworkIntxToolCursorText, 0, nullptr, &cursorText_, 0);
        }
        SetCursor(kNetworkIntxToolCursor);
    } else {
        if (!g_host || !g_host->LoadString(kIntersectionStringGroup, fitting->placeQueryId, cursorText_))
            cursorText_.AssignFromCString("## Intersection placement string missing ##");
        SetCursor(kNetworkIntxToolFitCursor);
        if (view) {
            view->SetCursorText(kNetworkIntxToolCursorText, 0, nullptr, &cursorText_, 0);
            view->SetCostIndicator(static_cast<std::uint32_t>(cost_));
        }
    }
    if (g_host && g_host->Funds() < cost_) fits_ = false;
    if (!fits_) {
        if (lastRule_) {
            anchorX_ -= lastRule_->handleX;
            anchorZ_ -= lastRule_->handleZ;
            UpdatePreviewEffect(lastRule_);
        }
    } else {
        UpdatePreviewEffect(fitting);
    }
    return fits_;
}

// The rule's preview effect over its first model, at the terrain height there plus 0.5, turned like the model (a flipped
// model mirrors the first row); "<name>_red" when the placement does not fit. Off the terrain the effect goes.
void cSC4ViewInputControlNetworkIntxTool::UpdatePreviewEffect(const nSC4Networks::cIntRule* rule) {
    if (!rule || rule->pieces.empty() || rule->previewEffect.empty()) return;
    const nSC4Networks::cIntPiece& piece = rule->pieces.front();
    const float x = (static_cast<float>(anchorX_) + 0.5f) * 16.0f + piece.x;
    const float z = (static_cast<float>(anchorZ_) + 0.5f) * 16.0f + piece.z;
    const int cellsX = g_host ? g_host->TerrainCellsX() : 0, cellsZ = g_host ? g_host->TerrainCellsZ() : 0;
    if (x < 0.0f || z < 0.0f || static_cast<float>(cellsX << 4) < x || static_cast<float>(cellsZ << 4) < z) {
        StopPreview();
        return;
    }
    const float y = (g_host ? g_host->TerrainAltitude(x, z) : 0.0f) + 0.5f;
    std::string name = rule->previewEffect;
    if (!fits_) name += "_red";
    if (name != previewName_) {
        StopPreview();
        if ((preview_ = CreateEffect(name.c_str())) != nullptr) {
            previewName_ = name;
            preview_->Start(0);
        }
    }
    if (!preview_) return;
    cS3DTransform transform;
    transform.translation[0] = x;
    transform.translation[1] = y;
    transform.translation[2] = z;
    transform.flags |= 1;
    transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 1);
    float rotation[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    RotateY(rotation, static_cast<float>(piece.rotation) * -1.5707964f);
    if (piece.flip != 0) {
        for (float& v : rotation[0]) v = -v;
    }
    std::memcpy(transform.rotation, rotation, sizeof rotation);
    transform.flags |= 4;
    transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 1);
    preview_->SetEffectTransform(transform);
}

}  // namespace sc4
