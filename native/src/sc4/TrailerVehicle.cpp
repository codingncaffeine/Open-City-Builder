#include "sc4/TrailerVehicle.h"

#include <algorithm>
#include <cmath>

#include "s3d/S3DMath.h"
#include "sc4/AutomataStates.h"
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/PathInfo.h"

namespace sc4 {

// 0x00392fc6: the vehicle with empty car lists and an unknown tail position.
cSC4TrailerVehicle::cSC4TrailerVehicle(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost, IVehicleHost* vehicleHost,
                                       const cGZPersistResourceKey& modelKey)
    : cSC4Vehicle(host, automatonHost, vehicleHost, modelKey) {}

// 0x0074bc30: drops the tail occupant; the cars were released by Shutdown.
cSC4TrailerVehicle::~cSC4TrailerVehicle() {
    if (tailOccupant_) tailOccupant_->Release();
}

// 0x00391a90: flag 0x10000000 goes on before the vehicle's Init.
bool cSC4TrailerVehicle::Init() {
    SetAutomataFlag(kAutomataFlagTrailer, true);
    return cSC4Vehicle::Init();
}

// 0x00391f16: the vehicle shuts down, the tail occupant goes, and every car is removed without a fade and released.
bool cSC4TrailerVehicle::Shutdown() {
    const bool result = cSC4Vehicle::Shutdown();
    if (tailOccupant_) {
        ITrafficAutomatonOccupant* old = tailOccupant_;
        tailOccupant_ = nullptr;
        old->Release();
    }
    for (cSC4Vehicle* car : cars_) {
        if (!car) continue;
        car->Remove(false);   // vslot 0x34
        car->Release();
    }
    cars_.clear();
    return result;
}

// 0x00391adc: 0x4a5b69ed answers this object; everything else goes to the vehicle.
bool cSC4TrailerVehicle::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kTrailerVehicleGZCLSID) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Vehicle::QueryInterface(iid, out);
}

// 0x00391dcc
bool cSC4TrailerVehicle::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    const bool ok = cSC4Vehicle::Write(stream);
    if (ok) {
        stream.SetUint32(static_cast<std::uint32_t>(cars_.size()));
        for (const cSC4Vehicle* car : cars_) SC4WriteGZSerializable(stream, static_cast<const cIGZSerializable*>(car));
        stream.SetUint32(static_cast<std::uint32_t>(desiredCars_.size()));
        for (std::uint32_t id : desiredCars_) stream.SetUint32(id);
        stream << tailPos_;
        SC4WriteGZSerializable(stream, tailOccupant_);
        stream.SetUint32(tailPathKey_);
    }
    return ok;
}

// 0x00391fae: version 2 only. Null car refs and zero model ids are dropped; the cars arrive counted.
bool cSC4TrailerVehicle::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (!cSC4Vehicle::Read(stream)) return false;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kVehicleIID, &raw, false);
        if (raw) cars_.push_back(static_cast<cSC4Vehicle*>(raw));
    }
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        stream.GetUint32(id);
        if (id) desiredCars_.push_back(id);
    }
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

// 0x00391c7e: the vehicle ticks; while the tail position is unknown (a negative x or z) it is taken from the tail car with
// that car's network occupant and path key; then a missing car is added and the cars follow (off the road with flag 0x2000).
bool cSC4TrailerVehicle::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    const bool result = cSC4Vehicle::DoAnimationTick(unknown, milliseconds);
    if (tailPos_.x < 0.0f || tailPos_.z < 0.0f) {
        cSC4Vehicle* tail = GetTailCar();
        float xyz[3];
        tail->GetPosition(xyz);
        tailPos_ = cS3DVector3(xyz[0], xyz[1], xyz[2]);
        ITrafficAutomatonOccupant* occupant = tail->GetNetworkOccupant();
        if (occupant) occupant->AddRef();
        if (tailOccupant_) tailOccupant_->Release();
        tailOccupant_ = occupant;
        tailPathKey_ = tail->GetPathKey();
    }
    if (cars_.size() < desiredCars_.size()) AddCarAtEnd();
    if (CheckAutomataFlag(0x2000)) TickFollowingCarsOffRoad(milliseconds);
    else TickFollowingCars(milliseconds);
    return result;
}

// 0x00391d80: the group's trailer list names the cars to pull.
void cSC4TrailerVehicle::SetAutomataGroup(IAutomatonGroupTemplate* group) {
    cSC4Automaton::SetAutomataGroup(group);
    const std::list<std::uint32_t>* trailers = group ? group->TrailerGroupIds() : nullptr;
    if (!trailers) return;
    for (std::uint32_t id : *trailers) AddTrailer(id);
}

// 0x00392116: once the tail is two of its radii away from the remembered tail position, the next car is made there by
// the vehicle's automata manager (with the lead's flags and priority), follows the tail, takes its heading and the
// remembered occupant and path, and joins the cars. True when a car was added.
bool cSC4TrailerVehicle::AddCarAtEnd() {
    cSC4Vehicle* tail = GetTailCar();
    const float radius = tail->GetRadius();
    const cS3DVector3 at = tail->GetPathPos();
    if (at.x < 0.0f || at.y < 0.0f || at.z < 0.0f) return false;
    const float dx = at.x - tailPos_.x, dy = at.y - tailPos_.y, dz = at.z - tailPos_.z;
    if (!(radius + radius <= std::sqrt(dx * dx + dy * dy + dz * dz))) return false;
    ITrafficAutomataManager* manager = GetTrafficAutomataManager();
    if (!manager || desiredCars_.empty() || cars_.size() >= desiredCars_.size()) return false;
    cSC4Automaton* made = manager->CreateAutomaton(desiredCars_[cars_.size()], tailPos_, automataFlags_, priority_);
    auto* car = made ? dynamic_cast<cSC4Vehicle*>(made) : nullptr;   // port guard: the cars are vehicles
    if (!car) return false;
    car->SetAutomataFlag(2, false);
    car->SetLeader(tail);
    float heading[3] = {0.0f, 0.0f, 0.0f};
    tail->GetOrientation(heading);
    car->MoveTo(&tailPos_.x, heading);
    car->SetPath(tailOccupant_, tailPathKey_, nullptr, true);
    cars_.push_back(car);
    car->AddRef();
    return true;
}

// 0x00392372: a line 200 long through `point`, square to the segment, is cut with the segment in the ground plane. The
// height is taken between the ends by the ratio of the SQUARED lengths (the original's).
bool cSC4TrailerVehicle::FindPointOnPathSegment(const cS3DVector3& point, const cS3DVector3& a, const cS3DVector3& b, cS3DVector3& out) const {
    if (a.x == b.x && a.y == b.y && a.z == b.z) return false;
    cS3DVector3 along{b.x - a.x, b.y - a.y, b.z - a.z};
    nS3D::Normalize(along);
    const float reach = 100.0f;
    const float left[2] = {along.z * reach + point.x, point.z - reach * along.x};
    const float right[2] = {point.x - along.z * reach, reach * along.x + point.z};
    const float from[2] = {a.x, a.z};
    const float to[2] = {b.x, b.z};
    float hit[2] = {0.0f, 0.0f};
    if (!SC4GeometryUtilities::LinesIntersect2D(left, right, from, to, hit)) return false;
    out.x = hit[0];
    out.z = hit[1];
    const float whole = SC4GeometryUtilities::LengthOfLineSquared2D(from, to);
    const float part = SC4GeometryUtilities::LengthOfLineSquared2D(from, hit);
    const float fraction = 1e-05f < whole ? part / whole : 0.0f;
    out.y = a.y + fraction * (b.y - a.y);
    return true;
}

// 0x003929ca: every car is put on the path behind its leader. From the lead's place on its path the search walks back
// point by point, and past the path's start through the lead's path history (newest first), until the perpendicular
// through the leader's rear meets a segment. The car takes that point as its path position, sits half the lead's
// radius behind it and turns halfway from its previous heading to the segment's. A fading lead fades its cars.
void cSC4TrailerVehicle::TickFollowingCars(float milliseconds) {
    const std::vector<cS3DVector3>* path = CurrentPath();
    const float opacity = GetOpacity();
    ITrafficAutomatonOccupant* occupant = GetNetworkOccupant();
    std::uint32_t pathKey = GetPathKey();
    std::uint32_t index = PathIndex();
    cS3DVector3 segmentEnd = pathPos_;
    auto history = pathHistory_.rend();   // rend = not started
    bool started = false;
    cSC4Automaton* leader = this;
    for (cSC4Vehicle* car : cars_) {
        if (opacity < 1.0f && CheckAutomataFlag(1)) {
            car->SetOpacity(opacity, 0.01f);
            car->UpdateOpacity(milliseconds);
        }
        float front[3] = {0.0f, 0.0f, 0.0f}, heading[3] = {0.0f, 0.0f, 0.0f};
        leader->GetPosition(front);
        leader->GetOrientation(heading);
        const float reach = leader->GetRadius();
        const cS3DVector3 rear{front[0] - reach * heading[0], front[1] - reach * heading[1], front[2] - reach * heading[2]};
        if (!path) return;   // a lead without a path places nobody (the first car still fades with it)
        cS3DVector3 point{0.0f, 0.0f, 0.0f}, found{0.0f, 0.0f, 0.0f};
        for (;;) {
            if (path && index < path->size()) {
                point = (*path)[index];
                if (FindPointOnPathSegment(rear, point, segmentEnd, found)) break;
            }
            if (index == 0) {
                if (!started) {
                    history = pathHistory_.rbegin();
                    started = true;
                } else {
                    ++history;
                }
                if (history == pathHistory_.rend()) return;
                occupant = history->occupant;
                if (!occupant) return;
                cISC4PathInfo* info = occupant->GetPathInfoObject();
                if (!info) return;
                const tPath* earlier = info->GetPath(history->key, nullptr);
                if (!earlier) return;
                path = &earlier->points;
                if (path->size() < 2) return;   // port guard: the original indexes size - 2
                index = static_cast<std::uint32_t>(path->size()) - 2;
                pathKey = history->key;
            } else {
                --index;
            }
            segmentEnd = point;
        }
        car->SetPathPos(found);
        car->SetPath(occupant, pathKey, path, true);
        cS3DVector3 turn{segmentEnd.x - point.x, segmentEnd.y - point.y, segmentEnd.z - point.z};
        nS3D::Normalize(turn);
        // Port guard: a car that has not been placed yet has no previous heading (the vehicle's unset vector); the
        // original averages with it all the same, which throws a new car far off for its first ticks.
        const cS3DVector3& previous = car->GetPrevOrient();
        if (previous.x > kAutomatonUnsetCoordinate + 1.0f) {
            turn.z = (turn.z + previous.z) * 0.5f;
            turn.y = (turn.y + previous.y) * 0.5f;
            turn.x = (turn.x + previous.x) * 0.5f;
        }
        const float back = radius_ * 0.5f;
        const cS3DVector3 place{found.x - back * turn.x, found.y - back * turn.y, found.z - turn.z * back};
        car->MoveTo(&place.x, &turn.x);
        car->SetPrevOrient(turn);
        leader = car;
    }
}

// 0x003924e8: off the road each car is pulled by its rear toward its leader's rear (at the lead's actual speed, turning
// halfway), then set one radius behind that point, facing it. A lead driven backwards by the player pushes: the car's rear
// goes two radii behind the leader's rear along the lead's heading, on the road or ground height.
void cSC4TrailerVehicle::TickFollowingCarsOffRoad(float milliseconds) {
    const float opacity = GetOpacity();
    const auto* drive = static_cast<const tAutomataStatePlayerDrive*>(FindState(kStatePlayerDrive));
    cSC4Automaton* leader = this;
    for (cSC4Vehicle* car : cars_) {
        if (opacity < 1.0f && CheckAutomataFlag(1)) {
            car->SetOpacity(opacity, 0.01f);
            car->UpdateOpacity(milliseconds);
        }
        float front[3] = {0.0f, 0.0f, 0.0f}, heading[3] = {0.0f, 0.0f, 0.0f};
        leader->GetPosition(front);
        leader->GetOrientation(heading);
        const float reach = leader->GetRadius();
        const cS3DVector3 hitch{front[0] - reach * heading[0], front[1] - reach * heading[1], front[2] - reach * heading[2]};
        float carAt[3] = {0.0f, 0.0f, 0.0f}, carHeading[3] = {0.0f, 0.0f, 0.0f};
        car->GetPosition(carAt);
        car->GetOrientation(carHeading);
        const float radius = car->GetRadius();
        cS3DVector3 rear{carAt[0] - radius * carHeading[0], carAt[1] - radius * carHeading[1], carAt[2] - radius * carHeading[2]};
        if (!drive || 0.0f <= drive->value3c) {
            cS3DVector3 pull{hitch.x - rear.x, hitch.y - rear.y, hitch.z - rear.z};
            if (std::sqrt(pull.x * pull.x + pull.y * pull.y + pull.z * pull.z) <= 1e-05f) pull = cS3DVector3{carHeading[0], carHeading[1], carHeading[2]};
            else nS3D::Normalize(pull);
            pull.z = (pull.z + carHeading[2]) * 0.5f;
            pull.y = (pull.y + carHeading[1]) * 0.5f;
            pull.x = (pull.x + carHeading[0]) * 0.5f;
            const float step = GetActualSpeed();
            rear.x = step * pull.x + rear.x;
            rear.y = step * pull.y + rear.y;
            rear.z = step * pull.z + rear.z;
        } else {
            rear.z = hitch.z - (radius * orientation_.z + radius * orientation_.z);
            rear.y = hitch.y - (radius * orientation_.y + radius * orientation_.y);
            rear.x = hitch.x - (radius * orientation_.x + radius * orientation_.x);
            if (Host()) rear.y = Host()->RoadAltitude(rear.x, rear.z);
        }
        cS3DVector3 facing{hitch.x - rear.x, hitch.y - rear.y, hitch.z - rear.z};
        if (std::sqrt(facing.x * facing.x + facing.y * facing.y + facing.z * facing.z) <= 1e-05f) facing = cS3DVector3{carHeading[0], carHeading[1], carHeading[2]};
        else nS3D::Normalize(facing);
        const cS3DVector3 place{hitch.x - radius * facing.x, hitch.y - radius * facing.y, hitch.z - radius * facing.z};
        car->MoveTo(&place.x, &facing.x);
        car->SetPathPos(hitch);
        car->SetPrevOrient(facing);
        leader = car;
    }
}

// 0x00391b18: never collides with its own cars.
bool cSC4TrailerVehicle::ShouldCollide(cSC4Automaton* other) const {
    if (std::find(cars_.begin(), cars_.end(), other) != cars_.end()) return false;
    return cSC4Automaton::ShouldCollide(other);
}

// 0x00391b80: the path history keeps one entry per car plus one before the vehicle takes the path.
void cSC4TrailerVehicle::SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey, const std::vector<cS3DVector3>* path,
                                 bool keepDestination) {
    SavePathHistory(pathHistory_, static_cast<std::uint8_t>(desiredCars_.size() + 1));
    cSC4Vehicle::SetPath(occupant, pathKey, path, keepDestination);
}

// 0x00391ac4
void cSC4TrailerVehicle::ShowHeadlights(bool on, bool instant) { cSC4Vehicle::ShowHeadlights(on, instant); }

// 0x00391c4c: the last car, or the vehicle itself without cars.
cSC4Vehicle* cSC4TrailerVehicle::GetTailCar() { return cars_.empty() ? this : cars_.back(); }

// 0x00391bf8: the tail car's collide position (the vehicle's own without cars).
void cSC4TrailerVehicle::GetCollidePosition(cS3DVector3& out) {
    if (cars_.empty()) cSC4Automaton::GetCollidePosition(out);
    else cars_.back()->GetCollidePosition(out);
}

}  // namespace sc4
