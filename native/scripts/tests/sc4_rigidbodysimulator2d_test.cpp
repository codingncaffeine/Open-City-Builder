// cSC4RigidBodySimulator2D (Mac i386 0x004bac04..0x004bc8b8) and the SGI-order hash map it files its sectors in.
#include <cmath>
#include <cstdio>
#include <vector>

#include "sc4/RigidBodySimulator2D.h"
#include "sc4/SgiHashMap.h"

using namespace sc4;
using Sim = cSC4RigidBodySimulator2D;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

static Sim::ObjectDesc Box(float x, float y, float inverseMass) {
    Sim::ObjectDesc d;
    d.x = x;
    d.y = y;
    d.halfX = 1.0f;
    d.halfY = 1.0f;
    d.halfHeight = 1.0f;
    d.height = 1.0f;
    d.inverseMass = inverseMass;
    return d;
}

int main() {
    {   // the SGI hash_map order: 193 buckets for 100 asked, new keys at the front of their chain, grown past 193 entries
        SgiHashMap<int> map;
        CHECK(map.BucketCount() == 193);
        map[5] = 1;
        map[5 + 193] = 2;     // same bucket, goes in front
        map[3] = 3;
        std::vector<unsigned> order;
        map.ForEach([&](std::uint32_t key, int&) { order.push_back(key); });
        CHECK(order.size() == 3 && order[0] == 3 && order[1] == 5 + 193 && order[2] == 5);
        CHECK(map.Erase(5 + 193) && !map.Find(5 + 193) && map.Find(5) && *map.Find(5) == 1 && map.Size() == 2);
        for (std::uint32_t k = 100; k < 300; ++k) map[k] = 0;
        CHECK(map.BucketCount() == 389 && map.Size() == 202);
    }
    // a pair belongs to the sector at the larger x and the larger y of their first sectors
    CHECK(Sim::IsPrimaryCollision(0x40024005u, 0x40024003u, 0x40014005u));
    CHECK(!Sim::IsPrimaryCollision(0x40014005u, 0x40024003u, 0x40014005u));

    {   // two dynamic boxes driving into each other: the corner contact (the closest-approach corner of b, at its +y edge)
        // gives a small impulse and a spin, and the bodies closer than 4 units are each pushed the whole shortfall apart
        Sim sim;
        Sim::ObjectDesc left = Box(97.0f, 100.0f, 1.0f);
        left.vx = 10.0f;
        Sim::ObjectDesc right = Box(103.5f, 100.0f, 1.0f);
        right.vx = -10.0f;
        Sim::Object* a = sim.AddObject(left, false);
        Sim::Object* b = sim.AddObject(right, false);
        CHECK(Near(a->inverseInertia, 24.0f));      // 1 x 48 / (1 + 1)
        CHECK(a->sectorsX == 1 && a->sectorsY == 1);   // ceil(sqrt 2 / 64)
        CHECK(sim.DynamicObjects().size() == 2 && sim.KinematicObjects().empty());
        for (int i = 0; i < 200 && a->contacts == 0; ++i) sim.AdvanceTime(0.005f);
        CHECK(a->contacts == 1 && b->contacts == 1);
        CHECK(Near(b->x - a->x, 6.0f, 1e-3f));       // were 2 apart: each moved 4 - 2
        CHECK(Near(a->vx, 8.44f, 1e-2f) && Near(b->vx, -8.44f, 1e-2f));   // j = -0.56, then the unit push-apart speed
        CHECK(Near(a->angularVelocity, 13.442f, 1e-2f) && Near(b->angularVelocity, -13.442f, 1e-2f));
        CHECK(Near(a->contact.y, 1.0f) && Near(a->normal.x, 1.0f) && Near(a->normal.y, 0.0f));
    }
    {   // a dynamic box against a fixed building (filed in its sectors at once): the box is slowed by contact after
        // contact as it presses into the face; the far-away building's box does not move
        Sim sim;
        Sim::ObjectDesc building = Box(200.0f, 100.0f, 0.0f);
        building.halfX = 40.0f;      // 80 wide: two sector columns
        building.halfY = 10.0f;
        building.halfHeight = 20.0f;
        building.height = 20.0f;
        Sim::Object* wall = sim.AddObject(building, true);
        CHECK(wall->fixed && wall->sectorsX == 2 && wall->sectorsY == 1 && sim.Sectors().Size() == 2);
        CHECK(Near(wall->extentX, 40.0f) && Near(wall->extentY, 10.0f));
        Sim::ObjectDesc cone = Box(150.0f, 100.0f, 0.5f);
        cone.vx = 20.0f;
        Sim::Object* prop = sim.AddObject(cone, false);
        for (int i = 0; i < 160; ++i) sim.AdvanceTime(0.005f);
        CHECK(prop->contacts > 20 && prop->vx >= 0.0f && prop->vx < 0.1f && prop->x < 162.0f);
        CHECK(wall->x == 200.0f);
        sim.RemoveObject(wall);
        CHECK(!wall->alive && sim.FixedObjects().size() == 1);
        sim.RemoveDeadObjects();
        CHECK(sim.FixedObjects().empty());
    }
    {   // the kinematic list takes an inverse mass below 1e-5 (the driven vehicle's 1e-7)
        Sim sim;
        Sim::Object* car = sim.AddObject(Box(10.0f, 10.0f, 1e-7f), false);
        CHECK(car->kinematic && sim.KinematicObjects().size() == 1);
    }
    {   // friction clamps the forward and side speeds and the spin at zero: a box sliding backwards stops at once
        Sim sim;
        Sim::ObjectDesc d = Box(500.0f, 500.0f, 1.0f);
        d.vx = -5.0f;                // backwards along its own x axis (angle 0)
        d.friction = true;
        d.forwardFriction = 1.0f;
        d.angularVelocity = -2.0f;
        Sim::Object* o = sim.AddObject(d, false);
        sim.AdvanceTime(0.005f);
        CHECK(o->vx == 0.0f && o->vy == 0.0f && o->angularVelocity == 0.0f);
        o->vx = 5.0f;                // forwards: slowed by 1 per second
        sim.AdvanceTime(0.5f);
        CHECK(Near(o->vx, 4.5f));
    }
    {   // a link pulls its stretched ends together and breaks above its momentum
        Sim sim;
        Sim::Object* a = sim.AddObject(Box(300.0f, 300.0f, 1.0f), false);
        Sim::Object* b = sim.AddObject(Box(310.0f, 300.0f, 1.0f), false);
        b->vx = 5.0f;
        Sim::Link link;
        link.a = a;
        link.b = b;
        link.maxDistance = 1.0f;
        link.breakMomentum = 500000.0f;
        sim.LinkObjects(link);
        sim.AdvanceTime(0.005f);
        CHECK(sim.Links().size() == 1 && Near(a->vx, 3.501f, 1e-2f) && Near(b->vx, 1.499f, 1e-2f));
        sim.RemoveObject(a);
        CHECK(sim.Links().empty());
        // a link with no strength breaks as soon as its separating ends carry momentum
        Sim::Object* c = sim.AddObject(Box(320.0f, 320.0f, 1.0f), false);
        Sim::Object* d = sim.AddObject(Box(330.0f, 320.0f, 1.0f), false);
        d->vx = 5.0f;
        Sim::Link weak;
        weak.a = c;
        weak.b = d;
        weak.maxDistance = 1.0f;
        weak.breakMomentum = 0.0f;
        sim.LinkObjects(weak);
        sim.AdvanceTime(0.005f);
        CHECK(sim.Links().empty());
    }
    std::puts(failures ? "sc4_rigidbodysimulator2d: FAILED" : "sc4_rigidbodysimulator2d: ok");
    return failures != 0;
}
