#include "sc4/Train.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rz/cRZRandom.h"
#include "s3d/S3DMath.h"
#include "sc4/AutomataStates.h"
#include "sc4/DBSegment.h"
#include "sc4/PathInfo.h"

namespace sc4 {

namespace {

constexpr float kOrientationEpsilon = 1e-05f;   // cSC4TrafficAutomaton::kfAutomatonOrientationEpsilon (0x00a70830)

cS3DVector3 Vec(float x, float y, float z) {
    cS3DVector3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}
cS3DVector3 Sub(const cS3DVector3& a, const cS3DVector3& b) { return Vec(a.x - b.x, a.y - b.y, a.z - b.z); }
float Length3(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// cSC4Automaton::kUninit3DVector: the vehicle's "never set" path position and previous heading.
bool IsUnset(const cS3DVector3& v) {
    float y = 0.0f, z = 0.0f;
    const std::uint32_t yBits = 0x01ffffffu, zBits = 0x4f5f4090u;
    std::memcpy(&y, &yBits, sizeof(y));
    std::memcpy(&z, &zBits, sizeof(z));
    return std::fabs(v.x - kAutomatonUnsetCoordinate) < 0.0001f && std::fabs(v.y - y) < 0.0001f && std::fabs(v.z - z) < 0.0001f;
}

}  // namespace

// ---- cSC4TrainCar --------------------------------------------------------------------------------------------------------

// 0x003942de: the vehicle, on the ground, with a default speed rolled in the trains' range (whole units per hour).
cSC4TrainCar::cSC4TrainCar(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost, IVehicleHost* vehicleHost,
                           const cGZPersistResourceKey& modelKey)
    : cSC4Vehicle(host, automatonHost, vehicleHost, modelKey) {
    const VehicleTuning& tuning = Tuning();
    float speed = 0.0f;
    if (IAutomatonHost* random = Host())
        speed = static_cast<float>(random->RandomInt(static_cast<std::int32_t>(tuning.trainSpeedMin), static_cast<std::int32_t>(tuning.trainSpeedMax))) *
                kAutomatonSpeedScale;
    SetDefaultSpeed(speed);
    SetSpeed(speed, 0.0f);
}

// 0x003930be: a train car never collides on its own, ignores attractors and counts as pulled; the two elevated groups
// fade with the track.
bool cSC4TrainCar::Init() {
    if (IsInitialized()) return true;
    const bool result = cSC4Vehicle::Init();
    SetAutomataFlag(kAutomataFlagNoCollision, true);
    SetAutomataFlag(kAutomataFlagIgnoreAttractors, true);
    SetAutomataFlag(kAutomataFlagPulled, true);
    elevated_ = IsOccupantGroup(kTrainGroupElevatedEngine) || IsOccupantGroup(kTrainGroupElevatedCar);
    return result;
}

// 0x003931fc
bool cSC4TrainCar::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kTrainCarGZCLSID) {
        *out = static_cast<cSC4TrainCar*>(this);
        AddRef();
        return true;
    }
    return cSC4Vehicle::QueryInterface(iid, out);
}

bool cSC4TrainCar::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    return cSC4Vehicle::Write(stream);
}

bool cSC4TrainCar::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 2 && cSC4Vehicle::Read(stream);
}

// 0x0039369c: the engine at the head of the chain of leaders; none for a car nobody leads.
cSC4Automaton* cSC4TrainCar::FindDrivingAutomaton() { return leader_ ? leader_->FindDrivingAutomaton() : nullptr; }

// 0x00393242: a car that is hit hands the collision to its engine, unless it is wrecked already.
bool cSC4TrainCar::HandleCollision(cSC4Automaton* other, const cS3DVector3& point) {
    if (GetBehavior() != kStateCollide) {
        if (cSC4Automaton* driver = FindDrivingAutomaton()) driver->HandleCollision(other, point);
    }
    return true;
}

// 0x003936c2: an engine entering the collide state wrecks its whole train. (The accident object of the state is also told
// which occupants crashed; see the header note.)
bool cSC4TrainCar::HandleEnterState(tAutomataState* state) {
    const bool result = cSC4Vehicle::HandleEnterState(state);
    if (state && state->stateType == kStateCollide && static_cast<tAutomataStateCollide*>(state)->attachment) {
        if (IsEngine()) HandleCollision(nullptr, Vec(0.0f, 0.0f, 0.0f));
    }
    return result;
}

// 0x003931a0: while a disaster is running a wreck stays for at least ten more seconds.
bool cSC4TrainCar::DoCollideTick(float milliseconds) {
    if (vehicleHost_ && vehicleHost_->DisasterOngoing()) {
        tAutomataState* state = GetState();
        if (state && kTrainCarWreckMilliseconds > state->timer) state->timer = kTrainCarWreckMilliseconds;
    }
    return cSC4Vehicle::DoCollideTick(milliseconds);
}

// 0x003938de: an elevated car on an underground piece shows only where the track comes up on both sides (zipper piece 2).
void cSC4TrainCar::SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey, const std::vector<cS3DVector3>* path,
                           bool keepDestination) {
    cSC4Vehicle::SetPath(occupant, pathKey, path, keepDestination);
    if (!elevated_ || !networkOccupant_ || !vehicleHost_) return;
    const int piece = vehicleHost_->ZipperPieceType(networkOccupant_);
    if (piece < 0) return;
    if (piece == 2) SetOpacity(1.0f, 0.1f);
    else SetOpacity(0.0f, 0.6f);
}

// ---- cSC4Train -----------------------------------------------------------------------------------------------------------

cSC4Train::cSC4Train(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost, IVehicleHost* vehicleHost,
                     const cGZPersistResourceKey& modelKey)
    : cSC4TrainCar(host, automatonHost, vehicleHost, modelKey) {}

// 0x0074c56c: the appear place's occupant goes; the cars were released by Shutdown.
cSC4Train::~cSC4Train() {
    if (tailOccupant_) tailOccupant_->Release();
}

// 0x003940b2: the train car's Init, then the engine drives itself again (the pulled flag comes off), rolls its speed,
// names the group of its cars from its own group and rolls how many it pulls.
bool cSC4Train::Init() {
    if (IsInitialized()) return true;
    const bool result = cSC4TrainCar::Init();
    SetAutomataFlag(kAutomataFlagPulled, false);
    const VehicleTuning& tuning = Tuning();
    IAutomatonHost* random = Host();
    const float speed = random ? static_cast<float>(random->RandomDouble(static_cast<double>(static_cast<float>(tuning.trainSpeedMin) * kAutomatonSpeedScale),
                                                                         static_cast<double>(static_cast<float>(tuning.trainSpeedMax) * kAutomatonSpeedScale)))
                               : 0.0f;
    SetDefaultSpeed(speed);
    SetSpeed(speed, 0.0f);
    if (IsOccupantGroup(kTrainGroupFreightEngine)) carGroup_ = kTrainGroupFreightCar;
    else if (IsOccupantGroup(kTrainGroupPassengerEngine)) carGroup_ = kTrainGroupPassengerCar;
    else if (IsOccupantGroup(kTrainGroupElevatedEngine)) carGroup_ = kTrainGroupElevatedCar;
    else if (IsOccupantGroup(kTrainGroupMonorailEngine)) carGroup_ = kTrainGroupMonorailCar;
    else if (IsOccupantGroup(kTrainGroupGroup422e)) carGroup_ = kTrainGroupGroup422f;
    if (carGroup_ != 0) {
        const bool freight = carGroup_ == kTrainGroupFreightCar;
        const std::uint32_t lo = freight ? tuning.freightCarsMin : tuning.passengerCarsMin;
        const std::uint32_t hi = freight ? tuning.freightCarsMax : tuning.passengerCarsMax;
        const std::uint32_t rolled = random ? random->RandomBelow(hi - lo) : 0;
        SetDesiredNumCars(static_cast<std::uint8_t>(lo + rolled));
    }
    derail_ = 0.0f;
    flag304_ = false;
    return result;
}

// 0x00393e62: the cells the train claimed (its lookahead tiles and the tiles behind it) are given back, the vehicle shuts
// down, and every car is removed without a fade and released.
bool cSC4Train::Shutdown() {
    for (const tLookaheadNode& node : lookahead_) MarkTrainInCell(node, false);
    for (const tLookaheadNode& node : pathHistory_) MarkTrainInCell(node, false);
    const bool result = cSC4Vehicle::Shutdown();
    if (tailOccupant_) {
        ITrafficAutomatonOccupant* old = tailOccupant_;
        tailOccupant_ = nullptr;
        old->Release();
    }
    for (cSC4TrainCar* car : cars_) {
        if (!car) continue;
        car->Remove(false);
        car->Release();
    }
    cars_.clear();
    return result;
}

// 0x003933a0
bool cSC4Train::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kTrainGZCLSID) {
        *out = static_cast<cSC4Train*>(this);
        AddRef();
        return true;
    }
    return cSC4TrainCar::QueryInterface(iid, out);
}

// 0x00393c3e
bool cSC4Train::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    const bool ok = cSC4TrainCar::Write(stream);
    if (ok) {
        stream.SetUint32(static_cast<std::uint32_t>(cars_.size()));
        for (const cSC4TrainCar* car : cars_) SC4WriteGZSerializable(stream, static_cast<const cIGZSerializable*>(car));
        stream.SetUint32(desiredCars_);
        stream << tailPos_;
        SC4WriteGZSerializable(stream, tailOccupant_);
        stream.SetUint32(tailPathKey_);
    }
    return ok;
}

// 0x00393d30: version 2 only; null car refs are dropped, the cars arrive counted.
bool cSC4Train::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (!cSC4TrainCar::Read(stream)) return false;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kTrainCarGZCLSID, &raw, false);
        if (raw) cars_.push_back(static_cast<cSC4TrainCar*>(raw));
    }
    stream.GetUint32(desiredCars_);
    stream >> tailPos_;
    if (tailOccupant_) {
        ITrafficAutomatonOccupant* old = tailOccupant_;
        tailOccupant_ = nullptr;
        old->Release();
    }
    if (trafficHost_) {
        tailOccupant_ = trafficHost_->ReadNetworkOccupant(stream);
    } else {
        void* raw = nullptr;   // no host: the reference is still consumed
        SC4ReadGZSerializable(stream, 0xa821ef94, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    }
    stream.GetUint32(tailPathKey_);
    return true;
}

// 0x00393a4a: the engine and every car go into the collide state for twenty seconds; each car takes the engine's speed
// and is knocked a quarter of a unit sideways off its heading. A car that is wrecked already ends the walk at once.
bool cSC4Train::HandleCollision(cSC4Automaton* other, const cS3DVector3& point) {
    (void)other;
    (void)point;
    bool result = GetBehavior() == kStateCollide || PushState(kStateCollide, nullptr, kTrainCollideMilliseconds, true, 0, 0, nullptr);
    cRZRandom random(0xffffffffu);
    for (cSC4TrainCar* car : cars_) {
        if (!car) continue;   // port guard
        car->actualSpeed_ = actualSpeed_;
        cS3DVector3 knock = nS3D::RandomDirection3(random);
        const float length = Length3(knock);
        if (length > 0.0f) knock = Vec(knock.x * 0.25f / length, 0.0f, knock.z * 0.25f / length);
        knock.y = 0.0f;
        car->orientation_.x += knock.x;
        car->orientation_.y += 0.0f;
        car->orientation_.z += knock.z;
        nS3D::Normalize(car->orientation_);
        if (car->GetBehavior() == kStateCollide) return true;
        if (!car->PushState(kStateCollide, nullptr, kTrainCollideMilliseconds, true, 0, 0, nullptr)) result = false;
    }
    SetSpeed(0.0f, 0.0f);
    return result;
}

// 0x00394646: the vehicle ticks; the appear place is taken from the tail the first time; a missing car is added; the cars
// follow unless the train is wrecked. A train that has stood still for ten seconds leaves on a coin toss each tick.
bool cSC4Train::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    const bool result = cSC4Vehicle::DoAnimationTick(unknown, milliseconds);
    if (tailPos_.x < 0.0f || tailPos_.z < 0.0f) {
        cSC4TrainCar* tail = GetTailCar();
        float xyz[3];
        tail->GetPosition(xyz);
        tailPos_ = Vec(xyz[0], xyz[1], xyz[2]);
        ITrafficAutomatonOccupant* occupant = tail->GetNetworkOccupant();
        if (occupant) occupant->AddRef();
        if (tailOccupant_) tailOccupant_->Release();
        tailOccupant_ = occupant;
        tailPathKey_ = tail->GetPathKey();
    }
    if (cars_.size() < desiredCars_) AddCarAtEnd();
    if (GetBehavior() != kStateCollide) TickFollowingCars(milliseconds);
    if (!CheckAutomataFlag(kTrafficFlagStopped)) {
        standStill_ = 0.0f;
    } else if (0.01f < GetSpeedModifier()) {
        standStill_ = milliseconds * 0.001f + standStill_;
        if (kTrainStandStillSeconds <= standStill_ && Host() && Host()->RandomBelow(2) == 0) Remove(false);
    }
    UpdateGhostModel();
    return result;
}

// 0x003933dc: never collides with its own cars.
bool cSC4Train::ShouldCollide(cSC4Automaton* other) const {
    if (std::find(cars_.begin(), cars_.end(), other) != cars_.end()) return false;
    return cSC4Automaton::ShouldCollide(other);
}

// 0x0039499e: the cars enter the player-drive and the station states with their engine.
bool cSC4Train::HandleEnterState(tAutomataState* state) {
    const bool result = cSC4TrainCar::HandleEnterState(state);
    if (!state) return result;
    const std::uint32_t type = state->stateType;
    if (type != kStatePlayerDrive && type != kTrafficStateStopAtStation) return result;
    for (cSC4TrainCar* car : cars_) {
        if (car && car->GetBehavior() != type) car->PushState(type, nullptr, -1.0f, false, 0, 0, nullptr);
    }
    if (type != kStatePlayerDrive) return result;
    auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
    drive->value40 = 0;
    const tAutomataPlayerDriveParams* params = nullptr;
    if (vehicleHost_) vehicleHost_->FindPlayerDriveParams(this, &params);
    drive->groupSpeed = params ? params->F(0x8) : 1.0f;
    return result;
}

// 0x0039355a: the cars leave the station state with their engine.
bool cSC4Train::HandleExitState(const tAutomataState* state) {
    if (state && state->stateType == kTrafficStateStopAtStation) {
        for (cSC4TrainCar* car : cars_) {
            if (car && car->GetBehavior() == state->stateType) car->PopState();
        }
    }
    return cSC4TrafficAutomaton::HandleExitState(state);
}

// 0x00393444: the simulator's speed, slower uphill by the slope (the heading's rise) over the tuned steepest slope, at
// most by the tuned share. Downhill the same term is added only when it is positive, which a falling heading never gives.
float cSC4Train::GetSpeedModifier() const {
    float modifier = cSC4Automaton::GetSpeedModifier();
    if (!networkOccupant_ || CheckAutomataFlag(kAutomataFlagSpecialRoute)) return modifier;
    const VehicleTuning& tuning = Tuning();
    const float rise = orientation_.y;
    if (rise > 0.0f) {
        const float share = std::asin(rise) * tuning.turnMax / tuning.turnDivisor;
        float factor = 1.0f;
        if (0.0f <= share) factor = tuning.turnMax < share ? 1.0f - tuning.turnMax : 1.0f - share;
        modifier = factor * modifier;
    } else if (rise < 0.0f) {
        const float share = std::asin(rise) * tuning.turnMax / tuning.turnDivisor;
        float factor = 1.0f;
        if (0.0f <= share) factor = (tuning.turnMax < share ? tuning.turnMax : share) + 1.0f;
        modifier = factor * modifier;
    }
    return modifier;
}

// 0x00393618: claims or gives back the cell of a tile for this train on the traffic network map.
void cSC4Train::MarkTrainInCell(const tLookaheadNode& node, bool mark) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map || !node.occupant) return;   // the original does not test the occupant
    std::uint32_t x = 0, z = 0;
    node.occupant->GetOccupiedCell(x, z);
    if (mark) map->MarkTrainInCell(this, static_cast<int>(x), static_cast<int>(z));
    else map->UnmarkTrainInCell(this, static_cast<int>(x), static_cast<int>(z));
}

// 0x003937d6
void cSC4Train::SetDesiredNumCars(std::uint8_t count) {
    if (count < cars_.size()) SetNumCars(count);
    desiredCars_ = count;
}

// 0x00393814: surplus cars leave from the tail; asking for more only raises the wish.
void cSC4Train::SetNumCars(std::uint8_t count) {
    if (count < cars_.size()) {
        while (count < cars_.size()) {
            cSC4TrainCar* car = cars_.back();
            if (car) {
                car->Remove(false);
                car->Release();
            }
            cars_.pop_back();
        }
    } else if (cars_.size() < count) {
        SetDesiredNumCars(count);
    }
}

// 0x003938a6: the last car, or the engine without cars.
cSC4TrainCar* cSC4Train::GetTailCar() { return cars_.empty() ? this : cars_.back(); }

// 0x003939a6: two history tiles per car are kept before the car takes the path.
void cSC4Train::SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey, const std::vector<cS3DVector3>* path,
                        bool keepDestination) {
    SavePathHistory(pathHistory_, static_cast<std::uint8_t>(desiredCars_ * 2));
    cSC4TrainCar::SetPath(occupant, pathKey, path, keepDestination);
}

// 0x00393f9c: a train with cars claims the cells ahead: all of its lookahead tiles and the tiles behind it the first
// time, afterwards the newest lookahead tile.
bool cSC4Train::ChooseNewDestination() {
    const bool wasEmpty = lookahead_.empty();
    const bool result = cSC4TrafficAutomaton::ChooseNewDestination();
    if (desiredCars_ != 0 && !lookahead_.empty()) {
        if (wasEmpty) {
            for (const tLookaheadNode& node : lookahead_) MarkTrainInCell(node, true);
            for (const tLookaheadNode& node : pathHistory_) MarkTrainInCell(node, true);
        } else {
            MarkTrainInCell(lookahead_.back(), true);
        }
    }
    return result;
}

// 0x00393292: leaving the network at a station (building groups 0x1300, 0x1302, 0x1509) the train takes a path of the
// tile and stands there for 4.8 seconds.
bool cSC4Train::HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect) {
    if (lot && vehicleHost_ &&
        (vehicleHost_->IsLotBuildingInGroup(lot, kStationGroup1300) || vehicleHost_->IsLotBuildingInGroup(lot, kStationGroup1302) ||
         vehicleHost_->IsLotBuildingInGroup(lot, kStationGroup1509)) &&
        ChooseRandomPath(occupant, key)) {
        PushState(kTrafficStateStopAtStation, nullptr, kTrainStationStopMilliseconds, false, 0, 0, nullptr);
        return true;
    }
    return cSC4TrafficAutomaton::HandleExitNetwork(occupant, key, lot, rect);
}

// 0x00395654: the engine advances its nose along the path by its speed (held back by the safe distance to what is ahead,
// and matching a leader closer than a cell), finds its back point on the track one radius behind, and sits halfway
// between the two, facing from the back point to the nose.
bool cSC4Train::DoPathTick(float milliseconds) {
    const float speed = GetCurrentSpeed();
    float distance = speed * GetSpeedModifier() * milliseconds;
    SetActualSpeed(speed);
    if (GetSpecialRouteFlag() && Route().empty() && CheckAutomataFlag(kTrafficFlagStopped)) {
        HandleReachedRouteDestination();
        return true;
    }
    UpdateCollision();
    const float safe = GetSafeDistance();
    if (leader_ && safe < 16.0f) SetSpeed(leader_->GetCurrentSpeed(), 0.5f);
    if (safe <= distance) distance = safe;
    if (distance <= 0.0f) {
        SetAutomataFlag(kTrafficFlagStopped, true);
        SetActualSpeed(0.0f);
        if (distance < -GetRadius() && !CheckAutomataFlag(kAutomataFlagSpecialRoute)) {
            Remove(false);
            return false;
        }
        return true;
    }
    if (pathPos_.x < 0.0f || pathPos_.y < 0.0f || pathPos_.z < 0.0f) pathPos_ = position_;
    cS3DVector3 nose = Vec(0.0f, 0.0f, 0.0f);
    if (!InterpolateAlongPath(distance, pathPos_, nose, reinterpret_cast<std::int32_t*>(&pathIndex_), path_, &pathRemaining_)) {
        SetAutomataFlag(kAutomataFlagReachedDestination, true);
        SendCallbackMessage(kAutomataEventDestinationReached, 0);
        return true;
    }
    FindBackPosition(nose, backPoint_, radius_, pathHistory_);
    cS3DVector3 along = Sub(pathPos_, backPoint_);   // the nose it had before this step
    if (Length3(along) < kOrientationEpsilon) {
        along = prevOrient_;
        if (IsUnset(along) || Length3(along) < kOrientationEpsilon) return true;
    }
    const cS3DVector3 place = Vec(nose.x - 0.5f * along.x, nose.y - along.y * 0.5f, nose.z - along.z * 0.5f);
    nS3D::Normalize(along);
    MoveTo(&place.x, &along.x);
    prevOrient_ = along;
    pathPos_ = nose;
    SetAutomataFlag(kTrafficFlagStopped, false);
    return true;
}

// 0x00395a98: puts the engine at the END of the best path of the tile it stands on (rail, elevated, or its group's own
// kind; leaving by the edge its attraction points at), forgets where it has been, and sets the back point one radius
// behind along the track.
bool cSC4Train::SnapToPath() {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map) return false;
    int cellX = 0, cellZ = 0;
    trafficHost_->PositionToCell(position_.x, position_.z, cellX, cellZ);
    auto* occupant = static_cast<ITrafficAutomatonOccupant*>(map->FindNetworkOccupant(cellX, cellZ));
    cISC4PathInfo* pathInfo = occupant ? occupant->GetPathInfoObject() : nullptr;
    if (!pathInfo) return false;
    std::uint32_t pathType = kTrainPathTypeElevated;
    if (!elevated_) {
        if (IsOccupantGroup(kTrainGroupMonorailCar) || IsOccupantGroup(kTrainGroupMonorailEngine)) pathType = kTrainPathTypeMonorail;
        else if (IsOccupantGroup(kTrainGroupGroup422f) || IsOccupantGroup(kTrainGroupGroup422e)) pathType = kTrainPathType7;
        else pathType = kTrainPathTypeRail;
    }
    std::uint8_t edge = kPathEdgeWildcard;
    if (!CheckAutomataFlag(kAutomataFlagIgnoreAttractors) && attractionController_ && kOrientationEpsilon < Length3(attraction_)) {
        nS3D::Normalize(attraction_);
        GetEdgeFromVector(occupant, attraction_, edge, true);
    }
    std::uint32_t key = 0;
    const tPath* found = pathInfo->GetBestPath(pathType, kPathEdgeWildcard, edge, position_, &key, nullptr);
    if (!found || found->points.empty()) return false;   // the original tests the path only
    SetPath(occupant, key, &found->points, true);
    pathHistory_.clear();
    if (!path_ || path_->empty()) return false;          // port guard: SetPath found no path information
    pathIndex_ = static_cast<std::uint32_t>(path_->size()) - 1;
    pathPos_ = (*path_)[pathIndex_];
    orientation_ = Sub(pathPos_, path_->front());
    cS3DVector3 along;
    cS3DVector3 place;
    if (!FindBackPosition(pathPos_, backPoint_, radius_, pathHistory_)) {
        along = pathIndex_ > 0 ? Sub(pathPos_, (*path_)[pathIndex_ - 1]) : orientation_;   // port guard: a one-point path
        nS3D::Normalize(along);
        backPoint_ = Vec(pathPos_.x - radius_ * along.x, pathPos_.y - radius_ * along.y, pathPos_.z - along.z * radius_);
        place = Vec(pathPos_.x - radius_ * along.x * 0.5f, pathPos_.y - radius_ * along.y * 0.5f, pathPos_.z - radius_ * along.z * 0.5f);
    } else {
        along = Sub(pathPos_, backPoint_);
        place = Vec(pathPos_.x - along.x * 0.5f, pathPos_.y - along.y * 0.5f, pathPos_.z - along.z * 0.5f);
        nS3D::Normalize(along);
    }
    MoveTo(&place.x, &along.x);
    return true;
}

// 0x00395f54: a train starting on a cell the traffic simulator marks for a rail accident leaves a twenty-second accident
// there and tells the city. Then the trip starts as any traffic automaton's, the nose goes to its place on the path (the
// distance already driven when the trip continues, the path's end otherwise) and the engine is set between nose and back
// point.
bool cSC4Train::StartTrip(ITrafficAutomatonOccupant* occupant, std::uint32_t key, bool param3) {
    if (occupant && vehicleHost_) {
        std::uint32_t cellX = 0, cellZ = 0;
        occupant->GetOccupiedCell(cellX, cellZ);
        if (vehicleHost_->CheckRailAccident(static_cast<int>(cellX), static_cast<int>(cellZ))) {
            if (cIGZUnknown* accident = vehicleHost_->CreateAccident(AsOccupant(), 2, 20000)) accident->Release();
            vehicleHost_->PostRailAccidentMessage(static_cast<int>(cellX), static_cast<int>(cellZ));
        }
    }
    const bool result = cSC4TrafficAutomaton::StartTrip(occupant, key, param3);
    if (!result || !path_ || path_->empty() || CheckAutomataFlag(kAutomataFlagIgnorePaths)) return result;
    if (param3) {
        if (pathRemaining_ <= 0.0f) {
            if (pathInfo_) pathInfo_->SnapPointToPath(pathPos_, path_);
        } else {
            const float safe = GetSafeDistance();
            if (safe <= pathRemaining_) pathRemaining_ = safe;
            const cS3DVector3 start = path_->front();
            InterpolateAlongPath(pathRemaining_, start, pathPos_, reinterpret_cast<std::int32_t*>(&pathIndex_), path_, &pathRemaining_);
        }
    } else {
        pathIndex_ = static_cast<std::uint32_t>(path_->size()) - 2;
        pathPos_ = path_->back();
    }
    FindBackPosition(pathPos_, backPoint_, radius_, pathHistory_);
    cS3DVector3 along;
    if (!std::isnan(backPoint_.x) && !std::isnan(backPoint_.y) && !std::isnan(backPoint_.z)) {
        along = Sub(pathPos_, backPoint_);
    } else {
        pathPos_ = path_->back();
        along = Sub(pathPos_, path_->front());
        nS3D::Normalize(along);
        along = Vec(radius_ * along.x, radius_ * along.y, radius_ * along.z);
    }
    const cS3DVector3 place = Vec(pathPos_.x - 0.5f * along.x, pathPos_.y - along.y * 0.5f, pathPos_.z - along.z * 0.5f);
    if (Length3(along) <= kOrientationEpsilon) {
        MoveTo(&place.x, nullptr);
    } else {
        nS3D::Normalize(along);
        MoveTo(&place.x, &along.x);
    }
    prevOrient_ = along;
    SetAutomataFlag(kTrafficFlagStopped, false);
    return result;
}

// 0x00394da6: every car is put on the track behind its leader, in two steps. First its nose: the point of the track
// half the leader's plus half its own radius behind the leader's back point. Then its back point: one radius behind its
// nose. Both are found walking the engine's path back from its end, and past the path's start through the tiles the
// engine has left (newest first). The car sits halfway between nose and back point, facing along them. A fading engine
// fades its cars. When the last car of a full train leaves a tile, the tile's cell is given back.
void cSC4Train::TickFollowingCars(float milliseconds) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    const std::vector<cS3DVector3>* path = path_;
    cS3DVector3 point = Vec(0.0f, 0.0f, 0.0f), segmentEnd = Vec(0.0f, 0.0f, 0.0f), found = Vec(0.0f, 0.0f, 0.0f);
    bool foundAny = false;
    auto history = pathHistory_.rend();   // rend = not started
    const float opacity = GetOpacity();
    ITrafficAutomatonOccupant* occupant = networkOccupant_;
    std::uint32_t pathKey = pathKey_;
    bool searching = path != nullptr;
    std::uint32_t index = 0;
    if (searching) {
        segmentEnd = path->back();
        index = static_cast<std::uint32_t>(path->size()) - 2;   // a one-point path wraps past every size test, as in the original
    }
    cSC4TrainCar* leader = this;
    cSC4TrainCar* const tail = cars_.empty() ? nullptr : cars_.back();
    cS3DVector3 centre = backPoint_;
    bool nosePass = true;
    for (std::size_t i = 0; i < cars_.size();) {
        cSC4TrainCar* car = cars_[i];
        if (!car) {   // port guard
            ++i;
            continue;
        }
        if (opacity < 1.0f && CheckAutomataFlag(kAutomataFlagMarkedForRemove)) {
            car->SetOpacity(opacity, 0.01f);
            car->UpdateOpacity(milliseconds);
        }
        const float reach = nosePass ? car->GetRadius() * 0.5f + leader->GetRadius() * 0.5f : car->GetRadius();
        bool hit = false;
        while (searching) {
            if (path && index < path->size()) {
                point = (*path)[index];
                if (FindPathIntersection(point, segmentEnd, centre, reach, found, 0.1f)) {
                    hit = true;
                    foundAny = true;
                    break;
                }
            }
            if (index == 0) {
                if (history == pathHistory_.rend()) history = pathHistory_.rbegin();
                else ++history;
                const tPath* earlier = nullptr;
                if (history != pathHistory_.rend() && history->occupant) {
                    cISC4PathInfo* info = history->occupant->GetPathInfoObject();
                    earlier = info ? info->GetPath(history->key, nullptr) : nullptr;
                }
                if (!earlier || earlier->points.empty()) {   // nothing further back
                    searching = false;
                    index = 0;
                    break;
                }
                path = &earlier->points;
                index = static_cast<std::uint32_t>(path->size()) - 2;
                pathKey = history->key;
                occupant = history->occupant.get();
            } else {
                --index;
            }
            segmentEnd = point;
        }
        if (nosePass) {
            // Port guard: before the first intersection the original stores an unset stack vector here.
            if (foundAny) car->pathPos_ = found;
        } else {
            if (!hit) {
                car->backPoint_ = Vec(car->pathPos_.x - reach * car->orientation_.x, car->pathPos_.y - reach * car->orientation_.y,
                                      car->pathPos_.z - reach * car->orientation_.z);
            } else {
                car->backPoint_ = found;
            }
            if (tail == car && cars_.size() == desiredCars_ && map) {
                ITrafficAutomatonOccupant* under = car->GetNetworkOccupant();
                if (under && under != occupant) {
                    std::uint32_t x = 0, z = 0;
                    under->GetOccupiedCell(x, z);
                    map->UnmarkTrainInCell(this, static_cast<int>(x), static_cast<int>(z));
                }
            }
            car->SetPath(occupant, pathKey, path, true);
            cS3DVector3 along = Sub(car->pathPos_, car->backPoint_);
            if (Length3(along) < kOrientationEpsilon) {
                along = car->prevOrient_;
                if (IsUnset(along) || Length3(along) < kOrientationEpsilon) along = orientation_;
            }
            const cS3DVector3 place = Vec(car->pathPos_.x - 0.5f * along.x, car->pathPos_.y - along.y * 0.5f, car->pathPos_.z - along.z * 0.5f);
            nS3D::Normalize(along);
            car->MoveTo(&place.x, &along.x);
            car->prevOrient_ = along;
            car->SetLeader(leader);
            ++i;
            leader = car;
        }
        nosePass = !nosePass;
        centre = found;
    }
}

// 0x00397524: once the tail's back point is two and a half of its radii away from the appear place, the next car is made
// there by the automata manager (a freight train's last car is the caboose group), follows the tail, takes its heading and
// the appear place's tile and path, and joins the cars. True when a car was added.
bool cSC4Train::AddCarAtEnd() {
    cSC4TrainCar* tail = GetTailCar();
    const float radius = tail->GetRadius();
    const cS3DVector3 gap = Sub(tail->backPoint_, tailPos_);
    if (!(radius + radius <= Length3(gap) + radius * -0.5f)) return false;
    ITrafficAutomataManager* manager = GetTrafficAutomataManager();
    if (!manager || carGroup_ == 0) return false;
    std::uint32_t group = carGroup_;
    if (group == kTrainGroupFreightCar && cars_.size() == desiredCars_ - 1) group = kTrainGroupFreightLastCar;
    cSC4Automaton* made = manager->CreateAutomaton(group, tailPos_, automataFlags_, priority_);
    void* raw = nullptr;
    if (!made || !made->QueryInterface(kTrainCarGZCLSID, &raw) || !raw) return false;
    auto* car = static_cast<cSC4TrainCar*>(raw);   // counted by the query
    car->SetAutomataFlag(kAutomataFlagSelfControlling, false);
    car->SetLeader(tail);
    car->MoveTo(&tailPos_.x, &tail->orientation_.x);
    car->SetPath(tailOccupant_, tailPathKey_, nullptr, true);
    cars_.push_back(car);   // the list keeps the query's reference
    if (GetBehavior() == kStatePlayerDrive) car->PushState(kStatePlayerDrive, nullptr, -1.0f, false, 0, 0, nullptr);
    return true;
}

// ---- cSC4MassTransitTrain ------------------------------------------------------------------------------------------------

bool cSC4MassTransitTrain::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    return cSC4Train::Write(stream);
}

bool cSC4MassTransitTrain::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 1 && cSC4Train::Read(stream);
}

// 0x004b11b2: after choosing where to go, a train whose nose is on a lot's cell while the cell its path leaves toward has
// no lot stands for 4.8 seconds: it has reached the far end of a station.
bool cSC4MassTransitTrain::ChooseNewDestination() {
    const bool result = cSC4Train::ChooseNewDestination();
    if (GetBehavior() == kStatePlayerDrive || CheckAutomataFlag(kAutomataFlagMarkedForRemove) || !vehicleHost_ || !trafficHost_) return result;
    int cellX = 0, cellZ = 0;
    trafficHost_->PositionToCell(pathPos_.x, pathPos_.z, cellX, cellZ);
    int nextX = cellX, nextZ = cellZ;
    switch (static_cast<std::uint8_t>(pathKey_ & 0xff)) {   // the path's exit edge
        case 0: nextX = cellX - 1; break;
        case 1: nextZ = cellZ - 1; break;
        case 2: nextX = cellX + 1; break;
        case 3: nextZ = cellZ + 1; break;
        default: break;
    }
    if (vehicleHost_->IsLotAtCell(cellX, cellZ) && !vehicleHost_->IsLotAtCell(nextX, nextZ))
        PushState(kTrafficStateStopAtStation, nullptr, kTrainStationStopMilliseconds, false, 0, kMassTransitStationSfx, nullptr);
    return result;
}

// 0x004b1034: the stations of its own kind (building groups 0x1303, 0x1307), then the train's.
bool cSC4MassTransitTrain::HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect) {
    if (lot && vehicleHost_ &&
        (vehicleHost_->IsLotBuildingInGroup(lot, kStationGroup1303) || vehicleHost_->IsLotBuildingInGroup(lot, kStationGroup1307)) &&
        ChooseRandomPath(occupant, key)) {
        PushState(kTrafficStateStopAtStation, nullptr, kTrainStationStopMilliseconds, false, 0, kMassTransitStationSfx, nullptr);
        return true;
    }
    return cSC4Train::HandleExitNetwork(occupant, key, lot, rect);
}

}  // namespace sc4
