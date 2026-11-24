// cSC4DynamicParticleSystem — see DynamicParticleSystem.h. Hand port of the Mac i386 functions (the tick and the draw
// read from the disassembly).
#include "sc4/DynamicParticleSystem.h"

#include <cmath>
#include <cstring>

#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DrawContext.h"
#include "sc4/Occupant.h"

namespace sc4 {

namespace {

IDynamicParticleHost* g_host = nullptr;

constexpr float kPhysicsQuantum = 0.005f;        // (anonymous)::kPhysicsQuantum (0x00a70b9c)
constexpr float kMaxTickSeconds = 0.1f;          // 0x00a70714
constexpr float kGravity = -9.8f;                // 0x00a70b64
constexpr float kOnGroundBand = 0.1f;            // 0x00a70714
constexpr float kMicroseconds = 1000000.0f;      // 0x00a708e0
constexpr std::int32_t kNoCell = static_cast<std::int32_t>(0xff800001u);

std::int32_t Bits(float f) {
    std::int32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}
// floor(v) through (v - 0.5) + 1.5 x 2^23 (one more when v is that next integer), as the original rounds a cell.
std::int32_t FloorCell(float v) {
    const float t = (v - 0.5f) + 12582912.0f;
    const std::int32_t low = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b400000u);
    const std::int32_t high = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b3fffffu);
    const float highF = static_cast<float>(high);
    if (std::isnan(v) || std::isnan(highF)) return low;
    return v == highF ? high : low;
}
// ceil(v) through (v + 0.5) + 1.5 x 2^23 (one less when v is that smaller integer).
std::int32_t CeilCells(float v) {
    const float t = v + 0.5f + 12582912.0f;
    const std::int32_t high = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b400000u);
    const std::int32_t low = static_cast<std::int32_t>(static_cast<std::uint32_t>(Bits(t)) - 0x4b400001u);
    const float lowF = static_cast<float>(low);
    if (std::isnan(v) || std::isnan(lowF)) return high;
    return v == lowF ? low : high;
}
cS3DVector3 Normalized(const cS3DVector3& v) {
    cS3DVector3 out = v;
    nS3D::Normalize(out);
    return out;
}
void SetColumn(cS3DTransform& t, int column, const cS3DVector3& v) {
    t.rotation[0][column] = v.x;
    t.rotation[1][column] = v.y;
    t.rotation[2][column] = v.z;
}

}  // namespace

IDynamicParticleHost* DynamicParticleHost() { return g_host; }
void SetDynamicParticleHost(IDynamicParticleHost* host) { g_host = host; }

// 0x004ba3ca: the cell size from the terrain map (1 / size kept beside it).
cSC4DynamicParticleSystem::cSC4DynamicParticleSystem() {
    if (g_host) {
        const float size = g_host->TerrainCellSize();
        cellSize_ = size;
        inverseCellSize_ = 1.0f / size;
    }
}

cSC4DynamicParticleSystem::~cSC4DynamicParticleSystem() {
    for (cSC4DynamicParticle& particle : particles_)
        if (particle.model) particle.model->Release();
}

bool cSC4DynamicParticleSystem::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    if (iid == 1) {   // cRZUnknown::QueryInterface: the object itself
        *out = static_cast<cISC4ViewObject3D*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4DynamicParticleSystem::Release() {
    if (refs_ <= 1) {
        delete this;
        return 0;
    }
    return --refs_;
}

// 0x004b9f80: the animation tick (data 2 = microseconds).
bool cSC4DynamicParticleSystem::DoMessage(cIGZMessage2* message) {
    if (!message || message->GetType() != kDynamicParticleTickMessage) return false;
    DoAnimationTick(static_cast<std::uint32_t>(static_cast<cIGZMessage2Standard*>(message)->GetData2()));
    return true;
}

// 0x004b9138 / 0x004b8bf4.
bool cSC4DynamicParticleSystem::Init() {
    if (++initCount_ == 1 && g_host) {
        g_host->AddViewObject(this);
        g_host->AddTick(this);
    }
    return true;
}
bool cSC4DynamicParticleSystem::Shutdown() {
    if (--initCount_ != 0) return false;
    if (g_host) {
        g_host->RemoveTick(this);
        g_host->RemoveViewObject(this);
    }
    return true;
}

// 0x004b8e30: the two particles with these link ids are tied end to end along their own y axes (a's -halfY end to b's
// +halfY end), within 1 unit, breaking above a momentum of 500000.
void cSC4DynamicParticleSystem::LinkParticles(std::uint32_t idA, std::uint32_t idB) {
    cSC4DynamicParticle* a = nullptr;
    cSC4DynamicParticle* b = nullptr;
    for (cSC4DynamicParticle& particle : particles_) {
        if (particle.linkId == idA) a = &particle;
        else if (particle.linkId == idB) b = &particle;
    }
    if (!a || !b) return;
    cSC4RigidBodySimulator2D::Link link;
    link.a = a->object;
    link.b = b->object;
    link.offsetA = cS3DVector2{0.0f, -link.a->halfY};
    link.offsetB = cS3DVector2{0.0f, link.b->halfY};
    link.maxDistance = 1.0f;
    link.breakMomentum = 500000.0f;
    simulator_.LinkObjects(link);
}

// 0x004ba58a: a body for the description, the cells within its radius active around it (from its first tick), the
// model kept and placed at the body (height 0 until the first tick).
cSC4DynamicParticle* cSC4DynamicParticleSystem::AddParticle(const cSC4RigidBodySimulator2D::ObjectDesc& desc,
                                                            s3d::cIS3DModelInstance* model) {
    particles_.emplace_back();
    cSC4DynamicParticle& particle = particles_.back();
    particle.object = simulator_.AddObject(desc, false);
    particle.cellX = kNoCell;
    particle.cellZ = kNoCell;
    particle.range = CeilCells(std::sqrt(desc.halfX * desc.halfX + desc.halfY * desc.halfY) * inverseCellSize_);
    if (model) model->AddRef();
    if (particle.model) particle.model->Release();
    particle.model = model;
    particle.transform = cS3DTransform{};
    particle.transform.translation[0] = desc.x;
    particle.transform.translation[1] = 0.0f;
    particle.transform.translation[2] = desc.y;
    particle.transform.flags |= 1;
    ++particle.transform.changeCounter;
    return &particle;
}

// 0x004ba9d2: a particle of this system gives up its cells and its body.
void cSC4DynamicParticleSystem::DeleteParticle(cSC4DynamicParticle* particle) {
    for (auto it = particles_.begin(); it != particles_.end(); ++it) {
        if (&*it != particle) continue;
        if (-1 < (particle->cellX | particle->cellZ)) DeactivateCells(particle->cellX, particle->cellZ, particle->range);
        if (it->object) simulator_.RemoveObject(it->object);
        if (it->model) it->model->Release();
        particles_.erase(it);
        return;
    }
}

// 0x004b9508: every cell within `range` of (x, z) (from 0) gains a user; a newly active cell files a fixed body for each
// static building whose box meets the cell column (centre, half extents and, as the original, the centre height as the
// half height).
void cSC4DynamicParticleSystem::ActivateCells(std::int32_t x, std::int32_t z, std::int32_t range) {
    const std::uint32_t startX = x - range < 0 ? 0u : static_cast<std::uint32_t>(x - range);
    const std::uint32_t startZ = z - range < 0 ? 0u : static_cast<std::uint32_t>(z - range);
    for (std::uint32_t row = startZ; row <= static_cast<std::uint32_t>(range + z); ++row) {
        for (std::uint32_t column = startX; column <= static_cast<std::uint32_t>(x + range); ++column) {
            const auto inserted = cells_.Insert(row * 0x10000u + column, Cell{});
            Cell& cell = *inserted.first;
            if (inserted.second) {
                cell.users = 0;
                const float min[3] = {static_cast<float>(column) * cellSize_, -1.7014117e+38f, static_cast<float>(row) * cellSize_};
                const float max[3] = {static_cast<float>(column + 1) * cellSize_, 1.7014117e+38f,
                                      static_cast<float>(row + 1) * cellSize_};
                std::list<cISC4Occupant*> occupants;
                if (g_host) g_host->OccupantsInBox(min, max, occupants);
                for (cISC4Occupant* occupant : occupants) {
                    if (g_host->IsStaticBuilding(occupant)) {
                        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
                        occupant->GetBoundingBox(lo, hi);
                        cSC4RigidBodySimulator2D::ObjectDesc desc;
                        desc.y = (hi[2] + lo[2]) * 0.5f;
                        desc.x = (hi[0] + lo[0]) * 0.5f;
                        desc.halfY = (hi[2] - lo[2]) * 0.5f;
                        desc.halfX = (hi[0] - lo[0]) * 0.5f;
                        desc.height = (hi[1] + lo[1]) * 0.5f;
                        desc.halfHeight = desc.height;
                        cell.buildings.push_back(simulator_.AddObject(desc, true));
                    }
                    g_host->ReleaseOccupant(occupant);
                }
            }
            ++cell.users;
        }
    }
}

// 0x004b92b8: the cells lose a user; a cell left without users drops its buildings' bodies.
void cSC4DynamicParticleSystem::DeactivateCells(std::int32_t x, std::int32_t z, std::int32_t range) {
    const std::uint32_t startX = x - range < 0 ? 0u : static_cast<std::uint32_t>(x - range);
    const std::uint32_t startZ = z - range < 0 ? 0u : static_cast<std::uint32_t>(z - range);
    for (std::uint32_t row = startZ; row <= static_cast<std::uint32_t>(range + z); ++row) {
        for (std::uint32_t column = startX; column <= static_cast<std::uint32_t>(x + range); ++column) {
            const std::uint32_t key = row * 0x10000u + column;
            Cell* cell = cells_.Find(key);
            if (!cell || --cell->users != 0) continue;
            for (cSC4RigidBodySimulator2D::Object* building : cell->buildings) simulator_.RemoveObject(building);
            cells_.Erase(key);
        }
    }
}

// 0x004b9992: up to 0.1 s of physics in 5 ms steps. After each step every particle's body is dropped when it left the
// city (x or z not inside (0, size)), held on the ground (terrain height + its half height) and pulled by gravity; on the
// ground (within 0.1) a velocity into the terrain is turned into an acceleration along the ground with friction on; its
// active cells follow it. Then each model transform: turned so its up is the terrain normal and its forward the body's
// heading, at the body's foot.
void cSC4DynamicParticleSystem::DoAnimationTick(std::uint32_t microseconds) {
    float remaining = static_cast<float>(microseconds) / kMicroseconds;
    if (!g_host) return;
    const float width = static_cast<float>(g_host->TerrainCellsX()) * g_host->TerrainCellSize();
    const float depth = static_cast<float>(g_host->TerrainCellsZ()) * g_host->TerrainCellSize();
    remaining = kMaxTickSeconds < remaining || std::isnan(remaining) ? kMaxTickSeconds : remaining;
    simulator_.RemoveDeadObjects();
    while (0.0f < remaining) {
        const float step = remaining <= kPhysicsQuantum ? remaining : kPhysicsQuantum;
        simulator_.AdvanceTime(step);
        const float fall = step * kGravity;
        for (cSC4DynamicParticle& particle : particles_) {
            cSC4RigidBodySimulator2D::Object* body = particle.object;
            if (!body) continue;
            const float x = body->x, z = body->y;
            if (x <= 0.0f || width <= x || z <= 0.0f || depth <= z) {
                simulator_.RemoveObject(body);
                particle.object = nullptr;
                continue;
            }
            cS3DVector3 normal{0.0f, 0.0f, 0.0f};
            g_host->TerrainNormal(x, z, normal);
            const float ground = g_host->TerrainHeight(x, z) + body->halfHeight;
            if (body->height < ground) body->height = ground;
            const float vh = fall + body->vh;
            body->vh = vh;
            body->friction = false;
            const float above = std::fabs(body->height - ground);
            if (above < kOnGroundBand) {
                const float into = body->vx * normal.x + vh * normal.y + body->vy * normal.z;
                if (into < 0.0f) {
                    body->ax = -normal.x * into;
                    body->ay = -normal.z * into;
                    body->vh = -normal.y * into + body->vh;
                    body->friction = true;
                }
            }
            const std::int32_t cellX = FloorCell(body->x * inverseCellSize_);
            const std::int32_t cellZ = FloorCell(body->y * inverseCellSize_);
            if (cellX != particle.cellX || cellZ != particle.cellZ) {
                ActivateCells(cellX, cellZ, particle.range);
                if (-1 < (particle.cellX | particle.cellZ)) DeactivateCells(particle.cellX, particle.cellZ, particle.range);
                particle.cellX = cellX;
                particle.cellZ = cellZ;
            }
        }
        remaining = remaining - step;
    }
    for (cSC4DynamicParticle& particle : particles_) {
        cSC4RigidBodySimulator2D::Object* body = particle.object;
        if (!body) continue;
        cS3DVector3 normal{0.0f, 0.0f, 0.0f};
        g_host->TerrainNormal(body->x, body->y, normal);
        cS3DTransform& t = particle.transform;
        t = cS3DTransform{};
        const cS3DVector3 heading{-body->s, 0.0f, body->c};
        const cS3DVector3 side = Normalized(::cross(normal, heading));
        const cS3DVector3 forward = Normalized(::cross(side, normal));
        SetColumn(t, 0, side);
        SetColumn(t, 1, normal);
        SetColumn(t, 2, forward);
        t.flags |= 4;
        ++t.changeCounter;
        t.translation[0] = body->x;
        t.translation[1] = body->height - body->halfHeight;
        t.translation[2] = body->y;
        t.flags |= 1;
        ++t.changeCounter;
    }
}

// Update (0x004b8ede): each particle with a body asks the terrain for its normal (unused), then draws its model at the
// particle's transform with a white, opaque shade (flag 2 cleared); the shade is reset afterwards.
void cSC4DynamicParticleSystem::DrawViewObject(SC4DrawContext& context) {
    for (cSC4DynamicParticle& particle : particles_) {
        cSC4RigidBodySimulator2D::Object* body = particle.object;
        if (!body) continue;
        cS3DVector3 normal{0.0f, 0.0f, 0.0f};
        if (g_host) g_host->TerrainNormal(body->x, body->y, normal);
        if (!particle.model) continue;
        s3d::SC4DrawAttributes attributes = particle.model->GetDrawAttributes();
        particle.model->SetTransform(particle.transform);
        attributes.shade[3] = 255.0f;
        attributes.flags &= ~std::uint32_t{2};
        attributes.shade[0] = 1.0f;
        attributes.shade[1] = 1.0f;
        attributes.shade[2] = 1.0f;
        particle.model->SetDrawAttributes(attributes);
        particle.model->Draw(&context, false);
    }
    context.ResetShade();
}

}  // namespace sc4
