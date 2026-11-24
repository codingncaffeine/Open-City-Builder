// cSC4DynamicParticleSystem (Mac i386 0x004b8bf4..0x004ba9d2, 0x74 bytes) and its particle (cSC4DynamicParticle, 0x50
// bytes) — the one process-wide world (cSC4DynamicParticleEffect::spDynamicSystem) of the bodies that get knocked about
// while the player drives in U-Drive-It: the props of dynamic-particle effects (cones, barrels: model-based, dynamic) and
// the driven vehicle itself (kinematic, from cSC4TrafficAutomaton's player-drive state). Each particle owns one body of
// the rigid body simulator and keeps the city cells (terrain cells) around it "active": an active cell holds a fixed body
// for each static building on it. The system is a renderer view object (list 3, priority 1000) that draws the particles'
// models, and an animation-tick target (priority 0xc0) that runs the physics in 5 ms steps (at most 0.1 s per tick):
// gravity, the ground (terrain height and normal), friction while on the ground, the active cells following each
// particle, and the model transform from the body (turned to the terrain normal).
//
// Init / Shutdown count users: the first Init registers the view object and the tick, the last Shutdown takes them
// away and answers true (the effect then releases the system).
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "sc4/Render3D.h"
#include "sc4/RigidBodySimulator2D.h"
#include "sc4/SgiHashMap.h"

namespace s3d { class cIS3DModelInstance; }

namespace sc4 {

class cISC4Occupant;

// The services the system calls (all on the original's globals).
class IDynamicParticleHost {
public:
    virtual ~IDynamicParticleHost() = default;
    // SL::spTerrainMap: vslots 0x1c / 0x20 (cells), 0x18 (cell size), 0x30 (height), 0x6c (unit normal)
    virtual std::int32_t TerrainCellsX() = 0;
    virtual std::int32_t TerrainCellsZ() = 0;
    virtual float TerrainCellSize() = 0;
    virtual float TerrainHeight(float x, float z) = 0;
    virtual void TerrainNormal(float x, float z, cS3DVector3& out) = 0;
    // SL::spOccupantManager vslot 0x90 (min, max, any type, no limit): each returned occupant holds a reference
    virtual void OccupantsInBox(const float min[3], const float max[3], std::list<cISC4Occupant*>& out) = 0;
    // IsStaticBuilding(occupant) (i386 0x004b93ea): a building-sized exemplar not flagged to be ignored
    virtual bool IsStaticBuilding(cISC4Occupant* occupant) = 0;
    virtual void ReleaseOccupant(cISC4Occupant* occupant) = 0;
    // SL::spRenderer vslots 0x80 (AddViewObject, list 3, priority 1000) / 0x84; SL::ATM() vslots 0x14 / 0x18 (0xc0)
    virtual void AddViewObject(cISC4ViewObject3D* object) = 0;
    virtual void RemoveViewObject(cISC4ViewObject3D* object) = 0;
    virtual void AddTick(cIGZMessageTarget2* target) = 0;
    virtual void RemoveTick(cIGZMessageTarget2* target) = 0;
};
IDynamicParticleHost* DynamicParticleHost();
void SetDynamicParticleHost(IDynamicParticleHost* host);

constexpr std::uint32_t kDynamicParticleTickMessage = 0x86ad10ee;   // the animation tick (data 2: microseconds)

struct cSC4DynamicParticle {
    cSC4RigidBodySimulator2D::Object* object = nullptr;   // +0x00 (null once it left the city)
    s3d::cIS3DModelInstance* model = nullptr;              // +0x04 (counted)
    std::int32_t range = 0;                                // +0x08: active cells around it (its radius in cells, up)
    std::int32_t cellX = -8388607, cellZ = -8388607;       // +0x0c / +0x10 (0xff800001: none yet)
    std::uint32_t linkId = 0;                              // +0x14 (SetVoidParams 0x10)
    cS3DTransform transform;                               // +0x18: the model's
    // The body as a float record (the driven vehicle writes [0] x, [1] z, [2] / [3] velocity, [4] height, [6] angle and
    // reads [10] the half height); null without a body.
    float* Fields() { return object ? &object->x : nullptr; }
};

class cSC4DynamicParticleSystem final : public cISC4ViewObject3D, public cIGZMessageTarget2 {
public:
    cSC4DynamicParticleSystem();                     // @ 0x004ba3ca
    ~cSC4DynamicParticleSystem();                    // @ 0x004ba50e

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x004ba304
    std::uint32_t AddRef() override { return ++refs_; }            // @ 0x004ba2e8
    std::uint32_t Release() override;                              // @ 0x004ba2f6
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x004b9f80
    void DrawViewObject(SC4DrawContext& context) override;         // Update @ 0x004b8ede

    bool Init();                                                   // @ 0x004b9138
    bool Shutdown();                                               // @ 0x004b8bf4
    cSC4DynamicParticle* AddParticle(const cSC4RigidBodySimulator2D::ObjectDesc& desc, s3d::cIS3DModelInstance* model);   // @ 0x004ba58a
    void DeleteParticle(cSC4DynamicParticle* particle);            // @ 0x004ba9d2
    void LinkParticles(std::uint32_t idA, std::uint32_t idB);      // @ 0x004b8e30
    void DoAnimationTick(std::uint32_t microseconds);              // @ 0x004b9992

    cSC4RigidBodySimulator2D& Simulator() { return simulator_; }
    std::list<cSC4DynamicParticle>& Particles() { return particles_; }
    std::size_t ActiveCellCount() const { return cells_.Size(); }
    std::int32_t InitCount() const { return initCount_; }

private:
    struct Cell {
        std::vector<cSC4RigidBodySimulator2D::Object*> buildings;   // +0x00: the fixed bodies of its static buildings
        std::uint32_t users = 0;                                    // +0x0c
    };
    void ActivateCells(std::int32_t x, std::int32_t z, std::int32_t range);     // ActivateSectors @ 0x004b9508
    void DeactivateCells(std::int32_t x, std::int32_t z, std::int32_t range);   // DeactivateSectors @ 0x004b92b8

    std::uint32_t refs_ = 0;
    std::list<cSC4DynamicParticle> particles_;             // +0x10
    SgiHashMap<Cell> cells_;                               // +0x18: by z << 16 | x
    float inverseCellSize_ = 0.0625f;                      // +0x2c
    float cellSize_ = 16.0f;                               // +0x30
    std::int32_t initCount_ = 0;                           // +0x34
    cSC4RigidBodySimulator2D simulator_;                   // +0x38
};

}  // namespace sc4
