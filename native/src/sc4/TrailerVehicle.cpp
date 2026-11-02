#include "sc4/TrailerVehicle.h"

#include <algorithm>

#include "platform/Unwired.h"
#include "sc4/DBSegment.h"

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
    if (cars_.size() < desiredCars_.size())
        SC4_UNWIRED("cSC4TrailerVehicle::AddCarAtEnd (the automata manager's CreateAutomaton for a trailer car)");
    if (!cars_.empty())
        SC4_UNWIRED(CheckAutomataFlag(0x2000) ? "cSC4TrailerVehicle::TickFollowingCarsOffRoad" : "cSC4TrailerVehicle::TickFollowingCars");
    return result;
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
