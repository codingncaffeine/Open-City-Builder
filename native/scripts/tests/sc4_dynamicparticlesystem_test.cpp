// cSC4DynamicParticleSystem (Mac i386 0x004b8bf4..0x004ba9d2): the use count, the active cells that follow a particle,
// gravity and the ground, the model transform, a body leaving the city.
#include <cmath>
#include <cstdio>
#include <list>

#include "sc4/DynamicParticleSystem.h"

using namespace sc4;
using Sim = cSC4RigidBodySimulator2D;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }

struct Host final : IDynamicParticleHost {
    std::int32_t TerrainCellsX() override { return 64; }
    std::int32_t TerrainCellsZ() override { return 64; }
    float TerrainCellSize() override { return 16.0f; }
    float TerrainHeight(float, float) override { return groundHeight; }
    void TerrainNormal(float, float, cS3DVector3& out) override { out = cS3DVector3{0.0f, 1.0f, 0.0f}; ++normals; }
    void OccupantsInBox(const float*, const float*, std::list<cISC4Occupant*>&) override { ++boxQueries; }
    bool IsStaticBuilding(cISC4Occupant*) override { return false; }
    void ReleaseOccupant(cISC4Occupant*) override {}
    void AddViewObject(cISC4ViewObject3D*) override { ++views; }
    void RemoveViewObject(cISC4ViewObject3D*) override { --views; }
    void AddTick(cIGZMessageTarget2*) override { ++ticks; }
    void RemoveTick(cIGZMessageTarget2*) override { --ticks; }
    float groundHeight = 10.0f;
    int views = 0, ticks = 0, boxQueries = 0, normals = 0;
};

int main() {
    Host host;
    SetDynamicParticleHost(&host);
    auto* system = new cSC4DynamicParticleSystem();
    system->AddRef();
    // Init / Shutdown count users: the first registers the view object and the tick, the last takes them away
    CHECK(system->Init() && system->Init() && host.views == 1 && host.ticks == 1);
    CHECK(!system->Shutdown() && host.views == 1);

    // the driven vehicle: a kinematic 2 x 2 x 2 body, one active cell around it (ceil(sqrt 2 / 16))
    Sim::ObjectDesc car;
    car.x = 100.0f;
    car.y = 100.0f;
    car.height = 50.0f;
    car.halfX = car.halfY = car.halfHeight = 1.0f;
    car.inverseMass = 1e-7f;
    cSC4DynamicParticle* particle = system->AddParticle(car, nullptr);
    CHECK(particle && particle->range == 1 && particle->cellX < 0 && particle->object && particle->object->kinematic);
    CHECK(Near(particle->transform.translation[0], 100.0f) && Near(particle->transform.translation[2], 100.0f));
    CHECK(particle->Fields() == &particle->object->x && particle->Fields()[10] == 1.0f);

    // one 16 ms tick (4 steps of 5 ms, the last 1 ms): the cells 5..7 x 5..7 around cell (6, 6) become active (nine
    // occupant queries), gravity pulls the body (which is above the ground) down
    system->DoAnimationTick(16000);
    CHECK(particle->cellX == 6 && particle->cellZ == 6 && system->ActiveCellCount() == 9 && host.boxQueries == 9);
    CHECK(particle->object->vh < 0.0f && particle->object->height < 50.0f);
    // the model transform: up the terrain normal, at the body's foot
    CHECK(Near(particle->transform.rotation[1][1], 1.0f) && Near(particle->transform.translation[1], particle->object->height - 1.0f));

    // on the ground: the height is held at the terrain + the half height, friction comes on and the fall is stopped
    particle->object->height = 11.0f;
    particle->object->vh = -1.0f;
    system->DoAnimationTick(5000);
    CHECK(Near(particle->object->height, 11.0f, 0.06f) && particle->object->friction);

    // a prop next to it: a dynamic body, the cells it shares are counted twice
    Sim::ObjectDesc cone = car;
    cone.x = 120.0f;
    cone.inverseMass = 1.0f;
    cone.height = 11.0f;
    cSC4DynamicParticle* prop = system->AddParticle(cone, nullptr);
    system->DoAnimationTick(5000);
    CHECK(prop->cellX == 7 && system->ActiveCellCount() == 12 && host.boxQueries == 12);
    // deleting it gives its cells back: those only it used go
    system->DeleteParticle(prop);
    CHECK(system->ActiveCellCount() == 9 && system->Particles().size() == 1);

    // a body that leaves the city loses its body (the particle stays)
    particle->object->x = -5.0f;
    system->DoAnimationTick(5000);
    CHECK(particle->object == nullptr && particle->Fields() == nullptr && system->Particles().size() == 1);
    system->DeleteParticle(particle);
    CHECK(system->Particles().empty() && system->ActiveCellCount() == 0);   // its cells go with it

    CHECK(system->Shutdown() && host.views == 0 && host.ticks == 0);
    system->Release();
    std::puts(failures ? "sc4_dynamicparticlesystem: FAILED" : "sc4_dynamicparticlesystem: ok");
    return failures != 0;
}
