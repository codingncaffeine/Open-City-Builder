// cSC4RigidBodySimulator2D — see RigidBodySimulator2D.h. Hand port of the Mac i386 functions; the float expressions keep
// the original's operand order (the collision test's double-precision sums included).
#include "sc4/RigidBodySimulator2D.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "rz/cRZRandom.h"
#include "s3d/S3DMath.h"

namespace sc4 {

static_assert(offsetof(cSC4RigidBodySimulator2D::ObjectDesc, halfHeight) == 0x28,
              "the vehicles read the object as a float record: [0] x .. [10] half height");
static_assert(offsetof(cSC4RigidBodySimulator2D::ObjectDesc, friction) == 0x3c, "ObjectDesc layout");
static_assert(offsetof(cSC4RigidBodySimulator2D::ObjectDesc, contacts) == 0x5c, "ObjectDesc layout");

namespace {

constexpr float kKinematicInverseMass = 1e-05f;     // cSC4TrafficAutomaton::kfAutomatonOrientationEpsilon (0x00a70830)
constexpr float kInertiaFactor = 48.0f;             // 0x00a7084c
constexpr float kRestitution = -1.4f;               // 0x00a70bf4: the impulse factor (1 + e, negated)
constexpr float kImpulseBias = 0.0001f;             // 0x00a7078c
constexpr float kSeparation = 4.0f;                 // 0x00a70718
constexpr float kNoCandidate = 9.9999999e+36f;      // 0x00a70bf0 (0x7cf0bdc2)
constexpr double kMaxPenetration = 1e+35;           // 0x00a71038
constexpr float kPi = 3.14159274f;                  // cGZWinPieMenu::pi
constexpr double kTwoPi = 6.2831854820251465;       // fmod's divisor (0x401921fb60000000)

float AsFloat(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
std::int32_t Bits(float f) {
    std::int32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}
// floor(v) as the original rounds a sector coordinate: (v - 0.5) + 1.5 x 2^23 read as an integer, one more when v is
// exactly that next integer.
std::int32_t FloorSector(float v) {
    const float t = (v - 0.5f) + 12582912.0f;
    const std::int32_t low = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b400000u);
    const std::int32_t high = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b3fffffu);
    const float highF = static_cast<float>(high);
    if (std::isnan(v) || std::isnan(highF)) return low;
    return v == highF ? high : low;
}
// ceil(v): (v + 0.5) + 1.5 x 2^23 read as an integer, one less when v is exactly that smaller integer.
std::int32_t CeilCount(float v) {
    const float t = v + 0.5f + 12582912.0f;
    const std::int32_t high = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b400000u);
    const std::int32_t low = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b400001u);
    const float lowF = static_cast<float>(low);
    if (std::isnan(v) || std::isnan(lowF)) return high;
    return v == lowF ? low : high;
}
std::uint32_t SectorKey(std::int32_t x, std::int32_t y) {
    return static_cast<std::uint32_t>(y) + 0x40004000u + static_cast<std::uint32_t>(x) * 0x10000u;
}
// cS3DTransform2::operator* (0x00638130): (c v.x - s v.y, s v.x + c v.y).
cS3DVector2 Turn(float c, float s, const cS3DVector2& v) { return {v.x * c - s * v.y, s * v.x + c * v.y}; }
// (anonymous)::rol90 (0x00803a5e): (-v.y, v.x).
cS3DVector2 Rol90(const cS3DVector2& v) { return {-v.y, v.x}; }
// cS3DVector2::SetLength (0x00804554): nothing for a zero vector.
void SetLength(cS3DVector2& v, float length) {
    const float squared = v.x * v.x + v.y * v.y;
    if (!(squared > 0.0f)) return;
    const float scale = length / std::sqrt(squared);
    v.x *= scale;
    v.y = scale * v.y;
}
// The three-axis overlap the collision passes test first (boxes' axis-aligned extents and the height bands).
bool BandsOverlap(const cSC4RigidBodySimulator2D::Object& a, const cSC4RigidBodySimulator2D::Object& b) {
    return std::fabs(b.x - a.x) <= a.extentX + b.extentX && std::fabs(b.y - a.y) <= a.extentY + b.extentY &&
           std::fabs(a.height - b.height) <= a.halfHeight + b.halfHeight;
}

}  // namespace

// 0x004bac04: the velocity of b relative to a at the point (each body's velocity plus its spin times the lever).
cS3DVector2 cSC4RigidBodySimulator2D::GetRelativeVelocityAtPoint(const Object& a, const Object& b, const cS3DVector2& point) {
    return {(b.angularVelocity * -(point.y - b.y) + b.vx) - (a.angularVelocity * -(point.y - a.y) + a.vx),
            (b.angularVelocity * (point.x - b.x) + b.vy) - (a.angularVelocity * (point.x - a.x) + a.vy)};
}

// 0x004bac9e: the same with momenta (1 / inverse mass, 1 / inverse inertia).
cS3DVector2 cSC4RigidBodySimulator2D::GetRelativeMomentumAtPoint(const Object& a, const Object& b, const cS3DVector2& point) {
    const float spinA = (1.0f / a.inverseInertia) * a.angularVelocity;
    const float massA = 1.0f / a.inverseMass;
    const float spinB = (1.0f / b.inverseInertia) * b.angularVelocity;
    const float massB = 1.0f / b.inverseMass;
    return {(massB * b.vx - (point.y - b.y) * spinB) - (massA * a.vx - spinA * (point.y - a.y)),
            (spinB * (point.x - b.x) + massB * b.vy) - (spinA * (point.x - a.x) + massA * a.vy)};
}

// 0x004badda: the corner of the box that lies furthest towards `point` along `direction` (each half axis flipped when it
// points the same way as the box centre's offset along the direction).
cS3DVector2 cSC4RigidBodySimulator2D::GetClosestApproach(const cS3DVector2& point, const cS3DVector2& direction,
                                                         const Object& object) {
    const float along = (object.x - point.x) * direction.x + (object.y - point.y) * direction.y;
    float axisX = object.halfX * object.c;
    float axisY = object.halfX * object.s;
    float sideX = -object.s * object.halfY;
    float sideY = object.c * object.halfY;
    if (0.0f < (direction.x * axisX + axisY * direction.y) * along) {
        axisY = -axisY;
        axisX = -axisX;
    }
    if (0.0f < along * (direction.x * sideX + direction.y * sideY)) {
        sideY = -sideY;
        sideX = -sideX;
    }
    return {sideX + axisX + object.x, object.y + axisY + sideY};
}

// 0x004baee0: the separating-axis test of the two boxes (sums in double precision), then the deepest of four corner
// contacts that are closing: a's corners against b's faces along b's axes and b's corners along a's. The answer is
// whether the shallowest accepted penetration is below 1e35 (a candidate was found).
bool cSC4RigidBodySimulator2D::TestForCollision(const Object& a, const Object& b, cS3DVector2& normal, cS3DVector2& point) {
    const cS3DVector2 offset{b.x - a.x, b.y - a.y};
    const cS3DVector2 local = Turn(a.c, -a.s, offset);              // the offset in a's frame
    const float cosRel = b.c * a.c - (-a.s) * b.s;
    const float sinRel = a.c * b.s + b.c * (-a.s);
    const double ahx = a.halfX, ahy = a.halfY;
    auto absD = [](float v) { return static_cast<double>(std::fabs(v)); };
    // a's x axis
    if (0.0f < static_cast<float>(absD(local.x) - ((absD(b.halfX * cosRel) + ahx) + absD(b.halfY * -sinRel)))) return false;
    // a's y axis
    if (0.0f < static_cast<float>(absD(local.y) - ((absD(b.halfX * sinRel) + ahy) + absD(b.halfY * cosRel)))) return false;
    // b's x axis
    const double absCos = absD(cosRel), absSin = absD(sinRel), absNegSin = absD(-sinRel);
    if (0.0f < static_cast<float>(absD(cosRel * local.x + sinRel * local.y) -
                                  ((static_cast<double>(b.halfX) + ahx * absCos) + absSin * ahy)))
        return false;
    // b's y axis
    if (0.0f < static_cast<float>(absD(-sinRel * local.x + cosRel * local.y) -
                                  ((static_cast<double>(b.halfY) + ahx * absNegSin) + absCos * ahy)))
        return false;

    const cS3DVector2 axisA{a.c, a.s}, sideA{-a.s, a.c}, axisB{b.c, b.s}, sideB{-b.s, b.c};
    const cS3DVector2 p1 = GetClosestApproach({a.x, a.y}, axisA, b);
    const cS3DVector2 p2 = GetClosestApproach({a.x, a.y}, sideA, b);
    const cS3DVector2 p3 = GetClosestApproach({b.x, b.y}, axisB, a);
    const cS3DVector2 p4 = GetClosestApproach({b.x, b.y}, sideB, a);
    float best = kNoCandidate;
    // b's corner against a's face across a's x axis
    {
        const float rx = p1.x - a.x, ry = p1.y - a.y;
        if (std::fabs(a.c * ry - a.s * rx) <= a.halfY) {
            const float along = a.c * rx + a.s * ry;
            const cS3DVector2 v = GetRelativeVelocityAtPoint(a, b, p1);
            if (!((a.c * v.x + a.s * v.y) * along > 0.0f) && !std::isnan((a.c * v.x + a.s * v.y) * along)) {
                best = std::fabs(along);
                point = p1;
                normal = axisA;
            }
        }
    }
    // across a's y axis
    {
        const float rx = p2.x - a.x, ry = p2.y - a.y;
        if (std::fabs(a.c * rx + a.s * ry) <= a.halfX) {
            const float negS = -a.s;
            const float along = rx * negS + a.c * ry;
            const cS3DVector2 v = GetRelativeVelocityAtPoint(a, b, p2);
            const float closing = (negS * v.x + a.c * v.y) * along;
            if (!(closing > 0.0f) && !std::isnan(closing) && std::fabs(along) < best) {
                point = p2;
                normal = sideA;
                best = std::fabs(along);
            }
        }
    }
    // a's corner against b's face across b's x axis
    {
        const float rx = p3.x - b.x, ry = p3.y - b.y;
        if (std::fabs(b.c * ry - b.s * rx) <= b.halfY) {
            const float along = b.c * rx + b.s * ry;
            const cS3DVector2 v = GetRelativeVelocityAtPoint(a, b, p3);
            const float closing = (b.c * v.x + b.s * v.y) * along;
            if (closing >= 0.0f && std::fabs(along) < best) {
                point = p3;
                normal = axisB;
                best = std::fabs(along);
            }
        }
    }
    // across b's y axis
    {
        const float rx = p4.x - b.x, ry = p4.y - b.y;
        if (std::fabs(b.c * rx + b.s * ry) <= b.halfX) {
            const float negS = -b.s;
            const float along = rx * negS + b.c * ry;
            const cS3DVector2 v = GetRelativeVelocityAtPoint(a, b, p4);
            const float closing = (negS * v.x + b.c * v.y) * along;
            if (closing >= 0.0f && std::fabs(along) < best) {
                point = p4;
                normal = sideB;
                best = std::fabs(along);
            }
        }
    }
    return static_cast<double>(best) < kMaxPenetration;
}

// 0x004bb99a: the impulse along the normal for a restitution of 0.4 (j = -1.4 (v . n) / (n.n (1/ma + 1/mb) + ra^2/Ia +
// rb^2/Ib), nudged 1e-4 away from zero), applied to both bodies' velocities and spins; bodies whose centres are closer
// than 4 units are also pushed apart (to 4) and given a unit velocity away from each other.
void cSC4RigidBodySimulator2D::ResolveCollision(Object& a, Object& b, const cS3DVector2& normal, const cS3DVector2& point,
                                                const cS3DVector2& extraVelocity) {
    const cS3DVector2 perpA = Rol90({point.x - a.x, point.y - a.y});
    const cS3DVector2 perpB = Rol90({point.x - b.x, point.y - b.y});
    const cS3DVector2 v = GetRelativeVelocityAtPoint(b, a, point);
    const float nx = normal.x, ny = normal.y;
    const float leverA = nx * perpA.x + ny * perpA.y;
    const float leverB = nx * perpB.x + ny * perpB.y;
    float j = ((v.x + extraVelocity.x) * nx + (v.y + extraVelocity.y) * ny) * kRestitution;
    j = j / ((((nx * nx + ny * ny) * (a.inverseMass + b.inverseMass)) + leverA * leverA * a.inverseInertia) +
             leverB * leverB * b.inverseInertia);
    if (j >= 0.0f || std::isnan(j)) j = j + kImpulseBias;
    else j = j - kImpulseBias;
    const float ja = j * a.inverseMass;
    a.vx = nx * ja + a.vx;
    a.vy = ny * ja + a.vy;
    const float jb = j * b.inverseMass;
    b.vx = b.vx - jb * nx;
    b.vy = b.vy - jb * ny;
    a.angularVelocity = (perpA.x * nx + perpA.y * ny) * j * a.inverseInertia + a.angularVelocity;
    b.angularVelocity = b.angularVelocity - j * (perpB.x * nx + perpB.y * ny) * b.inverseInertia;
    cS3DVector2 apart{a.x - b.x, a.y - b.y};
    const float distance = std::sqrt(apart.x * apart.x + apart.y * apart.y);
    if (!(distance < kSeparation)) return;   // NaN included
    if (distance == 0.0f) {
        cRZRandom random(0xffffffffu);
        apart = nS3D::RandomDirection2(random);
    }
    cS3DVector2 push = apart;
    SetLength(push, kSeparation - distance);
    SetLength(apart, 1.0f);
    a.vx = a.vx + apart.x;
    a.vy = a.vy + apart.y;
    a.x = a.x + push.x;
    a.y = a.y + push.y;
    b.vx = b.vx - apart.x;
    b.vy = b.vy - apart.y;
    b.x = b.x - push.x;
    b.y = b.y - push.y;
}

// 0x00803e78: the pair belongs to the sector at the larger x and the larger y of their first sectors.
bool cSC4RigidBodySimulator2D::IsPrimaryCollision(std::uint32_t sector, std::uint32_t keyA, std::uint32_t keyB) {
    const std::uint32_t high = std::max(keyB & 0xffff0000u, keyA & 0xffff0000u);
    const std::uint32_t low = std::max(keyB & 0xffffu, keyA & 0xffffu);
    return high + low == sector;
}

// Both bodies remember the contact: the point at the middle of the shared height band, and the normal.
void cSC4RigidBodySimulator2D::RecordContact(Object& a, Object& b, const cS3DVector2& normal, const cS3DVector2& point) {
    const float bottom = std::max(a.height - a.halfHeight, b.height - b.halfHeight);
    const float top = std::min(a.height + a.halfHeight, b.height + b.halfHeight);
    const float middle = (bottom + top) * 0.5f;
    ++a.contacts;
    ++b.contacts;
    b.contact = cS3DVector3{point.x, middle, point.y};
    a.contact = b.contact;
    b.normal = normal;
    a.normal = b.normal;
}

// 0x004bbd0a: one dynamic body against the kinematic or fixed bodies of the sector.
void cSC4RigidBodySimulator2D::CheckAgainstInfiniteMassObjects(std::uint32_t sector, Object& object, std::vector<Object*>& others) {
    for (Object* other : others) {
        if (!BandsOverlap(object, *other)) continue;
        if (!IsPrimaryCollision(sector, object.sectorKey, other->sectorKey)) continue;
        cS3DVector2 normal{0.0f, 0.0f}, point{0.0f, 0.0f};
        if (!TestForCollision(object, *other, normal, point)) continue;
        ResolveCollision(object, *other, normal, point, cS3DVector2{0.0f, 0.0f});
        RecordContact(object, *other, normal, point);
    }
}

// 0x004bbf1e: sector by sector (the hash map's order), every pair of dynamic bodies, then each dynamic body against the
// sector's kinematic and fixed bodies; then the links: a stretched link whose ends are separating gets an impulse along
// the stretch (when the relative speed is not negligible) and breaks above its momentum.
void cSC4RigidBodySimulator2D::DoCollisions() {
    sectors_.ForEach([this](std::uint32_t key, Sector& sector) {
        for (std::size_t i = 0; i < sector.dynamic.size(); ++i) {
            Object& a = *sector.dynamic[i];
            for (std::size_t k = i + 1; k < sector.dynamic.size(); ++k) {
                Object& b = *sector.dynamic[k];
                if (!BandsOverlap(a, b) || !IsPrimaryCollision(key, a.sectorKey, b.sectorKey)) continue;
                cS3DVector2 normal{0.0f, 0.0f}, point{0.0f, 0.0f};
                if (!TestForCollision(a, b, normal, point)) continue;
                ResolveCollision(a, b, normal, point, cS3DVector2{0.0f, 0.0f});
                RecordContact(a, b, normal, point);
            }
            CheckAgainstInfiniteMassObjects(key, a, sector.kinematic);
            CheckAgainstInfiniteMassObjects(key, a, sector.fixed);
        }
    });
    for (auto it = links_.begin(); it != links_.end();) {
        Object& a = *it->a;
        Object& b = *it->b;
        const cS3DVector2 turnedA = Turn(a.c, a.s, it->offsetA);
        const float endAy = a.y + turnedA.y;
        const float endAx = a.x + turnedA.x;
        const cS3DVector2 turnedB = Turn(b.c, b.s, it->offsetB);
        const cS3DVector2 stretch{(b.x + turnedB.x) - endAx, (b.y + turnedB.y) - endAy};
        const float length = std::fabs(std::sqrt(stretch.x * stretch.x + stretch.y * stretch.y));
        if (!(it->maxDistance < length)) {
            ++it;
            continue;
        }
        const cS3DVector2 middle{(endAx + b.x + turnedB.x) * 0.5f, (endAy + b.y + turnedB.y) * 0.5f};
        const cS3DVector2 v = GetRelativeVelocityAtPoint(a, b, middle);
        if (v.x * stretch.x + v.y * stretch.y < 0.0f) {
            ++it;
            continue;
        }
        if (kKinematicInverseMass < std::sqrt(v.x * v.x + v.y * v.y))
            ResolveCollision(a, b, stretch, middle, cS3DVector2{0.0f, 0.0f});
        const cS3DVector2 momentum = GetRelativeMomentumAtPoint(a, b, middle);
        const float amount = std::sqrt(momentum.x * momentum.x + momentum.y * momentum.y);
        if (amount < it->breakMomentum || amount == it->breakMomentum) {
            ++it;
            continue;
        }
        it = links_.erase(it);
    }
}

// 0x004bc4b8: file the object in every sector of its box (x columns outer, y rows inner).
void cSC4RigidBodySimulator2D::AddToSectorMap(Object& object) {
    const std::uint32_t key = object.sectorKey;
    for (std::uint32_t column = 0; column < object.sectorsX; ++column) {
        for (std::uint32_t row = 0; row < object.sectorsY; ++row) {
            const std::uint32_t at = (key & 0xffffu) + row + 0x40000000u + (column + ((key >> 16) - 0x4000u)) * 0x10000u;
            Sector& sector = sectors_[at];
            if (object.fixed) sector.fixed.push_back(&object);
            else if (object.kinematic) sector.kinematic.push_back(&object);
            else sector.dynamic.push_back(&object);
            ++sector.count;
        }
    }
}

// 0x004bb7ee: take the object out of its sectors; a sector left with nothing is dropped.
void cSC4RigidBodySimulator2D::RemoveFromSectorMap(Object& object) {
    const std::uint32_t key = object.sectorKey;
    for (std::uint32_t column = 0; column < object.sectorsX; ++column) {
        for (std::uint32_t row = 0; row < object.sectorsY; ++row) {
            const std::uint32_t at = (key & 0xffffu) + row + 0x40000000u + (((key >> 16) - 0x4000u) + column) * 0x10000u;
            Sector* sector = sectors_.Find(at);
            if (!sector) continue;
            std::vector<Object*>& list = object.fixed ? sector->fixed : object.kinematic ? sector->kinematic : sector->dynamic;
            auto found = std::find(list.begin(), list.end(), &object);
            if (found != list.end()) list.erase(found);
            if (--sector->count == 0) sectors_.Erase(at);
        }
    }
}

// 0x004bb942: the object's links go, it leaves its sectors and is marked dead (RemoveDeadObjects frees it).
void cSC4RigidBodySimulator2D::RemoveObject(Object* object) {
    for (auto it = links_.begin(); it != links_.end();) {
        if (object == it->a || object == it->b) it = links_.erase(it);
        else ++it;
    }
    if (object->sectorKey != 0) RemoveFromSectorMap(*object);
    object->alive = false;
}

void cSC4RigidBodySimulator2D::SweepObjectList(std::list<Object>& objects) {
    for (auto it = objects.begin(); it != objects.end();) {
        if (!it->alive) it = objects.erase(it);
        else ++it;
    }
}

void cSC4RigidBodySimulator2D::RemoveDeadObjects() {
    SweepObjectList(fixed_);
    SweepObjectList(kinematic_);
    SweepObjectList(dynamic_);
}

// 0x004bc584: integrate each live body: position, height and angle (wrapped past pi), velocity from the acceleration,
// the friction while it is on (forward and side speeds and the spin each reduced and clamped at zero), the axis-aligned
// extents, and the sector filing when the first sector changed.
void cSC4RigidBodySimulator2D::AdvanceTime(std::list<Object>& objects, float seconds) {
    for (Object& o : objects) {
        if (!o.alive) continue;
        o.x = seconds * o.vx + o.x;
        o.y = seconds * o.vy + o.y;
        o.height = seconds * o.vh + o.height;
        o.angle = seconds * o.angularVelocity + o.angle;
        if (kPi < std::fabs(o.angle)) o.angle = static_cast<float>(std::fmod(static_cast<double>(o.angle), kTwoPi));
        o.c = std::cos(o.angle);
        o.s = std::sin(o.angle);
        o.vx = seconds * o.ax + o.vx;
        o.vy = seconds * o.ay + o.vy;
        if (o.friction) {
            const float s = o.s, c = o.c;
            const float forwardRaw = (c * o.vx + s * o.vy) - seconds * o.forwardFriction;
            const float forward = 0.0f <= forwardRaw ? forwardRaw : 0.0f;
            const float sideRaw = (-s * o.vx + c * o.vy) - seconds * o.sideFriction;
            const float side = 0.0f <= sideRaw ? sideRaw : 0.0f;
            o.vx = -s * side + forward * c;
            o.vy = side * c + forward * s;
            const float spin = o.angularVelocity - seconds * o.angularFriction;
            o.angularVelocity = 0.0f <= spin ? spin : 0.0f;
        }
        const float absS = std::fabs(o.s), absC = std::fabs(o.c);
        o.extentX = o.halfX * absC + absS * o.halfY;
        o.extentY = o.halfX * absS + o.halfY * absC;
        const std::int32_t sectorY = FloorSector((o.y - o.extentY) * inverseSectorSize_);
        const std::int32_t sectorX = FloorSector((o.x - o.extentX) * inverseSectorSize_);
        const std::uint32_t key = SectorKey(sectorX, sectorY);
        if (key != o.sectorKey) {
            if (o.sectorKey != 0) RemoveFromSectorMap(o);
            o.sectorKey = key;
            AddToSectorMap(o);
        }
    }
}

// 0x004bc878.
void cSC4RigidBodySimulator2D::AdvanceTime(float seconds) {
    AdvanceTime(dynamic_, seconds);
    AdvanceTime(kinematic_, seconds);
    DoCollisions();
}

// 0x004bc8b8: the object from the description, its inverse inertia (inverse mass x 48 / (halfX^2 + halfY^2)). A fixed
// object is filed in its sectors at once (its box's extents in whole sectors, rounded up); a moving one covers the
// sectors of its radius (filed on its first step) and goes to the kinematic list below the inverse-mass threshold.
cSC4RigidBodySimulator2D::Object* cSC4RigidBodySimulator2D::AddObject(const ObjectDesc& desc, bool fixed) {
    Object o;
    static_cast<ObjectDesc&>(o) = desc;
    const float radiusSquared = o.halfX * o.halfX + o.halfY * o.halfY;
    o.inverseInertia = (o.inverseMass * kInertiaFactor) / radiusSquared;
    o.kinematic = false;
    o.fixed = false;
    o.sectorKey = 0;
    o.alive = true;
    if (fixed) {
        o.fixed = true;
        o.c = std::cos(o.angle);
        o.s = std::sin(o.angle);
        o.extentX = o.halfX * std::fabs(o.c) + std::fabs(o.s) * o.halfY;
        o.extentY = o.halfX * std::fabs(o.s) + o.halfY * std::fabs(o.c);
        const std::int32_t sectorY = FloorSector((o.y - o.extentY) * inverseSectorSize_);
        const std::int32_t sectorX = FloorSector((o.x - o.extentX) * inverseSectorSize_);
        o.sectorKey = SectorKey(sectorX, sectorY);
        o.sectorsX = static_cast<std::uint32_t>(CeilCount((o.extentX + o.extentX) * inverseSectorSize_));
        o.sectorsY = static_cast<std::uint32_t>(CeilCount((o.extentY + o.extentY) * inverseSectorSize_));
        fixed_.push_back(o);
        AddToSectorMap(fixed_.back());
        return &fixed_.back();
    }
    o.sectorsX = static_cast<std::uint32_t>(CeilCount(std::sqrt(radiusSquared) * inverseSectorSize_));
    o.sectorsY = o.sectorsX;
    if (kKinematicInverseMass <= o.inverseMass || std::isnan(o.inverseMass)) {
        dynamic_.push_back(o);
        return &dynamic_.back();
    }
    o.kinematic = true;
    kinematic_.push_back(o);
    return &kinematic_.back();
}

}  // namespace sc4
