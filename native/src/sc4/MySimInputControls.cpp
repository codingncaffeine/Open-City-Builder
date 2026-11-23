#include "sc4/MySimInputControls.h"

#include "gz/StreamOps.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/MySim.h"
#include "sc4/MySimAgentSimulator.h"
#include "sc4/Occupant.h"
#include "sc4/PylonOccupant.h"
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

// ---- dispatch -------------------------------------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kDispatchToolId = 0xebab04a9;
constexpr std::uint32_t kDispatchToolCursor = 0x2bac4970;
constexpr std::uint32_t kDispatchToolGrabCursor = 0x13b138d0;
constexpr std::uint32_t kDispatchToolCursorText = 0x49d02c4d;
constexpr std::uint32_t kMessageInputControlChanged = 0x09c8b8ea;   // data1 the old control, data2 the new one
constexpr std::uint32_t kDispatchMsgOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kDispatchMsgMySimChanged = 0xaa25827c;
constexpr std::uint32_t kMessageMySimDispatch = 0x8bbbd22a;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2Id = 0x452294aa;
constexpr std::uint32_t kGZIID_cISC4MySimId = 0x0a1da42f;
constexpr std::uint32_t kSwallowed[4] = {0xe94671eb, 0xe94671ec, 0xe94671ed, 0x894671f6};
constexpr std::uint32_t kDoubleClickMs = 500;
}  // namespace

cSC4MySimInputControlDispatch::cSC4MySimInputControlDispatch(cSC4MySim* sim) : sim_(sim) {
    if (sim_) sim_->AddRef();
}

cSC4MySimInputControlDispatch::~cSC4MySimInputControlDispatch() {
    if (hover_) hover_->Release();
    if (sim_) sim_->Release();
}

bool cSC4MySimInputControlDispatch::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZMessageTarget2Id) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cSC4ViewInputControl::QueryInterface(iid, out);
}

std::uint32_t cSC4MySimInputControlDispatch::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

bool cSC4MySimInputControlDispatch::Init() {
    if (initialized_) return true;
    id_ = kDispatchToolId;
    defaultCursor_ = kDispatchToolCursor;
    cursorId_ = kDispatchToolCursor;
    clickTimer_.SetUnits(cRZTimer::kMilliseconds);
    cSC4ViewInputControl::Init();
    if (g_host) {
        auto* target = static_cast<cIGZMessageTarget2*>(this);
        g_host->Listen(target, kMessageInputControlChanged, true);
        for (std::uint32_t type : kSwallowed) g_host->Listen(target, type, true);
        g_host->Listen(target, kDispatchMsgMySimChanged, true);
    }
    active_ = true;
    return true;
}

// The drag left unfinished puts the box back where it was, with its old state; the MySim's tool session ends.
bool cSC4MySimInputControlDispatch::Shutdown() {
    if (!initialized_) return true;
    if (g_host) {
        g_host->ClearCursorText(kDispatchToolCursorText);
        auto* target = static_cast<cIGZMessageTarget2*>(this);
        g_host->Listen(target, kDispatchMsgMySimChanged, false);
        g_host->Listen(target, kMessageInputControlChanged, false);
        for (std::uint32_t type : kSwallowed) g_host->Listen(target, type, false);
    }
    if (hover_) hover_->SetUIState(1);
    SetHoverPylon(nullptr);
    cISC4PylonOccupant* pylon = MySimPylon();
    if (pylon && pylon->GetDragState()) {
        if (cISC4Occupant* occupant = pylon->AsOccupant()) occupant->SetPosition(&dragStart_.x);   // 0x24
        pylon->SetDispatchState(dragStartState_);
    }
    if (sim_) sim_->DispatchToolEnd();
    if (pylon) pylon->SetDragState(false);
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4MySimInputControlDispatch::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    IsKeyToBePassed(key, modifiers);
    return false;
}

cISC4PylonOccupant* cSC4MySimInputControlDispatch::MySimPylon() {
    IMySimPylon* pylon = sim_ ? sim_->DispatchToolGetPylon() : nullptr;
    return pylon ? pylon->AsPylon() : nullptr;
}

// The renderer's pick through the MySim pylon filter; only a box whose owner is a MySim counts.
bool cSC4MySimInputControlDispatch::PickPylonOccupant(int x, int y, cISC4PylonOccupant*& out) {
    out = nullptr;
    cISC4PylonOccupant* pylon = g_host ? g_host->PickPylon(x, y) : nullptr;
    if (!pylon) return false;
    cIGZUnknown* owner = pylon->GetPylonOwner();   // 0x50
    void* raw = nullptr;
    if (owner && owner->QueryInterface(kGZIID_cISC4MySimId, &raw) && raw) {
        static_cast<cIGZUnknown*>(raw)->Release();
        out = pylon;   // the pick's reference goes to the caller
        return true;
    }
    pylon->Release();
    return false;
}

void cSC4MySimInputControlDispatch::SetHoverPylon(cISC4PylonOccupant* pylon) {
    if (hover_ == pylon) return;
    if (hover_) {
        hover_->SetUIState(1);   // 0x18(1)
        hover_->Release();
        hover_ = nullptr;
    }
    hover_ = pylon;
    if (hover_) {
        hover_->AddRef();
        hover_->SetUIState(3);   // 0x18(3), the hovered look
    }
}

bool cSC4MySimInputControlDispatch::SetNewMySim(cSC4MySim* sim) {
    if (sim_ != sim) {
        if (sim_) sim_->DispatchToolEnd();   // 0x174
        if (sim) sim->AddRef();
        if (sim_) sim_->Release();
        sim_ = sim;
    }
    return true;
}

bool cSC4MySimInputControlDispatch::LocationIsOKForDispatch(const float* xyz) {
    return sim_ && sim_->DispatchToolCanDispatch(xyz[0], xyz[2]);   // 0x188
}

void cSC4MySimInputControlDispatch::CalculateDragPylonOffset(cISC4PylonOccupant* pylon, int x, int y) {
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (cISC4Occupant* occupant = pylon->AsOccupant()) occupant->GetPosition(position);
    float sx = 0.0f, sy = 0.0f;
    if (g_host) g_host->ProjectToScreen(position, sx, sy);
    dragOffsetX_ = static_cast<int>(sx - static_cast<float>(x));
    dragOffsetY_ = static_cast<int>(sy - static_cast<float>(y));
}

// i386 0x00486510.
bool cSC4MySimInputControlDispatch::OnMouseDownL(int x, int y, std::uint32_t) {
    if (!g_host || !sim_) {
        PlayResultSound(0);
        return false;
    }
    cISC4PylonOccupant* pylon = MySimPylon();
    if (pylon) pylon->AddRef();
    cISC4PylonOccupant* picked = nullptr;
    bool doubleClick = false;
    if (!clickTimer_.IsTimerRunning() || clickTimer_.GetElapsedTimeU32() > kDoubleClickMs - 1) {
        clickTimer_.Restart();
    } else {
        clickTimer_.Reset();
        doubleClick = true;
    }
    bool result = true;
    bool pickBox = false;
    if (!pylon) {
        if (PickPylonOccupant(x, y, picked)) {
            pickBox = true;
        } else {
            void* begun = sim_->DispatchToolBegin();   // 0x170
            (void)begun;
            if (cISC4PylonOccupant* own = MySimPylon()) {
                own->AddRef();
                pylon = own;
            }
            float ground[3] = {0.0f, 0.0f, 0.0f};
            if (pylon && g_host->PickTerrain(x, y, ground)) {
                ground[1] = g_host->GroundAltitude(ground[0], ground[2]);
                if (LocationIsOKForDispatch(ground)) {
                    if (cISC4Occupant* occupant = pylon->AsOccupant()) occupant->SetPosition(ground);
                    sim_->DispatchToolLockPylon(true);   // 0x178(1)
                }
            }
        }
    } else if (pylon->GetDragState()) {   // 0x40: a drag ends with this click
        if (!doubleClick) {
            float ground[3] = {0.0f, 0.0f, 0.0f};
            if (g_host->PickTerrain(x + dragOffsetX_, y + dragOffsetY_, ground)) {
                ground[1] = g_host->GroundAltitude(ground[0], ground[2]);
                if (cISC4Occupant* occupant = pylon->AsOccupant()) occupant->SetPosition(ground);
            }
        } else {
            g_host->PostMessage(kMessageMySimDispatch, 1, static_cast<cISC4MySim*>(sim_));
        }
        pylon->SetDragState(false);
        sim_->DispatchToolLockPylon(true);
    } else {
        pickBox = true;
    }
    if (pickBox) {
        if (!picked) PickPylonOccupant(x, y, picked);
        if (picked) {
            cIGZUnknown* owner = picked->GetPylonOwner();
            void* raw = nullptr;
            cSC4MySim* other = nullptr;
            if (owner && owner->QueryInterface(kGZIID_cISC4MySimId, &raw) && raw) {
                other = dynamic_cast<cSC4MySim*>(static_cast<cISC4MySim*>(raw));
            }
            if (SetNewMySim(other)) {
                sim_->DispatchToolLockPylon(doubleClick);
                picked->SetDragState(!doubleClick);
                g_host->PostMessage(kMessageMySimDispatch, doubleClick ? 1u : 0u, static_cast<cISC4MySim*>(sim_));
                if (!doubleClick) {
                    CalculateDragPylonOffset(picked, x, y);
                    if (cISC4Occupant* occupant = picked->AsOccupant()) occupant->GetPosition(&dragStart_.x);
                    dragStartState_ = picked->GetDispatchState();   // 0x30
                }
            }
            if (raw) static_cast<cIGZUnknown*>(raw)->Release();
            picked->Release();
        } else {
            result = false;
        }
    }
    if (pylon) pylon->Release();
    PlayResultSound(0);
    return result;
}

// i386 0x00486986: without a drag, a MySim's call box under the pointer is hovered (grab cursor) when it has a UI state;
// during a drag the box follows the ground under the pointer (from 500 ms after the click) and shows whether the MySim
// may go there (dispatch state 3) or not (1).
bool cSC4MySimInputControlDispatch::OnMouseMove(int x, int y, std::uint32_t) {
    if (!g_host || !sim_) return true;
    cISC4PylonOccupant* pylon = MySimPylon();
    if (!pylon) return true;
    pylon->AddRef();
    if (!pylon->GetDragState()) {
        cISC4PylonOccupant* picked = nullptr;
        if (!PickPylonOccupant(x, y, picked)) {
            SetCursor(defaultCursor_);
            SetHoverPylon(nullptr);
        } else if (picked->GetUIState() == 0) {   // 0x1c
            SetHoverPylon(nullptr);
            SetCursor(defaultCursor_);
        } else {
            SetHoverPylon(picked);
            SetCursor(kDispatchToolGrabCursor);
        }
        if (picked) picked->Release();
    } else {
        SetCursor(kDispatchToolGrabCursor);
        bool move = true;
        if (clickTimer_.IsTimerRunning()) {
            if (clickTimer_.GetElapsedTimeU32() < kDoubleClickMs) move = false;
            else clickTimer_.Reset();
        }
        float ground[3] = {0.0f, 0.0f, 0.0f};
        if (move && g_host->PickTerrain(x + dragOffsetX_, y + dragOffsetY_, ground)) {
            ground[1] = g_host->GroundAltitude(ground[0], ground[2]);
            dragOK_ = LocationIsOKForDispatch(ground);
            if (dragOK_) {
                if (cISC4Occupant* occupant = pylon->AsOccupant()) occupant->SetPosition(ground);
                pylon->SetDispatchState(3);
            } else {
                pylon->SetDispatchState(1);
            }
        }
    }
    pylon->Release();
    return true;
}

// i386 0x00485e66.
bool cSC4MySimInputControlDispatch::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    if (type == kDispatchMsgOccupantRemoved) {
        if (hover_ && standard->GetVoid1() == static_cast<void*>(hover_->AsOccupant())) {
            hover_->Release();
            hover_ = nullptr;
        }
        return true;
    }
    if (type == kDispatchMsgMySimChanged) {
        if (sim_ && !sim_->IsActive() && g_host) g_host->RemoveAllTools();
        return false;
    }
    for (std::uint32_t swallowed : kSwallowed)
        if (type == swallowed) return true;
    if (type != kMessageInputControlChanged) return false;
    auto* oldControl = static_cast<cSC4ViewInputControl*>(standard->GetVoid1());
    auto* newControl = static_cast<cSC4ViewInputControl*>(standard->GetVoid2());
    if (oldControl == this) {
        if (newControl != this && (!newControl || newControl->GetID() != 0x48e2a1f8)) {
            active_ = false;
            if (g_host) g_host->ClearCursorText(kDispatchToolCursorText);
            SetHoverPylon(nullptr);
            cISC4PylonOccupant* pylon = MySimPylon();
            if (pylon && pylon->GetDragState()) {
                if (cISC4Occupant* occupant = pylon->AsOccupant()) occupant->SetPosition(&dragStart_.x);
                pylon->SetDispatchState(dragStartState_);
            }
        }
    } else if (newControl == this) {
        active_ = true;
    }
    return true;
}

}  // namespace sc4
