// cSC4Aircraft — see Aircraft.h for the recovered layout, quirks and seams.
#include "sc4/Aircraft.h"

#include <cmath>

#include "sc4/AutomataStates.h"
#include "sc4/OccupantManager.h"

namespace sc4 {
namespace {

IAircraftHost* g_aircraftHost = nullptr;

}  // namespace

IAircraftHost* AircraftHost() { return g_aircraftHost; }
void SetAircraftHost(IAircraftHost* host) { g_aircraftHost = host; }

cSC4Aircraft::cSC4Aircraft(IAutomatonHost* host, IAircraftHost* aircraftHost)
    : cSC4Automaton(host), aircraftHost_(aircraftHost ? aircraftHost : AircraftHost()) {
    type_ = kAircraftOccupantType;   // +0x48
    SetFlag(kAircraftOccupantFlag);  // +0x53 |= 0x10
    field308_ = 1.0f;                // +0x308
    playerDriving_ = false;          // +0x30c
}

cSC4Aircraft::~cSC4Aircraft() = default;

// @ 0x100501a34. Answers the aircraft id first, then the cSC4Automaton ids. (The original spells the base cases out
// inline in six thunks; the port relies on cSC4Automaton::QueryInterface for them.)
bool cSC4Aircraft::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4Aircraft) {
        if (!out) return false;
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Automaton::QueryInterface(iid, out);
}

// @ 0x1005019b4.
bool cSC4Aircraft::Init() {
    playerDriving_ = false;
    SetAutomataFlag(kAutomataFlagIgnorePaths, true);   // 0x2000
    return cSC4Automaton::Init();
}

// @ 0x100501a1c.
bool cSC4Aircraft::Shutdown() { return cSC4Automaton::Shutdown(); }

// @ 0x100501afc. Places the aircraft at a random point 500 m above the terrain, but only while it is self-controlling.
bool cSC4Aircraft::PlaceSelf() {
    const bool selfControlling = GetSelfControlling();
    if (!selfControlling) return false;
    IAircraftHost* host = aircraftHost_ ? aircraftHost_ : AircraftHost();
    if (!host) return true;
    const float x = static_cast<float>(host->RandomDoubleRange(0.0, static_cast<double>(host->CityCellCountX())));
    const float z = static_cast<float>(host->RandomDoubleRange(0.0, static_cast<double>(host->CityCellCountZ())));
    const cS3DVector3 position{x, host->TerrainHeight(x, z) + 500.0f, z};
    SetPosition(&position.x);
    return true;
}

// @ 0x100501bf2. Stream version 1, then the automaton record.
bool cSC4Aircraft::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    if (!cSC4Automaton::Write(stream)) return false;
    return !stream.GetError();
}

// @ 0x100501ca6. Only version 1 is accepted; the original reads the version through the 16-bit slot.
bool cSC4Aircraft::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    return cSC4Automaton::Read(stream);
}

// @ 0x100501dc2. Player drive becomes active whenever a state of type 0x10 is anywhere on the stack. The two fields the
// original stamps on the current top state record (+0x170 = 0xf, +0x1d8 = 2000.0f) only exist in the player-drive record;
// when the top is another record the original's write lands in its unused tail, which the port cannot reproduce, so it is
// applied only when the top really is the player-drive record (documented quirk).
bool cSC4Aircraft::HandleEnterState(tAutomataState* state) {
    if (!state || state->stateType != kStatePlayerDrive) {
        playerDriving_ = FindState(kStatePlayerDrive) != nullptr;
        if (!playerDriving_) return cSC4Automaton::HandleEnterState(state);
    } else {
        playerDriving_ = true;
    }
    tAutomataState* top = GetState();
    if (top && top->stateType == kStatePlayerDrive) {
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(top);
        drive->shells = 0xf;
        drive->timeout1d8 = 0x44fa0000;   // 2000.0f
    }
    return cSC4Automaton::HandleEnterState(state);
}

// @ 0x100501e36.
bool cSC4Aircraft::HandleExitState(const tAutomataState* state) {
    if (state && state->stateType == kStatePlayerDrive) playerDriving_ = false;
    return cSC4Automaton::HandleExitState(state);
}

// @ 0x100501e52. The base tick, then a GetSelfControlling() call whose answer is discarded (quirk).
bool cSC4Aircraft::DoAnimationTick(std::uint32_t data, float milliseconds) {
    const bool result = cSC4Automaton::DoAnimationTick(data, milliseconds);
    GetSelfControlling();
    return result;
}

// @ 0x100501e78. In player drive the heading is rotated by the state record's steering angle and the aircraft advances
// along it by the speed tween; then the actual speed (+0xc8) is the distance moved over the *unclamped* delta.
bool cSC4Aircraft::DoPlayerDriveTick(float milliseconds) {
    const float startX = position_.x;
    const float startY = position_.y;
    const float startZ = position_.z;
    const bool result = cSC4Automaton::DoPlayerDriveTick(milliseconds);
    if (result) {
        tAutomataState* state = GetState();
        if (state && state->stateType == kStatePlayerDrive) {
            auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
            const float angle = drive->steering6c;
            const float sine = std::sin(angle);
            const float cosine = std::cos(angle);
            const float ox = orientation_.x;
            const float oy = orientation_.y;
            const float oz = orientation_.z;
            orientation_.x = sine * oz + cosine * ox;
            orientation_.y = oy;
            orientation_.z = cosine * oz - sine * ox;
        }
        float speed;
        if (speedTween_.duration <= 0.0f) {
            speed = speedTween_.desired;
        } else {
            float ratio = speedTween_.elapsed / speedTween_.duration;
            if (ratio > 1.0f) ratio = 1.0f;
            if (ratio < 0.0f) ratio = 0.0f;
            speed = ratio * (speedTween_.desired - speedTween_.current) + speedTween_.current;
        }
        speed *= milliseconds;
        const float target[3] = {orientation_.x * speed + position_.x, orientation_.y * speed + position_.y,
                                 orientation_.z * speed + position_.z};
        MoveTo(target, &orientation_.x);
    }
    const float dx = position_.x - startX;
    const float dy = position_.y - startY;
    const float dz = position_.z - startZ;
    actualSpeed_ = std::sqrt(dx * dx + dy * dy + dz * dz) / milliseconds;
    return result;
}

// @ 0x100502098. The terrain height under the point, raised to the water surface, unless a lot there has a special
// altitude (lot +0x60 bit 0 -> lot +0x80). Outside the terrain, the terrain object's default altitude (slot 0xe8).
float cSC4Aircraft::GetAltitude(const cS3DVector3& position) const {
    IAircraftHost* host = aircraftHost_ ? aircraftHost_ : AircraftHost();
    if (!host) return 0.0f;
    if (!host->TerrainPointInBounds(position.x, position.z)) return host->DefaultAltitude();
    const float terrain = host->TerrainHeight(position.x, position.z);
    const float water = host->WaterHeight(position.x, position.z);
    float lotAltitude = 0.0f;
    if (host->LotAltitudeAt(position, &lotAltitude)) return lotAltitude;
    return water > terrain ? water : terrain;
}

// @ 0x100502160. Clamps the point to the city box: x/z in [-1000, size + 1000], y in [0, clamp + 1000].
void cSC4Aircraft::ClipToWorld(cS3DVector3& position) const {
    IAircraftHost* host = aircraftHost_ ? aircraftHost_ : AircraftHost();
    if (!host) return;
    const float maxX = host->CitySizeX() + 1000.0f;
    const float maxY = host->AltitudeClamp() + 1000.0f;
    const float maxZ = host->CitySizeZ() + 1000.0f;
    position.x = position.x < -1000.0f ? -1000.0f : (position.x <= maxX ? position.x : maxX);
    position.y = position.y < 0.0f ? 0.0f : (position.y <= maxY ? position.y : maxY);
    position.z = position.z < -1000.0f ? -1000.0f : (position.z <= maxZ ? position.z : maxZ);
}

}  // namespace sc4
