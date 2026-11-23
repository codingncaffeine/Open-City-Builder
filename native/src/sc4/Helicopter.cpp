// cSC4Helicopter — see Helicopter.h for the recovered layout, quirks, seams and the PARTIAL list.
#include "sc4/Helicopter.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

#include "platform/Unwired.h"
#include "sc4/AutomataStates.h"

namespace sc4 {

// The original's field offsets (Helicopter.h documents them); a regression here silently changes the physics.
static_assert(sizeof(HeliTransform) == 0x38, "HeliTransform must match cS3DTransform");
static_assert(offsetof(HeliTransform, rotation) == 0x04, "HeliTransform rotation");
static_assert(offsetof(HeliTransform, translation) == 0x28, "HeliTransform translation");
static_assert(offsetof(HeliTransform, scale) == 0x34, "HeliTransform scale");
static_assert(sizeof(HELI_ROTOR) == 0xb0, "HELI_ROTOR must be 0xb0 bytes");
static_assert(offsetof(HELI_ROTOR, position) == 0x30, "HELI_ROTOR position");
static_assert(offsetof(HELI_ROTOR, body) == 0x3c, "HELI_ROTOR body transform");
static_assert(offsetof(HELI_ROTOR, blade) == 0x74, "HELI_ROTOR blade transform");
static_assert(sizeof(HELI_ENGINE) == 0x40, "HELI_ENGINE must be 0x40 bytes");
static_assert(offsetof(HELI_ENGINE, liftControl) == 0x08, "HELI_ENGINE lift");
static_assert(offsetof(HELI_ENGINE, forceX) == 0x34, "HELI_ENGINE force");
static_assert(sizeof(HELI_DATA) == 0x218, "HELI_DATA must be 0x218 bytes");
static_assert(offsetof(HELI_DATA, state) == 0x08, "HELI_DATA state");
static_assert(offsetof(HELI_DATA, rotors) == 0x98, "HELI_DATA rotors");
static_assert(offsetof(HELI_DATA, mass) == 0x100, "HELI_DATA mv record");
static_assert(offsetof(HELI_DATA, heading) == 0x128, "HELI_DATA heading");
static_assert(offsetof(HELI_DATA, yawDeg) == 0x134, "HELI_DATA yaw");
static_assert(offsetof(HELI_DATA, enginePower) == 0x184, "HELI_DATA engine power");
static_assert(offsetof(HELI_DATA, engineEnabled) == 0x214, "HELI_DATA engine flag");

namespace {

IHelicopterHost* g_helicopterHost = nullptr;

constexpr float kDegToRad = 0.0017453292f;   // pi / 1800: the angle fields are stored in 0.1-degree units
constexpr float kModelScale = 0.3f;          // world units per rotor unit (1/0.3 is the inverse)
constexpr float kInvModelScale = 1.0f / kModelScale;

// HELI_DATA::rotors[] indices (the original addresses +0x98..+0xe0 in steps of 8).
constexpr int kRotor0x98 = 0;
constexpr int kRotorMain = 1;   // +0xa0
constexpr int kRotorA8 = 2;
constexpr int kRotorB0 = 3;
constexpr int kRotorB8 = 4;
constexpr int kRotorC0 = 5;
constexpr int kRotorC8 = 6;
constexpr int kRotorD0 = 7;
constexpr int kRotorD8 = 8;
constexpr int kRotorE0 = 9;

// The kInvalidVector sentinel (0xffffffff / 0x01ffffff / 0x4f5f4090, see Aircraft.cpp).
cS3DVector3 InvalidVector() {
    const std::uint32_t bits[3] = {kHeliInvalidVectorBits[0], kHeliInvalidVectorBits[1],
                                   kHeliInvalidVectorBits[2]};
    cS3DVector3 out;
    std::memcpy(&out.x, &bits[0], 4);
    std::memcpy(&out.y, &bits[1], 4);
    std::memcpy(&out.z, &bits[2], 4);
    return out;
}

bool SameVector(const cS3DVector3& a, const cS3DVector3& b) {
    // The original compares each component with the mask idiom (|a-b| < 0.0001) and treats NaN as different.
    return !std::isnan(a.x) && !std::isnan(a.y) && !std::isnan(a.z) &&
           std::fabs(a.x - b.x) < 0.0001f && std::fabs(a.y - b.y) < 0.0001f &&
           std::fabs(a.z - b.z) < 0.0001f;
}

IAircraftHost* HostFor(const cSC4Aircraft& aircraft) {
    IAircraftHost* host = aircraft.AircraftHostPtr();
    return host ? host : AircraftHost();
}

IHelicopterHost* HeliHostFor(const cSC4Helicopter& helicopter) {
    IHelicopterHost* host = helicopter.HelicopterHostPtr();
    return host ? host : HelicopterHost();
}

// The original's `min(value, limit)` with the -limit branch (NaN keeps the positive limit).
float ClampSymmetric(float value, float limit) {
    float result = (value <= limit) ? value : limit;
    if (value < -limit) result = -limit;
    return result;
}

// The original's sign-mask idiom `(0 < v) ? -k*v : k*v` multiplied by v again, i.e. -k*v*|v| (k >= 0).
float NegativeQuadratic(float k, float value) {
    const float scaled = k * value;
    const float signedScaled = (0.0f < value) ? -scaled : scaled;
    return signedScaled * value;
}

float Magnitude(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

cS3DVector3 Normalized(const cS3DVector3& v) {
    const float length = Magnitude(v);
    if (length <= 0.0f) return cS3DVector3{0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / length;
    return cS3DVector3{v.x * inv, v.y * inv, v.z * inv};
}

bool IsFinite(float value) { return std::isfinite(value); }

// The rotor / body transform is the cS3DTransform layout; the base class calls take a cS3DTransform reference.
const cS3DTransform& AsTransform(const HeliTransform& transform) {
    static_assert(sizeof(HeliTransform) == sizeof(cS3DTransform), "transform layouts must match");
    return reinterpret_cast<const cS3DTransform&>(transform);
}

// AI_AimSpotlight / SetOrientation build a quaternion from the model facing (+Z) to a direction. This mirrors the
// original's three special cases (aligned, antiparallel, general) and returns (x, y, z, w) in the original's order.
void QuaternionFromFacingTo(const cS3DVector3& direction, float& qx, float& qy, float& qz, float& qw) {
    // facing = kModelFacing = (0, 0, 1)
    const float dot = direction.z;
    qx = 0.0f;
    qy = 0.0f;
    qz = 0.0f;
    qw = 1.0f;
    if (dot < 0.9999f) {
        if (dot <= -0.9999f) {
            const float facingSquared = 0.0f;
            qx = 1.0f;
            qy = 0.0f;
            qz = 0.0f;
            qw = 0.0f;
            (void)facingSquared;
        } else {
            const float cx = -direction.y;   // cross(facing, direction) = (-dy, dx, 0)
            const float cy = direction.x;
            const float cz = 0.0f;
            const float length = std::sqrt(cx * cx + cy * cy + cz * cz);
            const float invLength = (length != 0.0f) ? 1.0f / length : 0.0f;
            const float half = std::sqrt((1.0f - dot) * 0.5f);
            qx = cx * invLength * half;
            qy = cy * invLength * half;
            qz = cz * invLength * half;
            qw = std::sqrt((dot + 1.0f) * 0.5f);
        }
    }
}

// The rotation matrix the original derives from the three Euler angles (degrees) at +0x134 (yaw), +0x13c (pitch) and
// +0x144 (roll). Row-major, the third column is the model facing (+Z) in world space.
void EulerMatrix(float yawDeg, float pitchDeg, float rollDeg, float (&m)[3][3]) {
    const float yaw = yawDeg * kDegToRad;
    const float pitch = pitchDeg * kDegToRad;
    const float roll = rollDeg * kDegToRad;
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sr = std::sin(roll), cr = std::cos(roll);
    m[0][0] = sy * sr * sp + cy * cp;
    m[0][1] = sy * sr * cp - cy * sp;
    m[0][2] = sy * cr;
    m[1][0] = cr * sp;
    m[1][1] = cp * cr;
    m[1][2] = -sr;
    m[2][0] = sr * sp * cy - sy * cp;
    m[2][1] = sr * cp * cy + sp * sy;
    m[2][2] = cy * cr;
}

// Record 0 of the _S_helitype_data table (the value the static initialiser at 0x1006f2684 leaves there; InitInstance is
// always called with index 0 by the recovered code). The words are stored verbatim: the record mixes ints (+0x00/+0x04/
// +0x08/+0x48/+0x4c) and floats, and the original reads each field with the type its consumer expects.
constexpr std::uint32_t kHeliType0Words[32] = {
    0x00000000u, 0x00000004u, 0x000003e8u, 0x43960000u,   // +0x00: 0, 4, 1000, 300.0f
    0x437a0000u, 0x43960000u, 0x43c80000u, 0x43c80000u,   // +0x10: 250, 300, 400, 400
    0x43c80000u, 0x43c80000u, 0x43480000u, 0x40000000u,   // +0x20: 400, 400, 200, 2
    0x4708b800u, 0x41000000u, 0xc1c80000u, 0x00000000u,   // +0x30: 35000, 8, -25, 0
    0x41a00000u, 0x44fa0000u, 0x0000c350u, 0x000003e8u,   // +0x40: 20, 2000, 50000, 1000
    0x3fa28f5cu, 0x40800000u, 0x3fa28f5cu, 0x40800000u,   // +0x50: 1.27, 4, 1.27, 4
    0x3fc00000u, 0x43700000u, 0x3f333333u, 0x43630000u,   // +0x60: 1.5 (mass), 240, 0.7, 227
    0x40800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,   // +0x70: 4, 1, 1, 1
};

float TypeField(const HELI_TYPE_DATA& type, int byteOffset) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(&type) + byteOffset, sizeof value);
    return value;
}

std::int32_t TypeInt(const HELI_TYPE_DATA& type, int byteOffset) {
    std::int32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(&type) + byteOffset, sizeof value);
    return value;
}

IAirplaneEffect* LookupRegEffect(cSC4Helicopter& helicopter, const char* name) {
    IAutomatonEffects* effects = helicopter.Effects();
    if (!effects) return nullptr;
    return static_cast<IAirplaneEffect*>(effects->FindRegEffect(cRZString(name)));
}

void AssignEffect(IAirplaneEffect*& slot, IAirplaneEffect* value) {
    if (slot == value) return;
    if (value) value->AddRef();
    IAirplaneEffect* old = slot;
    slot = value;
    if (old) old->Release();
}

void ReleaseEffect(IAirplaneEffect*& slot) {
    if (!slot) return;
    IAirplaneEffect* old = slot;
    slot = nullptr;
    old->Release();
}

// The claimed spawn positions the AI reserves (std::list<cS3DVector3> at 0x100b8fdb8); the port keeps one static list.
std::vector<cS3DVector3> g_claimedPositions;

bool ClaimedContains(const cS3DVector3& point) {
    for (const cS3DVector3& claimed : g_claimedPositions) {
        if (SameVector(claimed, point)) return true;
    }
    return false;
}

void ClaimPosition(const cS3DVector3& point) { g_claimedPositions.push_back(point); }

void UnclaimPosition(const cS3DVector3& point) {
    for (std::size_t i = 0; i < g_claimedPositions.size(); ++i) {
        if (SameVector(g_claimedPositions[i], point)) {
            g_claimedPositions.erase(g_claimedPositions.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void ResetClaimIfValid(cS3DVector3& destination) {
    if (SameVector(destination, InvalidVector()) || !std::isfinite(destination.x) ||
        !std::isfinite(destination.y) || !std::isfinite(destination.z)) {
        return;
    }
    UnclaimPosition(destination);
    destination = InvalidVector();
}

bool IsUnset(const cS3DVector3& point) { return SameVector(point, InvalidVector()); }

}  // namespace

IAircraftHost* AircraftHost();
IHelicopterHost* HelicopterHost() { return g_helicopterHost; }
void SetHelicopterHost(IHelicopterHost* host) { g_helicopterHost = host; }

// ---------------------------------------------------------------------------------------------------------------------
// HeliTransform
// ---------------------------------------------------------------------------------------------------------------------
void HeliTransform::ResetIfDirty() {
    if (flags == 0) return;
    rotation[0][0] = 1.0f; rotation[0][1] = 0.0f; rotation[0][2] = 0.0f;
    rotation[1][0] = 0.0f; rotation[1][1] = 1.0f; rotation[1][2] = 0.0f;
    rotation[2][0] = 0.0f; rotation[2][1] = 0.0f; rotation[2][2] = 1.0f;
    translation[0] = translation[1] = translation[2] = 0.0f;
    scale = 1.0f;
    flags = 0;
    ++counter;
}

void HeliTransform::SetRotation(const float (&matrix)[3][3]) {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) rotation[row][column] = matrix[row][column];
    }
    flags |= 4;
    ++counter;
}

void HeliTransform::SetTranslation(const cS3DVector3& value) {
    translation[0] = value.x;
    translation[1] = value.y;
    translation[2] = value.z;
    flags |= 1;
    ++counter;
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------------------------------------------------
cSC4Helicopter::cSC4Helicopter(IAutomatonHost* host, IAircraftHost* aircraftHost, IHelicopterHost* helicopterHost)
    : cSC4Aircraft(host, aircraftHost), helicopterHost_(helicopterHost ? helicopterHost : HelicopterHost()) {
    // +0x30d..+0x318
    positionLoaded_ = false;
    spawnPosition_ = cS3DVector3{0.0f, 0.0f, 0.0f};
    spawnAltitude_ = 0.0f;
    // +0x320..+0x358
    effectSearchlight_ = nullptr;
    effectSpotlight_ = nullptr;
    spotTarget_ = cS3DVector3{0.0f, 0.0f, 0.0f};
    spotAim_ = cS3DVector3{0.0f, -1.0f, 1.0f};   // the original's unnormalised constructor value
    spotOn_ = false;
    effectCloseToGround_ = nullptr;
    effectIndicate_ = nullptr;
    // The original constructs the two cSC4ModelRegEffects sub-objects inline; the port asks the host for them.
    if (helicopterHost_) {
        rotorStill_ = helicopterHost_->CreateEffects();
        rotorSpin_ = helicopterHost_->CreateEffects();
    }
    physics_ = nullptr;
    engine_ = nullptr;
    arrived_ = false;
    playerYawRate_ = 0.0f;
    playerPitchRate_ = 0.0f;
    playerRudderRate_ = 0.0f;
    playerSlowScale_ = 0.0f;
    callNumber_ = 0;
    pad3d6_ = 0;
    dispatchState_ = 0;
    // +0x8c / +0x80: the model facing and the invalid destination.
    orientation_ = kAircraftModelFacing;
    destination_ = InvalidVector();
    // The rotor objects' counters/flags of the HELI_DATA body/blade transforms start at zero (PhysicsInit's identity
    // write is unconditional on a fresh block).
}

cSC4Helicopter::~cSC4Helicopter() {
    // The two rotor collections, then the four effects. HELI_DATA/HELI_ENGINE are only freed by Shutdown (the original
    // destructor does not call PhysicsShutdown) — kept as an original quirk.
    if (rotorSpin_) {
        rotorSpin_->StopAll(1);
        rotorSpin_->Shutdown();
        if (helicopterHost_) helicopterHost_->DestroyEffects(rotorSpin_);
        rotorSpin_ = nullptr;
    }
    if (rotorStill_) {
        rotorStill_->StopAll(1);
        rotorStill_->Shutdown();
        if (helicopterHost_) helicopterHost_->DestroyEffects(rotorStill_);
        rotorStill_ = nullptr;
    }
    ReleaseEffect(effectIndicate_);
    ReleaseEffect(effectCloseToGround_);
    ReleaseEffect(effectSpotlight_);
    ReleaseEffect(effectSearchlight_);
}

// ---------------------------------------------------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006e86d6.
bool cSC4Helicopter::Init() {
    if (IsInitialized()) return true;
    cSC4Aircraft::Init();
    if (rotorStill_) rotorStill_->StopNamed(cRZString("helicoptermissle"), 1);
    if (rotorSpin_) rotorSpin_->StopNamed(cRZString("ufolaser"), 1);
    arrived_ = false;
    if (!positionLoaded_) {
        // Look the "fx_00_helipad" registration point up through the first model's reg-point map and subtract it,
        // rotated by the model facing -> heading quaternion, from the spawn position.
        IAirplaneEffect* unused = nullptr;
        (void)unused;
        IAircraftHost* host = HostFor(*this);
        cS3DVector3 regPoint;
        if (host && host->FindRegPointTransform(Models().empty() ? nullptr : Models()[0], cRZString("fx_00_helipad"), regPoint)) {
            // Quaternion from the model facing to the current orientation, then the original's rotation-matrix
            // application (its variable order is transcribed literally).
            const cS3DVector3 direction = Normalized(orientation_);
            const float oy = direction.y;
            const float ox = direction.x;
            float qz = -oy;
            float qx = ox;
            const float qy = 0.0f;
            const float invLength = 1.0f / std::sqrt(qx * qx + qz * qz);
            float half = 0.0f, qw = 1.0f;
            if (direction.z < 0.9999f) {
                half = std::sqrt((1.0f - direction.z) * 0.5f);
                qw = std::sqrt((direction.z + 1.0f) * 0.5f);
            } else {
                qx = 0.0f;
                qz = 0.0f;
            }
            if (half != 0.0f) {
                qx = ox * invLength * half;
                qz = -oy * invLength * half;
            }
            const float a = qz + qz;            // 2z
            const float b = qx + qx;            // 2x
            const float c = qy * (qy + qy);     // 2y*y
            const float d = (qy + qy) * qw;     // 2y*w
            const float px = regPoint.x, py = regPoint.y, pz = regPoint.z;
            position_.x -= (a * qy + b * qw) * pz + (a * qx - d) * py + (1.0f - (qx * b + c)) * px;
            position_.y -= (b * qy - a * qw) * pz + (1.0f - (c + qz * a)) * py + (d + a * qx) * px;
            position_.z -= (1.0f - (qx * b + qz * a)) * pz + (b * qy + a * qw) * py + (a * qy - b * qw) * px;
        }
        spawnPosition_ = position_;
        spawnAltitude_ = position_.y;
    } else {
        position_ = spawnPosition_;
        spawnAltitude_ = spawnPosition_.y;
    }
    InitInstance(0);
    PhysicsInit(physics_, position_);
    SetOrientation(&orientation_.x);
    // The searchlight comes from the model's registered effects, not from the effects manager.
    AssignEffect(effectSearchlight_, LookupRegEffect(*this, "fx_00_searchlight"));
    IAircraftHost* aircraftHost = HostFor(*this);
    if (aircraftHost) {
        AssignEffect(effectSpotlight_, aircraftHost->CreateEffect(cRZString("copter_spotlight")));
    }
    const float invSqrt2 = 1.0f / std::sqrt(2.0f);
    spotAim_ = cS3DVector3{0.0f, -invSqrt2, invSqrt2};
    spotTarget_ = spawnPosition_;
    if (aircraftHost) {
        AssignEffect(effectCloseToGround_, aircraftHost->CreateEffect(cRZString("heli_closetoground")));
        AssignEffect(effectIndicate_, aircraftHost->CreateEffect(cRZString("aircraftindicate")));
    }
    if (!Models().empty() && rotorStill_ && rotorSpin_) {
        IAutomatonModel* model = Models()[0];
        rotorStill_->SetModel(model);
        rotorSpin_->SetModel(model);
        if (!IsOccupantGroup(kHelicopterGroupSpinning)) {
            rotorStill_->AddRegEffect(cRZString("fx_00_rotor"), cRZString("helibladetsill"));
            rotorSpin_->AddRegEffect(cRZString("fx_00_rotor"), cRZString("heliblade"));
        } else {
            rotorStill_->AddRegEffect(cRZString("fx_00_rotor"), cRZString("mhelibladetsill"));
            rotorSpin_->AddRegEffect(cRZString("fx_00_rotor"), cRZString("mheliblade"));
        }
        rotorStill_->Update();
        rotorSpin_->Update();
        rotorStill_->StartAll(1);
    }
    return true;
}

// @ 0x1006ea340.
bool cSC4Helicopter::Shutdown() {
    // Release the claimed spawn position (the original compares against kInvalidVector component-wise first).
    if (!IsUnset(destination_)) {
        UnclaimPosition(destination_);
        destination_ = InvalidVector();
    }
    const bool result = cSC4Aircraft::Shutdown();
    ReleaseEffect(effectSpotlight_);
    ReleaseEffect(effectSearchlight_);
    ReleaseEffect(effectCloseToGround_);
    ReleaseEffect(effectIndicate_);
    if (rotorStill_) {
        rotorStill_->StopAll(1);
        rotorStill_->Shutdown();
    }
    if (rotorSpin_) {
        rotorSpin_->StopAll(1);
        rotorSpin_->Shutdown();
    }
    PhysicsShutdown();
    return result;
}

// @ 0x1006ea72e.
void cSC4Helicopter::PhysicsShutdown() {
    if (physics_) {
        for (HELI_ROTOR*& rotor : physics_->rotors) {
            delete rotor;
            rotor = nullptr;
        }
        delete physics_->type;
        physics_->type = nullptr;
        delete physics_;
        physics_ = nullptr;
    }
    delete engine_;
    engine_ = nullptr;
}

// @ 0x1006e9054. Builds HELP_DATA and the ten rotors (the original creates them in this exact order).
void cSC4Helicopter::InitInstance(int typeIndex) {
    if (physics_) return;
    (void)typeIndex;   // the recovered code only ever passes 0; the other type records are not reached
    auto* data = new HELI_DATA();
    physics_ = data;
    auto* type = new HELI_TYPE_DATA();
    std::memcpy(type->words, kHeliType0Words, sizeof(type->words));
    // The copy constructor the original uses zeroes the first word (see HELI_TYPE_DATA).
    type->words[0] = 0;
    data->type = type;
    LoadPlayerDriveParameters(type);
    LoadDriveParameters(type);
    data->flags = 0;   // +0x0c = 0
    // The rotor creation order (0xa0 first), then the axis ids. 0xc8's axis is never stamped (quirk).
    static const int kOrder[10] = {kRotorMain, kRotor0x98, kRotorC8, kRotorD0, kRotorD8,
                                   kRotorA8, kRotorB0, kRotorB8, kRotorE0, kRotorC0};
    for (int index : kOrder) {
        data->rotors[index] = new HELI_ROTOR();
    }
    data->rotors[0]->axis = 0x21;
    data->rotors[1]->axis = 5;
    data->rotors[2]->axis = 0x21;
    data->rotors[3]->axis = 0x21;
    data->rotors[4]->axis = 0x21;
    data->rotors[5]->axis = 0x21;
    data->rotors[7]->axis = 0x21;
    data->rotors[8]->axis = 0x21;
    data->rotors[9]->axis = 0x21;
    data->field204 = 0;
}

// @ 0x1006e9978. Positions/initialises the rotors, builds the two linked lists and the first transforms, and creates
// the engine control block.
void cSC4Helicopter::PhysicsInit(HELI_DATA* data, const cS3DVector3& position) {
    if (!data) return;
    // The main rotor (+0xa0) sits at the position in model units; the others at the world position.
    data->rotors[1]->position = cS3DVector3{position.x * kInvModelScale, position.y * kInvModelScale,
                                                   position.z * kInvModelScale};
    data->rotors[1]->body.ResetIfDirty();
    data->rotors[0]->position = position;
    data->rotors[0]->body.ResetIfDirty();
    data->rotors[6]->body.ResetIfDirty();
    data->rotors[7]->body.ResetIfDirty();
    data->rotors[8]->position = position;
    data->rotors[8]->body.ResetIfDirty();
    data->rotors[2]->position = position;
    data->rotors[2]->body.ResetIfDirty();
    data->rotors[3]->position = position;
    data->body.ResetIfDirty();
    data->rotors[9]->position = position;
    data->rotors[4]->position = position;
    data->blade.ResetIfDirty();
    data->rotors[5]->position = data->rotors[3]->position;
    // Two identical forward links per rotor (0xa0 -> b0 -> b8 -> c0 -> a8 -> c8 -> d0 -> 98 -> d8 -> e0 -> null).
    static const int kChain[11] = {kRotorMain, kRotorB0, kRotorB8, kRotorC0, kRotorA8,
                                   kRotorC8, kRotorD0, kRotor0x98, kRotorD8, kRotorE0, -1};
    for (int i = 0; i < 10; ++i) {
        HELI_ROTOR* rotor = data->rotors[kChain[i]];
        HELI_ROTOR* next = (kChain[i + 1] >= 0) ? data->rotors[kChain[i + 1]] : nullptr;
        rotor->link0 = next;
        rotor->link1 = next;
    }
    // The truncated spawn coordinates (the original casts through int16).
    const auto toShort = [](float value) {
        return static_cast<float>(static_cast<std::int16_t>(static_cast<int>(value)));
    };
    data->startX = toShort(position.x);
    data->currentX = toShort(position.x);
    data->startZ = toShort(position.z);
    data->currentZ = toShort(position.z);
    data->heading = cS3DVector3{0.0f, 0.0f, 1.0f};
    SetHelicopterState(0);
    data->field1f0 = 0x11;
    data->field1f4 = 1;
    data->field1f8 = 1;
    data->field1fc = 0;
    data->field200 = 0;
    data->field204 = 0;
    data->mass = TypeField(*data->type, 0x60);
    data->inertia = TypeField(*data->type, 0x60);
    data->altitudeState = static_cast<std::int32_t>(TypeField(*data->type, 0x4c));
    data->field208 = TypeField(*data->type, 0x40);
    data->flags |= 3;
    // First transform pair: the main rotor's body rotation (identity angles) and its translation, then the same call
    // with the type's rotor tilt for the blade.
    float matrix[3][3];
    EulerMatrix(data->yawDeg, data->pitchDeg, data->rollDeg, matrix);
    data->rotors[1]->body.SetRotation(matrix);
    data->rotors[1]->body.SetTranslation(position);
    SetTransform(AsTransform(data->rotors[1]->blade));
    EulerMatrix(data->yawDeg, data->pitchDeg * TypeField(*data->type, 0x78),
                data->rollDeg * TypeField(*data->type, 0x7c), matrix);
    data->rotors[1]->blade.SetRotation(matrix);
    data->rotors[1]->blade.SetTranslation(position);
    SetTransform(AsTransform(data->rotors[1]->blade));
    if (!engine_) {
        engine_ = new HELI_ENGINE();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Streams, placement, model hooks
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006ea8b2. Version 1, the aircraft record, then the spawn position.
bool cSC4Helicopter::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    if (cSC4Aircraft::Write(stream)) {
        stream << spawnPosition_;
    }
    return !stream.GetError();
}

// @ 0x1006ea92c. Only version 0 and 1 are accepted.
bool cSC4Helicopter::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 2) return false;
    if (cSC4Aircraft::Read(stream)) {
        stream >> spawnPosition_;
        positionLoaded_ = true;
    }
    return !stream.GetError();
}

// @ 0x1006ea9c2. Random point in the middle of the city (0.5..0.75 of the cell grid), 200 m above the altitude; the
// heading GetNewDirection computes is discarded (quirk).
bool cSC4Helicopter::PlaceSelf() {
    if (!GetSelfControlling()) return false;
    IHelicopterHost* heliHost = HeliHostFor(*this);
    IAircraftHost* aircraftHost = HostFor(*this);
    if (!heliHost || !aircraftHost) return true;
    const float width = static_cast<float>(aircraftHost->CityCellCountX()) * heliHost->CityCellSizeX();
    const float depth = static_cast<float>(aircraftHost->CityCellCountZ()) * heliHost->CityCellSizeZ();
    cS3DVector3 position{0.0f, 0.0f, 0.0f};
    cS3DVector3 direction{0.0f, 0.0f, 0.0f};
    position.x = static_cast<float>(aircraftHost->RandomDoubleRange(static_cast<double>(width * 0.5f),
                                                                   static_cast<double>(width * 3.0f * 0.25f)));
    position.z = static_cast<float>(aircraftHost->RandomDoubleRange(static_cast<double>(depth * 0.5f),
                                                                   static_cast<double>(depth * 3.0f * 0.25f)));
    position.y = GetAltitude(position) + 200.0f;
    GetNewDirection(position, direction);   // result discarded (quirk)
    SetPosition(&position.x);
    return true;
}

// @ 0x1006eab34. A random point in the city with the altitude of the higher of the current position and the terrain
// + 100, returned as a unit direction from `position`.
void cSC4Helicopter::GetNewDirection(const cS3DVector3& position, cS3DVector3& direction) {
    IHelicopterHost* heliHost = HeliHostFor(*this);
    IAircraftHost* aircraftHost = HostFor(*this);
    if (!heliHost || !aircraftHost) {
        direction = cS3DVector3{0.0f, 0.0f, 0.0f};
        return;
    }
    const float width = static_cast<float>(aircraftHost->CityCellCountX()) * heliHost->CityCellSizeX();
    const float depth = static_cast<float>(aircraftHost->CityCellCountZ()) * heliHost->CityCellSizeZ();
    direction.x = static_cast<float>(aircraftHost->RandomDoubleRange(0.0, static_cast<double>(width)));
    direction.z = static_cast<float>(aircraftHost->RandomDoubleRange(0.0, static_cast<double>(depth)));
    const float ground = GetAltitude(direction);
    direction.y = (position.y < ground + 100.0f) ? ground + 100.0f : position.y;
    direction.x -= position.x;
    direction.y -= position.y;
    direction.z -= position.z;
    const float length = Magnitude(direction);
    if (length > 0.0f) {
        const float inv = 1.0f / length;
        direction.x *= inv;
        direction.y *= inv;
        direction.z *= inv;
    }
}

// @ 0x1006eacaa.
void cSC4Helicopter::LoadModels() { cSC4Automaton::LoadModels(); }

// ---------------------------------------------------------------------------------------------------------------------
// Ticks
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006eacc0.
bool cSC4Helicopter::DoAnimationTick(std::uint32_t data, float milliseconds) {
    UpdateOpacity(milliseconds);
    UpdateAnimation(milliseconds);
    DoStateTick(milliseconds);
    UpdateActiveSound(milliseconds);
    bool useSimSpeed = !IsPlayerDriving();
    tAutomataState* pursuit = FindState(kStatePursuit);
    if (pursuit) {
        // The pursuit record's target automaton (+0x38) is player-driven: no simulation speed scaling.
        (void)pursuit;
        useSimSpeed = false;
    }
    float delta = milliseconds * 0.001f;
    if (useSimSpeed) {
        delta *= Host() ? Host()->SimSpeedModifier() : 1.0f;
    }
    if (GetBehavior() != kStatePlayerDrive) {
        float step = (delta <= 0.2f) ? delta : 0.2f;
        AI_PursueGoal(step);
        UpdatePhysics(step);
        UpdateLighting();
        UpdateEffects();
        UpdateSpotlight();
        FinishMove(true, true);
    }
    UpdateGhostModel();
    (void)data;
    return true;
}

// @ 0x1006f0222. The player-drive tick: the base tick, then one physics step and the spotlight update.
bool cSC4Helicopter::DoPlayerDriveTick(float milliseconds) {
    float step = milliseconds * 0.001f;
    if (step > 0.2f) step = 0.2f;
    const cS3DVector3 start = position_;
    tAutomataState* state = GetState();
    TranslatePlayerDriveCommands(static_cast<tAutomataStatePlayerDrive*>(state), milliseconds);
    UpdatePhysics(step);
    UpdateLighting();
    UpdateEffects();
    UpdateSpotlight();
    FinishMove(true, true);
    const cS3DVector3 moved{position_.x - start.x, position_.y - start.y, position_.z - start.z};
    actualSpeed_ = Magnitude(moved) / milliseconds;   // +0xc8
    if (state) {
        IAircraftHost* host = HostFor(*this);
        if (host && state->stateType == kStatePlayerDrive) {
            auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
            host->WorldToCell(position_.x, position_.z, drive->cellX298, drive->cellZ29c);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Parameters and engine state
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006ecb6a. Reads the player-drive block (+0xe8) of the group template; zero entries keep the current value.
void cSC4Helicopter::LoadPlayerDriveParameters(HELI_TYPE_DATA* type) {
    if (!type) return;
    IAircraftHost* host = HostFor(*this);
    if (!host) return;
    const float* values = host->FindPlayerDriveParameters(GetAutomataGroup());
    if (!values) return;
    const float collective = (values[2] * 1000.0f) / 0.3f;
    if (collective != 0.0f) {
        float converted = collective;
        std::memcpy(&type->words[0x6c / 4], &converted, sizeof converted);
    }
    if (values[0x18] != 0.0f) playerYawRate_ = values[0x18];      // +0x3c4
    if (values[0x19] != 0.0f) playerPitchRate_ = values[0x19];    // +0x3c8
    if (values[0x1a] != 0.0f) playerRudderRate_ = values[0x1a];   // +0x3cc
    if (values[0x1d] != 0.0f) playerSlowScale_ = values[0x1d];    // +0x3d0
}

// @ 0x1006ecc6a. Reads the 13-float drive block (+0xe0); zero entries keep the default.
void cSC4Helicopter::LoadDriveParameters(HELI_TYPE_DATA* type) {
    if (!type) return;
    IAircraftHost* host = HostFor(*this);
    if (!host) return;
    const float* values = host->FindDriveParameters(GetAutomataGroup());
    if (!values) return;   // no block: the static type record keeps its defaults
    const auto set = [&type](int byteOffset, float value) {
        if (value != 0.0f) std::memcpy(reinterpret_cast<std::uint8_t*>(type) + byteOffset, &value, sizeof value);
    };
    set(0x60, values[0]);
    set(0x64, values[1]);
    set(0x68, values[2]);
    set(0x6c, (values[3] * 1000.0f) / 0.3f);
    set(0x70, values[4]);
    set(0x74, values[5]);
    set(0x0c, values[6] * 572.9578f);
    set(0x24, values[7] * 572.9578f);
    set(0x14, values[8] * 572.9578f);
    set(0x1c, values[9] * 572.9578f);
    set(0x20, values[10] * 572.9578f);
    set(0x78, values[0x0b]);
    set(0x7c, values[0x0c]);
}

// @ 0x1006ece66. The state machine entry: resets the control rates, drives the two rotor effect collections and the
// active-sound flag, and plays the player-drive audio events.
void cSC4Helicopter::SetHelicopterState(std::uint32_t state) {
    if (!physics_) return;
    switch (state) {
        case 0:
            physics_->engineEnabled = 0;   // +0x214
            arrived_ = false;
            // The original normalises the two velocity vectors with a 0.0 factor, i.e. zeroes them.
            physics_->velX = physics_->velY = physics_->velZ = 0.0f;
            physics_->pitchRate = physics_->yawRate = physics_->rollRate = 0.0f;
            if (IsPlayerDriving()) {
                if (IAircraftHost* host = HostFor(*this)) {
                    host->PlayAudioEvent(9);
                    host->PlayAudioEvent(8);
                    host->PlayAudioEvent(10);
                    host->PlayAudioEvent(10);
                }
            }
            break;
        case 1:
            physics_->engineEnabled = 1;
            break;
        case 4:
            physics_->engineEnabled = 1;
            if (IsPlayerDriving()) {
                if (IAircraftHost* host = HostFor(*this)) host->PlayAudioEvent(9);
            }
            break;
        default:
            break;
    }
    physics_->state = static_cast<std::int32_t>(state);
    EnableActiveSound(physics_->engineEnabled != 0);
    if (physics_->engineEnabled != 0) {
        if (rotorSpin_) rotorSpin_->StartAll(0);
        if (rotorStill_) rotorStill_->StopAll(1);
    } else {
        if (rotorSpin_) rotorSpin_->StopAll(1);
        if (rotorStill_) rotorStill_->StartAll(0);
    }
}

// @ 0x1006ed0ac.
void cSC4Helicopter::NextFrame(HELI_DATA* data, float delta) {
    if (!data || !data->type) return;
    const HELI_TYPE_DATA& type = *data->type;
    const int variant = TypeInt(type, 0x04);
    const int engineCount = TypeInt(type, 0x08) + 0x1e + variant * 0x78;
    const int count = (variant < 3) ? variant : 2;
    data->flightRatio = (static_cast<float>(engineCount) -
                         (static_cast<float>(count * 0x78 + data->field204))) /
                        static_cast<float>(engineCount);
    data->field1fc = 0;
    data->field200 = 0;
    data->field188 = 0.0f;
    const auto land = [data]() {
        data->state = static_cast<std::int32_t>(HelicopterState::kLanded);
        data->field1a8 = 0;
    };
    if ((data->flags & 1) == 0) {
        if (data->altitudeState < 0) {
            if (data->state == static_cast<std::int32_t>(HelicopterState::kLanded)) return;
            land();
            return;
        }
        if (data->state == static_cast<std::int32_t>(HelicopterState::kLanded)) return;
    } else {
        HELI_ROTOR* rotor = data->rotors[1];
        position_ = cS3DVector3{rotor->position.x * kModelScale, rotor->position.y * kModelScale,
                                rotor->position.z * kModelScale};
        const float altitude = GetAltitude(position_);
        if (data->state == static_cast<std::int32_t>(HelicopterState::kLanded)) return;
        if (data->state == static_cast<std::int32_t>(HelicopterState::kDescending)) {
            if (position_.y <= altitude && altitude != position_.y) {
                land();
                return;
            }
        } else if (data->altitudeState < 0) {
            data->state = static_cast<std::int32_t>(HelicopterState::kDescending);
            data->landingTimer = 0.8f;   // 0x3f4ccccd
        }
    }
    CalculateEnginePower(data, delta);
    YawPitchRollSlideReal(data, delta);
    MoveXZReal(data, delta);
    MoveYReal(data, delta);
    const float x = data->rotors[1]->position.x;
    data->currentX = x;
    const float z = data->rotors[1]->position.z;
    data->currentZ = z;
    const bool same = data->startX == x && !std::isnan(data->startX) && !std::isnan(x) &&
                      data->startZ == z && !std::isnan(data->startZ) && !std::isnan(z);
    if (!same) {
        data->startX = x;
        data->startZ = z;
    }
    if (data->state == static_cast<std::int32_t>(HelicopterState::kHover)) data->field20c += delta;
}

// @ 0x1006ed36a.
void cSC4Helicopter::CalculateEnginePower(HELI_DATA* data, float delta) {
    if (!data) return;
    const HELI_TYPE_DATA& type = *data->type;
    if (data->engineEnabled == 0) {
        float next = data->enginePower + delta * -100.0f;
        float clamped = (next <= 400.0f) ? next : 400.0f;
        if (next < 0.0f) clamped = 0.0f;
        data->enginePower = clamped;
    } else if (data->enginePower < 400.0f) {
        float next = data->enginePower + delta * 100.0f;
        float clamped = (next <= 400.0f) ? next : 400.0f;
        if (next < 0.0f) clamped = 0.0f;
        data->enginePower = clamped;
    }
    if (!engine_) return;
    engine_->throttle = (engine_->liftControl + 100.0f) / 200.0f;
    engine_->power = 0.0f;
    if (data->engineEnabled != 0) {
        const float ratio = data->enginePower / 400.0f;
        const float clamped = (ratio <= 1.0f) ? ratio : 1.0f;
        engine_->power = engine_->throttle * TypeField(type, 0x64) * ((ratio < 0.0f) ? 0.0f : clamped);
        const float vertical = data->velY;
        float term = vertical * 0.018f * vertical * data->mass;
        if (vertical >= 0.0f) term *= -4.0f;
        engine_->power += term;
        return;
    }
    float term = data->velY * 0.02352f * data->velY * data->mass;
    if (data->rollDeg >= 0.0f || data->rollDeg == 0.0f || data->speed <= 10.0f) {
        const float tilt = (engine_->throttle <= 1.0f) ? engine_->throttle : 1.0f;
        engine_->throttle = (tilt < 0.0f) ? 0.0f : tilt;
        term *= 1.0f - engine_->throttle;
    } else {
        term *= engine_->throttle * 4.0f;
    }
    engine_->power = term + engine_->power;
}

// ---------------------------------------------------------------------------------------------------------------------
// Flight model
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006ed524. The angular integration: the weathervane/transverse force and the rudder drive the three rates, which
// are clamped by the type limits and integrated into yaw/pitch/roll; the same euler angles rebuild the rotor matrices.
void cSC4Helicopter::YawPitchRollSlideReal(HELI_DATA* data, float delta) {
    if (!data || !data->type || !engine_) return;
    const HELI_TYPE_DATA& type = *data->type;
    if (data->state == static_cast<std::int32_t>(HelicopterState::kDescending) ||
        data->state == static_cast<std::int32_t>(HelicopterState::kLanded)) {
        return;
    }
    HELI_ROTOR* rotor = data->rotors[1];
    float matrix[3][3];
    if (data->state == static_cast<std::int32_t>(HelicopterState::kIdle)) {
        data->pitchClamped = 0.0f;   // +0x154
        data->verticalSpeed = 0.0f;  // +0x158
        data->pad150 = 0;            // +0x150
        data->rollDeg = 0.0f;        // +0x144
        data->rollClamped = 0.0f;    // +0x148
        data->pitchDeg = 0.0f;       // +0x13c
        data->field140 = 0.0f;       // +0x140
        EulerMatrix(data->yawDeg, data->pitchDeg, data->rollDeg, matrix);
        rotor->body.SetRotation(matrix);
    } else {
        // The engine's up axis: -collective / 1000 / -field04, normalised.
        engine_->axisX = -engine_->collective;
        engine_->axisY = 1000.0f;
        engine_->axisZ = -engine_->field04;
        const cS3DVector3 up = Normalized(cS3DVector3{engine_->axisX, engine_->axisY, engine_->axisZ});
        engine_->axisX = up.x;
        engine_->axisY = up.y;
        engine_->axisZ = up.z;
        cS3DVector3 force{0.0f, 0.0f, 0.0f};
        WeathervaneTransverseForce(data, &force);
        const float scale = TypeField(type, 0x64);
        const float a = force.x + (engine_->axisZ * scale * 24.0f - data->rollDeg * 0.5f * scale * 16.0f);
        const float b = force.z + (engine_->axisX * scale * 24.0f - data->pitchDeg * 0.5f * scale * 14.0f);
        const float rollTerm = b * 0.25f + a;
        const float pitchTerm = a * 0.25f + b;
        if (rollTerm != 0.0f) {
            const float k = TypeField(type, 0x74) * 0.025f;
            float rate = data->pitchRate;   // +0x11c
            const float damping = NegativeQuadratic(k, rate);
            const float guess = ((rollTerm + damping) / data->inertia) * delta + rate;
            const float updated =
                (((NegativeQuadratic(k, guess) + damping) * 0.5f + rollTerm) / data->inertia) * delta + rate;
            if (IsFinite(updated)) {
                data->pitchRate = updated;
                rate = updated;
            }
            rate = ClampSymmetric(rate, TypeField(type, 0x1c));
            data->pitchRate = rate;
            data->rollDeg += rate * delta;
        } else {
            data->pitchRate = 0.0f;
        }
        data->rollDeg += 0.0f;
        if (pitchTerm != 0.0f) {
            const float k = TypeField(type, 0x74) * 0.009f;
            float rate = data->rollRate;   // +0x124
            const float damping = NegativeQuadratic(k, rate);
            const float guess = ((pitchTerm + damping) / data->inertia) * delta + rate;
            const float updated =
                (((NegativeQuadratic(k, guess) + damping) * 0.5f + pitchTerm) / data->inertia) * delta + rate;
            if (IsFinite(updated)) {
                data->rollRate = updated;
                rate = updated;
            }
            rate = ClampSymmetric(rate, TypeField(type, 0x24));
            data->rollRate = rate;
            data->pitchDeg += rate * delta;
        } else {
            data->rollRate = 0.0f;
        }
        data->pitchDeg += 0.0f;
        // Yaw: rudder^3 * 50 * scale + the transverse force's y, opposed by tilt * pitch.
        const float rudder = engine_->rudder;   // +0x0c
        float moment = (rudder * 0.01f * rudder * 0.01f * rudder * 0.01f * 50.0f * delta * scale + force.y) -
                       engine_->tilt * data->pitchDeg;
        const float ratio = (delta <= 1.0f) ? delta : 1.0f;
        const float k = TypeField(type, 0x74) *
                        ((delta < 0.0f) ? 0.014f : (1.0f - ratio) * 0.01f + 0.004f);
        float rate = data->yawRate;   // +0x120
        const float damping = NegativeQuadratic(k, rate);
        const float guess = ((moment + damping) / data->inertia) * delta + rate;
        const float updated = (((NegativeQuadratic(k, guess) + damping) * 0.5f + moment) / data->inertia) * delta + rate;
        if (IsFinite(updated)) {
            data->yawRate = updated;
            rate = updated;
        }
        rate = ClampSymmetric(rate, TypeField(type, 0x20));
        data->yawRate = rate;
        float yaw = data->yawDeg + rate * delta + 0.0f;
        data->yawDeg = yaw;
        if (yaw > 3600.0f) {
            yaw -= 3600.0f;
            data->yawDeg = yaw;
        } else if (yaw < 0.0f) {
            yaw += 3600.0f;
            data->yawDeg = yaw;
        }
        // Roll and pitch: first the hard +/-890 limit, then the type limit.
        float roll = data->rollDeg;
        if (roll > 890.0f) roll = 890.0f;
        if (roll < -890.0f) roll = -890.0f;
        roll = ClampSymmetric(roll, TypeField(type, 0x14));
        data->rollDeg = roll;
        float pitch = data->pitchDeg;
        if (pitch > 890.0f) pitch = 890.0f;
        if (pitch < -890.0f) pitch = -890.0f;
        pitch = ClampSymmetric(pitch, TypeField(type, 0x0c));
        data->pitchDeg = pitch;
        data->rollClamped = roll;      // +0x148
        data->pitchClamped = pitch;    // +0x154
        EulerMatrix(data->yawDeg, data->pitchDeg, data->rollDeg, matrix);
        rotor->body.SetRotation(matrix);
    }
    // The blade transform always carries the type's rotor tilt on the pitch/roll angles.
    EulerMatrix(data->yawDeg, data->pitchDeg * TypeField(type, 0x78),
                data->rollDeg * TypeField(type, 0x7c), matrix);
    rotor->blade.SetRotation(matrix);
}

// @ 0x1006ee136.
void cSC4Helicopter::MoveXZReal(HELI_DATA* data, float delta) {
    if (!data || !data->type || !engine_) return;
    const float startSpeed = data->speedXZ;
    if (data->state == static_cast<std::int32_t>(HelicopterState::kDescending)) return;
    if (data->state == static_cast<std::int32_t>(HelicopterState::kLanded)) {
        data->moveX = 0.0f;
        data->moveZ = 0.0f;
        return;
    }
    const HELI_TYPE_DATA& type = *data->type;
    HELI_ROTOR* rotor = data->rotors[1];
    const cS3DVector3 heading = Normalized(orientation_);
    data->heading = heading;
    cS3DVector3 up = Normalized(cS3DVector3{rotor->body.rotation[0][1], rotor->body.rotation[1][1],
                                            rotor->body.rotation[2][1]});
    engine_->upX = up.x;
    engine_->upY = up.y;
    engine_->upZ = up.z;
    float factor = 1.0f;
    if (data->engineEnabled == 1 && data->speedXZ < 120.0f) {
        factor = (data->speedXZ / 120.0f) * 0.65f + 0.35f;
    }
    engine_->forceX = engine_->power * up.x * factor;
    engine_->forceY = engine_->power * up.y * factor;
    engine_->forceZ = engine_->power * up.z * factor;
    engine_->forceX += (1.0f - TypeField(type, 0x68)) * std::fabs(heading.x) * engine_->forceX;
    engine_->forceZ += (1.0f - TypeField(type, 0x68)) * std::fabs(heading.z) * engine_->forceZ;
    engine_->forceY += data->mass * -32.0f;   // gravity
    float height = rotor->position.y - data->obstacleAltitude;
    if (height <= 0.0f) height = 0.0f;
    if (height < 100.0f) {
        engine_->forceY += data->mass * 8.0f * ((100.0f - height) / 100.0f);
    }
    cS3DVector3 drag{0.0f, 0.0f, 0.0f};
    Get3dDrag(engine_->throttle, data, &drag, data->speedXZ);
    const float dragScale = TypeField(type, 0x70);
    drag.x *= dragScale;
    drag.y *= dragScale;
    drag.z *= dragScale;
    const float lift = LiftCoeff(data) * data->speedOut * data->speedOut * data->mass;
    engine_->forceX += up.x * lift;
    engine_->forceY += up.y * lift;
    engine_->forceZ += up.z * lift;
    ApplyForce(cS3DVector3{engine_->forceX, engine_->forceY, engine_->forceZ}, &data->mass, delta, drag,
               &data->speedOut);
    float velX = data->velX;
    float velZ = data->velZ;
    const float speedXZ = std::sqrt(velZ * velZ + velX * velX);
    data->speedXZ = speedXZ;
    if (speedXZ > 0.0f) {
        const float inv = 1.0f / speedXZ;
        velX *= inv;
        velZ *= inv;
    }
    if (data->diveTimer > 0) {
        data->diveTimer = static_cast<std::int32_t>(static_cast<float>(data->diveTimer) - delta);
        data->speedXZ = data->rollDeg * 0.125f;
    }
    data->speedDelta = data->speedXZ - startSpeed;
    float speed = data->speedXZ;
    data->speed = (velZ * heading.z + velX * heading.x) * speed;   // +0x104
    engine_->tilt = data->speed * 0.0002f;
    engine_->tilt = data->speed * engine_->tilt + engine_->tilt;
    if (engine_->tilt > 1.0f) engine_->tilt = 1.0f;
    float vx = data->velX;
    float vy = data->velY;
    float vz = data->velZ;
    const float magnitudeSquared = vy * vy + vx * vx + vz * vz;
    const float magnitude = std::sqrt(magnitudeSquared);
    if (TypeField(type, 0x6c) < magnitude && magnitudeSquared > 0.0f) {
        const float inv = TypeField(type, 0x6c) / magnitude;
        vx *= inv;
        data->velX = vx;
        vy *= inv;
        data->velY = vy;
        vz *= inv;
        data->velZ = vz;
    }
    data->moveX = vx * delta;    // +0x168
    data->moveZ = vz * delta;    // +0x16c
    float moveZ = vz * delta;
    if (data->state == static_cast<std::int32_t>(HelicopterState::kIdle)) {
        data->speedXZ = 0.0f;
        data->pad14c = 0;
        data->pad150 = 0;
        data->pitchClamped = 0.0f;
        data->moveX = 0.0f;
        data->moveZ = 0.0f;
        moveZ = 0.0f;
    }
    rotor->position.z += moveZ;
    rotor->position.x += data->moveX;
}

// @ 0x1006ee796.
void cSC4Helicopter::MoveYReal(HELI_DATA* data, float delta) {
    if (!data) return;
    const float altitude = GetAltitude(position_);
    data->groundAltitude = altitude * kInvModelScale;    // +0x18c
    data->obstacleAltitude = altitude * kInvModelScale;  // +0x190
    data->field174 = 0;
    IAircraftHost* host = HostFor(*this);
    IHelicopterHost* heliHost = HeliHostFor(*this);
    float surface = altitude;
    if (!host) {
        surface = 0.0f;
    } else if (!host->TerrainPointInBounds(position_.x, position_.z)) {
        surface = host->DefaultAltitude();
    } else {
        if (heliHost) {
            IHelicopterObstacle* obstacle = heliHost->FindObstacleAt(position_.x, position_.z, kHelicopterOccupantType);
            if (obstacle) {
                cS3DVector3 minCorner, maxCorner;
                obstacle->GetBounds(minCorner, maxCorner);
                data->obstacleAltitude = minCorner.y * kInvModelScale;
                obstacle->Release();
            }
        }
        surface = host->WaterHeight(position_.x, position_.z);
    }
    data->grounded = (altitude < surface) ? 1 : 0;   // +0x1ac
    if (data->state == static_cast<std::int32_t>(HelicopterState::kDescending) ||
        data->state == static_cast<std::int32_t>(HelicopterState::kLanded)) {
        return;
    }
    float vertical = data->velY;
    data->verticalSpeed = vertical;   // +0x15c
    data->direction = (vertical > 0.0f) ? 1 : (vertical < 0.0f ? -1 : 0);
    const bool engineIdle = data->engineEnabled != 1 || data->enginePower >= 400.0f;
    if (!engineIdle) {
        // The engine is spinning up: no vertical motion.
        data->verticalSpeed = 0.0f;
        data->velY = 0.0f;
        return;
    }
    if (data->state == static_cast<std::int32_t>(HelicopterState::kIdle)) {
        if (data->engineEnabled == 0) {
            data->verticalSpeed = 0.0f;
            data->velY = 0.0f;
            return;
        }
        if (vertical < 0.0f) {
            data->verticalSpeed = 0.0f;
            data->velY = 0.0f;
            vertical = 0.0f;
        }
    }
    data->velY = vertical;
    data->rotors[1]->position.y += delta * vertical;
    data->heightAboveGround = data->rotors[1]->position.y - data->groundAltitude;
    if (data->state == static_cast<std::int32_t>(HelicopterState::kIdle) && data->verticalSpeed > 0.0f) {
        data->rollDeg = 0.0f;
        data->pitchDeg = 0.0f;
        data->field140 = 0.0f;
    }
}

// @ 0x1006ee9ee.
void cSC4Helicopter::WeathervaneTransverseForce(HELI_DATA* data, cS3DVector3* force) {
    if (!data || !force) return;
    const float (&rotation)[3][3] = data->rotors[1]->body.rotation;
    cS3DVector3 velocity{data->velX, data->velY, data->velZ};
    const float speed = Magnitude(velocity);
    if (speed > 0.0f) {
        const float inv = 1.0f / speed;
        velocity.x *= inv;
        velocity.y *= inv;
        velocity.z *= inv;
    }
    const float along = rotation[0][2] * velocity.z + rotation[1][2] * velocity.y + rotation[2][2] * velocity.x;
    float damping = std::fabs(along) + 1.0f;
    const float transverse = rotation[2][0] * velocity.z + rotation[1][0] * velocity.y + rotation[0][0] * velocity.x;
    force->y = 0.0f;
    if (transverse != 0.0f) {
        damping = speed * 0.1f * speed * damping;
        force->y = (transverse > 0.0f) ? -damping : damping;
    }
    const float term = speed * 0.011f * speed * along;
    force->x = -term;
    force->z = term * 0.5f + speed * -0.009f * speed * transverse;
}

// @ 0x1006eeb90.
void cSC4Helicopter::Get3dDrag(float throttle, HELI_DATA* data, cS3DVector3* drag, float altitude) {
    if (!data || !drag) return;
    const float (&rotation)[3][3] = data->rotors[1]->body.rotation;
    float low = 0.0f;
    if (altitude < 100.0f) low = (100.0f - altitude) / 100.0f;
    const float lateral = low * 0.001f + 0.005f;
    const float blunt = low * 0.0009f + 0.0005f;
    const float front = low * 0.001f + 0.04f;
    const float dragLateral = (lateral * throttle) + low * 0.005f + 0.004f;
    const float dragBlunt = (blunt * throttle) + low * 0.0003f + 0.0001f;
    const float dragFront = (front * throttle) + low * 0.01f + 0.003f;
    float x = std::fabs(rotation[0][1]) * dragFront * 0.25f;
    float z = std::fabs(rotation[2][1]) * dragFront * 0.25f;
    float y = std::fabs(rotation[1][1]) * dragFront;
    if (data->engineEnabled == 0 && data->rollDeg < 0.0f && data->speed > 5.0f) {
        const float dive = (data->rollDeg / 900.0f) * -14.0f;
        x += x * dive;
        y += y * dive + y * dive;
        z += dive * z;
    }
    drag->x = std::fabs(rotation[0][2]) * dragBlunt + std::fabs(rotation[0][0]) * dragLateral + x;
    drag->y = std::fabs(rotation[1][2]) * dragBlunt + std::fabs(rotation[1][0]) * dragLateral + y;
    drag->z = std::fabs(rotation[2][2]) * dragBlunt + std::fabs(rotation[2][0]) * dragLateral + z;
}

// @ 0x1006eed5a.
float cSC4Helicopter::LiftCoeff(HELI_DATA* data) {
    if (!data || !engine_) return 0.0f;
    cS3DVector3 velocity{data->velX, data->velY, data->velZ};
    const float speed = Magnitude(velocity);
    if (speed > 0.0f) {
        const float inv = 1.0f / speed;
        velocity.x *= inv;
        velocity.y *= inv;
        velocity.z *= inv;
    }
    const float dot = engine_->upX * velocity.x + engine_->upY * velocity.y + engine_->upZ * velocity.z;
    float factor = (1.0f - dot) * 0.5f;
    if (factor <= 0.45f) {
        factor = (factor / 0.45f) * 0.5f - 0.5f;
    } else if (factor <= 0.7f) {
        factor = ((factor - 0.45f) / 0.35f) * 0.2f;
    } else {
        factor = ((factor - 0.7f) / -0.3f) * 0.2f + 0.2f;
    }
    return factor * 0.015625f;
}

// @ 0x1006eee70. `massVelocity` points at HELI_DATA::mass (the struct: mass, speed, velocity x/y/z).
void cSC4Helicopter::ApplyForce(const cS3DVector3& force, float* massVelocity, float delta,
                                const cS3DVector3& drag, float* speedOut) {
    if (!massVelocity) return;
    float* velocity = massVelocity + 2;   // +0x08 of the mv record
    const float vx = velocity[0];
    const float vy = velocity[1];
    const float vz = velocity[2];
    const float speed = std::sqrt(vz * vz + vy * vy + vx * vx);
    const float dragScale = speed * 0.2f;
    const float invMass = 1.0f / massVelocity[0];
    velocity[0] = (force.x - drag.x * vx * dragScale) * invMass * delta + vx;
    velocity[1] = (force.y - drag.y * vy * dragScale) * invMass * delta + vy;
    velocity[2] = (force.z - drag.z * vz * dragScale) * invMass * delta + vz;
    if (speedOut) *speedOut = speed;
}

// @ 0x1006ebe98.
void cSC4Helicopter::UpdatePhysics(float delta) {
    if (!physics_) return;
    HELI_DATA* data = physics_;
    if (!arrived_) {
        NextFrame(data, delta);
        HELI_ROTOR* rotor = data->rotors[1];
        position_ = cS3DVector3{rotor->position.x * kModelScale, rotor->position.y * kModelScale,
                                rotor->position.z * kModelScale};
        orientation_ = Normalized(cS3DVector3{rotor->body.rotation[0][2], rotor->body.rotation[1][2],
                                              rotor->body.rotation[2][2]});
        DetectCollisions();
    } else {
        AI_ControlReset(delta);
        const std::uint32_t behavior = GetBehavior();
        if (behavior == kHeliBehaviorIdle || behavior == 0) {
            const cS3DVector3 toSpawn{spawnPosition_.x - position_.x, spawnPosition_.y - position_.y,
                                      spawnPosition_.z - position_.z};
            const float distance = Magnitude(toSpawn);
            if (distance >= 1.0f) {
                float t = delta / 3.0f;
                if (t > 1.0f) t = 1.0f;
                const float inv = 1.0f - t;
                position_ = cS3DVector3{spawnPosition_.x * t + position_.x * inv,
                                        spawnPosition_.y * t + position_.y * inv,
                                        spawnPosition_.z * t + position_.z * inv};
            } else {
                position_ = spawnPosition_;
                SetHelicopterState(0);
            }
        } else {
            arrived_ = false;
        }
        data->rotors[1]->position =
            cS3DVector3{position_.x * kInvModelScale, position_.y * kInvModelScale, position_.z * kInvModelScale};
    }
    // The NaN guard: if the position or the main rotor's transform is not finite, respawn at the spawn point.
    HELI_ROTOR* rotor = data->rotors[1];
    bool invalid = !IsFinite(position_.x) || !IsFinite(position_.y) || !IsFinite(position_.z);
    for (int i = 0; i < 3 && !invalid; ++i) invalid = !IsFinite(rotor->body.translation[i]);
    for (int row = 0; row < 3 && !invalid; ++row) {
        for (int column = 0; column < 3 && !invalid; ++column) invalid = !IsFinite(rotor->body.rotation[row][column]);
    }
    if (invalid) {
        position_ = spawnPosition_;
        orientation_ = kAircraftModelFacing;
        PhysicsInit(data, position_);
    }
    ClipToWorld(position_);
    rotor->position = cS3DVector3{position_.x * kInvModelScale, position_.y * kInvModelScale,
                                  position_.z * kInvModelScale};
    rotor->body.SetTranslation(position_);
    rotor->blade.SetTranslation(position_);
    SetTransform(AsTransform(rotor->blade));
}

// ---------------------------------------------------------------------------------------------------------------------
// AI control helpers
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006eef38.
void cSC4Helicopter::AI_ControlReset(float delta) {
    if (!engine_ || !physics_) return;
    const float step = delta * 100.0f;
    const float collectiveDelta = ClampSymmetric(0.0f - engine_->collective, step);
    const float fieldDelta = ClampSymmetric(engine_->field04, step) - engine_->field04;
    const float rudderDelta = ClampSymmetric(0.0f - engine_->rudder, step);
    const float liftDelta = ClampSymmetric(-100.0f - engine_->liftControl, step);
    engine_->collective += collectiveDelta;
    engine_->field04 = -fieldDelta;
    engine_->rudder += rudderDelta;
    engine_->liftControl += liftDelta;
    if (!physics_->type) return;
    const HELI_TYPE_DATA& type = *physics_->type;
    float roll = TypeField(type, 0x1c) * 0.025f * physics_->flightRatio * rudderDelta;
    physics_->rollDeg = ClampSymmetric(roll, TypeField(type, 0x14));
    float pitch = TypeField(type, 0x24) * 0.025f * engine_->collective;
    physics_->pitchDeg = ClampSymmetric(pitch, TypeField(type, 0x0c));
}

// @ 0x1006f0174.
void cSC4Helicopter::AI_ControlHover(float targetAltitude, float delta) {
    if (!engine_ || !physics_) return;
    const float vertical = physics_->velY * kModelScale;
    const float current = engine_->liftControl;
    const float step = delta * 100.0f;
    const float aim = ((targetAltitude - (vertical * 0.5f + position_.y)) / 10.0f - vertical) * 0.5f + current;
    const float next = current + ClampSymmetric(aim - current, step);
    float clamped = (next <= 100.0f) ? next : 100.0f;
    if (next < -150.0f) clamped = -150.0f;
    engine_->liftControl = clamped;
}

// @ 0x1006f0354.
void cSC4Helicopter::AI_StartEngine() {
    if (!physics_) return;
    if (physics_->state == static_cast<std::int32_t>(HelicopterState::kStarting)) {
        if (physics_->enginePower >= 400.0f && physics_->velY > 0.0f) SetHelicopterState(1);
    } else if (physics_->state == static_cast<std::int32_t>(HelicopterState::kIdle)) {
        SetHelicopterState(4);
    }
}

// @ 0x1006f03a4.
void cSC4Helicopter::AI_AimSpotlight(const cS3DVector3& target, float delta) {
    if (!effectSearchlight_ || !physics_) return;
    IHelicopterHost* host = HeliHostFor(*this);
    if (!host) return;
    cS3DTransform current;
    host->GetEffectTransform(effectSearchlight_, current);
    const cS3DVector3 origin{current.translation[0], current.translation[1], current.translation[2]};
    // The current aim: the effect transform's rotation applied to the default aim.
    const cS3DVector3 base = kDefaultSpotlightAim;   // (0, -1, 0)
    cS3DVector3 currentAim{
        current.rotation[0][0] * base.x + current.rotation[0][1] * base.y + current.rotation[0][2] * base.z,
        current.rotation[1][0] * base.x + current.rotation[1][1] * base.y + current.rotation[1][2] * base.z,
        current.rotation[2][0] * base.x + current.rotation[2][1] * base.y + current.rotation[2][2] * base.z};
    currentAim = Normalized(currentAim);
    float q1x, q1y, q1z, q1w;
    QuaternionFromFacingTo(currentAim, q1x, q1y, q1z, q1w);
    float q2x, q2y, q2z, q2w;
    const cS3DVector3 direction = Normalized(cS3DVector3{target.x - origin.x, target.y - origin.y,
                                                         target.z - origin.z});
    QuaternionFromFacingTo(direction, q2x, q2y, q2z, q2w);
    float t = delta + delta;
    if (t > 1.0f) t = 1.0f;
    if (q1x * q2x + q1y * q2y + q1z * q2z + q1w * q2w < 0.0f) {
        q2x = -q2x; q2y = -q2y; q2z = -q2z; q2w = -q2w;
    }
    float qx = (q2x - q1x) * t + q1x;
    float qy = (q2y - q1y) * t + q1y;
    float qz = (q2z - q1z) * t + q1z;
    float qw = (q2w - q1w) * t + q1w;
    const float inv = 1.0f / std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    qx *= inv; qy *= inv; qz *= inv; qw *= inv;
    // Build the rotation matrix from the quaternion and apply it to the default aim.
    const float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    const float xx = qx * x2, xy = qx * y2, xz = qx * z2, xw = qx * (qw + qw);
    const float yy = qy * y2, yz = qy * z2, yw = qy * (qw + qw);
    const float zz = qz * z2, zw = qz * (qw + qw);
    const float m00 = 1.0f - (yy + zz), m01 = xy - zw, m02 = xz + yw;
    const float m10 = xy + zw, m11 = 1.0f - (xx + zz), m12 = yz - xw;
    const float m20 = xz - yw, m21 = yz + xw, m22 = 1.0f - (xx + yy);
    const float ax = m00 * base.x + m01 * base.y + m02 * base.z;
    const float ay = m10 * base.x + m11 * base.y + m12 * base.z;
    const float az = m20 * base.x + m21 * base.y + m22 * base.z;
    // Finally the rotor body rotation maps the world aim back into the rotor frame.
    const float (&rotor)[3][3] = physics_->rotors[1]->body.rotation;
    spotAim_.x = rotor[0][0] * ax + rotor[1][0] * ay + rotor[2][0] * az;
    spotAim_.y = rotor[0][1] * ax + rotor[1][1] * ay + rotor[2][1] * az;
    spotAim_.z = rotor[0][2] * ax + rotor[1][2] * ay + rotor[2][2] * az;
}

// @ 0x1006f0af4. PARTIAL: the 2 KB original distributes the request between the collective, the cyclic lift and the
// rudder through the rotor tilt (1/scale) and the half-yaw rotation, with a cascade of per-field +/-delta*100 limits and
// three "already aligned / target behind / closing in" branches; that cascade is not transcribed. The stand-in keeps the
// observable contract: ease the collective/cyclic controls towards the requested horizontal direction within the same
// +/-100 limits and derive the attitude from the type limits.
void cSC4Helicopter::AI_ControlMoveXZ(const cS3DVector3& direction, float maxControl, float delta) {
    if (!engine_ || !physics_ || !physics_->type) return;
    SC4_UNWIRED("cSC4Helicopter::AI_ControlMoveXZ: the original's force-distribution cascade is not transcribed (approximate steering stand-in)");
    const float lengthXZ = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    const float step = delta * 100.0f;
    float targetCollective = 0.0f;
    float targetLift = 0.0f;
    if (lengthXZ > 0.0f) {
        const float scale = (maxControl * 10.0f) / lengthXZ;
        targetCollective = ClampSymmetric(direction.x * scale, 100.0f);
        targetLift = ClampSymmetric(direction.z * scale, 100.0f);
    }
    engine_->collective += ClampSymmetric(targetCollective - engine_->collective, step);
    engine_->collective = ClampSymmetric(engine_->collective, 100.0f);
    engine_->liftControl += ClampSymmetric(targetLift - engine_->liftControl, step);
    engine_->liftControl = ClampSymmetric(engine_->liftControl, 100.0f);
    const HELI_TYPE_DATA& type = *physics_->type;
    const float roll = ClampSymmetric(TypeField(type, 0x1c) * 0.025f * physics_->flightRatio * engine_->rudder,
                                      TypeField(type, 0x14));
    physics_->rollDeg = roll;
    const float pitch = ClampSymmetric(TypeField(type, 0x24) * 0.025f * engine_->collective, TypeField(type, 0x0c));
    physics_->pitchDeg = pitch;
}

// @ 0x1006f130c.
void cSC4Helicopter::AI_ControlLiftOverBuildings(float delta) {
    IAircraftHost* host = HostFor(*this);
    if (!host) return;
    cS3DVector3 point = position_;
    float altitude = GetAltitude(point);
    if (host->TerrainPointInBounds(point.x, point.z)) {
        const float effectHeight = host->EffectsWaterHeight(point.x, point.z);
        if (altitude <= effectHeight) altitude = effectHeight;
    }
    cS3DVector3 ahead{point.x + physics_->velX * kModelScale, point.y + physics_->velY * kModelScale,
                      point.z + physics_->velZ * kModelScale};
    float aheadAltitude = GetAltitude(ahead);
    if (host->TerrainPointInBounds(ahead.x, ahead.z)) {
        const float effectHeight = host->EffectsWaterHeight(ahead.x, ahead.z);
        if (aheadAltitude <= effectHeight) aheadAltitude = effectHeight;
    }
    if (aheadAltitude <= altitude) aheadAltitude = altitude;
    float target = spawnPosition_.y;
    if (spawnPosition_.y <= aheadAltitude) target = aheadAltitude;
    target += 20.0f;
    if (target < position_.y) {
        float gap = position_.y - target;
        if (gap > 50.0f) gap = 50.0f;
        target -= gap;
    }
    AI_ControlHover(target, delta);
}

// @ 0x1006f1582. Cancels the horizontal velocity through the rotor tilt and the collective/lift/rudder controls.
void cSC4Helicopter::AI_ControlMaintainPositionXZ(float delta) {
    if (!engine_ || !physics_) return;
    const HELI_TYPE_DATA& type = *physics_->type;
    const float (&rotor)[3][3] = physics_->rotors[kRotorMain]->body.rotation;
    const float velX = physics_->velX * kModelScale;
    const float velZ = physics_->velZ * kModelScale;
    const float invScale = 1.0f / rotor[2][1];
    const float step = delta * 100.0f;
    const float collectiveDelta =
        (rotor[2][0] * velZ + rotor[1][0] * 0.0f + rotor[0][0] * velX) * invScale * 10.0f - engine_->collective;
    engine_->collective += ClampSymmetric(collectiveDelta, step);
    const float fieldDelta = engine_->field04 -
                             invScale * (velZ * rotor[2][2] + rotor[1][2] * 0.0f + velX * rotor[0][2]) * 10.0f;
    const float fieldStep = ClampSymmetric(fieldDelta, step) - engine_->field04;
    engine_->field04 = -fieldStep;
    const float rudderDelta = -engine_->rudder;
    engine_->rudder += ClampSymmetric(rudderDelta, step);
    const float roll = ClampSymmetric(TypeField(type, 0x1c) * 0.025f * physics_->flightRatio * fieldStep,
                                      TypeField(type, 0x14));
    physics_->rollDeg = roll;
    const float pitch = ClampSymmetric(TypeField(type, 0x24) * 0.025f * engine_->collective,
                                       TypeField(type, 0x0c));
    physics_->pitchDeg = pitch;
}

// @ 0x1006f149c (i386 0x004a9778): within 20 m, a crime prop is cleared (state 1; up to five of them, asking again after each),
// and with none left the nearest criminal is held.
void cSC4Helicopter::RespondToCrimeScene() {
    IHelicopterHost* host = HeliHostFor(*this);
    if (!host) return;
    void* criminal = host->NearestCrimeSim(position_, 20.0f);
    void* prop = host->NearestCrimeProp(position_, 20.0f);
    if (!prop) {
        if (criminal) host->HoldCriminal(criminal);
        return;
    }
    for (int i = 0; i < 5 && prop; ++i) {
        host->SetCrimePropState(prop, 1);
        prop = host->NearestCrimeProp(position_, 20.0f);
    }
}

// @ 0x1006eadf8.
void cSC4Helicopter::AI_PursueGoal(float delta) {
    if (!physics_) return;
    const std::uint32_t behavior = GetBehavior();
    if (behavior == kHeliBehaviorSpawn) {
        SetDispatchState(3);
        AI_StartEngine();
        spotOn_ = false;
        const cS3DVector3 direction{destination_.x - position_.x, 0.0f, destination_.z - position_.z};
        if (std::sqrt(direction.x * direction.x + direction.z * direction.z) <= 4.0f) {
            PopState();
        } else {
            AI_ControlMoveXZ(direction, 10.0f, delta);
        }
        AI_ControlLiftOverBuildings(delta);
        return;
    }
    if (behavior == kHeliBehaviorPursue) {
        // i386 0x004b02c8: follow the chased automaton (the chase state's target) while it flees (its behavior 0x10): aim at where
        // it will be (its heading times its speed x 1000 x delta ahead), with the spotlight on. Too far away (192): a hidden
        // helicopter reappears 128 units behind the aim (y kept) and fades in, a visible one fades out (tuning fade time). Close
        // (32): hover at the target's height plus 20.
        auto* chase = dynamic_cast<tAutomataStateChase*>(GetState());
        auto* target = chase && chase->target ? dynamic_cast<cSC4Automaton*>(chase->target) : nullptr;
        if (!target || target->GetBehavior() != 0x10) {
            PopState();
            return;
        }
        cS3DVector3 aim{0.0f, 0.0f, 0.0f}, heading{0.0f, 0.0f, 0.0f};
        target->GetPosition(&aim.x);              // vslot 0x1c
        target->GetOrientation(&heading.x);       // vslot 0x3c
        const float lead = target->GetCurrentSpeed() * 1000.0f * delta;   // vslot 0x88, [0x00a706f8] = 1000
        aim = cS3DVector3{lead * heading.x + aim.x, lead * heading.y + aim.y, lead * heading.z + aim.z};
        const cS3DVector3 toward{aim.x - position_.x, 0.0f, aim.z - position_.z};
        const float distance = std::sqrt(toward.x * toward.x + toward.z * toward.z);
        cS3DVector3 unit = toward;
        if (distance > 0.0f) unit = cS3DVector3{toward.x / distance, 0.0f, toward.z / distance};
        AI_StartEngine();
        spotOn_ = true;
        AI_AimSpotlight(aim, delta);
        if (distance > 192.0f) {                                   // kTooFar
            const float opacity = GetOpacity();                    // vslot 0x80
            IHelicopterHost* host = HeliHostFor(*this);
            const float fade = host ? host->FadeSeconds() : 1.0f;  // sTuningParams i386 +0xbc
            if (opacity == 0.0f) {
                const float y = position_.y;
                position_ = cS3DVector3{aim.x - 128.0f * unit.x, y, aim.z - 128.0f * unit.z};   // kTeleportDistance
                physics_->rotors[1]->position = cS3DVector3{position_.x * kInvModelScale, position_.y * kInvModelScale,
                                                            position_.z * kInvModelScale};
                SetOpacity(1.0f, fade);
            } else if (opacity == 1.0f) {
                SetOpacity(0.0f, fade);
            }
        }
        AI_ControlMoveXZ(toward, 2.0f, delta);
        if (distance <= 32.0f) AI_ControlHover(20.0f + aim.y, delta);   // kCloseEnough; _kDefaultMaxDensity = 20
        return;
    }
    if (behavior == kHeliBehaviorArrive) {
        SetDispatchState(5);
        AI_StartEngine();
        spotOn_ = true;
        cS3DVector3 point{destination_.x, destination_.y, destination_.z};
        point.y = GetAltitude(point);
        AI_AimSpotlight(point, delta);
        if (FindState(kHeliBehaviorCrime)) RespondToCrimeScene();
        AI_ControlMaintainPositionXZ(delta);
        return;
    }
    if (behavior == kHeliBehaviorCrime) {
        ResetClaimIfValid(destination_);
        IHelicopterHost* host = HeliHostFor(*this);
        cS3DVector3 crime;
        if (!host || !host->CrimeAt(position_, 20.0f, crime)) {
            PushState(kHeliBehaviorIdle, nullptr, 0.0f, false, 0, 0, nullptr);
            return;
        }
        cS3DVector3 target{0.0f, 0.0f, 0.0f};
        host->CityCellCentre(crime.x, crime.z, target.x, target.z);
        destination_ = target;
        destination_.y = 0.0f;
        if (ClaimedContains(destination_)) {
            destination_ = InvalidVector();
            return;
        }
        ClaimPosition(destination_);
        PushState(kHeliBehaviorArrive, nullptr, 0.0f, false, 0, 0, nullptr);
        PushState(kHeliBehaviorSpawn, nullptr, 0.0f, false, 0, 0, nullptr);
        return;
    }
    if (behavior == kHeliBehaviorTraffic2) {
        ResetClaimIfValid(destination_);
        PushState(kHeliBehaviorIdle, nullptr, 0.0f, false, 0, 0, nullptr);
        IHelicopterHost* host = HeliHostFor(*this);
        cS3DVector3 accident;
        if (!host || !host->TrafficAccidentLocation(accident)) return;
        if (ClaimedContains(destination_)) {
            destination_ = InvalidVector();
            return;
        }
        ClaimPosition(destination_);
        PushState(kHeliBehaviorArrive, nullptr, 0.0f, false, 0, 0, nullptr);
        destination_ = accident;
        destination_.y = 0.0f;
        PushState(kHeliBehaviorSpawn, nullptr, 0.0f, false, 0, 0, nullptr);
        return;
    }
    if (behavior == kHeliBehaviorTraffic) {
        // PARTIAL: the traffic simulator's random cell iterator (spTrafficSim slots 0x50/0x70/0x78/0xa8/0xb0) is
        // unrecovered; the original searches a random rectangle for a cell with traffic and either claims it or falls
        // back to the idle behaviour.
        SC4_UNWIRED("cSC4Helicopter::AI_PursueGoal: the traffic behavior (find a random cell with traffic via the traffic simulator) is not ported");
        ResetClaimIfValid(destination_);
        PushState(kHeliBehaviorIdle, nullptr, 0.0f, false, 0, 0, nullptr);
        return;
    }
    if (behavior != 0) PopState();
    // kHeliBehaviorIdle (and the "no behaviour" fallthrough).
    if (physics_->state == static_cast<std::int32_t>(HelicopterState::kIdle)) {
        SetDispatchState(0);
        PopState();
    } else {
        SetDispatchState(6);
    }
    if (physics_->state == static_cast<std::int32_t>(HelicopterState::kIdle)) {
        AI_ControlReset(delta);
        spotOn_ = false;
        return;
    }
    AI_StartEngine();
    spotOn_ = false;
    const cS3DVector3 toSpawn{spawnPosition_.x - position_.x, 0.0f, spawnPosition_.z - position_.z};
    const float distanceXZ = std::sqrt(toSpawn.x * toSpawn.x + toSpawn.z * toSpawn.z);
    const float distance3D = std::sqrt(toSpawn.x * toSpawn.x + (spawnPosition_.y - position_.y) * (spawnPosition_.y - position_.y) +
                                       toSpawn.z * toSpawn.z);
    if (distanceXZ <= 8.0f) {
        AI_ControlMaintainPositionXZ(delta);
    } else {
        AI_ControlMoveXZ(toSpawn, 10.0f, delta);
    }
    if (distanceXZ <= 48.0f) {
        AI_ControlHover(distanceXZ + spawnPosition_.y - 2.0f, delta);
    } else {
        AI_ControlLiftOverBuildings(delta);
    }
    if (distance3D < 24.0f) arrived_ = true;
}

// @ 0x1006f1756.
void cSC4Helicopter::UpdateEffects() {
    IHelicopterHost* heliHost = HeliHostFor(*this);
    IAircraftHost* host = HostFor(*this);
    const int zoom = host ? host->ViewZoom() : 0;
    const float altitude = GetAltitude(position_);
    if (effectCloseToGround_ && host) {
        const bool inBounds = host->TerrainPointInBounds(position_.x, position_.z);
        bool show = false;
        if (zoom == 1 && physics_ && physics_->state != 0) {
            show = (position_.y - altitude < 60.0f) && inBounds;
        }
        if (!show) {
            if (effectCloseToGround_->IsPlaying()) effectCloseToGround_->Stop(false);
        } else {
            if (!effectCloseToGround_->IsPlaying()) effectCloseToGround_->Start(0);
            cS3DTransform transform;
            transform.translation[0] = position_.x;
            transform.translation[1] = position_.y;
            transform.translation[2] = position_.z;
            transform.flags |= 1;
            ++transform.changeCounter;
            effectCloseToGround_->SetTransform(transform);
        }
    }
    if (effectIndicate_) {
        bool running = false;
        if (zoom == 1 && FindState(kStatePlayerDrive)) {
            cS3DTransform transform;
            transform.translation[0] = position_.x;
            transform.translation[1] = altitude;
            transform.translation[2] = position_.z;
            transform.flags |= 1;
            ++transform.changeCounter;
            effectIndicate_->SetTransform(transform);
            if (!effectIndicate_->IsPlaying()) effectIndicate_->Start(0);
            running = true;
        }
        if (!running && effectIndicate_->IsPlaying()) effectIndicate_->Stop(false);
    }
    if (rotorStill_) rotorStill_->Update();
    if (rotorSpin_) rotorSpin_->Update();
    cSC4Automaton::UpdateEffects();
    (void)heliHost;
}

// @ 0x1006ec416. PARTIAL: the renderer's spotlight cone test (SL::spRenderer slot 0x1e0) and the player-drive "tracked"
// position are behind IHelicopterHost; the aim update is reproduced, the cone/visibility half is not.
void cSC4Helicopter::UpdateSpotlight() {
    IHelicopterHost* heliHost = HeliHostFor(*this);
    if (!heliHost || !effectSearchlight_ || !effectSpotlight_ || !physics_) return;
    IAircraftHost* host = HostFor(*this);
    const bool active = host && host->ViewZoom() == 1 && spotOn_ && physics_->state != 0;
    if (active) SC4_UNWIRED("cSC4Helicopter::UpdateSpotlight: the renderer's spotlight cone test (SL::spRenderer slot 0x1e0) is not ported");
    if (active) {
        if (!effectSearchlight_->IsPlaying()) effectSearchlight_->Start(0);
        if (!effectSpotlight_->IsPlaying()) effectSpotlight_->Start(0);
    } else {
        if (effectSearchlight_->IsPlaying()) effectSearchlight_->Stop(false);
        if (effectSpotlight_->IsPlaying()) effectSpotlight_->Stop(false);
    }
    cS3DTransform search;
    heliHost->GetEffectTransform(effectSearchlight_, search);
    const cS3DVector3 origin{search.translation[0], search.translation[1], search.translation[2]};
    cS3DVector3 aim = spotAim_;
    tAutomataState* driveState = FindState(kStatePlayerDrive);
    if (driveState && active) {
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(driveState);
        spotTarget_ = drive->effectPosition248;
        aim = Normalized(cS3DVector3{spotTarget_.x - origin.x, spotTarget_.y - origin.y,
                                     spotTarget_.z - origin.z});
        spotAim_ = aim;
    }
    cS3DTransform transform;
    transform.translation[0] = spotTarget_.x;
    transform.translation[1] = spotTarget_.y;
    transform.translation[2] = spotTarget_.z;
    transform.flags |= 1;
    ++transform.changeCounter;
    effectSpotlight_->SetTransform(transform);
}

// ---------------------------------------------------------------------------------------------------------------------
// Collisions
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006ef0b8.
void cSC4Helicopter::DetectCollisions() {
    if (!physics_) return;
    IHelicopterHost* heliHost = HeliHostFor(*this);
    IAircraftHost* host = HostFor(*this);
    if (!heliHost || !host) return;
    float minBounds[3] = {0.0f, 0.0f, 0.0f};
    float maxBounds[3] = {0.0f, 0.0f, 0.0f};
    if (physics_->state != 0 && host->TerrainPointInBounds(position_.x, position_.z)) {
        GetBoundingBox(minBounds, maxBounds);
        IHelicopterObstacle* obstacle = heliHost->FindObstacleInBox(
            cS3DVector3{minBounds[0], minBounds[1], minBounds[2]},
            cS3DVector3{maxBounds[0], maxBounds[1], maxBounds[2]}, kHelicopterOccupantType, nullptr);
        if (!obstacle) {
            obstacle = heliHost->FindObstacleInBox(
                cS3DVector3{minBounds[0], minBounds[1], minBounds[2]},
                cS3DVector3{maxBounds[0], maxBounds[1], maxBounds[2]}, 0xc772bf98u, nullptr);
        }
        if (obstacle) {
            obstacle->AddRef();
            if (IsPlayerDriving() && physics_->state == 1) {
                if (host) {
                    host->PlayAudioEvent(7);
                    host->PlayAudioEvent(7);
                }
            }
            cS3DVector3 obstacleMin, obstacleMax;
            obstacle->GetBounds(obstacleMin, obstacleMax);
            // Clip the segment origin -> position*100 against the obstacle box.
            float px = position_.x * 100.0f;
            float py = position_.y * 100.0f;
            float pz = position_.z * 100.0f;
            float tx = px, ty = py, tz = pz;
            if (px > 0.0f && obstacleMax.x < px) {
                const float s = obstacleMax.x / px;
                tx *= s; ty *= s; tz *= s;
            }
            if (px < 0.0f && obstacleMin.x < px) {
                const float s = obstacleMin.x / px;
                tx *= s; ty *= s; tz *= s;
            }
            if (ty > 0.0f && obstacleMax.y < ty) {
                const float s = obstacleMax.y / ty;
                tx *= s; ty *= s; tz *= s;
            }
            if (tz > 0.0f && obstacleMax.z < tz) {
                const float s = obstacleMax.z / tz;
                tx *= s; ty *= s; tz *= s;
            }
            if (tz < 0.0f && obstacleMin.z < tz) {
                const float s = obstacleMin.z / tz;
                tx *= s; ty *= s; tz *= s;
            }
            // Pick the axis with the smallest distance to the box.
            float bestX = std::fabs(tx - obstacleMin.x);
            float normalX = -1.0f, normalY = 0.0f, normalZ = 0.0f;
            float best = bestX;
            if (bestX >= FLT_MAX) { best = FLT_MAX; normalX = 0.0f; }
            const float distMaxX = std::fabs(tx - obstacleMax.x);
            if (distMaxX < best) { best = distMaxX; normalX = 1.0f; normalY = 0.0f; normalZ = 0.0f; }
            const float distMaxY = std::fabs(ty - obstacleMax.y);
            if (distMaxY < best) { best = distMaxY; normalX = 0.0f; normalY = 1.0f; normalZ = 0.0f; }
            const float distMinZ = std::fabs(tz - obstacleMin.z);
            if (distMinZ < best) { best = distMinZ; normalX = 0.0f; normalY = 0.0f; normalZ = -1.0f; }
            const float distMaxZ = std::fabs(tz - obstacleMax.z);
            if (distMaxZ < best) { best = distMaxZ; normalX = 0.0f; normalY = 0.0f; normalZ = 1.0f; }
            if (normalY != 0.0f) {
                // Vertical collision: push out along y and stop the vertical speed.
                if (std::fabs(position_.y) < std::fabs(ty)) {
                    physics_->rotors[1]->position.y = ty * kInvModelScale;
                }
                physics_->velY = 0.0f;
                const cS3DVector3 toSpawn{position_.x - spawnPosition_.x, position_.y - spawnPosition_.y,
                                          position_.z - spawnPosition_.z};
                if (Magnitude(toSpawn) < 8.0f) SetHelicopterState(0);
            } else {
                float speed = -(physics_->velY * normalY + physics_->velX * normalX + physics_->velZ * normalZ);
                if (std::fabs(position_.x) < std::fabs(tx)) {
                    physics_->rotors[1]->position.x = tx * kInvModelScale;
                }
                if (speed <= 0.1f) speed = 0.1f;
                if (std::fabs(position_.z) < std::fabs(tz)) {
                    physics_->rotors[1]->position.z = tz * kInvModelScale;
                }
                // Random +/-3 degree kicks on the three angles.
                const double kick1 = host->RandomDoubleRange(0.0, static_cast<double>(speed));
                const float rollDelta = static_cast<float>(kick1 * 3.0);
                physics_->rollDeg += (physics_->rollDeg > 0.0f) ? -rollDelta : rollDelta;
                const double kick2 = host->RandomDoubleRange(0.0, static_cast<double>(speed));
                const float pitchDelta = static_cast<float>(kick2 * 3.0);
                const float pitchSign = physics_->velY * normalY + physics_->velX * normalX +
                                        physics_->velZ * normalZ;
                physics_->pitchDeg += (pitchSign > 0.0f) ? pitchDelta : -pitchDelta;
                const double kick3 = host->RandomDoubleRange(0.0, static_cast<double>(speed));
                const float yawDelta = static_cast<float>(kick3 * 3.0);
                physics_->yawDeg += (pitchSign > 0.0f) ? -yawDelta : yawDelta;
                speed *= 0.03f;
                physics_->velX += normalX * speed;
                physics_->velY += normalY * speed;
                physics_->velZ += normalZ * speed;
                if (physics_->velY > 0.0f) physics_->velY = 0.0f;
            }
            position_ = cS3DVector3{physics_->rotors[1]->position.x * kModelScale,
                                    physics_->rotors[1]->position.y * kModelScale,
                                    physics_->rotors[1]->position.z * kModelScale};
            obstacle->Release();
            heliHost->EndObstacleQuery();
        }
    }
    // Ground/lot clamp: the helicopter must stay 2 m above the lot it spawned on (or 30 m above the ground otherwise).
    const float ground = GetAltitude(position_);
    const float high = ground + 30.0f;
    if (position_.y <= high && high != position_.y) {
        const void* lotHere = host->TerrainPointInBounds(position_.x, position_.z)
                                  ? heliHost->LotAtPoint(position_)
                                  : nullptr;
        const void* lotSpawn = heliHost->LotAtPoint(spawnPosition_);
        float target = high;
        if (lotHere == lotSpawn) {
            target = ground + 2.0f;
            if (!(position_.y <= target) || target == position_.y) return;
        }
        position_.y = target;
        physics_->rotors[1]->position.y = target * kInvModelScale;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Player drive
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006ef960. PARTIAL: the spotlight cone (SL::spRenderer slot 0x1e8) and the AEMS group-0x4308 event are behind
// IHelicopterHost; the control translation is reproduced.
bool cSC4Helicopter::TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) {
    if (!state || !physics_) return false;
    SC4_UNWIRED("cSC4Helicopter::TranslatePlayerDriveCommands: the spotlight cone (SL::spRenderer slot 0x1e8) and the AEMS group-0x4308 event are not ported");
    cSC4Automaton::TranslatePlayerDriveCommands(state, milliseconds);
    if (state->effectTimerC4 >= 0.0f && rotorStill_) {
        rotorStill_->Update();
        if (state->fireArmed) {
            bool started = rotorStill_->StartNamed(cRZString("helicoptermissle"), 0, false);
            if (!started) started = rotorStill_->StartNamed(cRZString("ufolaser"), 0, false);
            if (started) {
                state->effectDelay20c = 0;
                state->fireArmed = 0;
                ++state->shotsFired;
            }
        }
    }
    IHelicopterHost* host = HeliHostFor(*this);
    IAircraftHost* aircraftHost = HostFor(*this);
    if (IsOccupantGroup(kHelicopterGroupPlayer) && aircraftHost) {
        const float groupSpeed = state->history[9];   // +0xcc
        if (groupSpeed != -1.0f || std::isnan(groupSpeed)) {
            aircraftHost->PlayAudioEvent(0x0b);
        } else {
            aircraftHost->StopAudioEvent(0x0b);
        }
    }
    if (physics_->engineEnabled == 0) {
        state->steering6c = 0.0f;
        state->value38 = 0.0f;
        if (state->history[5] >= 0.0f) SetHelicopterState(4);   // +0xbc
    }
    if (physics_->engineEnabled == 1 && physics_->enginePower >= 400.0f) {
        const float delta = milliseconds * 0.001f;
        // Collective (steering6c, +0x6c).
        float collective = state->steering6c;
        const float upInput = state->history[0];   // +0xa8
        if (upInput >= 0.0f) {
            const float rate = (collective <= 0.0f) ? playerPitchRate_ : playerSlowScale_ * playerPitchRate_;
            collective -= rate * delta;
        } else if (state->history[3] >= 0.0f) {   // +0xb4
            const float rate = (collective >= 0.0f) ? playerPitchRate_ : playerSlowScale_ * playerPitchRate_;
            collective += rate * delta;
        } else {
            float factor = 1.0f - (delta + delta);
            if (factor < 0.0f) factor = 0.0f;
            collective = (std::fabs(factor * collective) >= 1.0f) ? factor * collective : 0.0f;
        }
        state->steering6c = ClampSymmetric(collective, 100.0f);
        // Pitch (value38, +0x38).
        float pitch = state->value38;
        const float forward = state->history[2];   // +0xb0
        if (forward >= 0.0f) {
            const float rate = (pitch <= 0.0f) ? playerYawRate_ : playerSlowScale_ * playerYawRate_;
            pitch += rate * delta;
        } else if (state->history[1] >= 0.0f) {   // +0xac
            const float rate = (pitch >= 0.0f) ? playerYawRate_ : playerSlowScale_ * playerYawRate_;
            pitch -= rate * delta;
        } else {
            const cS3DVector3 velocity{physics_->velX * kModelScale, 0.0f, physics_->velZ * kModelScale};
            const float speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
            if (speed >= 3.0f && speed <= 30.0f) {
                const float rate = (pitch <= 0.0f) ? playerYawRate_ : playerSlowScale_ * playerYawRate_;
                pitch += rate * delta;
            } else if (speed > 30.0f) {
                const float rate = (pitch >= 0.0f) ? playerYawRate_ : playerSlowScale_ * playerYawRate_;
                pitch -= rate * delta;
            } else {
                float factor = 1.0f - delta;
                if (factor < 0.0f) factor = 0.0f;
                pitch = (std::fabs(factor * pitch) >= 5.0f) ? factor * pitch : 0.0f;
            }
        }
        state->value38 = ClampSymmetric(pitch, 100.0f);
        // Yaw (value3c, +0x3c).
        if (state->history[5] >= 0.0f) {   // +0xbc
            state->value3c += delta * playerRudderRate_;
            spawnAltitude_ = physics_->rotors[1]->position.y * kModelScale;
        } else if (state->history[6] >= 0.0f) {   // +0xc0
            state->value3c -= delta * playerRudderRate_;
            spawnAltitude_ = physics_->rotors[1]->position.y * kModelScale;
        } else if (physics_->state != 0) {
            AI_ControlHover(spawnAltitude_, delta);
            state->value3c = engine_->liftControl;
        }
        state->value3c = ClampSymmetric(state->value3c, 100.0f);
    }
    engine_->collective = state->steering6c;
    engine_->field04 = -state->value38;
    engine_->liftControl = state->value3c;
    const float collective = state->steering6c;
    engine_->rudder = (collective > 100.0f) ? -100.0f : ClampSymmetric(-collective, 100.0f);
    if (physics_->type) {
        const HELI_TYPE_DATA& type = *physics_->type;
        const float roll = TypeField(type, 0x1c) * 0.025f * physics_->flightRatio * state->value38;
        physics_->rollDeg = ClampSymmetric(roll, TypeField(type, 0x14));
        const float pitch = TypeField(type, 0x24) * 0.025f * state->steering6c;
        physics_->pitchDeg = ClampSymmetric(pitch, TypeField(type, 0x0c));
        physics_->verticalSpeed = 0.0f;
        physics_->field140 = 0.0f;
        spotOn_ = state->spotlightStarted255 != 0;
        if (state->history[8] >= 0.0f && host) {   // +0xc8
            if (host->RendererSpotlightHit(state->effectParam240, state->effectParam244, spotTarget_)) {
                state->effectPosition248 = spotTarget_;
            }
            state->spotlightStarted255 = 1;
            spotOn_ = true;
        }
        const cS3DVector3 toSpawn{spawnPosition_.x - position_.x, spawnPosition_.z - position_.z};
        const float distance = std::sqrt((spawnPosition_.x - position_.x) * (spawnPosition_.x - position_.x) +
                                         (spawnPosition_.y - position_.y) * (spawnPosition_.y - position_.y) +
                                         (spawnPosition_.z - position_.z) * (spawnPosition_.z - position_.z));
        const float horizontal = std::sqrt(toSpawn.x * toSpawn.x + toSpawn.y * toSpawn.y);
        (void)horizontal;
        if (distance > 32.0f && physics_->state == 4) SetHelicopterState(1);
        const cS3DVector3 velocity{physics_->velX * kModelScale, physics_->velY * kModelScale,
                                   physics_->velZ * kModelScale};
        speedTween_.SetDesired(Magnitude(velocity) * 0.001f, 0.0f);
        // i386 0x004af136 tail: +0x238 (the pitch gauge) = 0.01 * steering * type +0xc * pi/1800, +0x234 its factor.
        state->pitchGauge = 0.01f * state->steering6c * TypeField(type, 0x0c) * kDegToRad;
        state->pitchGaugeScale = kDegToRad * TypeField(type, 0x0c);
    }
    return true;
}

// @ 0x1006f19cc.
void cSC4Helicopter::OnPlayerTakeOver() {
    spawnAltitude_ = position_.y;   // +0x31c
    if (!FindState(kStatePlayerDrive)) return;
    if (!physics_) return;
    if (physics_->state == 1) {
        if (IsPlayerDriving()) {
            if (IAircraftHost* host = HostFor(*this)) host->PlayAudioEvent(8);
        }
    } else if (physics_->state == 0 && physics_->engineEnabled == 1) {
        SetHelicopterState(4);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// State / dispatch / orientation
// ---------------------------------------------------------------------------------------------------------------------
// @ 0x1006f1a44.
bool cSC4Helicopter::HandleEnterState(tAutomataState* state) {
    const bool result = cSC4Aircraft::HandleEnterState(state);
    if (state && state->stateType == kStatePlayerDrive) {
        if (physics_) LoadPlayerDriveParameters(physics_->type);
        OnPlayerTakeOver();
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
        drive->value40 = 0xc2c80000u;    // +0x40 = -100.0f bits
        drive->groupSpeed = -100.0f;     // +0x44 = -100.0f
        drive->shells = 0xf;
        drive->shotsFired = 0;
        drive->timeout1d8 = 0x44fa0000;
    }
    return result;
}

// @ 0x1006f1ac6.
bool cSC4Helicopter::HandleExitState(const tAutomataState* state) {
    const bool result = cSC4Aircraft::HandleExitState(state);
    if (state) {
        if (state->stateType == 0x35) {
            SetDispatchState(2);
        } else if (state->stateType == kStatePlayerDrive && physics_) {
            LoadDriveParameters(physics_->type);
            const float mass = TypeField(*physics_->type, 0x60);
            physics_->inertia = mass;
            physics_->mass = mass;
        }
    }
    return result;
}

// @ 0x1006f1b44.
void cSC4Helicopter::SetOrientation(const float* direction) {
    cSC4Automaton::SetOrientation(direction);
    if (!physics_) return;
    const cS3DVector3 facing = kAircraftModelFacing;   // (0, 0, 1)
    const float ox = orientation_.x;
    const float oy = orientation_.y;
    const float oz = orientation_.z;
    const float dot = facing.z * oz + facing.y * oy + facing.x * ox;
    float cosHalf = 1.0f;
    float sinHalf = 0.0f;
    if (dot < 0.9999f) {
        if (dot <= -0.9999f) {
            const float facingSquared = facing.y * facing.y + facing.x * facing.x;
            float a = facing.y;
            float b = -facing.z;
            float c = facing.x;
            if (facingSquared >= 1e-8f) {
                a = -facing.y;
                c = 0.0f;
                b = 0.0f;
            }
            cosHalf = 0.0f;
            sinHalf = (1.0f / std::sqrt(a * a + b * b + c * c)) * c;
        } else {
            const float crossX = facing.y * oz - oy * facing.z;
            const float crossY = facing.z * ox - oz * facing.x;
            const float crossZ = facing.x * oy - ox * facing.y;
            const float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
            const float invLength = (length != 0.0f) ? 1.0f / length : 0.0f;
            const float half = std::sqrt((1.0f - dot) * 0.5f);
            sinHalf = half * (invLength * crossY);
            cosHalf = std::sqrt((dot + 1.0f) * 0.5f);
        }
    }
    const float sine = std::sqrt(1.0f - cosHalf * cosHalf);
    const float angle = 2.0f * std::acos(cosHalf);
    const float factor = (std::fabs(sine) < 0.0001f) ? 1.0f : 1.0f / sine;
    const float yaw = (factor * sinHalf < 0.0f) ? -angle : angle;
    physics_->yawDeg = yaw * 572.9578f;
}

// @ 0x1006f1d70.
void cSC4Helicopter::SetDispatchState(std::uint32_t state) {
    if (dispatchState_ == state) return;
    if (FindState(kHeliBehaviorCrime)) {
        if (IHelicopterHost* host = HeliHostFor(*this)) host->DispatchManagerReport(this, static_cast<int>(state));
    }
    dispatchState_ = state;
    SendCallbackMessage(7, state);
}

// @ 0x1006f1eb2 / 0x1006f1ec0 / 0x1006f1ece.
std::uint16_t cSC4Helicopter::GetCallNumber() const { return callNumber_; }
void cSC4Helicopter::SetCallNumber(std::uint16_t callNumber) { callNumber_ = callNumber; }
std::uint32_t cSC4Helicopter::GetDispatchState() const { return dispatchState_; }

}  // namespace sc4
