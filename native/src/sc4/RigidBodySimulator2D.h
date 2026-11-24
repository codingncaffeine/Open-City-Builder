// cSC4RigidBodySimulator2D (Mac i386 0x004bac04..0x004bc8b8 and the inlined helpers at 0x00803a5e..0x00804554) — the
// little physics world behind the dynamic particles: oriented boxes on the ground plane (x, z as the 2D x, y) with a
// height band, moved by their velocities, slowed by per-axis friction, bounced off each other with impulses and kept
// apart. Three kinds of objects: dynamic ones (finite mass: the props a driven vehicle knocks over), kinematic ones (an
// inverse mass below kfAutomatonOrientationEpsilon: the driven vehicle itself, moved by its owner) and fixed ones (static
// buildings around the active area, AddObject(desc, true)). Every object is filed in the sectors (64 units, inverse
// 1/64) its box covers; a pair is tested once, in the sector at the larger of their first sector coordinates
// (IsPrimaryCollision). Links tie two objects at a point each and pull them together past a distance, breaking above a
// momentum.
//
// Quirks kept (asm-checked): the separating-axis test sums in double precision; friction clamps the forward and side
// speeds and the spin at zero (an object sliding backwards stops at once); the push-apart of two bodies closer than 4
// units also moves an infinite-mass partner (a fixed building's box shifts); the fixed objects' half height is the
// centre height given by the caller (see cSC4DynamicParticleSystem::ActivateSectors).
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/SgiHashMap.h"

namespace sc4 {

class cSC4RigidBodySimulator2D {
public:
    // The object as the caller describes it (0x60 bytes; the constructor at 0x0063ebc6 clears +0x00..+0x0c, +0x20, +0x24,
    // +0x30, +0x34, +0x40, +0x44, the contact point and the normal; the callers set the rest).
    struct ObjectDesc {
        float x = 0.0f, y = 0.0f;                    // +0x00 / +0x04: position (world x, z)
        float vx = 0.0f, vy = 0.0f;                  // +0x08 / +0x0c
        float height = 0.0f;                         // +0x10: centre height
        float vh = 0.0f;                             // +0x14
        float angle = 0.0f;                          // +0x18 (radians)
        float angularVelocity = 0.0f;                // +0x1c
        float halfX = 0.0f, halfY = 0.0f;            // +0x20 / +0x24: box half extents along its own axes
        float halfHeight = 0.0f;                     // +0x28
        float inverseMass = 0.0f;                    // +0x2c (below 1e-5: kinematic)
        float forwardFriction = 0.0f;                // +0x30 (per second)
        float sideFriction = 0.0f;                   // +0x34
        float angularFriction = 0.0f;                // +0x38
        bool friction = false;                       // +0x3c: apply the three (the particle system sets it while on ground)
        float ax = 0.0f, ay = 0.0f;                  // +0x40 / +0x44: acceleration
        cS3DVector3 contact{0.0f, 0.0f, 0.0f};       // +0x48: last collision point (x, height, z)
        cS3DVector2 normal{0.0f, 0.0f};              // +0x54: last collision normal
        std::int32_t contacts = 0;                   // +0x5c: collisions counted (cleared by the effect that reads them)
    };
    struct Object : ObjectDesc {
        float extentX = 0.0f, extentY = 0.0f;        // +0x60 / +0x64: axis-aligned half extents of the turned box
        float inverseInertia = 0.0f;                 // +0x68: inverse mass x 48 / (halfX^2 + halfY^2)
        float c = 0.0f, s = 0.0f;                    // +0x6c / +0x70: cS3DTransform2(angle)
        bool kinematic = false;                      // +0x74
        bool fixed = false;                          // +0x75
        bool alive = true;                           // +0x76: RemoveObject clears it; RemoveDeadObjects frees it
        std::uint32_t sectorKey = 0;                 // +0x78: first sector, x << 16 | y biased by 0x4000 each (0 = none)
        std::uint32_t sectorsX = 0, sectorsY = 0;    // +0x7c / +0x80
    };
    struct Link {
        Object* a = nullptr;                         // +0x00
        Object* b = nullptr;                         // +0x04
        cS3DVector2 offsetA, offsetB;                // +0x08 / +0x10: the joint in each body's frame
        float maxDistance = 0.0f;                    // +0x18
        float breakMomentum = 0.0f;                  // +0x1c
    };
    struct Sector {
        std::vector<Object*> dynamic;                // +0x00
        std::vector<Object*> kinematic;              // +0x0c
        std::vector<Object*> fixed;                  // +0x18
        std::uint32_t count = 0;                     // +0x24: objects filed here (the sector is dropped at zero)
    };

    cSC4RigidBodySimulator2D() = default;            // @ 0x004bc40c

    Object* AddObject(const ObjectDesc& desc, bool fixed);   // @ 0x004bc8b8
    void RemoveObject(Object* object);                       // @ 0x004bb942
    void RemoveDeadObjects();                                // @ 0x004bada0
    void LinkObjects(const Link& link) { links_.push_back(link); }   // @ 0x004bbf0c
    void AdvanceTime(float seconds);                         // @ 0x004bc878

    // The pieces (public for the tests).
    static cS3DVector2 GetRelativeVelocityAtPoint(const Object& a, const Object& b, const cS3DVector2& point);   // @ 0x004bac04
    static cS3DVector2 GetRelativeMomentumAtPoint(const Object& a, const Object& b, const cS3DVector2& point);   // @ 0x004bac9e
    static cS3DVector2 GetClosestApproach(const cS3DVector2& point, const cS3DVector2& direction, const Object& object);   // @ 0x004badda
    static bool TestForCollision(const Object& a, const Object& b, cS3DVector2& normal, cS3DVector2& point);   // @ 0x004baee0
    static void ResolveCollision(Object& a, Object& b, const cS3DVector2& normal, const cS3DVector2& point,
                                 const cS3DVector2& extraVelocity);   // @ 0x004bb99a
    static bool IsPrimaryCollision(std::uint32_t sector, std::uint32_t keyA, std::uint32_t keyB);   // @ 0x00803e78

    const std::list<Object>& DynamicObjects() const { return dynamic_; }
    const std::list<Object>& KinematicObjects() const { return kinematic_; }
    const std::list<Object>& FixedObjects() const { return fixed_; }
    const std::list<Link>& Links() const { return links_; }
    SgiHashMap<Sector>& Sectors() { return sectors_; }

private:
    void AdvanceTime(std::list<Object>& objects, float seconds);   // @ 0x004bc584
    void DoCollisions();                                            // @ 0x004bbf1e
    void CheckAgainstInfiniteMassObjects(std::uint32_t sector, Object& object, std::vector<Object*>& others);   // @ 0x004bbd0a
    void AddToSectorMap(Object& object);                            // @ 0x004bc4b8
    void RemoveFromSectorMap(Object& object);                       // @ 0x004bb7ee
    static void SweepObjectList(std::list<Object>& objects);        // @ 0x004bad72
    static void RecordContact(Object& a, Object& b, const cS3DVector2& normal, const cS3DVector2& point);

    std::list<Object> dynamic_;                      // +0x00
    std::list<Object> kinematic_;                    // +0x08
    std::list<Object> fixed_;                        // +0x10
    std::list<Link> links_;                          // +0x18
    SgiHashMap<Sector> sectors_;                     // +0x20 (hash_map<unsigned long, Sector>, 100 buckets asked)
    float sectorSize_ = 64.0f;                       // +0x34 (0x42800000)
    float inverseSectorSize_ = 0.015625f;            // +0x38 (0x3c800000)
};

}  // namespace sc4
