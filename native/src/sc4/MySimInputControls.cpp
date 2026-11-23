#include "sc4/MySimInputControls.h"

#include "gz/StreamOps.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/MySim.h"
#include "sc4/MySimAgentSimulator.h"
#include "sc4/Occupant.h"
#include "sc4/View3DWin.h"

namespace sc4 {

namespace {
IMySimToolHost* g_host = nullptr;
constexpr float kComplete = 100.0f;   // __kSC4PercentageOneHundred
constexpr std::uint8_t kLotStateFunctional = 3;
}  // namespace
IMySimToolHost* MySimToolHost() { return g_host; }
void SetMySimToolHost(IMySimToolHost* host) { g_host = host; }

bool cSC4MySimBuildingTool::Init() {
    SetCursor(kMySimToolCursor);   // vslot 0x24
    return cSC4ViewInputControl::Init();
}

// The target's saved highlight goes back, the callout and the portraits go (the Shutdown of each tool).
bool cSC4MySimBuildingTool::Shutdown() {
    ClearTarget();
    agents_ = nullptr;
    if (g_host) g_host->HideTarget();
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4MySimBuildingTool::OnKeyDown(std::uint32_t key, std::uint32_t) {
    if (key == 0x1b) EndInput();   // vslot 0x70
    return key == 0x1b;
}

bool cSC4MySimBuildingTool::OnMouseExit() {
    if (g_host) g_host->HideTarget();   // the portrait windows' vslot 0x118 (hide)
    return true;
}

bool cSC4MySimBuildingTool::IsCurrentControl() {
    cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
    return view && view->GetCurrentViewInputControl() == this;   // spView3D vslot 0x60
}

void cSC4MySimBuildingTool::ClearTarget() {
    if (!target_) return;
    if (cISC4Occupant* occupant = target_->AsOccupant()) occupant->SetHighlight(static_cast<int>(savedHighlight_), false);   // 0x44
    target_ = nullptr;
}

// i386 0x00486e26 / 0x0048396e / 0x004854a2: the occupant under the pointer, when it is a building (type 0x278128a0,
// QI 0x87dda3a9) the tool accepts, becomes the highlighted target and the callout follows it; anything else shows the
// pointer's portrait and drops the target.
bool cSC4MySimBuildingTool::OnMouseMove(int x, int y, std::uint32_t) {
    cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
    cISC4Occupant* picked = nullptr;
    cRZAutoRefCount<cISC4BuildingOccupant> building;
    if (view && view->PickOccupant(x, y, picked) && picked) {
        void* raw = nullptr;
        if (picked->GetType() == kOccupantTypeBuilding && picked->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) && raw) {
            auto* found = static_cast<cISC4BuildingOccupant*>(raw);
            building = found;
            found->Release();   // the auto-ref holds it
        }
    }
    int wealth = 0;
    if (building && agents_ && building->GetCompletionPercent() >= kComplete && Accepts(building.get(), wealth)) {
        if (target_.get() != building.get()) {
            ClearTarget();
            target_ = building.get();
            if (cISC4Occupant* occupant = target_->AsOccupant()) {
                savedHighlight_ = occupant->GetHighlight();   // 0x40
                occupant->SetHighlight(1, false);             // 0x44(1, 0)
            }
        }
        if (g_host) g_host->ShowTarget(target_.get(), wealth, ForHome(), x, y);
    } else {
        if (g_host) g_host->ShowNoTarget(x, y);
        ClearTarget();
    }
    if (picked) picked->Release();
    return true;
}

// ---- move in -------------------------------------------------------------------------------------------------------

bool cSC4MySimInputControlMoveIn::Init(cSC4MySim* sim) {
    if (!Init()) return false;   // vslot 0xc
    cISC4MySim* view = sim ? static_cast<cISC4MySim*>(sim) : nullptr;   // the sim's cISC4MySim (+0xc4)
    if (view) view->AddRef();
    if (sim_) sim_->Release();
    sim_ = view;
    agents_ = g_host ? g_host->Agents() : nullptr;
    return true;
}

bool cSC4MySimInputControlMoveIn::Shutdown() {
    if (sim_) {
        sim_->Release();
        sim_ = nullptr;
    }
    return cSC4MySimBuildingTool::Shutdown();
}

// A home: no MySim lives there (agent vslot 0x24), a residential profile (purpose 1), the MySim would live there
// (cISC4MySim vslot 0x1c TestForResidence(building, true, false)) and its lot is functional (lot vslot 0x84 == 3).
bool cSC4MySimInputControlMoveIn::Accepts(cISC4BuildingOccupant* building, int& wealth) {
    if (!sim_ || agents_->MySimLivesHere(building) || building->GetBuildingProfile().purpose != 1) return false;
    if (!sim_->TestForResidence(building, true, false)) return false;
    cSC4Lot* lot = g_host ? g_host->OccupantLot(building->AsOccupant()) : nullptr;   // spLotManager 0x38
    if (!lot || lot->GetState() != kLotStateFunctional) return false;
    wealth = lot->GetOccupantWealth();   // lot vslot 0xa4
    return true;
}

// i386 0x00486c7e: on top of the view, the MySim moves into the target (cISC4MySim vslot 0x34 SetResidence) and the tool ends.
bool cSC4MySimInputControlMoveIn::OnMouseDownL(int, int, std::uint32_t) {
    if (!IsCurrentControl()) return false;
    if (target_ && sim_) {
        sim_->SetResidence(target_.get());
        EndInput();
    }
    return true;
}

// ---- change house ---------------------------------------------------------------------------------------------------

bool cSC4MySimInputControlChangeHouse::Init(cSC4MySim* sim) {
    if (!Init()) return false;
    sim_ = sim;
    agents_ = g_host ? g_host->Agents() : nullptr;
    return true;
}

bool cSC4MySimInputControlChangeHouse::Shutdown() {
    sim_ = nullptr;
    return cSC4MySimBuildingTool::Shutdown();
}

// As the move-in tool, with the wealth test (cSC4MySim vslot 0x94 TestForResidence(building, true, true)).
bool cSC4MySimInputControlChangeHouse::Accepts(cISC4BuildingOccupant* building, int& wealth) {
    if (!sim_ || agents_->MySimLivesHere(building) || building->GetBuildingProfile().purpose != 1) return false;
    if (!sim_->TestForResidence(building, true, true)) return false;
    cSC4Lot* lot = g_host ? g_host->OccupantLot(building->AsOccupant()) : nullptr;
    if (!lot || lot->GetState() != kLotStateFunctional) return false;
    wealth = lot->GetOccupantWealth();
    return true;
}

// i386 0x00483796: on top of the view, a target the MySim would still live in (TestForResidence(target, true, false)) that
// it moves to (vslot 0x8c ChangeResidence) ends the tool.
bool cSC4MySimInputControlChangeHouse::OnMouseDownL(int, int, std::uint32_t) {
    if (!IsCurrentControl()) return false;
    if (target_ && sim_ && sim_->TestForResidence(target_.get(), true, false) && sim_->ChangeResidence(target_.get())) EndInput();
    return true;
}

// ---- change job -----------------------------------------------------------------------------------------------------

bool cSC4MySimInputControlChangeJob::Init(cSC4MySim* sim) {
    if (!Init()) return false;
    sim_ = sim;
    agents_ = g_host ? g_host->Agents() : nullptr;
    return true;
}

bool cSC4MySimInputControlChangeJob::Shutdown() {
    sim_ = nullptr;
    return cSC4MySimBuildingTool::Shutdown();
}

// A workplace: not residential, and the lot at the building's position (spLotManager vslot 0x18) has jobs for the
// MySim's wealth (lot vslot 0xfc GetTravelJobs > 0), condition at most 3 (vslot 0x8c) and is functional (vslot 0x84 == 3).
bool cSC4MySimInputControlChangeJob::Accepts(cISC4BuildingOccupant* building, int& wealth) {
    if (!sim_ || building->GetBuildingProfile().purpose == 1) return false;
    cISC4Occupant* occupant = building->AsOccupant();
    if (!occupant || !g_host) return false;
    cS3DVector3 position{0.0f, 0.0f, 0.0f};
    occupant->GetPosition(&position.x);
    cSC4Lot* lot = g_host->LotAt(position);
    if (!lot || !(lot->GetTravelJobs(sim_->GetWealth()) > 0) || lot->GetCondition() > 3 || lot->GetState() != kLotStateFunctional)
        return false;
    wealth = lot->GetOccupantWealth();
    return true;
}

// i386 0x00484932: on top of the view, the MySim takes the job (cSC4MySim vslot 0x90 ChangeJob) and the tool ends.
bool cSC4MySimInputControlChangeJob::OnMouseDownL(int, int, std::uint32_t) {
    if (!IsCurrentControl()) return false;
    if (target_ && sim_) {
        sim_->ChangeJob(target_.get());
        EndInput();
    }
    return true;
}

}  // namespace sc4
