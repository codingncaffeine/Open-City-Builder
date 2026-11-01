#include "sc4/ViewInputControlFlora.h"

#include "sc4/View3DWin.h"

namespace sc4 {

namespace {
IFloraToolHost* g_host = nullptr;
}
IFloraToolHost* FloraToolHost() { return g_host; }
void SetFloraToolHost(IFloraToolHost* host) { g_host = host; }

bool cSC4ViewInputControlFlora::Init() {
    if (initialized_) return true;
    id_ = kFloraToolId;
    cursorId_ = kFloraToolCursor;
    cSC4ViewInputControl::Init();
    simulator_ = g_host && g_host->FloraSimulatorPresent();
    if (simulator_) {
        cost_ = g_host->FloraCost(floraType_);
        spent_ = 0;
        if (cISC4View3DWin* view = g_host->View()) view->SetCostIndicator(static_cast<std::uint32_t>(cost_));   // vslot 0xa8
    }
    dragging_ = false;
    return true;
}

bool cSC4ViewInputControlFlora::Shutdown() {
    if (!initialized_) return true;
    simulator_ = false;
    if (cISC4View3DWin* view = g_host ? g_host->View() : nullptr) view->ClearCostIndicator(true);   // vslot 0xb0
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4ViewInputControlFlora::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) { return !IsKeyToBePassed(key, modifiers); }

// One tree at xyz: a refusal on water shows the "no" cursor; a planted tree plays its sound, is paid for (the press counts the
// cost only when the withdrawal succeeds, the drag always), updates the cost indicator and gives its aura.
void cSC4ViewInputControlFlora::Plant(const float xyz[3], bool countOnlyWhenPaid) {
    float position[3] = {xyz[0], xyz[1], xyz[2]};
    if (!g_host->AddNewFloraOccupant(floraType_, position)) {
        if (g_host->IsWaterTerrain(xyz[0], xyz[2])) SetCursor(kCursorNo);
        return;
    }
    g_host->PlayPlantSound(xyz);
    const bool paid = g_host->WithdrawFunds(cost_);
    if (paid || !countOnlyWhenPaid) {
        spent_ += cost_;
        if (cISC4View3DWin* view = g_host->View())
            view->SetCostIndicator(static_cast<std::uint32_t>(spent_), static_cast<std::uint32_t>(cost_));   // vslot 0xac
    }
    g_host->AddAuraEffect(xyz[0], xyz[2]);
}

bool cSC4ViewInputControlFlora::OnMouseDownL(int x, int y, std::uint32_t) {
    cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    if (view && view->PickTerrain(x, y, xyz, false)) {                    // vslot 0x90
        if (simulator_ && cost_ <= g_host->Funds()) Plant(xyz, true);
        dragging_ = true;
    }
    UpdateCursor();
    return false;
}

bool cSC4ViewInputControlFlora::OnMouseUpL(int, int, std::uint32_t) {
    dragging_ = false;
    if (cISC4View3DWin* view = g_host ? g_host->View() : nullptr) {
        if (spent_ <= 0)
            view->SetCostIndicator(static_cast<std::uint32_t>(cost_), static_cast<std::uint32_t>(cost_));
        else
            view->ClearCostIndicator(true);
    }
    noFunds_ = false;
    spent_ = 0;
    return false;
}

bool cSC4ViewInputControlFlora::OnMouseMove(int x, int y, std::uint32_t) {
    bool valid = false;
    cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    if (view && simulator_ && view->PickTerrain(x, y, xyz, false)) {
        if (!dragging_) {
            valid = g_host->LocationIsOKForNewFlora(floraType_, xyz[0], xyz[2]);
        } else if (g_host->Funds() < cost_) {
            noFunds_ = true;
        } else {
            Plant(xyz, false);
        }
    }
    UpdateCursor();
    hoverValid_ = valid;
    return false;
}

}  // namespace sc4
