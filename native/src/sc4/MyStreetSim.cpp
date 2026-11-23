#include "sc4/MyStreetSim.h"

#include "sc4/AdvisorSystem.h"
#include "sc4/MySimDispatch.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/View3DWin.h"

namespace sc4 {

namespace {

void SetTableNumber(cISCLua* lua, const char* key, long value) { MySimSetTableNumber(lua, key, value); }

constexpr std::uint32_t kStreetAutomatonFlag = 0x2000000;      // set on a car that carries a street sim
constexpr std::uint8_t kStreetCellLifespan = 0xff;
constexpr std::uint32_t kAutomatonBehaviorIdle = 7;

// The automaton's occupant group decides the trip: the Lua flag, the travel type and the state (on foot a fixed 6).
struct TripKind { std::uint32_t groups[2]; const char* key; int travelType; };
constexpr TripKind kTripKinds[] = {
    {{0x420b, 0x420b}, "trip_in_car", 1},
    {{0x4200, 0x4200}, "trip_on_bus", 2},
    {{0x4201, 0x4215}, "trip_on_train", 3},
    {{0x422c, 0x422d}, "trip_on_elevated_train", 7},
    {{0x422e, 0x422f}, "trip_on_monorail", 8},
};

}  // namespace

// i386 0x00489340: the street bitmap, no track handle, and the street view's own (empty) string.
cSC4MyStreetSim::cSC4MyStreetSim() : extra_(new cRZString()) {
    SetBitmapId(kMyStreetSimBitmapId);
    SetTrackHandle(-1);
}

bool cSC4MyStreetSim::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZCLSID_cSC4MyStreetSim || iid == kGZIID_cISC4MyStreetSim) {
        *out = static_cast<cSC4MyStreetSim*>(this);   // the original answers its +0x2b8 view for 0x0bf61276
        AddRef();
        return true;
    }
    return cSC4MySim::QueryInterface(iid, out);
}

bool cSC4MyStreetSim::Init(int advisorType) {
    if (!cSC4MySim::Init(advisorType)) return false;
    if (LuaTableRef() == 0) CreateLuaTable();
    return true;
}

bool cSC4MyStreetSim::Shutdown() { return cSC4MySim::Shutdown(); }

bool cSC4MyStreetSim::Write(cIGZOStream& stream) const {
    cSC4MySim::Write(stream);
    return !stream.GetError();
}

bool cSC4MyStreetSim::Read(cIGZIStream& stream) {
    cSC4MySim::Read(stream);
    return !stream.GetError();
}

// i386 0x00487f3e / 0x00487f74 / 0x00487fa6: the sim lives only while it rides an automaton.
void cSC4MyStreetSim::simulate_day() {
    if (!automaton_) return;
    ActivateLuaContext();   // vslot 0x16c
    simulate_balloons(false);
}

void cSC4MyStreetSim::simulate_day_events() {
    if (!automaton_) return;
    ActivateLuaContext();
    cSC4MySim::simulate_day_events();
}

void cSC4MyStreetSim::simulate_month() {
    if (!automaton_) return;
    cSC4MySim::simulate_month();
}

// i386 0x00487fbe: attach again to the same automaton (the track and the trip variables come back).
void cSC4MyStreetSim::ShowLocation() {
    if (!automaton_) return;
    cRZAutoRefCount<IMySimDispatchAutomaton> automaton(automaton_.get());
    AttachAutomaton(automaton.get());   // vslot 0x1a8
}

// i386 0x00488d36: detach, end the street-sim tool if it is the current control, and keep the automaton for a later
// ShowLocation.
void cSC4MyStreetSim::HideLocation() {
    if (!automaton_) return;
    cRZAutoRefCount<IMySimDispatchAutomaton> automaton(automaton_.get());
    DetachAutomaton();   // vslot 0x1ac
    IMyStreetSimToolHost* tool = MyStreetSimToolHost();
    if (cISC4View3DWin* view = tool ? tool->View() : nullptr) {
        cSC4ViewInputControl* current = view->GetCurrentViewInputControl();   // spView3D vslot 0x60
        if (current && current->GetID() == kMyStreetSimToolId) view->RemoveCurrentViewInputControl(false);   // vslot 0x68(0)
    }
    automaton_ = automaton.get();
}

// i386 0x0048801e.
bool cSC4MyStreetSim::DetachAutomaton() {
    if (automaton_) {
        if (IMySimDispatchRoute* route = automaton_->RouteInterface()) route->Release();   // queried, then dropped
        automaton_->RemoveCallback(static_cast<cIGZMessageTarget2*>(this));                 // vslot 0xa0(this + 0x10)
        automaton_ = nullptr;
        state_ = 0;
    }
    if (trackHandle_ != -1) {
        IMySimHost* host = MySimHost();
        if (IMySimDispatchVehicleView* view = host ? host->DispatchVehicleView() : nullptr) view->UntrackOccupant(trackHandle_, false);
        trackHandle_ = -1;
    }
    state_ = 0;
    attached_ = false;
    return true;
}

// i386 0x00488e1a.
bool cSC4MyStreetSim::AttachAutomaton(IMySimDispatchAutomaton* automaton) {
    DetachAutomaton();   // vslot 0x1ac
    cISCLua* lua = lua_.get();
    if (!automaton || !lua) return true;
    automaton_ = automaton;
    if (IMySimDispatchRoute* route = automaton_->RouteInterface()) {   // QueryInterface(0x8b99ee7c): a car
        route->SetCellLifespan(kStreetCellLifespan);                     // vslot 0x284
        automaton_->SetAutomataFlag(kStreetAutomatonFlag, true);         // vslot 0x78
        route->Release();
    }
    automaton_->AddCallback(static_cast<cIGZMessageTarget2*>(this));     // vslot 0x9c(this + 0x10)
    lua->GetRef(luaTableRef_);                                           // vslot 0xac
    SetTableNumber(lua, "wealth", 2);
    SetTableNumber(lua, "trip_in_transit", 1);
    SetTableNumber(lua, "trip_at_location", 0);
    SetTableNumber(lua, "trip_at_transit_switch", 0);
    SetTableNumber(lua, "trip_idle", 0);
    SetTableNumber(lua, "trip_approaching_location", 0);
    cISC4Occupant* occupant = automaton->AsOccupant();                   // vslot 0x18
    SetTableNumber(lua, "trip_on_bus", 0);
    SetTableNumber(lua, "trip_on_foot", 0);
    SetTableNumber(lua, "trip_on_subway", 0);
    SetTableNumber(lua, "trip_in_car", 0);
    SetTableNumber(lua, "trip_on_train", 0);
    SetTableNumber(lua, "trip_on_elevated_train", 0);
    SetTableNumber(lua, "trip_on_monorail", 0);
    travelType_ = 9;
    if (occupant && occupant->IsOccupantGroup(0x4001)) {                 // a pedestrian
        SetTableNumber(lua, "trip_on_foot", 1);
        state_ = 6;
        travelType_ = 0;
        if (automaton_->GetBehavior() == kAutomatonBehaviorIdle) SetTableNumber(lua, "trip_idle", 1);   // vslot 0xec
    } else if (occupant) {
        for (const TripKind& kind : kTripKinds) {
            if (!occupant->IsOccupantGroup(kind.groups[0]) && !occupant->IsOccupantGroup(kind.groups[1])) continue;
            SetTableNumber(lua, kind.key, 1);
            // The advisor system's random number: below 0x7fffffff gives state 5, else 4.
            cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
            cRZRandom* random = system ? system->GetRandom() : nullptr;
            const std::uint32_t roll = random ? random->RandomUint32Uniform() : 0;
            state_ = roll < 0x7fffffffu ? 5 : 4;
            travelType_ = kind.travelType;
            break;
        }
    }
    SetTableNumber(lua, "state", state_);
    lua->Pop(1);                                                         // vslot 0x2c(1)
    update_trip_variables();
    IMySimHost* host = MySimHost();
    if (IMySimDispatchVehicleView* view = host ? host->DispatchVehicleView() : nullptr)
        trackHandle_ = view->TrackOccupant(automaton_->AsOccupant(), kMyStreetSimTrackerType, bitmapId_, nullptr);
    attached_ = true;
    return true;
}

// i386 0x00488112: the trip's surroundings at the automaton's occupant (the base class samples them at the dispatch's
// location). Quirk kept: without the traffic, pollution and crime simulators the table stays pushed, as in the base.
void cSC4MyStreetSim::update_trip_variables() {
    cISCLua* lua = lua_.get();
    if (!lua) return;
    lua->GetRef(luaTableRef_);                                           // vslot 0xac
    MySimLocation location;
    location.SetByOccupant(automaton_ ? automaton_->AsOccupant() : nullptr);
    IMySimHost* host = MySimHost();
    const int half = localAreaSize_ / 2;
    const int x0 = location.cellX - half, z0 = location.cellZ - half;
    const int x1 = location.cellX + half, z1 = location.cellZ + half;
    if (!host || !host->TripSimulatorsPresent()) return;
    using Grid = IMySimHost::TripGrid;
    auto grid = [host, &location](Grid which) { return host->TripGridValue(which, location.x, location.z); };
    SetTableNumber(lua, "trip_traffic_congestion", static_cast<long>(grid(Grid::Congestion)) & 0xff);
    SetTableNumber(lua, "trip_time_ratio", static_cast<long>(host->TravelTimeRatio(location.cellX, location.cellZ, travelType_)));
    SetTableNumber(lua, "trip_wait_length", 0);
    SetTableNumber(lua, "trip_pollution_air", static_cast<std::int16_t>(static_cast<long>(grid(Grid::PollutionAir))));
    SetTableNumber(lua, "trip_pollution_water", static_cast<std::int16_t>(static_cast<long>(grid(Grid::PollutionWater))));
    SetTableNumber(lua, "trip_pollution_garbage", static_cast<std::int16_t>(static_cast<long>(grid(Grid::PollutionGarbage))));
    SetTableNumber(lua, "trip_crime", static_cast<long>(grid(Grid::Crime)) & 0xff);
    SetTableNumber(lua, "trip_mayor_rating", static_cast<std::int8_t>(static_cast<long>(grid(Grid::MayorRating))));
    SetTableNumber(lua, "trip_eq", static_cast<int>(grid(Grid::EducationQuotient)));
    SetTableNumber(lua, "trip_hq", static_cast<int>(grid(Grid::HealthQuotient)));
    SampleTripLocalArea(lua, host, x0, z0, x1, z1, extra_.get());
    lua->Pop(1);                                                         // vslot 0x2c(1)
}

// i386 0x00488c34: its own "changed" post and, while the game is paused, every 2 seconds, sample the trip again; a
// removal of the automaton (0x09d92fc3 with data2 2, 3 or 5) detaches. The base class sees every message after.
bool cSC4MyStreetSim::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message ? message->GetType() : 0;
    if (type == kMessageMySimChanged) {
        // Only a post that carries the street view counts (the pointer QueryInterface(0x0bf61276) answers); the base
        // class posts its cISC4MySim view, which is another sub-object, as in the original (+0x2b8 vs the base view).
        cIGZUnknown* subject = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();   // vslot 0x80
        if (subject && static_cast<void*>(subject) == static_cast<void*>(static_cast<cSC4MyStreetSim*>(this))) {
            update_trip_variables();
            simulate_balloons(true);
        }
    } else if (type == kMessageMySimSimulate) {
        IMySimHost* host = MySimHost();
        if (state_ != 0 && host && host->SimulatorAnyPaused() && balloonsTimer_.GetElapsedTimeU32() > 2) {
            update_trip_variables();
            simulate_balloons(true);
            balloonsTimer_.Restart();
        }
    } else if (type == kMessageMyStreetSimRemoved) {
        const std::uint64_t reason = static_cast<cIGZMessage2Standard*>(message)->GetData2();   // vslot 0x38
        if (reason < 6 && ((1u << reason) & 0x2cu) != 0) DetachAutomaton();
    }
    return cSC4MySim::DoMessage(message);
}

// i386 0x00487ea2: the tool goes on the view's control stack (operation 2) when it initialises.
bool cSC4MyStreetSim::CreateMyStreetSimTool() {
    cRZAutoRefCount<cSC4MyStreetSimInputControl> control(new cSC4MyStreetSimInputControl());
    const bool ok = control->Init(this);
    IMyStreetSimToolHost* host = MyStreetSimToolHost();
    if (ok && host && host->View()) host->View()->SetCurrentViewInputControl(control.get(), 2);   // spView3D vslot 0x64
    return ok;
}

// ---------------------------------------------------------------------------------------------- the street-sim tool

namespace {
IMyStreetSimToolHost* g_toolHost = nullptr;
constexpr float kPersonBoxHalfX = 3.0f, kPersonBoxHalfY = 7.0f, kPersonBoxHalfZ = 3.0f;   // i386 0x00a70728 / 0x00a708d0
}  // namespace
IMyStreetSimToolHost* MyStreetSimToolHost() { return g_toolHost; }
void SetMyStreetSimToolHost(IMyStreetSimToolHost* host) { g_toolHost = host; }

bool cSC4MyStreetSimInputControl::Init() {
    SetCursor(kMyStreetSimCursor);   // vslot 0x24
    return cSC4ViewInputControl::Init();
}

bool cSC4MyStreetSimInputControl::Init(cSC4MyStreetSim* sim) {
    if (!Init()) return false;   // vslot 0xc
    sim_ = sim;
    return true;
}

bool cSC4MyStreetSimInputControl::Shutdown() {
    sim_ = nullptr;
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4MyStreetSimInputControl::OnKeyDown(std::uint32_t key, std::uint32_t) {
    if (key != 0x1b) return false;
    EndInput();
    return true;
}

// i386 0x00489612: on top of the stack, a hovered automaton whose exemplar lacks 0x1bc42c49 becomes the street sim's.
bool cSC4MyStreetSimInputControl::OnMouseDownL(int, int, std::uint32_t) {
    IMyStreetSimToolHost* host = g_toolHost;
    cISC4View3DWin* view = host ? host->View() : nullptr;
    if (!view || view->GetCurrentViewInputControl() != this) return false;   // spView3D vslot 0x60
    if (hovered_) {
        bool value = false;
        cISC4Occupant* occupant = hovered_->AsOccupant();
        cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kPropertyNoStreetSim, value) && sim_)
            sim_->AttachAutomaton(hovered_.get());   // street view vslot 0xc
    }
    return true;
}

// i386 0x0048968a.
bool cSC4MyStreetSimInputControl::OnMouseMove(int x, int y, std::uint32_t) {
    hovered_ = nullptr;
    IMyStreetSimToolHost* host = g_toolHost;
    cISC4View3DWin* view = host ? host->View() : nullptr;
    if (!view) return true;
    cRZAutoRefCount<IMySimDispatchAutomaton> automaton;
    cISC4Occupant* picked = nullptr;
    if (view->PickOccupant(x, y, picked) && picked) {                  // vslot 0x94
        IMySimDispatchAutomaton* found = host->StreetAutomaton(picked);
        automaton = found;
        if (found) found->Release();   // the auto-ref holds it
        if (automaton && !automaton->CheckAutomataFlag(1) &&
            (picked->IsOccupantGroup(0x420b) || picked->IsOccupantGroup(0x4201) || picked->IsOccupantGroup(0x4215) ||
             picked->IsOccupantGroup(0x4001) || picked->IsOccupantGroup(0x4200)))
            hovered_ = automaton.get();
    }
    if (!automaton) {   // nothing (or no automaton) under the pointer: a person near the picked ground
        float position[3] = {0.0f, 0.0f, 0.0f};
        if (view->PickTerrain(x, y, position, true)) {                  // vslot 0x90
            const float min[3] = {position[0] - kPersonBoxHalfX, position[1] - kPersonBoxHalfY, position[2] - kPersonBoxHalfZ};
            const float max[3] = {position[0] + kPersonBoxHalfX, position[1] + kPersonBoxHalfY, position[2] + kPersonBoxHalfZ};
            std::vector<cISC4Occupant*> occupants;
            host->OccupantsInBox(min, max, occupants);
            for (cISC4Occupant* occupant : occupants) {
                IMySimDispatchAutomaton* found = occupant ? host->StreetAutomaton(occupant) : nullptr;
                automaton = found;
                if (found) found->Release();
                if (automaton && !automaton->CheckAutomataFlag(1) && occupant->IsOccupantGroup(0x4001)) {
                    hovered_ = automaton.get();
                    break;
                }
            }
        }
    }
    SetCursor(hovered_ ? kMyStreetSimCursorPick : kMyStreetSimCursor);   // vslot 0x24
    return true;
}

}  // namespace sc4
