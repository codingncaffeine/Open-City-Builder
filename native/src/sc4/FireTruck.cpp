// cSC4FireTruck — see sc4/FireTruck.h.
#include "sc4/FireTruck.h"

#include <cfloat>
#include <cmath>

#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
void Assign(cIGZUnknown*& slot, cIGZUnknown* value) {
    cIGZUnknown* old = slot;
    if (old == value) return;
    if (value) value->AddRef();
    slot = value;
    if (old) old->Release();
}
}  // namespace

// 0x10050f670: the two filters are built by the host (the filter classes are not recovered) and held by the truck.
cSC4FireTruck::cSC4FireTruck(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost, IVehicleHost* vehicleHost,
                             IEmergencyVehicleHost* emergencyHost, IFireTruckHost* fireHost, const cGZPersistResourceKey& modelKey)
    : cSC4EmergencyVehicle(host, automatonHost, vehicleHost, emergencyHost, modelKey), fireHost_(fireHost) {
    if (fireHost_) {
        Assign(fireFilter_, fireHost_->NewFireFilter());
        Assign(spillFilter_, fireHost_->NewSpillFilter());
    }
}

cSC4FireTruck::~cSC4FireTruck() {
    Assign(crew_, nullptr);
    Assign(spillFilter_, nullptr);
    Assign(fireFilter_, nullptr);
}

void cSC4FireTruck::SetCrew(cIGZUnknown* crew) { Assign(crew_, crew); }

// 0x10050f926: the crew and both filters go before the emergency vehicle shuts down.
bool cSC4FireTruck::Shutdown() {
    ReleaseFireCrew();
    Assign(fireFilter_, nullptr);
    Assign(spillFilter_, nullptr);
    return cSC4EmergencyVehicle::Shutdown();
}

// 0x10050f97c.
void cSC4FireTruck::ReleaseFireCrew() {
    if (!crew_) return;
    if (fireHost_) fireHost_->RemoveFireCrew(crew_);
    SetCrew(nullptr);
}

// 0x10051019c: the reach of the station that sent the truck, or 16 cells when unknown.
float cSC4FireTruck::GetEffectiveRadius() const {
    float radius = 0.0f;
    if (dispatchSource_ != nullptr && fireHost_ && fireHost_->StationRadius(dispatchSource_, radius)) return radius;
    return kFireTruckDefaultRadius;
}

// 0x10050fd50: the fires burning in the city, and the fire occupants in a square of the truck's reach around it.
void cSC4FireTruck::GetFireCount(int& citywide, int& nearby) {
    citywide = fireHost_ ? fireHost_->FireCount() : 0;
    nearby = 0;
    const float radius = GetEffectiveRadius();
    float position[3] = {0.0f, 0.0f, 0.0f};
    GetPosition(position);
    const float min[3] = {position[0] - radius, -FLT_MAX, position[2] - radius};
    const float max[3] = {radius + position[0], FLT_MAX, radius + position[2]};
    int count = 0;
    if (fireHost_ && fireHost_->OccupantsInBox(min, max, fireFilter_, count)) nearby = count;
}

// 0x10050ff3c: the toxic spills in the city, and those inside a square of cells around the truck (radius * 3 / 16 cells).
void cSC4FireTruck::GetToxicSpillCount(int& citywide, int& nearby) {
    citywide = 0;
    nearby = 0;
    if (!fireHost_) return;
    citywide = fireHost_->SpillTotal();
    int cellX = 0, cellZ = 0;
    if (emergencyHost_) emergencyHost_->PositionToCell(position_.x, position_.z, cellX, cellZ);
    const int reach = static_cast<int>(GetEffectiveRadius() * 3.0f * 0.0625f);
    const int rect[4] = {cellX - reach, cellZ - reach, cellX + reach, cellZ + reach};
    nearby = fireHost_->SpillsInRect(rect);
}

// 0x100510060: one crew per truck, competence from the station that sent it (1.0 when unknown).
void cSC4FireTruck::CreateFireCrew() {
    if (crew_ != nullptr || !fireHost_) return;
    SetCrew(fireHost_->NewFireCrew(*this));
    float competence = 1.0f;
    if (dispatchSource_ != nullptr) {
        float value = 0.0f;
        if (fireHost_->FireCompetence(dispatchSource_, value)) competence = value;
    }
    fireHost_->SetFireCrewCompetence(crew_, competence);
    fireHost_->RegisterFireCrew(crew_);
    fireHost_->SetupFireCrew(crew_);
}

// 0x10050fbb2: a truck the player is driving keeps its crew.
void cSC4FireTruck::HandleCrewFinished() {
    if (GetBehavior() == kStatePlayerDrive) return;
    ReleaseFireCrew();
    SetDispatchState(2);
}

// 0x10050fbf2. At the call (5) the crew comes only when there is something burning or spilled within reach, or the player is driving;
// a blocked call (1) brings the crew unconditionally (without a competence).
void cSC4FireTruck::SetDispatchState(std::uint32_t state) {
    cSC4EmergencyVehicle::SetDispatchState(state);
    if (FindState(kStatePlayerDrive) != nullptr) return;
    const std::uint32_t now = GetDispatchState();
    if (now == 1) {
        ReleaseFireCrew();
        if (!fireHost_) return;
        SetCrew(fireHost_->NewFireCrew(*this));
        fireHost_->RegisterFireCrew(crew_);
        fireHost_->SetupFireCrew(crew_);
        return;
    }
    if (now != 5) { ReleaseFireCrew(); return; }
    ReleaseFireCrew();
    int fireCity = 0, fireNear = 0, spillCity = 0, spillNear = 0;
    GetFireCount(fireCity, fireNear);
    GetToxicSpillCount(spillCity, spillNear);
    if (fireNear < 1 && spillNear < 1 && GetBehavior() != kStatePlayerDrive) return;
    CreateFireCrew();
}

// 0x1005101fc.
bool cSC4FireTruck::DoHandlingEventTick(float milliseconds) {
    (void)milliseconds;
    int fireCity = 0, fireNear = 0, spillCity = 0, spillNear = 0;
    GetFireCount(fireCity, fireNear);
    GetToxicSpillCount(spillCity, spillNear);
    if (GetDispatchState() == 2 && crew_ == nullptr && (fireNear > 0 || spillNear > 0)) {
        SetDispatchState(5);
        return true;
    }
    if ((spillNear | fireNear) == 0 && GetDispatchState() == 5 && GetBehavior() != kStatePlayerDrive) {
        SetDispatchState(2);
        return true;
    }
    if (GetDispatchState() == 2 && (spillCity | fireCity) == 0) ReturnToSourceOccupant();
    return true;
}

// 0x1005102c2. The stop key (8) with the engine stopped, and the fire key (7) pressed with the engine stopped (read through the speed
// tween), start the crew; the fire key with a crew already out sends the truck home. Keys 0 and 1 drop the crew first.
bool cSC4FireTruck::HandlePlayerDriveCommand(std::uint32_t command, bool on) {
    if (command < 2 && crew_ != nullptr) ReleaseFireCrew();
    bool startCrew = false;
    if (command == kFireTruckCommandStop) {
        if (crew_ == nullptr && GetActualSpeed() < kFireStopSpeed) startCrew = true;
    } else if (command == kFireTruckCommandFire && on) {
        if (crew_ != nullptr) {
            dispatchState_ = 2;
            SendCallbackMessage(7, GetDispatchState());
            ReleaseFireCrew();
        } else {
            const float speed = GetCurrentSpeed();
            const float magnitude = speed < 0.0f ? -speed : speed;
            if (kFireStopSpeed > magnitude) startCrew = true;
        }
    }
    if (startCrew) {
        dispatchState_ = 5;
        SendCallbackMessage(7, GetDispatchState());
        CreateFireCrew();
    }
    return cSC4EmergencyVehicle::HandlePlayerDriveCommand(command, on);
}

// 0x100510444: the player letting go of the wheel sends the crew home.
bool cSC4FireTruck::HandleExitState(const tAutomataState* state) {
    if (state != nullptr && state->stateType == kStatePlayerDrive) ReleaseFireCrew();
    return cSC4EmergencyVehicle::HandleExitState(state);
}

// 0x100510680: the point the water stream aims at while the player drives.
bool cSC4FireTruck::GetStreamTargetPoint(cS3DVector3& out) {
    auto* drive = static_cast<tAutomataStatePlayerDrive*>(FindState(kStatePlayerDrive));
    if (drive == nullptr || drive->history[8] < 0.0f) return false;
    out = drive->effectPosition248;
    return true;
}

// 0x10050f9ec. A truck in emergency mode (0x80) that is following a route (0x40), not ignoring paths, has an empty route and a
// lookahead, and is close enough to its call, steers the last lookahead tile's path to the call point and drives there in a straight line.
// Everything else chooses like a normal automaton.
bool cSC4FireTruck::ChooseNewDestination() {
    if (CheckAutomataFlag(0x80) && CheckAutomataFlag(0x40) && !CheckAutomataFlag(0x2000) && networkOccupant_ != nullptr &&
        route_.empty() && !lookahead_.empty() && CloseEnoughToDispatchDestination()) {
        ITrafficAutomatonOccupant* last = lookahead_.back().occupant.get();
        if (last) last->AddRef();
        cS3DVector3 target{};
        cISC4PathInfo* pathInfo = last->GetPathInfoObject();
        if (pathInfo != nullptr && pathInfo->GetDispatchDestination(1, GetDispatchDestination(), target, nullptr)) {
            SetPath(last, GetPathKey(), nullptr, true);
            path_ = nullptr;
            SetAutomataFlag(0x2000, true);
            SetDestination(&target.x);
            SetAutomataFlag(4, false);
        }
        last->Release();
        return true;
    }
    return cSC4TrafficAutomaton::ChooseNewDestination();
}

// 0x100510474: the aim point of the water stream follows the cursor on the ground, kept inside the city and no farther from the truck
// than the tuning allows (the length is sqrt(dz*dz + (dy*dy + dx*dx)): the disassembly's grouping).
bool cSC4FireTruck::TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) {
    if (!(state->history[8] < 0.0f) && fireHost_) {
        cS3DVector3 hit{0.0f, 0.0f, 0.0f};
        if (fireHost_->PickGroundPoint(static_cast<float>(state->mouseX214), static_cast<float>(state->mouseY218), hit)) {
            int width = 0, height = 0;
            if (fireHost_->CityCells(width, height)) {
                const float limitX = static_cast<float>(width << 4), limitZ = static_cast<float>(height << 4);
                if (0.0f > hit.x) hit.x = 0.0f;
                else if (!(hit.x < limitX) && hit.x == hit.x) hit.x = limitX + -1.0f;
                if (hit.z < 0.0f) hit.z = 0.0f;
                else if (hit.z >= limitZ) hit.z = limitZ + -1.0f;
            }
            const float dx = hit.x - position_.x, dy = hit.y - position_.y, dz = hit.z - position_.z;
            const float length = std::sqrt(dz * dz + (dy * dy + dx * dx));
            const float reach = fireHost_->MaxStreamDistance();
            if (length > reach) {
                const float scale = 1.0f / length;
                hit.x = position_.x + dx * scale * reach;
                hit.y = position_.y + dy * scale * reach;
                hit.z = position_.z + dz * scale * reach;
            }
            state->effectPosition248 = hit;
        }
    }
    return cSC4Vehicle::TranslatePlayerDriveCommands(state, milliseconds);
}

// 0x1005106d4: version 2, the emergency vehicle's record, the crew.
bool cSC4FireTruck::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    const bool ok = cSC4EmergencyVehicle::Write(stream);
    if (ok) SC4WriteGZSerializable(stream, crew_);
    return ok;
}

// 0x10051074c.
bool cSC4FireTruck::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2 || !cSC4EmergencyVehicle::Read(stream)) return false;
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, kAutomatonIid, &raw, false);
    Assign(crew_, static_cast<cIGZUnknown*>(raw));
    return true;
}

}  // namespace sc4
