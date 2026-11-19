// cSC4EmergencyVehicle — see sc4/EmergencyVehicle.h for what is and is not recovered.
#include "sc4/EmergencyVehicle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"

namespace sc4 {

cSC4EmergencyVehicle::cSC4EmergencyVehicle(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost,
                                           IVehicleHost* vehicleHost, IEmergencyVehicleHost* emergencyHost,
                                           const cGZPersistResourceKey& modelKey)
    : cSC4Vehicle(host, automatonHost, vehicleHost, modelKey), emergencyHost_(emergencyHost) {}

cSC4EmergencyVehicle::~cSC4EmergencyVehicle() {
    if (dispatchSource_) { cIGZUnknown* old = dispatchSource_; dispatchSource_ = nullptr; old->Release(); }
    if (lightModel_) { IAutomatonModel* old = lightModel_; lightModel_ = nullptr; old->Release(); }
    if (lightEffect_) { cIGZUnknown* old = lightEffect_; lightEffect_ = nullptr; old->Release(); }
}

void cSC4EmergencyVehicle::SetDispatchSource(cIGZUnknown* source) {
    if (dispatchSource_ == source) return;
    if (source) source->AddRef();
    cIGZUnknown* old = dispatchSource_;
    dispatchSource_ = source;
    if (old) old->Release();
}

// 0x1002f56dc: the dispatch source and the light model are released, the siren is silenced, then the vehicle shuts down.
bool cSC4EmergencyVehicle::Shutdown() {
    if (dispatchSource_) {
        cIGZUnknown* old = dispatchSource_;
        dispatchSource_ = nullptr;
        old->Release();
    }
    if (lightModel_) {
        IAutomatonModel* old = lightModel_;
        lightModel_ = nullptr;
        old->Release();
    }
    if (emergencyHost_) emergencyHost_->SirenStopped(*this);
    return cSC4Vehicle::Shutdown();
}

// 0x1002f7f02: version 2, the vehicle record, the destination, the dispatch source, repath counter, call number and state.
bool cSC4EmergencyVehicle::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    const bool ok = cSC4Vehicle::Write(stream);
    if (ok) {
        stream << dispatchDestination_;
        SC4WriteGZSerializable(stream, dispatchSource_);
        stream.SetUint8(repathCount_);
        stream.SetUint16(callNumber_);
        stream.SetUint32(dispatchState_);
    }
    return ok;
}

bool cSC4EmergencyVehicle::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (!cSC4Vehicle::Read(stream)) return false;
    stream >> dispatchDestination_;
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, kEmergencyDispatchedObjectIid, &raw, false);
    if (dispatchSource_) dispatchSource_->Release();            // (the reader stores straight into the slot)
    dispatchSource_ = static_cast<cIGZUnknown*>(raw);
    stream.GetUint8(repathCount_);
    stream.GetUint16(callNumber_);
    stream.GetUint32(dispatchState_);
    return true;
}

// 0x1002f576e: after the vehicle tick, an automaton in emergency mode that is not being removed clears the cars around its
// network occupant and keeps the siren positioned on it.
bool cSC4EmergencyVehicle::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    const bool result = cSC4Vehicle::DoAnimationTick(unknown, milliseconds);
    if (CheckAutomataFlag(0x80) && !GetMarkedForRemove()) {
        RemoveSurroundingAutomata(networkOccupant_, 0);
        if (emergencyHost_) {
            float position[3];
            GetPosition(position);
            emergencyHost_->SirenMoved(*this, static_cast<int>(position[0]), static_cast<int>(position[1]),
                                       static_cast<int>(position[2]));
        }
    }
    return result;
}

// 0x1002f78e6.
bool cSC4EmergencyVehicle::ShouldCollide(cSC4Automaton* other) const {
    if (!CheckAutomataFlag(0x80)) return cSC4Vehicle::ShouldCollide(other);
    if (this == other) return false;
    if (!other) return false;
    if (other->CheckAutomataFlag(0x80)) {
        if (!other->CheckAutomataFlag(0x20)) return true;
        if (route_.empty()) return lookahead_.size() < 3;
    }
    return false;
}

// 0x1002f57fc: the four emergency states have their own handlers; every other state runs the vehicle tick, and the tick
// reports "done" when it ends the state it started in.
bool cSC4EmergencyVehicle::DoStateTick(float milliseconds) {
    if (states_.empty()) return false;
    tAutomataState* state = GetState();
    if (!state) return false;
    switch (state->stateType) {
        case kEmergencyStateEnRoute: return DoEnRouteTick(milliseconds);
        case kEmergencyStateRepathing: return DoRepathingTick(milliseconds);
        case kEmergencyStateHandlingEvent: return DoHandlingEventTick(milliseconds);
        case kEmergencyStateTeleport: return DoTeleportTick(milliseconds);
        default:
            if (cSC4Vehicle::DoStateTick(milliseconds)) return true;
            return state != GetState();
    }
}

// 0x1002f58d4: drive (along the path, or straight when paths are ignored), pick the next destination on arrival, and keep the
// leader reference fresh while queueing.
bool cSC4EmergencyVehicle::DoEnRouteTick(float milliseconds) {
    if (!GetReachedDestination() && networkOccupant_ != nullptr) {
        if (!CheckAutomataFlag(0x2000)) cSC4Vehicle::DoPathTick(milliseconds);
        else DoStraightLineTick(milliseconds);
    }
    if (GetReachedDestination() && !GetMarkedForRemove() && !ChooseNewDestination()) {
        if (!GetSpecialRouteFlag()) Remove(false);
        else HandleReachedRouteDestination();
    }
    if (!ValidateNetworkOccupant()) {
        Remove(false);
    } else if (!route_.empty()) {
        if (CheckAutomataFlag(0x20) && leader_ != nullptr && leader_->CheckAutomataFlag(0x20)) {
            cSC4Automaton* leader = FindLeader();
            if (leader_ != leader) {
                if (leader) leader->AddRef();
                cSC4Automaton* old = leader_;
                leader_ = leader;
                if (old) old->Release();
            }
        }
    }
    return true;
}

bool cSC4EmergencyVehicle::DoPlayerDriveTick(float milliseconds) { return cSC4Vehicle::DoPlayerDriveTick(milliseconds); }

// 0x1002f5fa8: emergency automata (and ones in the pursuit state) do not attract anything; their attraction is dropped.
void cSC4EmergencyVehicle::UpdateAttraction() {
    if (!CheckAutomataFlag(0x80) && GetBehavior() != kStatePursuit) {
        cSC4Vehicle::UpdateAttraction();
        return;
    }
    if (attractionController_) {
        cSC4AutomataController* old = attractionController_;
        attractionController_ = nullptr;
        old->Release();
    }
}

// 0x1002f600c: the lights key (9) toggles the lights when it is released.
bool cSC4EmergencyVehicle::HandlePlayerDriveCommand(std::uint32_t command, bool on) {
    if (command == kEmergencyPlayerDriveLightsCommand && !on) {
        ShowEmergencyLights(lightModel_ == nullptr);
        return true;
    }
    return cSC4Vehicle::HandlePlayerDriveCommand(command, on);
}

// 0x1002f603c.
bool cSC4EmergencyVehicle::HandleEnterState(tAutomataState* state) {
    if (state) {
        const std::uint32_t id = state->stateType;
        if (id == kStatePlayerDrive) {
            cSC4Vehicle::HandleEnterState(state);
            dispatchState_ = 3;
            SendCallbackMessage(7, GetDispatchState());
            ShowEmergencyLights(false);
            if (emergencyHost_) emergencyHost_->SirenStopped(*this);
            return true;
        }
        if (id == kStatePursuit) ShowEmergencyLights(true);
        else if (id == kEmergencyStateTeleport) cSC4Automaton::SetOpacity(0.0f, cSC4TrafficAutomaton::Tuning().fadeDuration);
    }
    return cSC4Vehicle::HandleEnterState(state);
}

// 0x1002f611e: leaving the player-drive or pursuit state turns the lights off.
bool cSC4EmergencyVehicle::HandleExitState(const tAutomataState* state) {
    if (state && (state->stateType == kStatePlayerDrive || state->stateType == kStatePursuit)) ShowEmergencyLights(false);
    return cSC4TrafficAutomaton::HandleExitState(state);
}

// 0x1002f62e6: an emergency vehicle that loses its road counts as having reached its destination.
void cSC4EmergencyVehicle::HandleNetworkOccupantRemoved() {
    if (CheckAutomataFlag(0x80)) {
        HandleReachedRouteDestination();
        return;
    }
    cSC4TrafficAutomaton::HandleNetworkOccupantRemoved();
}

// 0x1002f5a26: a vehicle without a network occupant looks up the one under it and attaches to it.
bool cSC4EmergencyVehicle::ValidateNetworkOccupant() {
    if (networkOccupant_ == nullptr && emergencyHost_) {
        float position[3] = {0, 0, 0};
        GetPosition(position);
        SetPath(emergencyHost_->NetworkOccupantAt(position[0], position[2]), 0, nullptr, false);
    }
    return networkOccupant_ != nullptr;
}

// 0x1002f5c36 (the posted Data2 is the unsigned repath byte; the decompile's (char) cast is wrong).
void cSC4EmergencyVehicle::SendPathBlockedMessage() {
    if (emergencyHost_) emergencyHost_->PostPathBlocked(*this, GetDispatchType(), repathCount_);
}

// 0x1002f5ae2: look for a route to the call; found -> back to driving; not found -> retry a few times after a pause, then
// give up and leave the map.
bool cSC4EmergencyVehicle::DoRepathingTick(float) {
    if (ValidateNetworkOccupant() || repathCount_ == 0) {
        if (emergencyHost_ && emergencyHost_->FindRoute(*this, dispatchDestination_)) {
            SetDispatchState(3);
            PopState();
            repathCount_ = 0;
            return true;
        }
        ++repathCount_;
        if (emergencyHost_ && repathCount_ <= emergencyHost_->Tuning().maxRepaths) {
            SendPathBlockedMessage();
            PushState(7, nullptr, emergencyHost_->Tuning().retrySeconds * 1000.0f, false, 0, 0, nullptr);
            return true;
        }
    }
    Remove(false);
    return true;
}

// 0x1002f796c: the dispatch state drives the automaton's mode (3 = answering the call: emergency flags, lights and siren on;
// 6 = finished: back to normal; 1 = blocked: tell the dispatcher). The sound system hears the matching message.
void cSC4EmergencyVehicle::SetDispatchState(std::uint32_t state) {
    if (dispatchState_ == state) return;
    if (FindState(kStatePlayerDrive) != nullptr) return;
    dispatchState_ = state;
    SendCallbackMessage(7, GetDispatchState());
    switch (state) {
        case 1:
            if (emergencyHost_) emergencyHost_->SirenStopped(*this);
            SendPathBlockedMessage();
            return;
        case 3: {
            SetState(kEmergencyStateEnRoute, nullptr, -1.0f, false, 0, 0, nullptr);
            SetAutomataFlag(0x80, true);
            SetAutomataFlag(0x10000, true);
            SetAutomataFlag(0x800, true);
            SetAutomataFlag(0x1000, true);
            ShowEmergencyLights(true);
            std::uint32_t sound = 0x29de9021;
            if (!IsOccupantGroup(0x4228)) sound = IsOccupantGroup(0x4202) ? 0x9dc5728 : 0;
            if (emergencyHost_ && sound != 0) emergencyHost_->SirenStarted(*this, sound);
            return;
        }
        case 6:
            SetState(0, nullptr, -1.0f, false, 0, 0, nullptr);
            SetAutomataFlag(0x80, false);
            SetAutomataFlag(0x10000, true);
            SetAutomataFlag(0x800, false);
            SetAutomataFlag(0x1000, false);
            ShowEmergencyLights(false);
            [[fallthrough]];
        case 0:
        case 2:
        case 4:
        case 5:
            if (emergencyHost_) emergencyHost_->SirenStopped(*this);
            return;
        default:
            return;
    }
}

// 0x1002f563c: an emergency-mode vehicle starts with its lights on and the emergency speed (km/h -> units: * 1/3600).
bool cSC4EmergencyVehicle::Init() {
    if (IsInitialized()) return true;
    if (!cSC4Vehicle::Init()) return false;
    if (CheckAutomataFlag(0x80)) {
        ShowEmergencyLights(true);
        const std::uint32_t kmh = emergencyHost_ ? emergencyHost_->Tuning().speedKmh : 0;
        defaultSpeed_ = static_cast<float>(static_cast<std::uint64_t>(kmh)) * 0.0002777778f;
        SetSpeed(defaultSpeed_, 1.0f);
    }
    return true;
}

// 0x1002f7bf8 (the distance is sqrt(dz*dz + (dy*dy + dx*dx)): the disassembly's grouping). Close enough = within
// teleportCells cells of the call and no tunnel / bridge tile (network flags 0x304) under the vehicle.
bool cSC4EmergencyVehicle::CloseEnoughToDispatchDestination() const {
    const cS3DVector3& destination = dispatchDestination_;
    float position[3];
    const_cast<cSC4EmergencyVehicle*>(this)->GetPosition(position);
    const float dy = destination.y - position[1];
    const float dx = destination.x - position[0];
    const float dz = destination.z - position[2];
    const float distance = std::sqrt(dz * dz + (dy * dy + dx * dx));
    const std::uint32_t limit = emergencyHost_ ? emergencyHost_->Tuning().teleportCells : 0;
    static const bool trace = std::getenv("SC4_DISPATCHDEBUG") != nullptr;
    if (trace)
        std::fprintf(stderr, "emergency: route end at (%.0f, %.0f), call at (%.0f, %.0f), distance %.0f, close enough within %u cells\n",
                     position[0], position[2], destination.x, destination.z, distance, limit);
    if (static_cast<float>(limit << 4) < distance || std::isnan(distance)) return false;
    if (!emergencyHost_ || !trafficHost_) return true;
    int cellX = 0, cellZ = 0;
    emergencyHost_->PositionToCell(position[0], position[2], cellX, cellZ);
    cSC4TrafficNetworkMap* map = trafficHost_->TrafficNetworkMap();
    if (!map) return true;
    for (TrafficTileInfo* tile = map->GetTileInfoAtCell(cellX, cellZ); tile != nullptr; tile = tile->next)
        if (tile->occupant && tile->occupant->HasAnyNetworkFlag(0x304)) return false;
    return true;
}

// 0x1002f615a: reaching the end of the route as an emergency car. Answering a call (state 3): near enough to the call hands
// over to the event handler; too far reports the path blocked and repaths after a pause. A blocked call (1) waits.
void cSC4EmergencyVehicle::HandleReachedRouteDestination() {
    if (CheckAutomataFlag(0x80)) {
        if (dispatchState_ == 1) return;
        if (dispatchState_ == 3) {
            SendCallbackMessage(5, 0);
            if (CloseEnoughToDispatchDestination()) {
                SetAutomataFlag(4, false);
                repathCount_ = 0;
                SetOpacity(1.0f, 0.0f);
                SetState(kEmergencyStateHandlingEvent, nullptr, -1.0f, false, 0, 0, nullptr);
                SetAutomataFlag(0x4000, true);
                SetDispatchState(5);
            } else {
                SetDispatchState(1);
                repathCount_ = 0;
                PushState(kEmergencyStateRepathing, nullptr, -1.0f, false, 0, 0, nullptr);
                PushState(7, nullptr, (emergencyHost_ ? emergencyHost_->Tuning().retrySeconds : 0.0f) * 1000.0f, false, 0, 0, nullptr);
            }
            return;
        }
    }
    cSC4Automaton::HandleReachedRouteDestination();
}

// 0x1002f6322: a new call. The vehicle remembers the destination and who sent it, leaves the "arrived" look (flag 0x400) and, when
// its last routed cell is a road, extends the route towards the call.
void cSC4EmergencyVehicle::HandleDispatched(const cS3DVector3& position, cISC4Occupant* occupant) {
    cSC4Automaton::HandleDispatched(position, occupant);
    dispatchDestination_ = position;
    if (CheckAutomataFlag(0x400)) {
        SetAutomataFlag(0x400, false);
        SetOpacity(1.0f, 0.0f);
    }
    if (occupant != nullptr) {
        if (dispatchSource_) dispatchSource_->Release();
        dispatchSource_ = occupant;
        occupant->AddRef();
    }
    if (route_.empty() || !trafficHost_) return;
    cSC4TrafficNetworkMap* map = trafficHost_->TrafficNetworkMap();
    if (!map) return;
    const RouteData last = route_.back();
    for (TrafficTileInfo* tile = map->GetTileInfoAtCell(last.x, last.z); tile != nullptr; tile = tile->next) {
        if (tile->occupant && tile->occupant->HasAnyNetworkFlag(0x304)) {
            AppendRouteToNearbyRoad(last.x, last.z, position);
            return;
        }
    }
}

// i386 0x0023622c: the path finder ends a call's route on the road cell before the call's own cell. When that last cell is a
// road, the route is extended to the usable road piece nearest to the call (within teleportCells cells of the call's cell)
// that the path finder reaches from the last cell. (The original releases neither the path finder nor the road pieces.)
void cSC4EmergencyVehicle::AppendRouteToNearbyRoad(int cellX, int cellZ, const cS3DVector3& destination) {
    if (!emergencyHost_ || !trafficHost_) return;
    cSC4TrafficNetworkMap* map = trafficHost_->TrafficNetworkMap();
    if (!map) return;
    int callX = 0, callZ = 0;
    emergencyHost_->PositionToCell(destination.x, destination.z, callX, callZ);   // spCity vslot 0x17c
    std::vector<ITrafficNetworkMapOccupant*> roads;
    if (!map->GetSurroundingRoads(roads, SC4RectInt32{callX, callZ, callX, callZ}, emergencyHost_->Tuning().teleportCells, 0, 0x1c49))
        return;
    if (cISC4PathFinder* finder = emergencyHost_->CreatePathFinder()) {
        std::deque<RouteData> route, best;
        finder->SetStartRect({cellX, cellZ, cellX, cellZ});
        finder->SetTravelTypeFlag(2, true);
        finder->SetTrafficDensity(0);
        finder->SetFlag(1);
        finder->SetFlag(2);
        finder->SetTripDestinationFilter(3);
        finder->SetRouteInfo(&route);
        const float call[2] = {static_cast<float>(callX), static_cast<float>(callZ)};
        float nearest = 3.4028235e+38f;
        bool found = false;
        for (ITrafficNetworkMapOccupant* road : roads) {
            std::uint32_t x = 0, z = 0;
            road->GetOccupiedCell(x, z);
            const float cell[2] = {static_cast<float>(x), static_cast<float>(z)};
            const float distance = SC4GeometryUtilities::LengthOfLineSquared2D(call, cell);
            if (distance >= nearest) continue;
            route.clear();
            finder->SetEndRect({static_cast<std::int32_t>(x), static_cast<std::int32_t>(z), static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)});
            if (finder->FindPath() == 1) {
                best = route;
                found = true;
                nearest = distance;
            }
        }
        static const bool trace = std::getenv("SC4_DISPATCHDEBUG") != nullptr;
        if (trace)
            std::fprintf(stderr, "emergency: route from %d,%d towards the call at %d,%d: %zu roads, %s %zu cells\n", cellX, cellZ, callX, callZ,
                         roads.size(), found ? "appended" : "none", best.size());
        if (found) route_.insert(route_.end(), best.begin(), best.end());
        finder->Release();
    }
    for (ITrafficNetworkMapOccupant* road : roads) road->Release();
}

// 0x1002f7670: tell the dispatcher the call is over and drive home (unless a player is driving).
void cSC4EmergencyVehicle::ReturnToSourceOccupant() {
    if (GetDispatchState() == 6) return;
    if (FindState(kStatePlayerDrive) != nullptr) return;
    SetDispatchState(6);
    PushState(kEmergencyStateTeleport, nullptr, -1.0f, false, 0, 0, nullptr);
}

// 0x1002f5d08: the "go home" state. While the vehicle is mostly faded out (opacity <= 0.6) and still here it fades back in,
// and, when its dispatch source is farther than the teleport distance, asks for a route to it; found -> drive home, not found
// or too close -> leave the map.
bool cSC4EmergencyVehicle::DoTeleportTick(float) {
    const float opacity = GetOpacity();
    if (opacity <= 0.6f && !GetMarkedForRemove()) {            // (a NaN opacity skips the handler, like the flags of ucomiss)
        SetOpacity(1.0f, cSC4TrafficAutomaton::Tuning().fadeDuration);
        bool leave = true;
        if (dispatchSource_) {
            float source[3] = {0, 0, 0};
            static_cast<cISC4Occupant*>(dispatchSource_)->GetPosition(source);
            float here[3] = {0, 0, 0};
            GetPosition(here);
            const float dx = here[0] - source[0];
            const float dy = here[1] - source[1];
            const float dz = here[2] - source[2];
            const float distance = std::sqrt(dz * dz + (dx * dx + dy * dy));
            const std::uint32_t limit = emergencyHost_ ? emergencyHost_->Tuning().teleportCells : 0;
            if (distance >= static_cast<float>(limit << 4)) {
                SetDispatchState(6);
                const cS3DVector3 position{here[0], here[1], here[2]};
                if (emergencyHost_ && emergencyHost_->FindReturnRoute(*this, position, dispatchSource_)) {
                    repathCount_ = 0;
                    SetState(0, nullptr, -1.0f, false, 0, 0, nullptr);
                    leave = false;
                }
            }
        }
        if (leave) Remove(false);
    }
    return true;
}

void cSC4EmergencyVehicle::ShowEmergencyLights(bool on) {
    if ((lightModel_ != nullptr) != on && emergencyHost_) emergencyHost_->SetEmergencyLights(*this, on);
}

}  // namespace sc4
