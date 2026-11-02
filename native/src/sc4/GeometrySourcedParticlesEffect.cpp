#include "sc4/GeometrySourcedParticlesEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "s3d/MaterialManager.h"
#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"
#include "sc4/DrawContext.h"

namespace sc4 {
namespace {

constexpr float kSizeDivisor = 50.0f;        // i386 0x00a7079c: the size curve is relative to 50
constexpr float kExplosionHeight = 0.1f;     // i386 0x00a70714: the explosion point is a tenth up the model
constexpr float kTwoPi = 6.2831855f;         // i386 0x00a707e4
constexpr float kMinDistance2 = 1.0e-4f;     // i386 0x00a7078c

// CurveLookup: uniformly spaced keys over [0, 1], linear between them; a single key is constant.
float Curve(const std::vector<float>& curve, float t) {
    if (curve.empty()) return 0.0f;
    if (curve.size() == 1) return curve[0];
    const float position = static_cast<float>(curve.size() - 1) * t;
    const std::size_t index = static_cast<std::size_t>(std::max(0.0f, std::floor(position)));
    if (index + 1 >= curve.size()) return curve.back();
    const float fraction = position - static_cast<float>(index);
    return 0.0f < fraction ? (curve[index + 1] - curve[index]) * fraction + curve[index] : curve[index];
}
cS3DVector3 Curve(const std::vector<cS3DVector3>& curve, float t) {
    if (curve.empty()) return cS3DVector3{1, 1, 1};
    if (curve.size() == 1) return curve[0];
    const float position = static_cast<float>(curve.size() - 1) * t;
    const std::size_t index = static_cast<std::size_t>(std::max(0.0f, std::floor(position)));
    if (index + 1 >= curve.size()) return curve.back();
    const float f = position - static_cast<float>(index), r = 1.0f - f;
    const cS3DVector3& a = curve[index];
    const cS3DVector3& b = curve[index + 1];
    return cS3DVector3{f * b.x + r * a.x, f * b.y + r * a.y, f * b.z + r * a.z};
}
cS3DVector3 Sub(const cS3DVector3& a, const cS3DVector3& b) { return cS3DVector3{a.x - b.x, a.y - b.y, a.z - b.z}; }
float Length(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
// cS3DTransform::TransformVector / TransformPoint (rows of the rotation, times the scale, plus the translation for a point).
cS3DVector3 TransformVector(const cS3DTransform& t, const cS3DVector3& v) {
    const float s = t.scale;
    return cS3DVector3{(t.rotation[0][0] * v.x + t.rotation[0][1] * v.y + t.rotation[0][2] * v.z) * s,
                       (t.rotation[1][0] * v.x + t.rotation[1][1] * v.y + t.rotation[1][2] * v.z) * s,
                       (t.rotation[2][0] * v.x + t.rotation[2][1] * v.y + t.rotation[2][2] * v.z) * s};
}
cS3DVector3 TransformPoint(const cS3DTransform& t, const cS3DVector3& v) {
    cS3DVector3 out = TransformVector(t, v);
    return cS3DVector3{out.x + t.translation[0], out.y + t.translation[1], out.z + t.translation[2]};
}
// The texture of a model material's first layer: the binding's record starts with the driver texture id.
std::uint32_t MaterialTexture(const s3d::cS3DMaterial& material) {
    if (material.layers[0].texture == 0) return 0;
    const std::uint32_t* const* record = reinterpret_cast<const std::uint32_t* const*>(material.layers[0].texture);
    return *record ? **record : 0;
}
const SC4Mesh& MeshOf(const s3d::MeshRecord& record) { return *reinterpret_cast<const SC4Mesh*>(record.data()); }
const s3d::cS3DMaterial& MaterialOf(const s3d::MaterialRecord& record) { return *reinterpret_cast<const s3d::cS3DMaterial*>(record.data()); }
cSC4GSParticle* GS(cSCParticle* particle) { return static_cast<cSC4GSParticle*>(cSC4ParticlesEffect::EffectsParticle(particle)); }
}  // namespace

cSC4GeometrySourcedParticlesEffect::cSC4GeometrySourcedParticlesEffect() = default;

cSC4GeometrySourcedParticlesEffect::~cSC4GeometrySourcedParticlesEffect() {
    if (model_) static_cast<s3d::cS3DMeshAndMaterial*>(model_)->Release();
}

// 0x0040e372: model descriptions only (11-bit flag 0); triangles, white, nothing generated yet; a description that names a
// model takes it as its source (the demolition ones name none: the occupant's model comes as void parameter 7).
void cSC4GeometrySourcedParticlesEffect::SetDescription(cSC4ParticlesDescription* description) {
    if (!description || (description->flag10 & 1) == 0) return;
    description_ = description;
    SetDrawMode(description->drawMode);
    inverseRate_ = 1.0 / static_cast<double>(std::max(1.0e-6f, description->spawnPhase));
    colour_ = cS3DVector3{1.0f, 1.0f, 1.0f};
    generated_ = false;
    radius_ = description->terrainRadius * description->terrainRadius * 1024.0f;
}

// 0x0040e922: the model's meshes become the source, and the explosion point its foot (a tenth up its bounds).
void cSC4GeometrySourcedParticlesEffect::SetModel(s3d::cIS3DModelInstance* model) {
    auto* instance = dynamic_cast<s3d::cS3DModelInstance*>(model);
    if (instance) static_cast<s3d::cS3DMeshAndMaterial*>(instance)->AddRef();
    if (model_) static_cast<s3d::cS3DMeshAndMaterial*>(model_)->Release();
    model_ = instance;
    SetupArrays();
    if (!model) return;
    cS3DBoundingBox box{};
    model->GetWorldBounds(box);   // instance vslot 0x20 (i386 slot 8)
    explosionPoint_ = cS3DVector3{(box.min[0] + box.max[0]) * 0.5f, box.min[1] + (box.max[1] - box.min[1]) * kExplosionHeight,
                                  (box.min[2] + box.max[2]) * 0.5f};
}

bool cSC4GeometrySourcedParticlesEffect::SetVoidParams(int type, void** values, int count) {
    if (type == 7 && values) {   // cSC4VisualEffect::SetModelFromOccupant
        SetModel(static_cast<s3d::cIS3DModelInstance*>(values[0]));
        return true;
    }
    return cSC4ParticlesEffect::SetVoidParams(type, values, count);
}

// 0x0040e7e4: 8 the explosion point, 9 the point from an eye and a view direction, 0x101 the explosion depth offset.
bool cSC4GeometrySourcedParticlesEffect::SetFloatParams(int type, const float* values, int count) {
    if (type == 9) {
        SetExplosionPointFromViewDir(cS3DVector3{values[0], values[1], values[2]}, cS3DVector3{values[3], values[4], values[5]});
        return true;
    }
    if (type == 0x101) {
        depthOffset_ = values[0];
        return true;
    }
    if (type == 8) {
        explosionPoint_ = cS3DVector3{values[0], values[1], values[2]};
        return true;
    }
    return cSC4ParticlesEffect::SetFloatParams(type, values, count);
}

// 0x0040e4e6: along the view direction, halfway through the model's depth (plus the depth offset) from the eye.
void cSC4GeometrySourcedParticlesEffect::SetExplosionPointFromViewDir(const cS3DVector3& eye, const cS3DVector3& direction) {
    if (!model_) return;
    const cS3DVector3 origin = TransformPoint(sourceTransform_, cS3DVector3{effectTransform_.translation[0], effectTransform_.translation[1],
                                                                            effectTransform_.translation[2]});
    bool any = false;
    float nearest = 0.0f, farthest = 0.0f;
    for (const s3d::MeshRecord& record : model_->Meshes()) {
        const SC4Mesh& mesh = MeshOf(record);
        if (!mesh.vertices || !mesh.vertices->data) continue;
        const auto* bytes = static_cast<const std::uint8_t*>(mesh.vertices->data);
        for (std::uint32_t v = 0; v < mesh.vertices->count; ++v) {
            float p[3];
            std::memcpy(p, bytes + std::size_t(v) * 24, 12);
            const float d = (p[0] + origin.x) * direction.x + (p[1] + origin.y) * direction.y + (p[2] + origin.z) * direction.z;
            if (!any) nearest = farthest = d, any = true;
            nearest = std::min(nearest, d);
            farthest = std::max(farthest, d);
        }
    }
    if (!any) return;
    const float t = ((nearest + farthest) * 0.5f - (eye.x * direction.x + eye.y * direction.y + eye.z * direction.z)) + depthOffset_;
    explosionPoint_ = cS3DVector3{eye.x + direction.x * t, eye.y + direction.y * t, eye.z + direction.z * t};
}

// 0x0040e0a4 with SetGeometrySource (0x0040e1f0): one triangle pool (format 10, three vertices a piece) per textured material.
void cSC4GeometrySourcedParticlesEffect::SetupArrays() {
    textures_.clear();
    if (model_)
        for (const s3d::MaterialRecord& record : model_->Materials())
            if (MaterialOf(record).layerCount != 0) textures_.push_back(MaterialTexture(MaterialOf(record)));
    pools_.assign(textures_.size(), cSC4PrimitivePool{});
    for (cSC4PrimitivePool& pool : pools_) pool.SetFormat(0, 3, 0, 10, nullptr, nullptr);
}

// 0x0040e9f2: a piece at `position` flying in a random direction of the description's velocity box at a random speed,
// pushed away from the description's centre; random life, size, alpha and spin.
cSC4GSParticle* cSC4GeometrySourcedParticlesEffect::NewParticle(const cS3DVector3& position) {
    auto* particle = static_cast<cSC4GSParticle*>(std::calloc(1, sizeof(cSC4GSParticle)));
    const cSC4ParticlesDescription* d = description_;
    cRZRandom& random = Random();
    particle->alive = 1;
    particle->age = 0.0f;
    particle->position = position;
    const cS3DVector3 direction{static_cast<float>(random.RandomDoubleRangeUniform(d->velocityMin[0], d->velocityMax[0])),
                                static_cast<float>(random.RandomDoubleRangeUniform(d->velocityMin[1], d->velocityMax[1])),
                                static_cast<float>(random.RandomDoubleRangeUniform(d->velocityMin[2], d->velocityMax[2]))};
    const float speed = static_cast<float>(random.RandomDoubleRangeUniform(d->speedMin, d->speedMax));
    const float length = Length(direction);
    particle->velocity = length > 0.0f ? cS3DVector3{direction.x * speed / length, direction.y * speed / length, direction.z * speed / length}
                                       : cS3DVector3{0.0f, speed, 0.0f};
    if (0.0f < d->terrainRadius) {   // +0xf0 push away from +0xf4
        const cS3DVector3 away = Sub(position, cS3DVector3{d->terrainCentre[0], d->terrainCentre[1], d->terrainCentre[2]});
        const float distance2 = away.x * away.x + away.y * away.y + away.z * away.z;
        if (kMinDistance2 <= distance2) {
            const float k = d->terrainRadius / std::sqrt(distance2);
            particle->velocity = cS3DVector3{away.x * k + particle->velocity.x, away.y * k + particle->velocity.y, away.z * k + particle->velocity.z};
        }
    }
    particle->lifetime = static_cast<float>(random.RandomDoubleRangeUniform(d->lifetimeMin, d->lifetimeMax));
    particle->size = static_cast<float>(random.RandomDoubleRangeUniform(1.0 - d->sizeJitter, d->sizeJitter + 1.0)) * sizeScale_;
    particle->alpha = static_cast<float>(random.RandomDoubleRangeUniform(1.0 - d->alphaJitter, d->alphaJitter + 1.0));
    particle->rotation = d->spinRange != 0.0f ? static_cast<float>(random.RandomDoubleRangeUniform(-d->spinRange, d->spinRange)) : 1.0f;
    return particle;
}

// 0x0040ec96: one piece: its centroid is the particle, the two edges and the three texture coordinates are kept.
void cSC4GeometrySourcedParticlesEffect::AddTriangle(const std::vector<cS3DVector3>& points, const std::vector<cS3DVector2>& uvs,
                                                     const int corners[3], std::uint32_t material) {
    const cS3DVector3& p0 = points[static_cast<std::size_t>(corners[0])];
    const cS3DVector3& p1 = points[static_cast<std::size_t>(corners[1])];
    const cS3DVector3& p2 = points[static_cast<std::size_t>(corners[2])];
    cS3DVector3 edgeA = Sub(p0, p1), edgeB = Sub(p2, p1);
    cS3DVector3 centroid{(edgeA.x + edgeB.x) / 3.0f, (edgeA.y + edgeB.y) / 3.0f, (edgeA.z + edgeB.z) / 3.0f};
    edgeA = TransformVector(effectTransform_, edgeA);
    edgeB = TransformVector(effectTransform_, edgeB);
    centroid = TransformVector(effectTransform_, centroid);
    const cS3DVector3 base = TransformPoint(effectTransform_, p1);
    const cS3DVector2& t0 = uvs[static_cast<std::size_t>(corners[0])];
    const cS3DVector2& t1 = uvs[static_cast<std::size_t>(corners[1])];
    const cS3DVector2& t2 = uvs[static_cast<std::size_t>(corners[2])];
    cQuadTri piece{};
    piece.offset = cS3DVector3{-centroid.x, -centroid.y, -centroid.z};
    piece.edgeB = edgeB;
    piece.edgeA = edgeA;
    piece.uv1[0] = t1.x; piece.uv1[1] = t1.y;
    piece.uv2[0] = t2.x; piece.uv2[1] = t2.y;
    piece.uv0[0] = t0.x; piece.uv0[1] = t0.y;
    const std::uint32_t index = static_cast<std::uint32_t>(pieces_.size());
    pieces_.push_back(piece);
    cSC4GSParticle* particle = NewParticle(cS3DVector3{base.x + centroid.x, base.y + centroid.y, base.z + centroid.z});
    particle->reserved18 = static_cast<std::uint8_t>(material);
    particle->quadTri = index;
    AddParticle(SimulatorParticle(particle));
}

// 0x0040effc: a triangle whose longest edge is longer than `maxEdge` is split at that edge's midpoint, recursively.
void cSC4GeometrySourcedParticlesEffect::DiceAndAddTriangle(std::vector<cS3DVector3>& points, std::vector<cS3DVector2>& uvs,
                                                            const int corners[3], float maxEdge, std::uint32_t material) {
    const float lengths[3] = {Length(Sub(points[static_cast<std::size_t>(corners[1])], points[static_cast<std::size_t>(corners[0])])),
                              Length(Sub(points[static_cast<std::size_t>(corners[2])], points[static_cast<std::size_t>(corners[1])])),
                              Length(Sub(points[static_cast<std::size_t>(corners[0])], points[static_cast<std::size_t>(corners[2])]))};
    int longest = 2;
    if (lengths[0] <= lengths[1]) {
        if (lengths[2] < lengths[1]) longest = 1;
    } else if (lengths[2] < lengths[0]) {
        longest = 0;
    }
    if (!(maxEdge <= lengths[longest]) || points.size() > 60000) {
        AddTriangle(points, uvs, corners, material);
        return;
    }
    const int a = corners[longest], b = corners[(longest + 1) % 3], c = corners[(longest + 2) % 3];
    const int middle = static_cast<int>(points.size());
    const cS3DVector3 pa = points[static_cast<std::size_t>(a)], pb = points[static_cast<std::size_t>(b)];
    points.push_back(cS3DVector3{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, (pa.z + pb.z) * 0.5f});
    const cS3DVector2 ta = uvs[static_cast<std::size_t>(a)], tb = uvs[static_cast<std::size_t>(b)];
    uvs.push_back(cS3DVector2{(ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f});
    const int first[3] = {a, middle, c};
    const int second[3] = {middle, b, c};
    DiceAndAddTriangle(points, uvs, first, maxEdge, material);
    DiceAndAddTriangle(points, uvs, second, maxEdge, material);
}

// 0x0040f348: every triangle of every textured mesh, diced to 16 / (sqrt(rate bias * density) * description +0x1d0) units.
void cSC4GeometrySourcedParticlesEffect::CreateParticlesFromModelTriangles() {
    pieces_.clear();
    if (!model_) return;
    const float density = globalParams_ ? globalParams_->rateScale : 1.0f;
    const float maxEdge = 16.0f / (std::sqrt(std::max(1.0e-6f, rateBias_ * density)) * std::max(1.0e-6f, description_->values278[0]));
    std::uint32_t material = 0;
    for (std::size_t m = 0; m < model_->Meshes().size(); ++m) {
        const s3d::cS3DMaterial& mat = MaterialOf(model_->Materials()[m]);
        if (mat.layerCount == 0) continue;
        const SC4Mesh& mesh = MeshOf(model_->Meshes()[m]);
        const std::uint32_t pool = material++;
        if (!mesh.vertices || !mesh.vertices->data || !mesh.draws) continue;
        std::vector<cS3DVector3> points(mesh.vertices->count);
        std::vector<cS3DVector2> uvs(mesh.vertices->count);
        const auto* bytes = static_cast<const std::uint8_t*>(mesh.vertices->data);
        for (std::uint32_t v = 0; v < mesh.vertices->count; ++v) {   // V3F_C4UB_T2F: position at 0, texture coordinates at 16
            std::memcpy(&points[v], bytes + std::size_t(v) * 24, 12);
            std::memcpy(&uvs[v], bytes + std::size_t(v) * 24 + 16, 8);
        }
        const std::uint16_t* indices = mesh.indexGroup ? *mesh.indexGroup : nullptr;
        for (const SC4MeshDrawRecord* draw = mesh.draws->begin; draw != mesh.draws->end; ++draw) {
            auto corner = [&](std::uint32_t k) { return static_cast<int>(indices ? indices[draw->start + k] : draw->start + k); };
            for (std::uint32_t k = 0; k + 2 < draw->count; k += 3) {   // cS3DTriangleIterator over triangle lists
                if (draw->primitive != 0) break;
                const int corners[3] = {corner(k), corner(k + 1), corner(k + 2)};
                if (corners[0] >= static_cast<int>(points.size()) || corners[1] >= static_cast<int>(points.size()) ||
                    corners[2] >= static_cast<int>(points.size()))
                    continue;
                DiceAndAddTriangle(points, uvs, corners, maxEdge, pool);
            }
        }
    }
}

// 0x0040fd54: after the start delay the model is cut into pieces once; the phase runs at the description's rate and the
// system stops when no piece is left.
void cSC4GeometrySourcedParticlesEffect::GenerateNewParticles() {
    if (!model_) {
        if (liveCount_ == 0) Stop(0);
        return;
    }
    float elapsed = deltaTime_;
    if (0.0f < spawnDelay_) {
        spawnDelay_ -= elapsed;
        if (0.0f < spawnDelay_) return;
        elapsed = -spawnDelay_;
    }
    if (!generated_ && spawnAccumulator_ == 0.0) {
        front_ = 0.0f;
        frontStrength_ = 0.0f;
        generated_ = true;
        CreateParticlesFromModelTriangles();
        if (std::getenv("SC4_FXDEBUG"))
            std::fprintf(stderr, "fx: geometry pieces %zu (meshes %zu, pools %zu) explosion at %.1f,%.1f,%.1f\n", pieces_.size(),
                         model_->Meshes().size(), pools_.size(), explosionPoint_.x, explosionPoint_.y, explosionPoint_.z);
    }
    previousFront_ = front_;
    spawnAccumulator_ = static_cast<double>(elapsed) * inverseRate_ + spawnAccumulator_;
    if (1.0 <= spawnAccumulator_) spawnAccumulator_ = 0.99999;
    if (liveCount_ == 0) Stop(0);
}

// 0x0040dc2a: the explosion front grows at description +0x1e4; a piece starts moving once the front passed it, pushed outward
// by the front's strength curve, then falls under gravity (+0xe0), drifts with the wind (+0xec) and is damped (+0x100).
bool cSC4GeometrySourcedParticlesEffect::UpdateParticlePhysics(cSC4EffectsParticle* particle, float elapsed) {
    const cSC4ParticlesDescription* d = description_;
    if (d->values298[0] == 0.0f) return cSC4ParticlesEffect::UpdateParticlePhysics(particle, elapsed);
    if (front_ == previousFront_) {   // once per frame
        front_ += elapsed * d->values298[0];
        const double phase = spawnAccumulator_ - std::floor(spawnAccumulator_);
        frontStrength_ = Curve(d->curve2a0, static_cast<float>(phase));
    }
    const cS3DVector3 away = Sub(particle->position, explosionPoint_);
    const float distance = std::sqrt(std::max(kMinDistance2, away.x * away.x + away.y * away.y + away.z * away.z));
    if (0.0f < particle->age || distance < front_) {
        particle->age += elapsed;
        if (particle->lifetime <= particle->age) {
            particle->alive = 0;
            return false;
        }
        const float inverseExponent = 1.0f / d->value2b8;
        const float before = std::max(0.0f, (previousFront_ - distance) / d->values298[1]);
        const float after = std::max(0.0f, (front_ - distance) / d->values298[1]);
        const float e0 = std::exp(before * -d->value2b8), e1 = std::exp(after * -d->value2b8);
        const float shift = inverseExponent - 1.0f;
        const cS3DVector3 old = particle->velocity;
        cS3DVector3 v{old.x + elapsed * d->origin[0], old.y + elapsed * d->origin[1], old.z + elapsed * d->origin[2]};
        const float wind = d->globalOffsetScale;
        v = cS3DVector3{wind * velocityOffset_.x * elapsed + v.x, wind * velocityOffset_.y * elapsed + v.y, wind * velocityOffset_.z * elapsed + v.z};
        const float push = ((e1 * (shift + after) * inverseExponent - inverseExponent * (shift + before) * e0) * frontStrength_) / distance;
        v = cS3DVector3{away.x * push + v.x, away.y * push + v.y, away.z * push + v.z};
        const float damping = 1.0f - elapsed * d->damping;
        particle->velocity = cS3DVector3{damping * v.x, damping * v.y, damping * v.z};
        const float half = elapsed * 0.5f;
        particle->position = cS3DVector3{half * (old.x + particle->velocity.x) + particle->position.x,
                                         (old.y + particle->velocity.y) * half + particle->position.y,
                                         (old.z + particle->velocity.z) * half + particle->position.z};
    }
    return true;
}

// 0x0040c428: before the pieces exist the model is drawn whole at the effect transform; after, every piece is a triangle in its
// material's pool, scaled by the size curve (/50), turned by the spin curve in its own plane, coloured by the colour curve
// times the effect colour with the alpha curve (+0xa0), then each pool is drawn with its texture.
void cSC4GeometrySourcedParticlesEffect::RenderActiveParticles(IParticlesDrawContext* context) {
    if (!generated_ && model_) {
        SC4DrawContext* draw = context->DrawContext();
        if (!draw) return;
        cS3DTransform transform = sourceTransform_;
        nS3D::MultiplyTransform(transform, effectTransform_);
        draw->SetModelTransform(transform);
        s3d::SC4DrawAttributes attributes{};
        for (std::size_t m = 0; m < model_->Meshes().size(); ++m) {
            draw->SetRenderState(MaterialOf(model_->Materials()[m]), &attributes);
            draw->RenderMesh(MeshOf(model_->Meshes()[m]));
        }
        context->ResetModelViewTransform();
        return;
    }
    for (cSC4PrimitivePool& pool : pools_) pool.RestartArrays();
    box_ = cS3DBoundingBox{};
    bool boxSet = false;
    const cSC4ParticlesDescription* d = description_;
    for (cSCParticle* base : particles_) {
        cSC4GSParticle* particle = GS(base);
        if (particle->reserved18 >= pools_.size() || particle->quadTri >= pieces_.size()) continue;
        cSC4PrimitivePool& pool = pools_[particle->reserved18];
        auto* vertices = static_cast<float*>(pool.GetPrimVertices(1));
        const float t = particle->lifetime > 0.0f ? particle->age / particle->lifetime : 0.0f;
        const float spin = particle->rotation * Curve(d->spinCurve, t);
        const float scale = Curve(d->sizeCurve, t) / kSizeDivisor;
        const cS3DVector3 colourCurve = Curve(d->colorCurve, t);
        const float colour[4] = {colourCurve.x * colour_.x, colourCurve.y * colour_.y, colourCurve.z * colour_.z, Curve(d->curveC8, t)};
        std::uint8_t argb[4];   // ColorToColorARGB8 (cS3DColorARGB8: r, g, b, a in memory, as the base particles write it)
        for (int k = 0; k < 4; ++k) argb[k] = static_cast<std::uint8_t>(std::clamp(colour[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        const cQuadTri& piece = pieces_[particle->quadTri];
        const cS3DVector3 base3{piece.offset.x + particle->position.x, piece.offset.y + particle->position.y, piece.offset.z + particle->position.z};
        cS3DVector3 b{piece.edgeB.x * scale, piece.edgeB.y * scale, piece.edgeB.z * scale};
        cS3DVector3 a{piece.edgeA.x * scale, piece.edgeA.y * scale, piece.edgeA.z * scale};
        if (spin != 0.0f) {
            const float s = std::sin(spin * kTwoPi), c = std::cos(spin * kTwoPi);
            const cS3DVector3 nb{a.x * s + b.x * c, a.y * s + b.y * c, a.z * s + b.z * c};
            const cS3DVector3 na{a.x * c - b.x * s, a.y * c - b.y * s, a.z * c - b.z * s};
            a = na;
            b = nb;
        }
        if (!boxSet) {
            for (int k = 0; k < 3; ++k) box_.min[k] = box_.max[k] = (&base3.x)[k];
            boxSet = true;
        }
        for (int k = 0; k < 3; ++k) {
            box_.min[k] = std::min(box_.min[k], (&base3.x)[k]);
            box_.max[k] = std::max(box_.max[k], (&base3.x)[k]);
        }
        auto put = [&](float* out, const cS3DVector3& p, const float uv[2]) {
            out[0] = p.x; out[1] = p.y; out[2] = p.z;
            std::memcpy(&out[3], argb, 4);
            out[4] = uv[0]; out[5] = uv[1];
        };
        put(vertices, cS3DVector3{a.x + base3.x, a.y + base3.y, a.z + base3.z}, piece.uv0);
        put(vertices + 6, base3, piece.uv1);
        put(vertices + 12, cS3DVector3{b.x + base3.x, b.y + base3.y, b.z + base3.z}, piece.uv2);
    }
    for (std::size_t i = 0; i < pools_.size(); ++i) {
        if (pools_[i].VerticesUsed() == 0) continue;
        context->SetTexture(static_cast<int>(textures_[i]), 0);
        pools_[i].Draw(*context);
    }
}

}  // namespace sc4
