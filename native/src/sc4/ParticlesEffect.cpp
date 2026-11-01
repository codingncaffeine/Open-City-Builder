// cSC4ParticlesEffect — see ParticlesEffect.h for the layout, the quirks and the seams.
// Compile without FMA contraction so the particle arithmetic rounds like the original single-precision code.
#pragma STDC FP_CONTRACT OFF

#include "sc4/ParticlesEffect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

#include "s3d/S3DMath.h"

namespace {

// Host objects (the game installs adapters over SL::spTerrainMap / spCity / spOccupantManager / spEffectsManager and the
// texture-binding factory; the test installs fakes).
IParticlesTerrain* g_terrain = nullptr;
IParticlesCity* g_city = nullptr;
IParticlesOccupants* g_occupants = nullptr;
IParticlesEffects* g_effects = nullptr;
IParticlesTextures* g_textures = nullptr;

void (*g_descriptionRetain)(cSC4ParticlesDescription*) = nullptr;
void (*g_descriptionRelease)(cSC4ParticlesDescription*) = nullptr;

// The effect's shared generator (the original's static cSC4ParticlesEffect::mRandom, seeded from the clock at start-up:
// 0x1005fa31a..0x1005fa423). Kept in one place so a test can seed it.
cRZRandom g_random(0xec09c261u);

// The Hermite basis matrix the original builds at start-up (kHermite, 0x100b8dac8, from kHermiteArray44 at 0x100b10500,
// transposed into place). Rows: h00, h01, h10, h11; columns: t^3, t^2, t, 1.
constexpr float kHermite[4][4] = {
    {2.0f, -3.0f, 0.0f, 1.0f},
    {-2.0f, 3.0f, 0.0f, 0.0f},
    {1.0f, -2.0f, 1.0f, 0.0f},
    {1.0f, -1.0f, 0.0f, 0.0f},
};

constexpr float kTwoPi = 6.2831855f;
constexpr float kInvTwoPi = 0.15915494f;
constexpr float kStep = 0.06666667f;         // 1/15 s, the largest physics step
constexpr float kMagic = 12582912.0f;        // 1.5 * 2^23, the float -> int conversion constant
constexpr std::int32_t kMagicBits = 0x4b400000;
constexpr float kUnitEpsilon = 1e-6f;
constexpr float kColorMagic = 49152.0f;      // 1.5 * 2^15: one ulp is 1/256, so the bit delta is value * 256
constexpr std::int32_t kColorMagicBits = 0x47400000;

// (int)v for 0 <= v < 2^23, the way the original computes it: add the 1.5 * 2^23 magic, take the float's bits and
// subtract the magic's bits. The compiler emitted this for the plain `(int)v` casts.
inline int TruncateToInt(float v) {
    const float added = v - 0.5f + kMagic;
    std::int32_t bits;
    std::memcpy(&bits, &added, sizeof(bits));
    const int lower = bits - kMagicBits;
    if (v == static_cast<float>(lower + 1)) return lower + 1;
    return lower;
}

// A unit colour to the packed byte ProcessParticle writes (see the header quirk): the float's bits, minus the bits of
// 49152.0f, give round(value * 256); folding the high byte into the low one turns 256 into 255.
inline std::uint8_t PackedColorByte(float value) {
    const float added = value + kColorMagic;
    std::int32_t bits;
    std::memcpy(&bits, &added, sizeof(bits));
    const std::int32_t v = bits - kColorMagicBits;
    return static_cast<std::uint8_t>((v & 0xff) - ((v >> 8) & 0xff));
}

// Curve sampling of the description: index = (count - 1) * t, truncated, then a linear interpolation when the fraction is
// positive (the original keeps the first sample when it is exactly 0). An empty curve reads as 0 and the last index is
// clamped instead of reading past the vector (the original trusts t < 1).
inline float CurveValue(const std::vector<float>& curve, float t) {
    if (curve.empty()) return 0.0f;
    const int last = static_cast<int>(curve.size()) - 1;
    if (last == 0) return curve[0];
    const float f = static_cast<float>(last) * t;
    int index = TruncateToInt(f);
    index = std::clamp(index, 0, last);
    float frac = f - static_cast<float>(index);
    float value = curve[static_cast<std::size_t>(index)];
    if (frac > 0.0f) {
        const std::size_t next = static_cast<std::size_t>(std::min(index + 1, last));
        value += frac * (curve[next] - value);
    }
    return value;
}

inline float Sqrt(float v) { return std::sqrt(v); }
inline float Sin(float v) { return std::sin(v); }
inline float Cos(float v) { return std::cos(v); }
inline float ATan2(float y, float x) { return std::atan2(y, x); }

// A description curve's first element; the original dereferences the pointer without a size test (the guard here keeps
// an empty curve from reading unmapped memory).
inline float CurveFirst(const std::vector<float>& curve) { return curve.empty() ? 0.0f : curve.front(); }

// "min 1,1,1 / max 0,0,0" is the empty bounding box the original starts from.
inline cS3DBoundingBox EmptyBox() {
    cS3DBoundingBox box;
    box.min[0] = box.min[1] = box.min[2] = 1.0f;
    box.max[0] = box.max[1] = box.max[2] = 0.0f;
    return box;
}

void RetainDescription(cSC4ParticlesDescription* description) {
    if (description && g_descriptionRetain) g_descriptionRetain(description);
}
void ReleaseDescription(cSC4ParticlesDescription* description) {
    if (description && g_descriptionRelease) g_descriptionRelease(description);
}

}  // namespace

// ---- hosts --------------------------------------------------------------------------------------
IParticlesTerrain* ParticlesTerrain() { return g_terrain; }
void SetParticlesTerrain(IParticlesTerrain* terrain) { g_terrain = terrain; }
IParticlesCity* ParticlesCity() { return g_city; }
void SetParticlesCity(IParticlesCity* city) { g_city = city; }
IParticlesOccupants* ParticlesOccupants() { return g_occupants; }
void SetParticlesOccupants(IParticlesOccupants* occupants) { g_occupants = occupants; }
IParticlesEffects* ParticlesEffects() { return g_effects; }
void SetParticlesEffects(IParticlesEffects* effects) { g_effects = effects; }
IParticlesTextures* ParticlesTextures() { return g_textures; }
void SetParticlesTextures(IParticlesTextures* textures) { g_textures = textures; }

// The description is refcounted in the original (cSC4ParticlesDescription::Release @ 0x1005ed5a0, the retain half is
// cRZAutoRefCount::operator= @ 0x1005ed5a0's counterpart). The layout is recovered here, the class is not.
void SetParticlesDescriptionLifecycle(void (*retain)(cSC4ParticlesDescription*),
                                      void (*release)(cSC4ParticlesDescription*)) {
    g_descriptionRetain = retain;
    g_descriptionRelease = release;
}

namespace sc4 {

cRZRandom& cSC4ParticlesEffect::Random() { return g_random; }

// ---- construction ------------------------------------------------------------------------------
cSC4ParticlesEffect::cSC4ParticlesEffect() {
    renderState_.value = 0;
    renderState_.mask = 0xffffffffu;
    description_ = nullptr;
    textureId_ = 0;
    texture_ = nullptr;
    basisA_ = cS3DVector3{};
    basisB_ = cS3DVector3{};
    basisRotation_ = 0;
    effectTransform_ = cS3DTransform{};
    sourceTransform_ = cS3DTransform{};
    velocityOffset_ = cS3DVector3{};
    deltaTime_ = 0;
    timeAccumulator_ = 0;
    firstUpdate_ = false;
    spawnAccumulator_ = 0;
    spawnCounter_ = 0;
    inverseRate_ = -1.0;
    spawnFraction_ = 0;
    position_ = cS3DVector3{};
    walkTimer_ = 0;
    walkDirection_ = cS3DVector3{};
    citySideEmission_ = false;
    walkBasis_ = cS3DVector3{};
    attractorForce_ = cS3DVector3{};
    attractorTarget_ = cS3DVector3{};
    attractorLength_ = 0;
    startTransition_ = 1;
    stopTransition_ = 1;
    spawnDelay_ = 0;
    attractorOrigin_ = cS3DVector3{};
    box_ = EmptyBox();
    rateBias_ = 1.0f;
    scaleBias_ = 1.0f;
    velocityBias_ = 1.0f;
    attractorScale_ = 1.0f;
    colour_ = cS3DVector3{1.0f, 1.0f, 1.0f};
    rateScale_ = 1.0f;
    sizeScale_ = 1.0f;
    globalParams_ = nullptr;
    vertexBuffer_ = nullptr;
    terrain_ = nullptr;
    worldWidth_ = 0;
    worldDepth_ = 0;
    waterLevel_ = 0;
    emissionBox_ = cS3DBoundingBox{};
    emissionBox_.min[0] = emissionBox_.min[1] = emissionBox_.min[2] = 1.0f;   // the ctor's 1,1,1 / 0,0,0 pair
    hasTerrainEffect_ = false;
    hasWaterEffect_ = false;
    suppressEmission_ = false;
}

cSC4ParticlesEffect::cSC4ParticlesEffect(const cSC4ParticlesEffect& other) : cSC4ParticlesEffect() {
    (void)other;   // 0x1005f2a9c is the C2 alias of the default constructor
}

cSC4ParticlesEffect::~cSC4ParticlesEffect() {
    if (vertexBuffer_) {
        IParticlesVertexBuffer* buffer = vertexBuffer_;
        vertexBuffer_ = nullptr;
        buffer->Release();
    }
    if (texture_) {
        IParticlesTextureBinding* texture = texture_;
        texture_ = nullptr;
        texture->Release();
    }
    // pool_ destructs here
    if (description_) {
        cSC4ParticlesDescription* description = description_;
        description_ = nullptr;
        ReleaseDescription(description);
    }
    // cSCParticleSimulatorBase destructs here
}

std::uint32_t cSC4ParticlesEffect::AddRef() { return cSCParticleSimulatorBase::AddRef(); }

std::uint32_t cSC4ParticlesEffect::Release() { return cSCParticleSimulatorBase::Release(); }

bool cSC4ParticlesEffect::IsStarted() const { return cSCParticleSimulatorBase::IsStarted(); }

// 0x1005f2aa6
bool cSC4ParticlesEffect::Init() {
    if (initialised_) return true;
    IParticlesTerrain* terrain = ParticlesTerrain();
    if (terrain_ != terrain) {
        if (terrain) terrain->AddRef();
        IParticlesTerrain* previous = terrain_;
        terrain_ = terrain;
        if (previous) previous->Release();
    }
    worldWidth_ = static_cast<float>(terrain_->CellCountX()) * terrain_->CellSize();
    worldDepth_ = static_cast<float>(terrain_->CellCountZ()) * terrain_->CellSize();
    return cSCParticleSimulatorBase::Init();
}

// 0x1005f2b94
bool cSC4ParticlesEffect::Shutdown() {
    if (terrain_) {
        IParticlesTerrain* terrain = terrain_;
        terrain_ = nullptr;
        terrain->Release();
    }
    if (description_) {
        cSC4ParticlesDescription* description = description_;
        description_ = nullptr;
        ReleaseDescription(description);
    }
    if (vertexBuffer_) {
        IParticlesVertexBuffer* buffer = vertexBuffer_;
        vertexBuffer_ = nullptr;
        buffer->Release();
    }
    return cSCParticleSimulatorBase::Shutdown();
}

// ---- lifecycle -----------------------------------------------------------------------------------
// 0x1005f2c12
void cSC4ParticlesEffect::Start(int transition) {
    if (transition != 0 && description_->maxParticles != 0 && (description_->flags & 8) == 0) return;
    if (!cSCParticleSimulatorBase::Start()) return;
    spawnAccumulator_ = 0;
    spawnCounter_ = 0;
    timeAccumulator_ = 0;
    spawnFraction_ = 0;
    position_.x = description_->origin[0];
    position_.y = description_->origin[1];
    position_.z = description_->origin[2];
    if ((description_->flags & 0x8000) == 0) {
        walkDirection_ = cS3DVector3{};
    } else {
        // Three draws, in this order: x, (y discarded), z.
        const float x = static_cast<float>(g_random.RandomDoubleRangeUniform(description_->velocityMin[0],
                                                                            description_->velocityMax[0]));
        (void)g_random.RandomDoubleRangeUniform(description_->velocityMin[1], description_->velocityMax[1]);
        const float z = static_cast<float>(g_random.RandomDoubleRangeUniform(description_->velocityMin[2],
                                                                            description_->velocityMax[2]));
        walkDirection_.x = x;
        walkDirection_.y = 0.0f;
        walkDirection_.z = z;
        if ((description_->flags & 0x1000000) == 0) {
            walkTimer_ = 0.0f;
            UpdateRandomWalk(0.0f);
        } else {
            const float speed = static_cast<float>(g_random.RandomDoubleRangeUniform(description_->walkSpeedMin,
                                                                                    description_->walkSpeedMax));
            const float length2 = walkDirection_.x * walkDirection_.x + walkDirection_.y * walkDirection_.y +
                                  walkDirection_.z * walkDirection_.z;
            if (0.0f < length2) {
                const float scale = speed / Sqrt(length2);
                walkDirection_.x *= scale;
                walkDirection_.y *= scale;
                walkDirection_.z *= scale;
            }
            walkTimer_ = static_cast<float>(g_random.RandomDoubleRangeUniform(description_->walkIntervalMin,
                                                                             description_->walkIntervalMax));
        }
    }
    if ((description_->flags & 0x2000000) != 0) {
        citySideEmission_ = true;
        int cellX = 0, cellZ = 0;
        if (ParticlesCity()) {
            ParticlesCity()->WorldToCell(attractorOrigin_.x, attractorOrigin_.z, cellX, cellZ);
        }
        unsigned flags = 0;
        if (ParticlesCity() && ParticlesCity()->Cells()) flags = ParticlesCity()->Cells()->CellFlags(cellX, cellZ);
        float directionX = 0.0f, directionZ = 0.0f;
        if ((flags & 1) != 0) {
            directionX = -1.0f;
            directionZ = 0.0f;
        } else if ((flags & 2) != 0) {
            directionX = 0.0f;
            directionZ = -1.0f;
        } else if ((flags & 8) != 0) {
            directionX = 0.0f;
            directionZ = 1.0f;
        } else if ((flags & 4) != 0) {
            directionX = 0.0f;
            directionZ = 1.0f;
        } else {
            citySideEmission_ = false;
        }
        if (citySideEmission_) {
            walkBasis_.x = directionZ;
            walkBasis_.y = 0.0f;
            walkBasis_.z = directionX;
            if (citySideEmission_) {
                const float invScale = 1.0f / sourceTransform_.scale;
                const float x = directionZ;
                const float z = directionX;
                walkBasis_.x = (sourceTransform_.rotation[2][0] * z + sourceTransform_.rotation[1][0] * 0.0f +
                                sourceTransform_.rotation[0][0] * x) * invScale;
                walkBasis_.y = (sourceTransform_.rotation[2][1] * z + sourceTransform_.rotation[1][1] * 0.0f +
                                sourceTransform_.rotation[0][1] * x) * invScale;
                walkBasis_.z = invScale * (z * sourceTransform_.rotation[2][2] +
                                           sourceTransform_.rotation[1][2] * 0.0f +
                                           x * sourceTransform_.rotation[0][2]);
            }
        }
    }
    float delay = 0.0f;
    float minDelay = description_->delayMinA;
    bool haveDelay = false;
    if (0.0f <= minDelay) {
        haveDelay = true;
    } else {
        minDelay = description_->delayMinB;
        if (!(minDelay < 0.0f)) haveDelay = true;
    }
    if (haveDelay) {
        const float maxDelay = (0.0f <= description_->delayMinA) ? description_->delayMaxA : description_->delayMaxB;
        delay = static_cast<float>(g_random.RandomDoubleRangeUniform(minDelay, maxDelay));
    }
    spawnDelay_ = delay;
    startTransition_ = transition;
    stopTransition_ = 0;
    firstUpdate_ = transition != 0;
}

// 0x1005f33ac
void cSC4ParticlesEffect::Stop(int transition) {
    stopTransition_ = transition;
    cSCParticleSimulatorBase::Stop(transition == 0, false);
}

// 0x1005f33cc
void cSC4ParticlesEffect::SetTransforms(const cS3DTransform& effect, const cS3DTransform& source) {
    // i386 0x00414bf4: both transforms are copied whole (cS3DTransform::operator=: the copy bumps the destination's change
    // counter). The effect translation is where new particles are born (InitEffectsParticle transforms by the whole
    // effect transform); the port used to keep only its rotation, so every system emitted at the map's corner.
    const std::uint8_t effectCounter = effectTransform_.changeCounter;
    const std::uint8_t sourceCounter = sourceTransform_.changeCounter;
    effectTransform_ = effect;
    effectTransform_.changeCounter = static_cast<std::uint8_t>(effectCounter + 1);
    sourceTransform_ = source;
    sourceTransform_.changeCounter = static_cast<std::uint8_t>(sourceCounter + 1);
    if (!suppressEmission_ && (description_->flags & 0x1000) != 0) {
        // Aligned: both translations are dropped and the box is reset to the emission box, whose centre becomes the
        // attractor origin.
        for (int i = 0; i < 3; ++i) effectTransform_.translation[i] = 0.0f;
        effectTransform_.flags |= 1;
        ++effectTransform_.changeCounter;
        for (int i = 0; i < 3; ++i) sourceTransform_.translation[i] = 0.0f;
        sourceTransform_.flags |= 1;
        ++sourceTransform_.changeCounter;
        box_ = emissionBox_;
        attractorOrigin_.x = (emissionBox_.min[0] + emissionBox_.max[0]) * 0.5f;
        attractorOrigin_.y = (emissionBox_.min[1] + emissionBox_.max[1]) * 0.5f;
        attractorOrigin_.z = (emissionBox_.min[2] + emissionBox_.max[2]) * 0.5f;
    }
    // The attractor origin becomes the effect's position seen through the source transform; the box moves with it.
    const float oldX = attractorOrigin_.x, oldY = attractorOrigin_.y, oldZ = attractorOrigin_.z;
    const float* t = effectTransform_.translation;
    const auto& r = sourceTransform_.rotation;
    const float scale = sourceTransform_.scale;
    attractorOrigin_.x = (r[0][0] * t[0] + r[0][1] * t[1] + r[0][2] * t[2]) * scale + sourceTransform_.translation[0];
    attractorOrigin_.y = (r[1][0] * t[0] + r[1][1] * t[1] + r[1][2] * t[2]) * scale + sourceTransform_.translation[1];
    attractorOrigin_.z = (r[2][0] * t[0] + r[2][1] * t[1] + r[2][2] * t[2]) * scale + sourceTransform_.translation[2];
    const float dx = attractorOrigin_.x - oldX, dy = attractorOrigin_.y - oldY, dz = attractorOrigin_.z - oldZ;
    box_.min[0] += dx;
    box_.min[1] += dy;
    box_.min[2] += dz;
    box_.max[0] += dx;
    box_.max[1] += dy;
    box_.max[2] += dz;
}

// 0x1005f3738
bool cSC4ParticlesEffect::SetFloatParams(int type, const float* values, int count) {
    (void)count;
    switch (type) {
        case 0: velocityBias_ = values[0]; break;
        case 1: sizeScale_ = values[0]; break;
        case 2: rateScale_ = values[0]; break;     // *(float *)(param_1 + 0x47)
        case 3: attractorScale_ = values[0]; break;   // +0x228
        case 4:
            colour_.x = values[0];
            colour_.y = values[1];
            colour_.z = values[2];
            break;
        case 5: SetTarget(cS3DVector3{values[0], values[1], values[2]}); return true;
        case 6:
        case 7:
        case 8:
        case 0xc:
        case 0xd:
        case 0xe:
            return false;
        case 9: {
            const float y = values[1];
            const float ratio = (box_.min[1] + box_.max[1]) * 0.5f - y;
            const float scaleZ = values[4];
            const float invScaleZ = 1.0f / scaleZ;
            const float scaleY = values[5];
            attractorTarget_.x = values[3] * ratio * invScaleZ + values[0];
            attractorTarget_.y = scaleZ * ratio * invScaleZ + y;
            attractorTarget_.z = invScaleZ * ratio * scaleY + values[2];
            attractorLength_ = 1.0f;
            break;
        }
        case 10:
            attractorTarget_.x = values[0];
            attractorTarget_.y = values[1];
            attractorTarget_.z = values[2];
            break;
        case 0xb: attractorLength_ = values[0]; break;
        case 0xf:
            velocityOffset_.x = values[0];
            velocityOffset_.y = values[1];
            velocityOffset_.z = values[2];
            break;
        default:
            if (type != 0x102) return false;
            rateBias_ = values[0];
            scaleBias_ = values[1];
            break;
    }
    return true;
}

// 0x1005f38da
bool cSC4ParticlesEffect::SetVoidParams(int type, void** values, int count) {
    (void)count;
    if (type == 7) {
        SetModel(static_cast<IParticlesModelInstance*>(values[0]));
    }
    return type == 7;
}

// ---- next group -----------------------------------------------------------------------------------

// ---- update --------------------------------------------------------------------------------------
// 0x1005f3060
void cSC4ParticlesEffect::UpdateRandomWalk(float elapsed) {
    walkTimer_ -= elapsed;
    if (!(walkTimer_ <= 0.0f)) return;
    const cSC4ParticlesDescription* desc = description_;
    do {
        walkTimer_ += static_cast<float>(g_random.RandomDoubleRangeUniform(desc->walkIntervalMin,
                                                                          desc->walkIntervalMax));
        if (walkDirection_.x * walkDirection_.x + walkDirection_.z * walkDirection_.z < kUnitEpsilon) {
            const cS3DVector2 direction = nS3D::RandomDirection2(g_random);
            walkDirection_.x = direction.x;
            walkDirection_.y = 0.0f;
            walkDirection_.z = direction.y;
        }
        const float angle = ATan2(walkDirection_.z, walkDirection_.x);
        const float magnitude = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->walkSpeedMin,
                                                                                    desc->walkSpeedMax));
        if (!citySideEmission_) {
            const float spread = desc->walkAngle + 1e-05f;
            const float delta = static_cast<float>(g_random.RandomDoubleRangeUniform(-spread, spread));
            const float base = desc->walkAngle2 * kTwoPi;
            const float signedBase = (delta < 0.0f) ? -base : base;
            const float a = signedBase + angle + delta * kTwoPi;
            const float s = Sin(a) * magnitude;
            const float c = Cos(a) * magnitude;
            walkDirection_.x = c;
            walkDirection_.y = 0.0f;
            walkDirection_.z = s;
        } else {
            const float delta = static_cast<float>(g_random.RandomDoubleRangeUniform(0.0f, desc->walkAngle));
            const float base = (delta + desc->walkAngle2) * kTwoPi;
            const float angleA = angle + base;
            const float angleB = angle - base;
            const float sinA = Sin(angleA) * magnitude;
            const float cosA = Cos(angleA) * magnitude;
            const float sinB = Sin(angleB) * magnitude;
            const float cosB = Cos(angleB) * magnitude;
            const float length = Sqrt(walkDirection_.x * walkDirection_.x + walkDirection_.y * walkDirection_.y +
                                      walkDirection_.z * walkDirection_.z);
            const float projected = (walkDirection_.z * walkBasis_.z + walkDirection_.y * walkBasis_.y +
                                     walkDirection_.x * walkBasis_.x) * magnitude / length;
            const float candidateA = cosA * walkBasis_.x + sinA * walkBasis_.z;
            const float candidateB = cosB * walkBasis_.x + sinB * walkBasis_.z;
            if (candidateA <= candidateB) {
                if (!(candidateB <= projected)) {
                    walkDirection_.x = cosB;
                    walkDirection_.y = 0.0f;
                    walkDirection_.z = sinB;
                }
            } else if (!(candidateA <= projected)) {
                walkDirection_.x = cosA;
                walkDirection_.y = 0.0f;
                walkDirection_.z = sinA;
            }
        }
    } while (walkTimer_ <= 0.0f);
}

// 0x1005f38f4
bool cSC4ParticlesEffect::Update(float elapsed, IParticlesDrawContext* context) {
    if (!started_) return false;
    if (firstUpdate_) {
        elapsed += description_->startDelay;
        firstUpdate_ = false;
    }
    deltaTime_ = elapsed;
    timeAccumulator_ += static_cast<double>(elapsed);
    waterLevel_ = terrain_ ? terrain_->WaterLevel() : 0.0f;
    if ((description_->flags & 0x2000) != 0) SetCitySideEmissionVolume(0);
    float forceX = 0.0f, forceY = 0.0f, forceZ = 0.0f;
    if (description_->attractorCurve.empty() || description_->attractorTime <= 0.0f) {
        attractorForce_ = cS3DVector3{};
    } else {
        UpdateAttractor();
        forceX = attractorForce_.x;
        forceY = attractorForce_.y;
        forceZ = attractorForce_.z;
    }
    const cS3DVector3 wind = globalParams_ ? globalParams_->velocity : cS3DVector3{};
    const float windScale = description_->globalOffsetScale;
    position_.x = description_->origin[0] + wind.x * windScale + walkDirection_.x + forceX;
    position_.y = description_->origin[1] + wind.y * windScale + walkDirection_.y + forceY;
    position_.z = description_->origin[2] + wind.z * windScale + walkDirection_.z + forceZ;
    if ((description_->flags & 0x80000000ull) != 0) LaunchTimedEffects();
    for (auto it = particles_.begin(); it != particles_.end();) {
        cSC4EffectsParticle* particle = EffectsParticle(*it);
        if (particle->alive && UpdateParticlePhysics(particle, deltaTime_)) {
            ++it;
            continue;
        }
        it = RemoveDeadParticle(it, particle);
    }
    if (!suppressEmission_) GenerateNewParticles();
    if (context == nullptr || liveCount_ < 1) {
        UpdateActiveParticles();
    } else {
        PrepareForRendering(context);
        const bool selfLit = (description_->flags & 1) != 0;
        if (selfLit) context->SetSelfLitShade(cS3DVector3{1.0f, 1.0f, 1.0f});
        context->ResetModelViewTransform();
        context->SetRenderState(renderState_);
        if (vertexBuffer_ == nullptr) {
            RenderActiveParticles(context);
        } else {
            RenderActiveParticlesVB(context);
        }
        if (selfLit) context->ResetShade();
    }
    const bool stopped = CheckForStop();
    if (!stopped && (description_->flags & 0x8000) != 0) UpdateRandomWalk(deltaTime_);
    return true;
}

// 0x1005f3c10
void cSC4ParticlesEffect::SetCitySideEmissionVolume(int volume) {
    if ((description_->flags & 0x2000) != 0) {
        const float windX = globalParams_ ? globalParams_->velocity.x : 0.0f;
        const float windZ = globalParams_ ? globalParams_->velocity.z : 0.0f;
        unsigned code = static_cast<unsigned>(windZ <= 0.0f);
        if (windZ < windX) code = static_cast<unsigned>(0.0f < windX) | 2u;
        volume += static_cast<int>(code);
    }
    float maxZ = 0.0f;
    switch (volume) {
        case 0:
            emissionBox_.min[0] = 0.0f;
            emissionBox_.max[0] = worldWidth_;
            emissionBox_.min[2] = 0.0f;
            maxZ = 0.0f;
            break;
        case 1:
            emissionBox_.min[0] = worldWidth_;
            emissionBox_.max[0] = worldWidth_;
            emissionBox_.min[2] = 0.0f;
            maxZ = worldDepth_;
            break;
        case 2:
            emissionBox_.min[0] = 0.0f;
            emissionBox_.max[0] = worldWidth_;
            emissionBox_.min[2] = worldDepth_;
            maxZ = worldDepth_;
            break;
        case 3:
            emissionBox_.min[0] = 0.0f;
            emissionBox_.min[2] = 0.0f;
            emissionBox_.max[0] = 0.0f;
            maxZ = worldDepth_;
            break;
        default:
            return;
    }
    emissionBox_.max[2] = maxZ;
}

// ---- next group -----------------------------------------------------------------------------------

// ---- description / attractor -----------------------------------------------------------------------
// 0x1005f3d08
void cSC4ParticlesEffect::UpdateAttractor() {
    const cSC4ParticlesDescription* desc = description_;
    if ((desc->flags & 0x8000000) != 0) {
        attractorTarget_ = cS3DVector3{sourceTransform_.translation[0], sourceTransform_.translation[1],
                                       sourceTransform_.translation[2]};
        return;
    }
    cS3DVector3 direction{};
    float distance = 0.0f;   // the curve input (fVar11 in the original)
    float length = 0.0f;
    if (desc->attractorMode == 0) {
        direction.x = attractorTarget_.x - (box_.min[0] + box_.max[0]) * 0.5f;
        direction.z = attractorTarget_.z - (box_.min[2] + box_.max[2]) * 0.5f;
        direction.y = 0.0f;
        distance = Sqrt(direction.x * direction.x + 0.0f + direction.z * direction.z);
        if (0.0f < distance) {
            const float inv = 1.0f / distance;
            direction.x *= inv;
            direction.y = inv * 0.0f;
            direction.z *= inv;
        }
        length = attractorLength_;
    } else {
        const cS3DVector3 centre{(box_.min[0] + box_.max[0]) * 0.5f, (box_.min[1] + box_.max[1]) * 0.5f,
                                 (box_.min[2] + box_.max[2]) * 0.5f};
        float groundLength = 0.0f;
        if (ParticlesOccupants()) {
            ParticlesOccupants()->AttractorGroundPoint(centre, desc->attractorMode, direction, groundLength);
        }
        // The original keeps the negative distance and normalises by it.
        length = -groundLength;
        if (length != 0.0f) {
            const float inv = 1.0f / length;
            direction.x *= inv;
            direction.y *= inv;
            direction.z *= inv;
        }
    }
    float forceX = 0.0f, forceY = 0.0f, forceZ = 0.0f;
    if (length != 0.0f || std::isnan(length)) {
        float fraction = distance / desc->attractorTime;
        float clamped = 1.0f;
        if (fraction <= 1.0f) clamped = fraction;
        if (fraction < 0.0f) clamped = 0.0f;
        length *= CurveValue(desc->attractorCurve, clamped);
        if ((desc->flags & 0x4000000) == 0) {
            const float inv = 1.0f / sourceTransform_.scale;
            forceX = (sourceTransform_.rotation[2][0] * direction.z + sourceTransform_.rotation[1][0] * direction.y +
                      sourceTransform_.rotation[0][0] * direction.x) * inv * length;
            forceY = (sourceTransform_.rotation[2][1] * direction.z + sourceTransform_.rotation[1][1] * direction.y +
                      sourceTransform_.rotation[0][1] * direction.x) * inv * length;
            forceZ = inv * (direction.z * sourceTransform_.rotation[2][2] +
                            direction.y * sourceTransform_.rotation[1][2] +
                            direction.x * sourceTransform_.rotation[0][2]) * length;
        } else {
            attractorScale_ = length;
        }
    }
    attractorForce_ = cS3DVector3{forceX, forceY, forceZ};
}

// 0x1005f413c
void cSC4ParticlesEffect::LaunchTimedEffects() {
    if (particles_.empty()) return;
    for (cSCParticle* baseParticle : particles_) {
        cSC4EffectsParticle* particle = EffectsParticle(baseParticle);
        if (!particle->alive) continue;
        for (const cSC4TimedEffectRecord& timed : description_->timedEffects) {
            const float delta = particle->age - timed.time;
            if (delta <= 0.0f && 0.0f < delta + deltaTime_) {
                TriggerEffect(timed.name.c_str(), particle->position);
            }
        }
    }
}

// 0x1005f4226
void cSC4ParticlesEffect::SetDescription(cSC4ParticlesDescription* description) {
    if (description_ != description) {
        RetainDescription(description);
        cSC4ParticlesDescription* previous = description_;
        description_ = description;
        if (previous) ReleaseDescription(previous);
    }
    suppressEmission_ = false;
    SetDrawMode(description_->drawMode);
    SetTexture(description_->textureId);
    SetPrimitivePoolFormat(&pool_);
    inverseRate_ = 1.0 / static_cast<double>(std::max(description_->spawnPhase, 1e-6f));
    basisRotation_ = -1;
    velocityBias_ = 1.0f;
    attractorScale_ = 1.0f;
    rateScale_ = 1.0f;
    sizeScale_ = 1.0f;
    colour_ = cS3DVector3{1.0f, 1.0f, 1.0f};
    box_.max[2] = 0.0f;
    box_.max[0] = 0.0f;
    box_.max[1] = 0.0f;
    box_.min[2] = 0.0f;
    box_.min[1] = 0.0f;
    box_.min[0] = 0.0f;
    attractorOrigin_ = cS3DVector3{};
    emissionBox_.min[0] = description_->emitMin[0];
    emissionBox_.min[1] = description_->emitMin[1];
    emissionBox_.min[2] = description_->emitMin[2];
    emissionBox_.max[0] = description_->emitMax[0];
    emissionBox_.max[1] = description_->emitMax[1];
    emissionBox_.max[2] = description_->emitMax[2];
    velocityOffset_ = cS3DVector3{};
    if ((description_->flags & 0x1000) != 0) {
        if (((description_->flags & 0x4000) == 0) && static_cast<std::int32_t>(description_->flags) >= 0) {
            const float water = terrain_ ? terrain_->WaterLevel() : 0.0f;
            emissionBox_.min[1] += water;
            emissionBox_.max[1] += water;
        }
        if ((description_->flags & 0x2000) == 0) {
            emissionBox_.min[0] = 0.0f;
            emissionBox_.max[0] = worldWidth_;
            emissionBox_.min[2] = 0.0f;
            emissionBox_.max[2] = worldDepth_;
        } else {
            SetCitySideEmissionVolume(0);
        }
    }
    float bias = description_->walkBias[0] - 0.0f;
    bool haveBias = std::fabs(bias) >= 0.0001f;
    if (!haveBias) {
        bias = description_->walkBias[1] - 0.0f;
        haveBias = std::fabs(bias) >= 0.0001f;
    }
    citySideEmission_ = haveBias;
    if (!haveBias) {
        bias = description_->walkBias[2] - 0.0f;
        citySideEmission_ = std::fabs(bias) >= 0.0001f;
    }
    if (citySideEmission_) {
        walkBasis_.x = description_->walkBias[0];
        walkBasis_.y = description_->walkBias[1];
        walkBasis_.z = description_->walkBias[2];
    }
    if ((description_->flags & 0x100) != 0) {
        if (ParticlesEffects()) {
            hasTerrainEffect_ = ParticlesEffects()->FindEffect((description_->name + "_terrain").c_str()) != nullptr;
            hasWaterEffect_ = ParticlesEffects()->FindEffect((description_->name + "_water").c_str()) != nullptr;
        } else {
            hasTerrainEffect_ = false;
            hasWaterEffect_ = false;
        }
    }
}

// 0x1005f4602
void cSC4ParticlesEffect::SetDrawMode(std::uint32_t mode) {
    bool depthWrite = false;
    bool depthTest = false;
    float alphaRef = 0.0f;
    int depthFunc = -1;
    int blendSrc = 4;
    int blendDst = 5;
    switch (mode) {
        case 0:
            depthWrite = false;
            alphaRef = 0.0f;
            depthTest = true;
            depthFunc = 3;
            break;
        case 1:
            depthWrite = false;
            depthTest = true;
            alphaRef = 0.0f;
            depthFunc = 4;
            break;
        case 2:
            depthWrite = false;
            depthTest = false;
            alphaRef = 0.0f;
            depthFunc = -1;
            break;
        case 3:
            alphaRef = 0.9f;
            depthWrite = true;
            depthTest = true;
            depthFunc = 3;
            break;
        case 4:
            alphaRef = 0.5f;
            depthWrite = true;
            depthTest = true;
            depthFunc = 3;
            break;
        case 5:
            depthWrite = false;
            depthTest = true;
            depthFunc = 3;
            blendDst = 1;
            alphaRef = 0.0f;
            break;
        case 6:
            depthWrite = false;
            depthTest = false;
            depthFunc = -1;
            blendDst = 1;
            alphaRef = 0.0f;
            break;
        case 7:
            depthWrite = false;
            depthTest = true;
            depthFunc = 3;
            blendSrc = 0;
            blendDst = 2;
            alphaRef = 0.0f;
            break;
        default:
            return;
    }
    renderState_.PackRenderState(true, depthWrite, depthTest, true, true, 1, false, depthFunc, blendSrc, blendDst,
                                 4, alphaRef, 1, -1);
}

// 0x1005f4808
void cSC4ParticlesEffect::SetTexture(std::uint32_t texture) {
    textureId_ = 0;
    IParticlesTextures* factory = ParticlesTextures();
    if (!factory) return;
    IParticlesTextureBinding* binding = factory->Create(texture);
    if (!binding) return;
    if (texture_) {
        IParticlesTextureBinding* previous = texture_;
        texture_ = nullptr;
        previous->Release();
    }
    texture_ = binding;
    textureId_ = binding->TextureId();
}

// 0x1005f48da
void cSC4ParticlesEffect::SetModel(IParticlesModelInstance* model) {
    const std::uint64_t flags = description_->flags;
    if ((flags & 0x40) != 0 || (static_cast<std::int8_t>(flags & 0xff) < 0)) {
        model->SetPosition(cS3DVector3{emissionBox_.min[0], emissionBox_.min[1], emissionBox_.min[2]});
        if (static_cast<std::int8_t>(flags & 0xff) < 0) emissionBox_.max[1] = emissionBox_.min[1];
    }
}

// 0x1005f4922
void cSC4ParticlesEffect::SetTarget(const cS3DVector3& target) {
    const float invScale = 1.0f / sourceTransform_.scale;
    const float x = (target.x - sourceTransform_.translation[0]) * invScale;
    const float y = (target.y - sourceTransform_.translation[1]) * invScale;
    const float z = (target.z - sourceTransform_.translation[2]) * invScale;
    const float dx = (sourceTransform_.rotation[2][0] * z + sourceTransform_.rotation[1][0] * y +
                      sourceTransform_.rotation[0][0] * x) - effectTransform_.translation[0];
    const float dy = (sourceTransform_.rotation[2][1] * z + sourceTransform_.rotation[1][1] * y +
                      sourceTransform_.rotation[0][1] * x) - effectTransform_.translation[1];
    const float dz = (sourceTransform_.rotation[2][2] * z + sourceTransform_.rotation[1][2] * y +
                      sourceTransform_.rotation[0][2] * x) - effectTransform_.translation[2];
    const float averageLifetime = (description_->lifetimeMin + description_->lifetimeMax) * 0.5f;
    const float shrink = averageLifetime * averageLifetime * 0.5f;
    // The direction, pulled towards the effect position by the lifetime square (the original's billboard anchor).
    float vx = (effectTransform_.rotation[2][0] * dz + effectTransform_.rotation[1][0] * dy +
                effectTransform_.rotation[0][0] * dx) - position_.x * shrink;
    float vy = (effectTransform_.rotation[2][1] * dz + effectTransform_.rotation[1][1] * dy +
                effectTransform_.rotation[0][1] * dx) - position_.y * shrink;
    float vz = (effectTransform_.rotation[2][2] * dz + effectTransform_.rotation[1][2] * dy +
                effectTransform_.rotation[0][2] * dx) - position_.z * shrink;
    const float length = Sqrt(vz * vz + vy * vy + vx * vx);
    if (0.0f < length) {
        const float inv = 1.0f / length;
        vx *= inv;
        vy *= inv;
        vz *= inv;
    }
    const float horizontal = Sqrt(vz * vz + 0.0f + vx * vx);
    float m10 = 0.0f;
    float m11 = 1.0f;
    float r10 = 0.0f;
    float r12 = 0.0f;
    float r14 = 0.0f;
    float r6 = 0.0f;
    if (0.0f < horizontal) {
        const float t = 1.0f / horizontal;
        r6 = -(vz * t);
        r10 = t * 0.0f;
        m11 = t * vx;
        r12 = vz * r10 - vy * m11;
        r14 = vz * t * vz + m11 * vx;
        m10 = vy * r6 - vx * r10;
    }
    effectTransform_.rotation[0][0] = vx;
    effectTransform_.rotation[0][1] = r12;
    effectTransform_.rotation[0][2] = r6;
    effectTransform_.rotation[1][0] = vy;
    effectTransform_.rotation[1][1] = r14;
    effectTransform_.rotation[1][2] = r10;
    effectTransform_.rotation[2][0] = vz;
    effectTransform_.rotation[2][1] = m10;
    effectTransform_.rotation[2][2] = m11;
    effectTransform_.flags |= 4;
    ++effectTransform_.changeCounter;
    velocityBias_ = (length / ((description_->speedMin + description_->speedMax) * 0.5f)) / averageLifetime;
}

// 0x1005f4c9e
void cSC4ParticlesEffect::SetGlobalParams(const cSC4GlobalEffectParams* params) { globalParams_ = params; }

// 0x1005f4cac
void cSC4ParticlesEffect::SuppressEmission() { suppressEmission_ = true; }

// 0x1005f4cba
void cSC4ParticlesEffect::SetVertexBufferExtension(IParticlesVertexBuffer* extension) {
    if (vertexBuffer_ == extension) return;
    if (extension) extension->AddRef();
    IParticlesVertexBuffer* previous = vertexBuffer_;
    vertexBuffer_ = extension;
    if (previous) previous->Release();
}

// 0x1005f4d0e
cS3DBoundingBox cSC4ParticlesEffect::BoundingBox() const { return box_; }

// 0x1005f4d34
void cSC4ParticlesEffect::PrepareForRendering(IParticlesDrawContext* context) {
    const int rotation = context->Rotation();
    if (rotation == basisRotation_) return;
    basisRotation_ = rotation;
    if (description_->billboardMode == 1) {
        const float x = description_->velocityMin[0];
        const float z = description_->velocityMin[2];
        const float length = Sqrt(z * z + x * x);
        if (!(std::fabs(length) >= 0.0001f)) {
            basisA_ = cS3DVector3{0.0f, 0.0f, 1.0f};
            basisB_ = cS3DVector3{0.0f, 0.0f, 0.0f};
        } else {
            const float inv = 1.0f / length;
            basisA_ = cS3DVector3{-z * inv, 0.0f, x * inv};
            basisB_ = cS3DVector3{x * inv, 0.0f, z * inv};
        }
    } else {
        context->GetBillboardBasis(basisA_, basisB_);
    }
}

// ---- next group -----------------------------------------------------------------------------------

namespace {
// (int)v for non-negative v, the way the original rounds upwards: (v + 0.5) is rounded through the float bit trick and
// the exact-integer case is folded back.
inline int RoundUpToInt(float v) {
    const float added = v + 0.5f + kMagic;
    std::int32_t bits;
    std::memcpy(&bits, &added, sizeof(bits));
    const int lower = bits - kMagicBits;
    if (v == static_cast<float>(lower - 1)) return lower - 1;
    return lower;
}
}  // namespace

// 0x1005f4ea0
void cSC4ParticlesEffect::GenerateNewParticles() {
    if (delayedStop_) return;
    float elapsed = deltaTime_;
    if (0.0f < spawnDelay_) {
        spawnDelay_ -= elapsed;
        if (0.0f < spawnDelay_) return;
        elapsed = -spawnDelay_;
    }
    float limit = description_->startDelay;
    if ((0.0f < limit) && (limit = limit - 0.01f, limit < elapsed)) elapsed = limit;
    const std::uint64_t flags = description_->flags;
    if ((flags & 4) != 0) {
        MaintainParticles(elapsed);
    } else if ((flags & 2) != 0) {
        InjectParticles(elapsed);
    } else {
        AddRateCurveParticles(elapsed);
    }
}

// 0x1005f4f2c
void cSC4ParticlesEffect::MaintainParticles(float elapsed) {
    spawnAccumulator_ += static_cast<double>(elapsed) * inverseRate_;
    if (static_cast<double>(spawnCounter_) < spawnAccumulator_) {
        const int target = TruncateToInt(CurveFirst(description_->rateCurve));
        if (target != liveCount_) {
            int count = liveCount_ - target;
            do {
                cSCParticle* base = CreateParticle();
                if (base) {
                    cSC4EffectsParticle* particle = EffectsParticle(base);
                    if (!particle->alive) {
                        DestroyParticle(base);
                    } else {
                        AddParticle(base);
                    }
                }
                ++count;
            } while (count != 0);
        }
    }
}

// 0x1005f5000
void cSC4ParticlesEffect::InjectParticles(float elapsed) {
    spawnAccumulator_ += static_cast<double>(elapsed) * inverseRate_;
    std::uint32_t counter = spawnCounter_;
    double accumulated = spawnAccumulator_;
    double counterAsDouble = static_cast<double>(counter);
    if (!(counterAsDouble < accumulated)) return;
    const cSC4ParticlesDescription* desc = description_;
    float rate = CurveFirst(desc->rateCurve);
    if (20.0f < rate && (desc->flag10 & 1) == 0) {
        rate = rate * rateScale_ * rateBias_ * (globalParams_ ? globalParams_->rateScale : 1.0f);
    }
    int count = RoundUpToInt(rate);
    if (!(counterAsDouble < accumulated)) return;
    while (true) {
        const float age = static_cast<float>(static_cast<double>(desc->spawnPhase) * (accumulated - counterAsDouble));
        if (age < desc->lifetimeMax && count != 0) {
            int remaining = count;
            do {
                cSCParticle* base = CreateParticle();
                if (base) {
                    cSC4EffectsParticle* particle = EffectsParticle(base);
                    if (!particle->alive || !UpdateParticlePhysics(particle, age)) {
                        DestroyParticle(base);
                    } else {
                        if ((description_->flags & 0x80000000ull) != 0) LaunchTimeZeroEffects(base);
                        AddParticle(base);
                    }
                }
                --remaining;
            } while (remaining != 0);
            counter = spawnCounter_;
            desc = description_;
        }
        ++counter;
        spawnCounter_ = counter;
        if (desc->maxParticles - 1u < counter) {
            Stop(0);
            return;
        }
        if (0.0f <= desc->delayMinB) {
            const float delay = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->delayMinB, desc->delayMaxB));
            accumulated = spawnAccumulator_ - static_cast<double>(delay) * inverseRate_;
            spawnAccumulator_ = accumulated;
            counter = spawnCounter_;
            counterAsDouble = static_cast<double>(counter);
            if (accumulated < counterAsDouble) {
                spawnDelay_ = static_cast<float>(static_cast<double>(desc->spawnPhase) * (counterAsDouble - accumulated));
                spawnAccumulator_ = counterAsDouble;
                return;
            }
        } else {
            accumulated = spawnAccumulator_;
        }
        counterAsDouble = static_cast<double>(counter);
        if (accumulated <= counterAsDouble) return;
        desc = description_;
    }
}

// 0x1005f5288
void cSC4ParticlesEffect::AddRateCurveParticles(float elapsed) {
    float rateMultiplier = rateScale_ * rateBias_ * (globalParams_ ? globalParams_->rateScale : 1.0f);
    const std::uint64_t flags = description_->flags;
    if ((flags & 0x80000) != 0) {
        rateMultiplier *= effectTransform_.scale;
    } else if ((flags & 0x100000) != 0) {
        rateMultiplier *= effectTransform_.scale * effectTransform_.scale;
    } else if ((flags & 0x200000) != 0) {
        rateMultiplier *= effectTransform_.scale * effectTransform_.scale * effectTransform_.scale;
    }
    bool delayed = false;
    do {
        float step = elapsed;
        if (kStep < step) step = kStep;
        double accumulated = static_cast<double>(step) * inverseRate_ + spawnAccumulator_;
        spawnAccumulator_ = accumulated;
        const std::uint32_t oldCounter = spawnCounter_;
        const std::uint32_t next = oldCounter + 1;
        const double nextAsDouble = static_cast<double>(next);
        std::uint32_t effectiveCounter = oldCounter;
        const cSC4ParticlesDescription* desc = description_;
        if (!(accumulated <= nextAsDouble)) {
            spawnCounter_ = next;
            effectiveCounter = next;
            if (desc->maxParticles != 0 && next == desc->maxParticles) {
                if ((desc->flags & 8) == 0) {
                    Stop(0);
                    return;
                }
                spawnCounter_ = oldCounter;
                effectiveCounter = oldCounter;
                spawnAccumulator_ = static_cast<double>(desc->maxParticles) - 1e-06;
                accumulated = spawnAccumulator_;
            } else if (0.0f <= desc->delayMinB) {
                step = static_cast<float>(static_cast<double>(step) -
                                          static_cast<double>(desc->spawnPhase) * (accumulated - nextAsDouble));
                spawnAccumulator_ = nextAsDouble;
                delayed = true;
                accumulated = nextAsDouble;
            }
        }
        const float fraction = static_cast<float>(accumulated - static_cast<double>(effectiveCounter));
        const float curve = CurveValue(desc->rateCurve, fraction);
        const float spawned = rateMultiplier * step * curve + spawnFraction_;
        const int count = TruncateToInt(spawned);
        spawnFraction_ = spawned - static_cast<float>(count);
        if (count > 0) {
            float age = elapsed;
            int remaining = count;
            do {
                cSCParticle* base = CreateParticle();
                if (base) {
                    cSC4EffectsParticle* particle = EffectsParticle(base);
                    if (!particle->alive || !UpdateParticlePhysics(particle, age)) {
                        DestroyParticle(base);
                    } else {
                        if ((description_->flags & 0x80000000ull) != 0) LaunchTimeZeroEffects(base);
                        AddParticle(base);
                    }
                }
                age -= step / static_cast<float>(count);
                --remaining;
            } while (remaining != 0);
        }
        elapsed -= step;
        if (delayed) {
            const float delay = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->delayMinB, desc->delayMaxB));
            elapsed -= delay;
            if (elapsed < 0.0f) {
                spawnDelay_ = -elapsed;
                return;
            }
        }
    } while (0.0f < elapsed);
}

// 0x1005f564e
void cSC4ParticlesEffect::LaunchTimeZeroEffects(cSCParticle* particle) {
    const cSC4EffectsParticle* p = EffectsParticle(particle);
    if (!p->alive) return;
    const std::vector<cSC4TimedEffectRecord>& timed = description_->timedEffects;
    if (timed.empty() || timed[0].time != 0.0f) return;
    for (const cSC4TimedEffectRecord& record : timed) {
        if (record.time != 0.0f) return;
        TriggerEffect(record.name.c_str(), p->position);
    }
}

// 0x1005f9224
cSCParticle* cSC4ParticlesEffect::CreateParticle() {
    auto* particle = static_cast<cSC4EffectsParticle*>(std::malloc(sizeof(cSC4EffectsParticle)));
    if (!particle) return nullptr;
    // The original only memset()s the 0x1c-byte cSCParticle part and then clears +0x1c..+0x24 / +0x38..+0x40, leaving the
    // remaining padding uninitialised; zeroing the whole record removes that nondeterminism (same visible fields).
    std::memset(particle, 0, sizeof(cSC4EffectsParticle));
    InitEffectsParticle(particle);
    return SimulatorParticle(particle);
}

// The Update loop unlinks the particle, drops the live count and frees the record through the simulator's DestroyParticle
// (base vslot 0x88 = +0x88 of the second vtable group).
std::list<cSCParticle*>::iterator cSC4ParticlesEffect::RemoveDeadParticle(std::list<cSCParticle*>::iterator it,
                                                                          cSC4EffectsParticle* particle) {
    it = particles_.erase(it);
    --liveCount_;
    DestroyParticle(SimulatorParticle(particle));
    return it;
}

// 0x1005f9d98
void cSC4ParticlesEffect::DestroyParticle(cSCParticle* particle) {
    if (particle) std::free(particle);
}

// 0x1005f9288
void cSC4ParticlesEffect::InitEffectsParticle(cSC4EffectsParticle* particle) {
    const cSC4ParticlesDescription* desc = description_;
    particle->alive = 1;
    particle->lifetime = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->lifetimeMin, desc->lifetimeMax));
    particle->age = 0.0f;
    particle->position.x = static_cast<float>(g_random.RandomDoubleRangeUniform(emissionBox_.min[0],
                                                                                emissionBox_.max[0]));
    particle->position.y = static_cast<float>(g_random.RandomDoubleRangeUniform(emissionBox_.min[1],
                                                                                emissionBox_.max[1]));
    particle->position.z = static_cast<float>(g_random.RandomDoubleRangeUniform(emissionBox_.min[2],
                                                                                emissionBox_.max[2]));
    particle->velocity.x = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->velocityMin[0],
                                                                                desc->velocityMax[0]));
    particle->velocity.y = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->velocityMin[1],
                                                                                desc->velocityMax[1]));
    particle->velocity.z = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->velocityMin[2],
                                                                                desc->velocityMax[2]));
    const float speed = static_cast<float>(g_random.RandomDoubleRangeUniform(desc->speedMin, desc->speedMax));
    {
        const float x = particle->velocity.x;
        const float y = particle->velocity.y;
        const float z = particle->velocity.z;
        const float length2 = z * z + y * y + x * x;
        if (0.0f < length2) {
            const float scale = (speed * velocityBias_) / Sqrt(length2);
            particle->velocity.x = x * scale;
            particle->velocity.y = y * scale;
            particle->velocity.z = z * scale;
        }
    }
    if (0.0f < desc->terrainRadius) {
        const float dx = particle->position.x - desc->terrainCentre[0];
        const float dy = particle->position.y - desc->terrainCentre[1];
        const float dz = particle->position.z - desc->terrainCentre[2];
        const float length2 = dz * dz + dy * dy + dx * dx;
        if (0.0001f <= length2) {
            const float push = desc->terrainRadius / Sqrt(length2);
            particle->velocity.x += dx * push;
            particle->velocity.y += dy * push;
            particle->velocity.z += dz * push;
        }
    }
    {
        const float x = particle->position.x;
        const float y = particle->position.y;
        const float z = particle->position.z;
        const float scale = effectTransform_.scale;
        particle->position.x = (effectTransform_.rotation[0][2] * z + effectTransform_.rotation[0][1] * y +
                                effectTransform_.rotation[0][0] * x) * scale + effectTransform_.translation[0];
        particle->position.y = (effectTransform_.rotation[1][2] * z + effectTransform_.rotation[1][1] * y +
                                effectTransform_.rotation[1][0] * x) * scale + effectTransform_.translation[1];
        particle->position.z = (effectTransform_.rotation[2][2] * z + effectTransform_.rotation[2][1] * y +
                                effectTransform_.rotation[2][0] * x) * scale + effectTransform_.translation[2];
    }
    {
        const float x = particle->velocity.x;
        const float y = particle->velocity.y;
        const float z = particle->velocity.z;
        particle->velocity.x = effectTransform_.rotation[0][2] * z + effectTransform_.rotation[0][1] * y +
                               effectTransform_.rotation[0][0] * x + velocityOffset_.x;
        particle->velocity.y = effectTransform_.rotation[1][2] * z + effectTransform_.rotation[1][1] * y +
                               effectTransform_.rotation[1][0] * x + velocityOffset_.y;
        particle->velocity.z = effectTransform_.rotation[2][2] * z + effectTransform_.rotation[2][1] * y +
                               effectTransform_.rotation[2][0] * x + velocityOffset_.z;
    }
    if ((desc->flags & kSpecialEmit) != 0) {
        if (!DoHeightBasedEmit(particle)) return;
        desc = description_;
    }
    if ((desc->flags & 0x20) != 0) {
        const float x = particle->velocity.x;
        const float y = particle->velocity.y;
        const float z = particle->velocity.z;
        float factor = 1.0f;
        if (desc->maxWidth != 0.0f || std::isnan(desc->maxWidth)) {
            const float dotB = basisB_.z * z + basisB_.y * y + basisB_.x * x;
            const float dotA = basisA_.z * z + basisA_.y * y + basisA_.x * x;
            factor = Sqrt(dotA * dotA + dotB * dotB) / desc->maxWidth;
        }
        const float seed = CurveFirst(desc->sizeCurve);
        const float invSpeed = 1.0f / (speed * velocityBias_);
        particle->position.x += x * 0.5f * factor * seed * invSpeed;
        particle->position.y += y * 0.5f * factor * seed * invSpeed;
        particle->position.z += invSpeed * z * 0.5f * factor * seed;
    }
    particle->reserved18 = 0;
    particle->size = static_cast<float>(g_random.RandomDoubleRangeUniform(1.0f - desc->sizeJitter,
                                                                          desc->sizeJitter + 1.0f)) *
                     sizeScale_ * scaleBias_;
    particle->alpha = static_cast<float>(g_random.RandomDoubleRangeUniform(1.0f - desc->alphaJitter,
                                                                           desc->alphaJitter + 1.0f));
    particle->alphaBias = static_cast<float>(g_random.RandomDoubleRangeUniform(1.0f - desc->jitter84,
                                                                               desc->jitter84 + 1.0f));
    particle->colourR = static_cast<float>(g_random.RandomDoubleRangeUniform(1.0f - desc->jitter88,
                                                                             desc->jitter88 + 1.0f));
    particle->colourG = static_cast<float>(g_random.RandomDoubleRangeUniform(1.0f - desc->jitter8c,
                                                                             desc->jitter8c + 1.0f));
    particle->colourB = static_cast<float>(g_random.RandomDoubleRangeUniform(1.0f - desc->jitter90,
                                                                             desc->jitter90 + 1.0f));
    float spin = 1.0f;
    if (0.0f < desc->spinRange) {
        spin = static_cast<float>(g_random.RandomDoubleRangeUniform(-desc->spinRange, desc->spinRange));
    }
    particle->rotation = spin;
    if ((desc->flag10 & 1) == 0) {
        particle->size *= globalParams_ ? globalParams_->sizeScale : 1.0f;
        desc = description_;
    }
    if ((desc->flags & 0x4000) != 0) {
        particle->size *= effectTransform_.scale;
        desc = description_;
    }
    if (desc->billboardMode == 1) {
        const float angle = ATan2(-effectTransform_.rotation[2][0], effectTransform_.rotation[0][0]);
        particle->rotation = spin + angle * kInvTwoPi;
    }
}

// 0x1005f9ac2
bool cSC4ParticlesEffect::DoHeightBasedEmit(cSC4EffectsParticle* particle) {
    const cSC4ParticlesDescription* desc = description_;
    const float x = particle->position.x;
    const float y = particle->position.y;
    const float z = particle->position.z;
    const float worldX = (sourceTransform_.rotation[0][2] * z + sourceTransform_.rotation[0][1] * y +
                          sourceTransform_.rotation[0][0] * x) * sourceTransform_.scale +
                         sourceTransform_.translation[0];
    const float localZ = (z * sourceTransform_.rotation[2][2] + x * sourceTransform_.rotation[2][0] +
                          y * sourceTransform_.rotation[2][1]) * sourceTransform_.scale +
                         sourceTransform_.translation[2];
    if (!(0.0f <= worldX) || !(0.0f <= localZ) || !(worldX < worldWidth_) || !(localZ < worldDepth_)) {
        particle->alive = 0;
        return false;
    }
    const float height = terrain_ ? terrain_->TerrainHeight(worldX, localZ) : 0.0f;
    float span = height - waterLevel_;
    if ((desc->flags & 0x10000) != 0 &&
        !(desc->heightMin <= span && (span < desc->heightMax || span == desc->heightMax))) {
        particle->alive = 0;
        return false;
    }
    std::uint64_t flags = desc->flags;
    if ((kFlagsLakeSea & flags) != 0) {
        int cellX = 0, cellZ = 0;
        if (ParticlesCity()) ParticlesCity()->WorldToCell(worldX, localZ, cellX, cellZ);
        unsigned cellFlags = 0;
        if (ParticlesCity() && ParticlesCity()->Cells()) cellFlags = ParticlesCity()->Cells()->CellFlags(cellX, cellZ);
        flags = desc->flags;
        bool blocked = cellFlags != 0x80;
        if ((flags & 0x20000) != 0) blocked = (cellFlags & 0xf) == 0;
        if (blocked) {
            particle->alive = 0;
            return false;
        }
    }
    float emitY = height;
    if ((flags & 0x4000) != 0) {
        if (static_cast<std::int16_t>(flags & 0xffff) < 0 && emitY < waterLevel_) emitY = waterLevel_;
    } else {
        if (static_cast<std::int16_t>(flags & 0xffff) >= 0) {
            particle->alive = 0;
            return false;
        }
        emitY = waterLevel_;
    }
    const float jitter = static_cast<float>(g_random.RandomDoubleRangeUniform(emissionBox_.min[1],
                                                                              emissionBox_.max[1]));
    const float invScale = 1.0f / sourceTransform_.scale;
    const float localX = (worldX - sourceTransform_.translation[0]) * invScale;
    const float localY = ((emitY + jitter) - sourceTransform_.translation[1]) * invScale;
    const float localZ2 = invScale * (localZ - sourceTransform_.translation[2]);
    particle->position.x = sourceTransform_.rotation[2][0] * localZ2 +
                           sourceTransform_.rotation[1][0] * localY +
                           sourceTransform_.rotation[0][0] * localX;
    particle->position.y = sourceTransform_.rotation[2][1] * localZ2 +
                           sourceTransform_.rotation[1][1] * localY +
                           sourceTransform_.rotation[0][1] * localX;
    particle->position.z = localZ2 * sourceTransform_.rotation[2][2] +
                           localY * sourceTransform_.rotation[1][2] +
                           localX * sourceTransform_.rotation[0][2];
    return true;
}

// ---- next group -----------------------------------------------------------------------------------

// ---- physics ---------------------------------------------------------------------------------------
// 0x1005f7630
void cSC4ParticlesEffect::UpdateActiveParticles() {
    box_ = EmptyBox();
    if (particles_.empty()) return;
    float minX = 1.0f, maxX = 0.0f;
    float minY = 1.0f, maxY = 0.0f;
    float minZ = 1.0f, maxZ = 0.0f;
    const float scale = sourceTransform_.scale;
    for (cSCParticle* baseParticle : particles_) {
        const cSC4EffectsParticle* particle = EffectsParticle(baseParticle);
        const float x = particle->position.x;
        const float y = particle->position.y;
        const float z = particle->position.z;
        const float worldX = (sourceTransform_.rotation[0][2] * z + sourceTransform_.rotation[0][1] * y +
                              sourceTransform_.rotation[0][0] * x) * scale + sourceTransform_.translation[0];
        const float worldY = (sourceTransform_.rotation[1][2] * z + sourceTransform_.rotation[1][1] * y +
                              sourceTransform_.rotation[1][0] * x) * scale + sourceTransform_.translation[1];
        const float worldZ = (sourceTransform_.rotation[2][2] * z + sourceTransform_.rotation[2][1] * y +
                              sourceTransform_.rotation[2][0] * x) * scale + sourceTransform_.translation[2];
        if (minX <= maxX) {
            if (maxX <= worldX) maxX = worldX;
            box_.max[0] = maxX;
            if (worldX <= minX) minX = worldX;
            box_.min[0] = minX;
            if (maxY <= worldY) maxY = worldY;
            box_.max[1] = maxY;
            if (worldY <= minY) minY = worldY;
            box_.min[1] = minY;
            if (maxZ <= worldZ) maxZ = worldZ;
            box_.max[2] = maxZ;
            if (worldZ <= minZ) minZ = worldZ;
        } else {
            box_.max[0] = worldX;
            box_.max[1] = worldY;
            box_.max[2] = worldZ;
            box_.min[0] = worldX;
            box_.min[1] = worldY;
            minX = worldX;
            minY = worldY;
            minZ = worldZ;
            maxX = worldX;
            maxY = worldY;
            maxZ = worldZ;
        }
        box_.min[2] = minZ;
    }
    if (minX <= maxX) {
        const float expand = CurveFirst(description_->sizeCurve);
        box_.min[0] = minX - expand;
        box_.min[1] = minY - expand;
        box_.min[2] = minZ - expand;
        box_.max[0] = maxX + expand;
        box_.max[1] = maxY + expand;
        box_.max[2] = maxZ + expand;
    }
}

// 0x1005f78ba
bool cSC4ParticlesEffect::UpdateParticlePhysics(cSC4EffectsParticle* particle, float elapsed) {
    if ((description_->flags & kExtraPhysicsFlags) != 0) return UpdateParticlePhysicsExtra(particle, elapsed);
    const float age = particle->age;
    particle->age = age + elapsed;
    if (particle->lifetime <= age + elapsed) {
        particle->alive = 0;
        return false;
    }
    const float vx = particle->velocity.x;
    const float vy = particle->velocity.y;
    const float vz = particle->velocity.z;
    particle->velocity.x = position_.x * elapsed + vx;
    particle->velocity.y = position_.y * elapsed + vy;
    particle->velocity.z = position_.z * elapsed + vz;
    const float damping = 1.0f - description_->damping * elapsed;
    particle->velocity.x *= damping;
    particle->velocity.y *= damping;
    particle->velocity.z *= damping;
    const float half = elapsed * 0.5f;
    particle->position.x = (particle->velocity.x + vx) * half + particle->position.x;
    particle->position.y = (particle->velocity.y + vy) * half + particle->position.y;
    particle->position.z = half * (particle->velocity.z + vz) + particle->position.z;
    return true;
}

// 0x1005f79bc
bool cSC4ParticlesEffect::UpdateParticlePhysicsExtra(cSC4EffectsParticle* particle, float elapsed) {
    const float age = particle->age;
    particle->age = age + elapsed;
    if (particle->lifetime <= age + elapsed) {
        particle->alive = 0;
        return false;
    }
    do {
        float step = elapsed;
        if (kStep < step) step = kStep;
        if ((description_->flags & 0x10000000) == 0) {
            particle->position.x += particle->velocity.x * step;
            particle->position.y += particle->velocity.y * step;
            particle->position.z += particle->velocity.z * step;
        } else {
            ApplyTractorForce(particle, step);
        }
        float vx = particle->velocity.x;
        float vy = particle->velocity.y;
        float vz = particle->velocity.z;
        vx += position_.x * step;
        vy += position_.y * step;
        vz += position_.z * step;
        if ((description_->flags & 0x8000000) != 0) {
            const float dx = attractorOrigin_.x - particle->position.x;
            const float dy = attractorOrigin_.y - particle->position.y;
            const float dz = attractorOrigin_.z - particle->position.z;
            const float length2 = dz * dz + dy * dy + dx * dx;
            if (0.0f < length2) {
                const float length = Sqrt(length2);
                const float inv = 1.0f / length;
                float t = length / description_->attractorTime;
                if (1.0f < t) t = 1.0f;
                const float curve = CurveValue(description_->attractorCurve, t);
                vx += dx * inv * curve;
                vy += dy * inv * curve;
                vz += dz * inv * curve;
            }
        }
        const float damping = 1.0f - description_->damping * elapsed;
        particle->velocity.x = vx * damping;
        particle->velocity.y = vy * damping;
        particle->velocity.z = vz * damping;
        const float x = particle->position.x;
        const float y = particle->position.y;
        const float z = particle->position.z;
        const float scale = sourceTransform_.scale;
        const cS3DVector3 world{
            (sourceTransform_.rotation[0][2] * z + sourceTransform_.rotation[0][1] * y +
             sourceTransform_.rotation[0][0] * x) * scale + sourceTransform_.translation[0],
            (sourceTransform_.rotation[1][2] * z + sourceTransform_.rotation[1][1] * y +
             sourceTransform_.rotation[1][0] * x) * scale + sourceTransform_.translation[1],
            (sourceTransform_.rotation[2][2] * z + sourceTransform_.rotation[2][1] * y +
             sourceTransform_.rotation[2][0] * x) * scale + sourceTransform_.translation[2]};
        if (world.x <= 0.0f || world.z <= 0.0f || worldWidth_ <= world.x || worldDepth_ <= world.z) {
            if ((description_->flags & 0x800) != 0) {
                particle->alive = 0;
                return false;
            }
        } else if ((description_->flags & 0x500) != 0) {   // byte +1 bits 0 and 2: collide with the terrain
            if (!ApplyTerrainForces(particle, world, step)) {
                particle->alive = 0;
                return false;
            }
        }
        elapsed -= step;
    } while (0.0f < elapsed);
    if ((description_->flags8 & 1) != 0) TestBuildingCollision(particle);   // byte +8 bit 0
    return particle->alive != 0;
}

// 0x1005f7de6
void cSC4ParticlesEffect::ApplyTractorForce(cSC4EffectsParticle* particle, float elapsed) {
    const float age = particle->age;
    if (description_->tractors.empty()) return;
    const cSC4TractorRecord* const begin = description_->tractors.data();
    const cSC4TractorRecord* record = begin + description_->tractors.size();
    float time = 0.0f;
    do {
        time = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(record) - 8);
        --record;
        if (!(time > age)) break;
        if (record == begin) break;   // the original trusts the times; do not walk before the array
    } while (true);
    float end = record->end;
    if (end != 0.0f || std::isnan(end)) {
        if (end < age) {
            particle->position.x += particle->velocity.x * elapsed;
            particle->position.y += particle->velocity.y * elapsed;
            particle->position.z += particle->velocity.z * elapsed;
            return;
        }
    } else {
        end = particle->lifetime;
    }
    const float scale = effectTransform_.scale;
    const cS3DVector3 from = record->from;
    const cS3DVector3 to = record->to;
    const float ax = (effectTransform_.rotation[0][2] * from.z + effectTransform_.rotation[0][1] * from.y +
                      effectTransform_.rotation[0][0] * from.x) * scale + effectTransform_.translation[0];
    const float ay = (effectTransform_.rotation[1][2] * from.z + effectTransform_.rotation[1][1] * from.y +
                      effectTransform_.rotation[1][0] * from.x) * scale + effectTransform_.translation[1];
    const float az = (effectTransform_.rotation[2][2] * from.z + effectTransform_.rotation[2][1] * from.y +
                      effectTransform_.rotation[2][0] * from.x) * scale + effectTransform_.translation[2];
    const float bx = (effectTransform_.rotation[0][2] * to.z + effectTransform_.rotation[0][1] * to.y +
                      effectTransform_.rotation[0][0] * to.x) * scale;
    const float by = (effectTransform_.rotation[1][2] * to.z + effectTransform_.rotation[1][1] * to.y +
                      effectTransform_.rotation[1][0] * to.x) * scale;
    const float bz = (effectTransform_.rotation[2][2] * to.z + effectTransform_.rotation[2][1] * to.y +
                      effectTransform_.rotation[2][0] * to.x) * scale;
    float length = (end - age) + elapsed;
    const float radius = description_->tractorRadius;
    if ((radius != 0.0f || std::isnan(radius))) {
        const float dx = particle->position.x - ax;
        const float dy = particle->position.y - ay;
        const float dz = particle->position.z - az;
        if (radius * length < Sqrt(dy * dy + dz * dz + dx * dx)) {
            const float pull = age - time;
            length += pull;
            particle->age = age - pull;
        }
    }
    const float t = elapsed / length;
    const float t2 = t * t;
    const float t3 = t * t2;
    const float twice = t + t;
    const float threeT2 = t2 * 3.0f;
    const float h00 = t * kHermite[0][2] + t2 * kHermite[0][1] + t3 * kHermite[0][0] + kHermite[0][3];
    const float h01 = t * kHermite[1][2] + t2 * kHermite[1][1] + t3 * kHermite[1][0] + kHermite[1][3];
    const float h10d = kHermite[2][3] + kHermite[2][1] * twice + kHermite[2][0] * threeT2;
    const float h11d = kHermite[3][3] + kHermite[3][1] * twice + kHermite[3][0] * threeT2;
    const float h10 = (t * kHermite[2][2] + t2 * kHermite[2][1] + t3 * kHermite[2][0] + kHermite[2][3]) * length;
    const float h11 = (t * kHermite[3][2] + t2 * kHermite[3][1] + t3 * kHermite[3][0] + kHermite[3][3]) * length;
    const float h00d = (kHermite[0][1] * twice + kHermite[0][0] * threeT2 + kHermite[0][2]) / length;
    const float h01d = (kHermite[1][1] * twice + kHermite[1][0] * threeT2 + kHermite[1][2]) / length;
    const float px = particle->position.x;
    const float py = particle->position.y;
    const float pz = particle->position.z;
    const float vx = particle->velocity.x;
    const float vy = particle->velocity.y;
    const float vz = particle->velocity.z;
    particle->position.x = bx * h11 + h10 * vx + ax * h01 + px * h00;
    particle->position.y = by * h11 + h10 * vy + ay * h01 + py * h00;
    particle->position.z = h11 * bz + h10 * vz + h01 * az + h00 * pz;
    particle->velocity.x = bx * h11d + vx * h10d + ax * h01d + px * h00d;
    particle->velocity.y = by * h11d + vy * h10d + ay * h01d + py * h00d;
    particle->velocity.z = h11d * bz + h10d * vz + h01d * az + h00d * pz;
}

// 0x1005f83da
// PARTIAL: see the ledger. The two collision branches read the unrecovered effect-maps / terrain services; the water
// kill switch and the velocity reflection are recovered, the vertical-plane bounce of flag 0x400 is left out.
bool cSC4ParticlesEffect::ApplyTerrainForces(cSC4EffectsParticle* particle, const cS3DVector3& position,
                                             float) {
    const cSC4ParticlesDescription* desc = description_;
    if (position.y <= waterLevel_ && waterLevel_ != position.y) {
        const float chance = desc->terrainBounce[6];   // description +0x1c4
        if (chance != 0.0f || std::isnan(chance)) {
            bool kill = true;
            if (chance != 1.0f || std::isnan(chance)) {
                kill = static_cast<double>(chance) >= g_random.RandomDoubleUniform();
            }
            if (kill) {
                if (hasWaterEffect_) {
                    TriggerEffect("_water", cS3DVector3{position.x, waterLevel_, position.z});
                }
                return false;
            }
        }
    }
    // The original asks the effects manager for its effect maps first and does nothing without them; only the flag
    // 0x400 branch reads them, so the port runs the two terrain branches without and skips that one.
    IParticlesEffectMaps* maps = ParticlesEffects() ? ParticlesEffects()->EffectMaps() : nullptr;
    bool alive = true;
    desc = description_;
    if ((desc->flags & 0x400) == 0) {
        if (desc->terrainDrag == 0.0f) {
            // i386 0x004176ae (the branch the port had dropped): no drag = follow the ground. Below the surface (or
            // always, with flag 0x200) the particle is put on it and its velocity into the ground is removed.
            const float height = terrain_ ? terrain_->TerrainHeight(position.x, position.z) : position.y;
            if ((desc->flags & 0x200) != 0 || (position.y <= height && height != position.y)) {
                const float invScale = 1.0f / sourceTransform_.scale;
                const float lx = (position.x - sourceTransform_.translation[0]) * invScale;
                const float ly = (height - sourceTransform_.translation[1]) * invScale;
                const float lz = (position.z - sourceTransform_.translation[2]) * invScale;
                const auto& r = sourceTransform_.rotation;   // BackTransformPoint / BackTransformDirection: the transpose
                particle->position.x = r[0][0] * lx + r[1][0] * ly + r[2][0] * lz;
                particle->position.y = r[0][1] * lx + r[1][1] * ly + r[2][1] * lz;
                particle->position.z = r[0][2] * lx + r[1][2] * ly + r[2][2] * lz;
                cS3DVector3 normal{0.0f, 1.0f, 0.0f};
                if (terrain_) terrain_->TerrainNormal(position.x, position.z, normal);
                const float nx = r[0][0] * normal.x + r[1][0] * normal.y + r[2][0] * normal.z;
                const float ny = r[0][1] * normal.x + r[1][1] * normal.y + r[2][1] * normal.z;
                const float nz = r[0][2] * normal.x + r[1][2] * normal.y + r[2][2] * normal.z;
                const float into = -(nx * particle->velocity.x + ny * particle->velocity.y + nz * particle->velocity.z);
                if (0.0f < into) {
                    particle->velocity.x += nx * into;
                    particle->velocity.y += ny * into;
                    particle->velocity.z += nz * into;
                }
            }
        } else {
            const float water = terrain_ ? terrain_->WaterHeight(position.x, position.z) : 0.0f;
            if (position.y <= water && water != position.y) {
                const float height = terrain_ ? terrain_->TerrainHeight(position.x, position.z) : 0.0f;
                if (hasTerrainEffect_) {
                    TriggerEffect("_terrain", cS3DVector3{position.x, height, position.z});
                }
                const float chance = desc->terrainBounce[5];   // description +0x1c0
                bool bounce = true;
                if (chance != 0.0f || std::isnan(chance)) {
                    if (chance != 1.0f || std::isnan(chance)) {
                        bounce = static_cast<double>(chance) < g_random.RandomDoubleUniform();
                    } else {
                        bounce = false;
                    }
                }
                if (!bounce) {
                    alive = false;
                } else {
                    // Place the particle on the surface, back in the effect's local space.
                    const float invScale = 1.0f / sourceTransform_.scale;
                    const float localX = (position.x - sourceTransform_.translation[0]) * invScale;
                    const float localY = (height - sourceTransform_.translation[1]) * invScale;
                    const float localZ = (position.z - sourceTransform_.translation[2]) * invScale;
                    particle->position.x = (sourceTransform_.rotation[0][2] * localZ +
                                            sourceTransform_.rotation[0][1] * localY +
                                            sourceTransform_.rotation[0][0] * localX);
                    particle->position.y = (sourceTransform_.rotation[1][2] * localZ +
                                            sourceTransform_.rotation[1][1] * localY +
                                            sourceTransform_.rotation[1][0] * localX);
                    particle->position.z = (sourceTransform_.rotation[2][2] * localZ +
                                            sourceTransform_.rotation[2][1] * localY +
                                            sourceTransform_.rotation[2][0] * localX);
                    cS3DVector3 normal{};
                    if (terrain_) terrain_->TerrainNormal(position.x, position.z, normal);
                    const float nx = sourceTransform_.rotation[0][2] * normal.z +
                                     sourceTransform_.rotation[0][1] * normal.y +
                                     sourceTransform_.rotation[0][0] * normal.x;
                    const float ny = sourceTransform_.rotation[1][2] * normal.z +
                                     sourceTransform_.rotation[1][1] * normal.y +
                                     sourceTransform_.rotation[1][0] * normal.x;
                    const float nz = sourceTransform_.rotation[2][2] * normal.z +
                                     sourceTransform_.rotation[2][1] * normal.y +
                                     sourceTransform_.rotation[2][0] * normal.x;
                    const float dot = (nz * particle->velocity.z + ny * particle->velocity.y +
                                       nx * particle->velocity.x) *
                                      (desc->terrainDrag + 1.0f);
                    particle->velocity.x -= nx * dot;
                    particle->velocity.y -= ny * dot;
                    particle->velocity.z -= nz * dot;
                }
            }
        }
    } else if (maps) {
        // PARTIAL: the vertical bounce plane of description flag 0x400 needs the effect map's height field; the
        // recovered part only keeps the particle alive (see the ledger).
        const float height = maps->Height(position.x, position.z);
        (void)height;
    }
    if (maps) maps->Release();
    return alive;
}

// 0x1005f8d84
// PARTIAL: the occupant enumeration itself lives in the unrecovered occupant manager (seam). The box the original
// builds is the particle position +-1 in x / y / z; a building occupant in it kills the particle.
void cSC4ParticlesEffect::TestBuildingCollision(cSC4EffectsParticle* particle) {
    if (!ParticlesOccupants()) return;
    const cS3DVector3 boxMin{particle->position.x - 1.0f, particle->position.y - 1.0f, particle->position.z - 1.0f};
    const cS3DVector3 boxMax{particle->position.x + 1.0f, particle->position.y + 1.0f, particle->position.z + 1.0f};
    if (ParticlesOccupants()->BuildingInBox(boxMin, boxMax)) particle->alive = 0;
}

// ---- next group -----------------------------------------------------------------------------------

// ---- rendering ---------------------------------------------------------------------------------------
// 0x1005f56f8
void cSC4ParticlesEffect::RenderActiveParticles(IParticlesDrawContext* context) {
    if (textureId_ == 0) return;
    box_ = EmptyBox();
    cS3DVector3 corner{}, right{}, up{};
    cS3DColorARGB8 colour{};
    pool_.RestartArrays();
    auto* vertices = static_cast<float*>(pool_.GetPrimVertices(static_cast<std::uint32_t>(liveCount_)));
    pool_.GetPrimIndices(static_cast<std::uint32_t>(liveCount_));
    const int last = static_cast<int>(description_->colorCurve.size()) - 1;
    for (cSCParticle* baseParticle : particles_) {
        ProcessParticle(EffectsParticle(baseParticle), last, corner, right, up, colour);
        if (colour.a != 0) {
            vertices[0] = corner.x + right.x;
            vertices[1] = corner.y + right.y;
            vertices[2] = corner.z + right.z;
            std::memcpy(&vertices[3], &colour, 4);
            vertices[6] = corner.x;
            vertices[7] = corner.y;
            vertices[8] = corner.z;
            std::memcpy(&vertices[9], &colour, 4);
            vertices[0xc] = corner.x + up.x;
            vertices[0xd] = corner.y + up.y;
            vertices[0xe] = corner.z + up.z;
            std::memcpy(&vertices[0xf], &colour, 4);
            vertices[0x12] = corner.x + up.x + right.x;
            vertices[0x13] = corner.y + up.y + right.y;
            vertices[0x14] = corner.z + up.z + right.z;
            std::memcpy(&vertices[0x15], &colour, 4);
            vertices += 0x18;
        }
    }
    if (pool_.VerticesUsed() != 0) {
        context->SetTexture(static_cast<int>(textureId_), 0);
        pool_.Draw(*context);
    }
    if (box_.min[0] <= box_.max[0]) {
        const float expand = CurveFirst(description_->sizeCurve);
        box_.min[0] -= expand;
        box_.min[1] -= expand;
        box_.min[2] -= expand;
        box_.max[0] += expand;
        box_.max[1] += expand;
        box_.max[2] += expand;
    }
}

// 0x1005f5976 / 0x1005f6ab2 (shared computation)
void cSC4ParticlesEffect::ProcessParticleGeometry(cSC4EffectsParticle* particle, int index, cS3DVector3& outCorner,
                                                  cS3DVector3& outRight, cS3DVector3& outUp, float& outRed,
                                                  float& outGreen, float& outBlue, float& outAlpha) {
    const cSC4ParticlesDescription* desc = description_;
    const float fraction = particle->age / particle->lifetime;
    // The colour curve is sampled with the curve's last index the caller computed once (param_2 in the original).
    cS3DVector3 colour{};
    if (index == 0) {
        if (!desc->colorCurve.empty()) colour = desc->colorCurve[0];
    } else {
        const float f = static_cast<float>(index) * fraction;
        int i = TruncateToInt(f);
        i = std::clamp(i, 0, index);
        const float frac = f - static_cast<float>(i);
        colour = desc->colorCurve[static_cast<std::size_t>(i)];
        if (frac > 0.0f) {
            const cS3DVector3& next = desc->colorCurve[static_cast<std::size_t>(std::min(i + 1, index))];
            colour.x += frac * (next.x - colour.x);
            colour.y += frac * (next.y - colour.y);
            colour.z += frac * (next.z - colour.z);
        }
    }
    outRed = colour.x * particle->colourR;
    outGreen = colour.y * particle->colourG;
    outBlue = colour.z * particle->colourB;
    const float alpha = particle->alphaBias;
    const float sizeCurve = CurveValue(desc->rateCurve, fraction);
    const float alphaCurve = CurveValue(desc->alphaCurve, fraction);
    const float sizeCurve2 = CurveValue(desc->sizeCurve, fraction);
    const float spinCurve = CurveValue(desc->spinCurve, fraction);
    const float rotation = particle->rotation * spinCurve + desc->spinBase;
    float x = particle->position.x;
    float y = particle->position.y;
    float z = particle->position.z;
    if (desc->spinAroundY != 0.0f || std::isnan(desc->spinAroundY)) {
        const float centreX = sourceTransform_.translation[0];
        const float centreZ = sourceTransform_.translation[2];
        const float dx = x - centreX;
        const float dz = z - centreZ;
        const float angle = desc->spinAroundY * (y - sourceTransform_.translation[1]);
        const float s = Sin(angle);
        const float c = Cos(angle);
        x = (dx * c - dz * s) + centreX;
        z = (dz * c + dx * s) + centreZ;
    }
    const std::vector<float>& wave = desc->waveRows;
    for (std::size_t i = 0; i + 6 < wave.size(); i += 7) {
        const float phase = wave[i] * static_cast<float>(static_cast<double>(desc->spawnPhase) * spawnAccumulator_);
        const float angle = phase + wave[i + 3] * z + wave[i + 2] * y + wave[i + 1] * x;
        const float s = Sin(angle);
        x += wave[i + 4] * s;
        y += wave[i + 5] * s;
        z += wave[i + 6] * s;
    }
    outRight = basisB_;
    outUp = basisA_;
    if (desc->maxWidth != 0.0f || std::isnan(desc->maxWidth)) {
        const float vx = particle->velocity.x;
        const float vy = particle->velocity.y;
        const float vz = particle->velocity.z;
        float dotB = basisB_.z * vz + basisB_.y * vy + basisB_.x * vx;
        float dotA = basisA_.z * vz + basisA_.y * vy + basisA_.x * vx;
        const float length = Sqrt(dotB * dotB + dotA * dotA);
        const float inv = 1.0f / length;   // the original divides without testing (length 0 gives inf/nan)
        dotB *= inv;
        dotA *= inv;
        const float rx = outRight.x;
        const float ry = outRight.y;
        const float rz = outRight.z;
        outRight.x = dotA * basisA_.x + rx * dotB;
        outRight.y = dotA * basisA_.y + ry * dotB;
        outRight.z = dotA * basisA_.z + rz * dotB;
        outUp.x = basisA_.x * dotB - rx * dotA;
        outUp.y = basisA_.y * dotB - ry * dotA;
        outUp.z = basisA_.z * dotB - rz * dotA;
        if (desc->maxWidth < length) {
            const float stretch = length / desc->maxWidth;
            outRight.x *= stretch;
            outRight.y *= stretch;
            outRight.z *= stretch;
        }
    }
    float clamp = globalParams_ ? globalParams_->maxAlpha : 1.0f;
    if (particle->size * sizeCurve2 <= clamp) clamp = particle->size * sizeCurve2;
    outRight.x *= clamp;
    outRight.y *= clamp;
    outRight.z *= clamp;
    const float upScale = particle->alpha * alphaCurve * clamp;
    outUp.x *= upScale;
    outUp.y *= upScale;
    outUp.z *= upScale;
    if (rotation != 0.0f || std::isnan(rotation)) {
        const float angle = rotation * kTwoPi;
        const float s = Sin(angle);
        const float c = Cos(angle);
        const float rx = outRight.x, ry = outRight.y, rz = outRight.z;
        const float ux = outUp.x, uy = outUp.y, uz = outUp.z;
        outUp.x = rx * s + ux * c;
        outUp.y = ry * s + uy * c;
        outUp.z = rz * s + uz * c;
        outRight.x = c * rx - ux * s;
        outRight.y = c * ry - uy * s;
        outRight.z = c * rz - uz * s;
    }
    float worldX = x;
    float worldY = y;
    float worldZ = z;
    if (sourceTransform_.flags != 0) {
        const float scale = sourceTransform_.scale;
        worldX = (sourceTransform_.rotation[0][2] * z + sourceTransform_.rotation[0][1] * y +
                  sourceTransform_.rotation[0][0] * x) * scale + sourceTransform_.translation[0];
        worldY = (sourceTransform_.rotation[1][2] * z + sourceTransform_.rotation[1][1] * y +
                  sourceTransform_.rotation[1][0] * x) * scale + sourceTransform_.translation[1];
        worldZ = (sourceTransform_.rotation[2][2] * z + sourceTransform_.rotation[2][1] * y +
                  sourceTransform_.rotation[2][0] * x) * scale + sourceTransform_.translation[2];
        outRight.x *= scale;
        outRight.y *= scale;
        outRight.z *= scale;
        outUp.x *= scale;
        outUp.y *= scale;
        outUp.z *= scale;
    }
    outCorner.x = worldX - (outUp.x + outRight.x) * 0.5f;
    outCorner.y = worldY - (outUp.y + outRight.y) * 0.5f;
    outCorner.z = worldZ - (outUp.z + outRight.z) * 0.5f;
    outAlpha = alpha * sizeCurve * attractorScale_;
    if (box_.min[0] <= box_.max[0]) {
        if (box_.max[0] <= worldX) box_.max[0] = worldX;
        if (worldX <= box_.min[0]) box_.min[0] = worldX;
        if (box_.max[1] <= worldY) box_.max[1] = worldY;
        if (worldY <= box_.min[1]) box_.min[1] = worldY;
        if (box_.max[2] <= worldZ) box_.max[2] = worldZ;
        if (worldZ <= box_.min[2]) box_.min[2] = worldZ;
    } else {
        box_.max[0] = worldX;
        box_.max[1] = worldY;
        box_.max[2] = worldZ;
        box_.min[0] = worldX;
        box_.min[1] = worldY;
        box_.min[2] = worldZ;
    }
}

// 0x1005f5976
void cSC4ParticlesEffect::ProcessParticle(cSC4EffectsParticle* particle, int index, cS3DVector3& outCorner,
                                          cS3DVector3& outRight, cS3DVector3& outUp, cS3DColorARGB8& outColor) {
    float red = 0.0f, green = 0.0f, blue = 0.0f, alpha = 0.0f;
    ProcessParticleGeometry(particle, index, outCorner, outRight, outUp, red, green, blue, alpha);
    outColor.r = PackedColorByte(red);
    outColor.g = PackedColorByte(green);
    outColor.b = PackedColorByte(blue);
    outColor.a = PackedColorByte(alpha);
}

// 0x1005f6ab2
void cSC4ParticlesEffect::ProcessParticle(cSC4EffectsParticle* particle, int index, cS3DVector3& outCorner,
                                          cS3DVector3& outRight, cS3DVector3& outUp, cS3DColorARGB32& outColor) {
    float red = 0.0f, green = 0.0f, blue = 0.0f, alpha = 0.0f;
    ProcessParticleGeometry(particle, index, outCorner, outRight, outUp, red, green, blue, alpha);
    outColor.r = red;
    outColor.g = green;
    outColor.b = blue;
    outColor.a = alpha;
}

// 0x1005f6548
// PARTIAL: streams through the unrecovered cIGZGDriverVertexBufferExtension (seam). The fast-ATI variant of the vertex
// writes is dead code in this build (MacOpenGL::IsFastATICard() always answers false), so only the ARGB8 path is written.
void cSC4ParticlesEffect::RenderActiveParticlesVB(IParticlesDrawContext* context) {
    if (textureId_ == 0) return;
    context->SetTexture(static_cast<int>(textureId_), 0);
    box_ = EmptyBox();
    const std::uint64_t flags = description_->flags;
    const bool flipV = (flags & 0x40000000) != 0;
    const bool flipH = (flags & 0x20000000) != 0;
    const float u0 = flipH ? 1.0f : 0.0f;
    const float v0 = flipV ? 1.0f : 0.0f;
    const float u1 = flipH ? 0.0f : 1.0f;
    const float v1 = flipV ? 0.0f : 1.0f;
    if (!vertexBuffer_) return;
    std::uint32_t remaining = static_cast<std::uint32_t>(liveCount_);
    const std::uint32_t capacity = vertexBuffer_->VertexCapacity() >> 2;
    if (capacity == 0) return;
    const int last = static_cast<int>(description_->colorCurve.size()) - 1;
    auto it = particles_.begin();
    cS3DVector3 corner{}, right{}, up{};
    cS3DColorARGB8 colour{};
    // CDirect3DVertexBuffer7::GetFVFSize(0x242) (0x10063af00): XYZ + diffuse + two 2-float texture-coordinate sets.
    constexpr int kVertexBytes242 = 0x20;
    while (static_cast<std::int32_t>(remaining) > 0) {
        const std::uint32_t batch = std::min<std::uint32_t>(remaining, capacity);
        pool_.RestartArrays();
        pool_.GetPrimIndices(batch);
        const std::uint32_t vertexCount = 4u * batch;
        auto* vertices = static_cast<float*>(vertexBuffer_->Lock(0, vertexCount));
        if (!vertices) return;
        for (std::uint32_t i = 0; i < batch && it != particles_.end(); ++i, ++it) {
            ProcessParticle(EffectsParticle(*it), last, corner, right, up, colour);
            vertices[0] = corner.x + right.x;
            vertices[1] = corner.y + right.y;
            vertices[2] = corner.z + right.z;
            std::memcpy(&vertices[3], &colour, 4);
            vertices[4] = u0;
            vertices[5] = v0;
            vertices[8] = corner.x;
            vertices[9] = corner.y;
            vertices[10] = corner.z;
            std::memcpy(&vertices[0xb], &colour, 4);
            vertices[0xc] = 0.0f;
            vertices[0xd] = v1;
            vertices[0x10] = corner.x + up.x;
            vertices[0x11] = corner.y + up.y;
            vertices[0x12] = corner.z + up.z;
            std::memcpy(&vertices[0x13], &colour, 4);
            vertices[0x14] = u1;
            vertices[0x15] = v1;
            vertices[0x18] = corner.x + up.x + right.x;
            vertices[0x19] = corner.y + up.y + right.y;
            vertices[0x1a] = corner.z + up.z + right.z;
            std::memcpy(&vertices[0x1b], &colour, 4);
            vertices[0x1c] = 1.0f;
            vertices[0x1d] = v0;
            vertices += 8;
        }
        vertexBuffer_->Unlock(0);
        vertexBuffer_->DrawIndexed(0, 0, pool_.IndicesUsed(), pool_.Indices(), vertices - 8u * batch,
                                   static_cast<int>(static_cast<std::uint32_t>(kVertexBytes242) * vertexCount));
        remaining -= batch;
    }
    if (box_.min[0] <= box_.max[0]) {
        const float expand = CurveFirst(description_->sizeCurve);
        box_.min[0] -= expand;
        box_.min[1] -= expand;
        box_.min[2] -= expand;
        box_.max[0] += expand;
        box_.max[1] += expand;
        box_.max[2] += expand;
    }
}

// ---- next group -----------------------------------------------------------------------------------

// 0x1005f9018
// PARTIAL: the trigger object comes from the unrecovered effects manager (seam) and the legacy transform the original
// builds inline (flags 0x1005f9074..0x1005f90a0) is passed as the effect's source transform; only its translation is
// recovered with confidence.
void cSC4ParticlesEffect::TriggerEffect(const char* suffix, const cS3DVector3& position) {
    static std::string name;
    name = description_->name;
    name += suffix;
    IParticlesEffectLookup* effect = nullptr;
    if (ParticlesEffects()) effect = ParticlesEffects()->FindTriggerEffect(name.c_str());
    if (effect) {
        cS3DTransform transform{};
        transform.translation[0] = position.x;
        transform.translation[1] = position.y;
        transform.translation[2] = position.z;
        effect->SetSourceTransform(transform);
        effect->Start(0);
        effect->Release();
    }
}

// 0x1005f91a4
void cSC4ParticlesEffect::SetPrimitivePoolFormat(cSC4PrimitivePool* pool) {
    cSC4PrimitivePool::VertexInit init = cSC4PrimitivePool::QuadVertexInit_V3F_C4UB_T2F;
    if ((description_->flags & 0x40000000) != 0) {
        init = cSC4PrimitivePool::VFlip_QuadVertexInit_V3F_C4UB_T2F;
    } else if ((description_->flags & 0x20000000) != 0) {
        init = cSC4PrimitivePool::HFlip_QuadVertexInit_V3F_C4UB_T2F;
    }
    pool->SetFormat(0, 4, 6, 10, init, cSC4PrimitivePool::QuadTriListIndexInit);
}

}  // namespace sc4
