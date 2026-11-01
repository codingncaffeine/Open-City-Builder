// cSC4ParticlesEffect (61 symbols, 0x1005e87ac..0x1005f9e5c): one running particle-system effect. It is a visual-effect
// component (the first vtable group, 0x100ad4d30: Start, Stop, IsStarted, SetTransforms, ApplyEffect, SetFloatParams,
// SetVoidParams, AddRef, Release, Init, Shutdown, Update, SetDescription, SetModel, SetTarget, SetGlobalParams,
// PrepareForRendering, GenerateNewParticles, SetPrimitivePoolFormat, CreateParticle, DestroyParticle,
// RenderActiveParticles, RenderActiveParticlesVB, UpdateActiveParticles, UpdateParticlePhysics, dtor) and a particle
// simulator (the second group at +8, 0x100ad4e18: cSCParticleSimulatorBase with CreateParticle / DestroyParticle
// overridden).
//
// Layout (0x280 bytes). The first member lands at +0x3c because it reuses the tail padding of the second base, exactly
// like the original (whose packed render state is at +0x3c):
//   +0x00 component vtable                    +0x08 cSCParticleSimulatorBase (vtable; +0x10 initialised, +0x11 started,
//                                             +0x14 refcount, +0x18 delayed stop, +0x19 release when done,
//                                             +0x20 particle list, +0x30 its size, +0x38 live count)
//   +0x3c cSC4PackedRenderState               +0x44 reserved
//   +0x48 cSC4ParticlesDescription* (refcounted)
//   +0x50 cSC4PrimitivePool (0x68: vertices / indices of the current batch)
//   +0xb8 texture id (0 = none)               +0xc0 IParticlesTextureBinding* (refcounted)
//   +0xc8 / +0xd4 billboard basis A / B (vec3 each)   +0xe0 rotation the basis was built for
//   +0xe4 effect transform (cS3DTransform)    +0x11c source transform (cS3DTransform)
//   +0x154 velocity offset (vec3)             +0x160 frame dt
//   +0x168 elapsed time (double)              +0x170 first update after Start
//   +0x178 particle spawn accumulator (double) +0x180 particles spawned (u32)
//   +0x188 1 / description spawnPhase (double) +0x190 fractional particle accumulator
//   +0x194 world position (vec3)              +0x1a0 random-walk timer
//   +0x1a8 random-walk direction (vec3)       +0x1b4 city-side emission active
//   +0x1b8 random-walk basis / city side (vec3) +0x1c4 attractor force (vec3)
//   +0x1d0 attractor target (vec3)            +0x1dc attractor length
//   +0x1e0 start transition                   +0x1e4 stop transition   +0x1e8 spawn delay
//   +0x1ec attractor origin (vec3)            +0x1f8 bounding box (cS3DBoundingBox; empty = min 1,1,1 / max 0,0,0)
//   +0x210 reserved                           +0x21c emission rate bias, +0x220 scale bias
//   +0x224 velocity bias                      +0x228 attractor strength   +0x22c colour (vec3)
//   +0x238 rate scale                         +0x23c size scale
//   +0x240 cSC4GlobalEffectParams*            +0x248 IParticlesVertexBuffer*
//   +0x250 IParticlesTerrain* (refcounted)    +0x258 world width / +0x25c world depth
//   +0x260 water level                        +0x264 emission box (cS3DBoundingBox)
//   +0x27c terrain effect found               +0x27d water effect found      +0x27e emission suppressed
//
// Quirks kept (each asserted in the test):
//  * Start's "transition without a description" test uses description +0x24 (the particle cap) and flag bit 3.
//  * The unit-colour -> byte conversion adds 49152.0f, subtracts the float's bit pattern from 49152.0f's and folds the
//    high byte into the low one, so 1.0 becomes 255 and colours above 1 wrap (see PackedColorByte).
//  * ProcessParticle samples the description curves with the truncated index; a fraction of exactly 0 keeps the first
//    sample (the original never interpolates at t == 0).
//  * RenderActiveParticles expands the bounding box by sizeCurve[0] after drawing (the original reads the curve
//    unconditionally; the port guards an empty curve).
//  * The terrain / occupant / effects / texture / model / vertex-buffer / view neighbours are unrecovered; every reach
//    into them is a seam below, with the original vtable slot in a comment.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/StreamOps.h"
#include "render/PrimitivePool.h"
#include "rz/cRZRandom.h"
#include "sc4/PackedRenderState.h"
#include "sc4/ParticleSimulatorBase.h"

// ---------------------------------------------------------------------------------------------
// Data records
// ---------------------------------------------------------------------------------------------

// A particle of the effects system (0x44 bytes; CreateParticle allocates exactly that much). The first 0x1c bytes are the
// cSCParticle record the simulator base allocates and whose first byte is the alive flag; the recovered
// sc4::cSCParticle only spells out that flag, so this record is a stand-alone struct and the effect converts with
// EffectsParticle() / SimulatorParticle() (the base only ever reads the flag at +0).
struct cSC4EffectsParticle {
    std::uint8_t alive = 0;        // +0x00 (also cSCParticle::alive)
    std::uint8_t reserved01[3] = {0, 0, 0};
    float age = 0;                 // +0x04 seconds since emission
    float lifetime = 0;            // +0x08 dies at age >= lifetime
    cS3DVector3 position;          // +0x0c
    std::uint8_t reserved18 = 0;   // +0x18 cleared by InitEffectsParticle
    std::uint8_t reserved19[3] = {0, 0, 0};
    cS3DVector3 velocity;          // +0x1c
    float size = 0;                // +0x28 size multiplier
    float alpha = 0;               // +0x2c alpha multiplier
    float rotation = 0;            // +0x30 rotation in turns (plus description +0x80)
    float alphaBias = 0;           // +0x34 per-particle alpha bias
    float colourR = 0;             // +0x38 per-particle colour multipliers
    float colourG = 0;             // +0x3c
    float colourB = 0;             // +0x40
};

// A "timed effect" entry of the description: a name and the time (since start) at which it fires.
struct cSC4TimedEffectRecord {     // 0x20 bytes
    std::string name;              // +0x00
    float time = 0;                // +0x18
    std::uint32_t reserved1c = 0;
};

// A tractor-force key of the description (0x20 bytes). The description stores the *end* pointer of the array and the
// code walks it backwards by 0x20 until it finds a record whose +0x18 time is not after the particle's age.
struct cSC4TractorRecord {         // 0x20 bytes
    cS3DVector3 from;              // +0x00 local start point
    cS3DVector3 to;                // +0x0c local end point
    float time = 0;                // +0x18
    float end = 0;                 // +0x1c 0 = "use the particle lifetime"
};

// cSC4GlobalEffectParams (unrecovered; the manager hands one to every effect at +0x240). Only the fields the particle
// code reads are named; the rest is reserved.
struct cSC4GlobalEffectParams {
    float rateScale = 1.0f;        // +0x00 multiplier of the emission rate
    float sizeScale = 1.0f;        // +0x04 multiplier of the particle size
    cS3DVector3 velocity;          // +0x08 global wind
    std::uint8_t reserved14[0x30] = {};
    float maxAlpha = 1.0f;         // +0x44 clamp of the size * sizeCurve product
};

// cSC4ParticlesDescription — the data record cSC4ParticlesDescription reads/writes. The class itself (stream operators,
// RefCount, Instantiate) is not recovered; this is the layout the effect reads, with the original byte offsets. Unknown
// regions are reserved so that every named field sits at its original offset (asserted below).
struct cSC4ParticlesDescription {
    // +0x00 flag word. Bits used here (in the order the code tests them):
    //   0x00000001 self-lit (SetSelfLitShade around the draw)        0x00000002 emit by injection
    //   0x00000004 emit by maintaining the rate curve target         0x00000008 "start transition without rest"
    //   0x00000020 billboard / velocity compensation                 0x00000040 place the model at the emission box
    //   0x00000100 needs an effect-maps terrain query                0x00000200 collide with terrain
    //   0x00000400 collide with the terrain / die outside it         0x00000800 extra physics (see kExtraPhysicsFlags)
    //   0x00001000 align the transform (skip the view basis)         0x00002000 city-side emission volume
    //   0x00004000 height-based emission ("higher" side)             0x00008000 random walk
    //   0x00010000 height-based emission from the terrain height     0x00020000 land only
    //   0x00040000 water only                                        0x00080000..0x00200000 rate scale powers
    //   0x01000000 walk direction comes from the description         0x02000000 city-side walk direction
    //   0x08000000 attractor (extra physics)                         0x10000000 tractor force (extra physics)
    //   0x20000000 horizontal texture flip                           0x40000000 vertical texture flip
    //   0x80000000 launch the timed effects
    // The three words are std::bitset<32>, <8> and <11> (u32 each in the stream, one 8-byte word each here).
    std::uint64_t flags = 0;
    std::uint64_t flags8 = 0;                  // +0x08 bit 0: test the buildings each step
    std::uint8_t flag10 = 0;                   // +0x10 bit 0: keep the global size scale
    std::uint8_t reserved11 = 0;
    std::uint16_t flag10High = 0;              // +0x12 the other bits of the 11-bit word
    std::uint32_t reserved14 = 0;
    float lifetimeMin = 0;                     // +0x18
    float lifetimeMax = 0;                     // +0x1c
    float spawnPhase = 0;                      // +0x20 wave phase rate / spawn time scale
    std::uint32_t maxParticles = 0;            // +0x24
    float startDelay = 0;                      // +0x28 added to the first Update's dt
    float delayMinA = 0, delayMaxA = 0;        // +0x2c / +0x30 first-spawn delay range
    float delayMinB = 0, delayMaxB = 0;        // +0x34 / +0x38 extra delay range (used when +0x34 >= 0)
    float velocityMin[3] = {0, 0, 0};          // +0x3c / +0x40 / +0x44
    float velocityMax[3] = {0, 0, 0};          // +0x48 / +0x4c / +0x50
    float speedMin = 0, speedMax = 0;          // +0x54 / +0x58
    float emitMin[3] = {0, 0, 0};              // +0x5c / +0x60 / +0x64
    float emitMax[3] = {0, 0, 0};              // +0x68 / +0x6c / +0x70
    float sizeJitter = 0;                      // +0x74
    float alphaJitter = 0;                     // +0x78
    float spinRange = 0;                       // +0x7c
    float spinBase = 0;                        // +0x80
    float jitter84 = 0, jitter88 = 0;          // +0x84 / +0x88
    float jitter8c = 0, jitter90 = 0;          // +0x8c / +0x90
    std::uint32_t reserved94 = 0;              // +0x94
    std::vector<float> rateCurve;              // +0x98
    std::vector<cS3DVector3> colorCurve;       // +0xb0
    std::vector<float> curveC8;                // +0xc8 (a curve the particle code never reads)
    std::vector<float> sizeCurve;              // +0xe0
    std::vector<float> alphaCurve;             // +0xf8
    std::vector<float> spinCurve;              // +0x110
    std::uint32_t textureId = 0;               // +0x128 (the model id of a model-based particle system)
    std::uint8_t drawMode = 0;                 // +0x12c SetDrawMode argument
    std::uint8_t billboardMode = 0;            // +0x12d 1 = build the basis from the effect transform
    std::uint16_t reserved12e = 0;             // +0x12e
    float value130 = 0;                        // +0x130
    float maxWidth = 0;                        // +0x134 clamp of the billboard half width
    float origin[3] = {0, 0, 0};               // +0x138 / +0x13c / +0x140
    float globalOffsetScale = 0;               // +0x144 multiplies the global wind offset
    float terrainRadius = 0;                   // +0x148 push particles this far from the terrain centre
    float terrainCentre[3] = {0, 0, 0};        // +0x14c / +0x150 / +0x154
    float damping = 0;                         // +0x158
    float spinAroundY = 0;                     // +0x15c rotates particles around the effect's Y axis
    std::vector<float> waveRows;               // +0x160 the cWiggle records, 7 floats each: rate, two vectors
    float value178 = 0;                        // +0x178
    float range17c[2] = {0, 0};                // +0x17c
    float vector184[3] = {0, 0, 0};            // +0x184
    std::vector<float> curve190;               // +0x190
    float terrainDrag = 0;                     // +0x1a8
    float terrainBounce[7] = {0, 0, 0, 0, 0, 0, 0};   // +0x1ac..+0x1c4
    float heightMin = 0;                       // +0x1c8 height-based emission band (terrain height - water level)
    float heightMax = 0;                       // +0x1cc
    std::string name;                          // +0x1d0
    std::uint16_t value1e8 = 0;                // +0x1e8
    std::uint16_t value1ea = 0;                // +0x1ea
    float value1ec = 0;                        // +0x1ec
    float walkIntervalMin = 0, walkIntervalMax = 0;   // +0x1f0 / +0x1f4
    float walkSpeedMin = 0, walkSpeedMax = 0;         // +0x1f8 / +0x1fc
    float walkAngle = 0;                       // +0x200
    float walkAngle2 = 0;                      // +0x204
    float walkBias[3] = {0, 0, 0};             // +0x208 / +0x20c / +0x210
    float values214[3] = {0, 0, 0};            // +0x214
    std::vector<float> attractorCurve;         // +0x220
    float attractorTime = 0;                   // +0x238
    std::int32_t attractorMode = 0;            // +0x23c
    std::vector<cSC4TractorRecord> tractors;   // +0x240 (the original reads the end pointer at +0x248)
    float tractorRadius = 0;                   // +0x258
    std::uint32_t reserved25c = 0;             // +0x25c
    std::vector<cSC4TimedEffectRecord> timedEffects;   // +0x260
    float values278[2] = {0, 0};               // +0x278
    std::vector<std::uint32_t> modelIds;       // +0x280 model particles pick one of these at random (else textureId)
    float values298[2] = {0, 0};               // +0x298
    std::vector<float> curve2a0;               // +0x2a0
    float value2b8 = 0;                        // +0x2b8
    std::uint32_t refCount = 0;                // +0x2bc (the original's cRZAutoRefCount counter)
};

// A colour as the fast vertex path stores it: four floats (R, G, B, A). cS3DColorARGB32 is not recovered separately;
// the arrays are plain 0..1 floats.
struct cS3DColorARGB32 {
    float r = 0, g = 0, b = 0, a = 0;
};

// The packed colour ProcessParticle writes: four bytes in R, G, B, A order (the vertex code copies them as one dword).
struct cS3DColorARGB8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};

// The component half of the effect (original vtable 0x100ad4d30). cISC4VisualEffectComponent in EffectComponents.h
// carries a refs_ member that would move the simulator base from +8 to +0x10, so this interface mirrors the vtable
// (and is data free) instead. Slots 7 / 8 are AddRef / Release, exactly like the original.
class IParticlesEffectComponent {
public:
    virtual void Start(int transition) = 0;                                        // [0] 0x1005f2c12
    virtual void Stop(int transition) = 0;                                         // [1] 0x1005f33ac
    virtual bool IsStarted() const = 0;                                            // [2] 0x1005f9dc0
    virtual void SetTransforms(const cS3DTransform& effect, const cS3DTransform& source) = 0;   // [3] 0x1005f33cc
    virtual void ApplyEffect(float elapsed) = 0;                                   // [4] 0x1005f3732
    virtual bool SetFloatParams(int type, const float* values, int count) = 0;     // [5] 0x1005f3738
    virtual bool SetVoidParams(int type, void** values, int count) = 0;            // [6] 0x1005f38da
    virtual std::uint32_t AddRef() = 0;                                            // [7] 0x1005ecaa0
    virtual std::uint32_t Release() = 0;                                           // [8] 0x1005f9ddc

protected:
    ~IParticlesEffectComponent() = default;   // non-virtual, like cIGZUnknown: the concrete class declares the dtor
};

// ---------------------------------------------------------------------------------------------
// Seams (unrecovered neighbours; each method mirrors one original call)
// ---------------------------------------------------------------------------------------------

// SL::spTerrainMap (cISTETerrain). Slots from the calls in Init / Update / DoHeightBasedEmit / ApplyTerrainForces.
class IParticlesTerrain {
public:
    virtual ~IParticlesTerrain() = default;
    virtual void AddRef() = 0;                                              // +0x08
    virtual void Release() = 0;                                             // +0x10
    virtual float CellSize() const = 0;                                     // +0x30
    virtual int CellCountX() const = 0;                                     // +0x38
    virtual int CellCountZ() const = 0;                                     // +0x40
    virtual float TerrainHeight(float x, float z) = 0;                      // +0x60
    virtual float WaterHeight(float x, float z) = 0;                        // +0x98
    virtual void TerrainNormal(float x, float z, cS3DVector3& out) = 0;     // +0xd8
    virtual float WaterLevel() const = 0;                                   // +0xe8 (no position argument in Update)
};

// The city's cell map: SL::spCity vslot 0x2f8 (world -> cell) and the object city +0x260 returns, vslot 0x80 (cell
// flags: bit 0 water, bit 1..3 the side the cell touches, 0x80 = neither land nor water).
class IParticlesCityCells {
public:
    virtual ~IParticlesCityCells() = default;
    virtual unsigned CellFlags(int cellX, int cellZ) = 0;                   // +0x80
};
class IParticlesCity {
public:
    virtual ~IParticlesCity() = default;
    virtual void WorldToCell(float x, float z, int& cellX, int& cellZ) = 0;   // +0x2f8
    virtual IParticlesCityCells* Cells() = 0;                               // +0x260
};

// SL::spOccupantManager: the box query of TestBuildingCollision (vslots 0x128 enumerate / 0x180 release) and the ground
// query UpdateAttractor makes through city +0x178 -> +0xb0.
class IParticlesOccupants {
public:
    virtual ~IParticlesOccupants() = default;
    // True when a building occupant overlaps the box (the original walks the occupants and tests two type ids).
    virtual bool BuildingInBox(const cS3DVector3& boxMin, const cS3DVector3& boxMax) = 0;   // +0x128
    // The attractor's ground point for a position and a description attractor mode (city +0x178 -> +0xb0).
    virtual bool AttractorGroundPoint(const cS3DVector3& from, std::int32_t mode,
                                      cS3DVector3& out, float& length) = 0;
};

// The effect object the effects manager hands back by name (SetDescription / TriggerEffect).
class IParticlesEffectLookup {
public:
    virtual ~IParticlesEffectLookup() = default;
    virtual void Release() = 0;                                             // +0x10
    virtual void Start(int transition) = 0;                                 // +0x18
    virtual void SetSourceTransform(const cS3DTransform& transform) = 0;     // +0x40
};

// cISC4EffectMaps (GZCOM interface 0x0a5eaa1a): the effect-map queries ApplyTerrainForces makes.
class IParticlesEffectMaps {
public:
    virtual ~IParticlesEffectMaps() = default;
    virtual void Release() = 0;                                             // +0x10
    virtual float Height(float x, float z) = 0;                             // +0x30
    virtual void Normal(float x, float z, cS3DVector3& out) = 0;            // +0x40
};

// SL::spEffectsManager: name lookup (vslot 0x28) and the trigger lookup TriggerEffect uses (vslot 0x30, success in a
// reference) plus the GZCOM query for cISC4EffectMaps.
class IParticlesEffects {
public:
    virtual ~IParticlesEffects() = default;
    virtual IParticlesEffectLookup* FindEffect(const char* name) = 0;        // +0x28
    virtual IParticlesEffectLookup* FindTriggerEffect(const char* name) = 0; // +0x30 (null = not found)
    virtual IParticlesEffectMaps* EffectMaps() = 0;                          // cISC4EffectMaps (iid 0x0a5eaa1a)
};

// A texture binding (cS3DTextureBinding) and the factory SetTexture asks the RZ framework for (iid 0x1ac0e11a, group
// 0xfac0e219, factory vslot 0x28).
class IParticlesTextureBinding {
public:
    virtual ~IParticlesTextureBinding() = default;
    virtual void Release() = 0;                                             // 0x1005b5920
    virtual std::uint32_t TextureId() const = 0;                            // the binding's first field
};
class IParticlesTextures {
public:
    virtual ~IParticlesTextures() = default;
    virtual IParticlesTextureBinding* Create(std::uint32_t textureId) = 0;   // +0x28
};

// cIS3DModelInstance::SetPosition (vslot 0x38; the original hands it the emission box minimum).
class IParticlesModelInstance {
public:
    virtual ~IParticlesModelInstance() = default;
    virtual void SetPosition(const cS3DVector3& position) = 0;               // +0x38
};

// The cIGZGDriverVertexBufferExtension RenderActiveParticlesVB streams through.
class IParticlesVertexBuffer {
public:
    virtual ~IParticlesVertexBuffer() = default;
    virtual void AddRef() = 0;                                              // +0x08
    virtual void Release() = 0;                                             // +0x10
    virtual std::uint32_t VertexCapacity() = 0;                             // +0x28 (stream 0)
    virtual void* Lock(std::uint32_t stream, std::uint32_t count) = 0;      // +0x30
    virtual void Unlock(std::uint32_t stream) = 0;                          // +0x40
    virtual void DrawIndexed(std::uint32_t stream, std::uint32_t base, std::uint32_t indexCount,
                             const std::uint16_t* indices, const void* vertices, int vertexBytes) = 0;   // +0x50
};

// What the effect needs from SC4DrawContext (recovered in src/sc4/DrawContext.h). The three IPrimitiveDrawTarget methods
// are the ones cSC4PrimitivePool::Draw already needs; the rest are the calls Update makes, plus the two fields
// SC4DrawContext does not expose yet: its rotation (+0x18) and the SC4ViewUtilities billboard basis (draw context +0x28
// -> SC4ViewUtilities::right_ +0x68 and up_ +0x74; src/sc4/ViewUtilities.h). The game-side adapter is a one-line
// forwarder per method below.
class IParticlesDrawContext : public IPrimitiveDrawTarget {
public:
    virtual ~IParticlesDrawContext() = default;
    virtual void SetTexture(int texture, int stage) = 0;                    // @ 0x100772d0e
    virtual void SetRenderState(const cSC4PackedRenderState& state) = 0;    // @ 0x100773906
    virtual void SetSelfLitShade(const cS3DVector3& shade) = 0;             // @ 0x1007735d8
    virtual void ResetShade() = 0;                                          // @ 0x1007736be
    virtual void ResetModelViewTransform() = 0;                             // @ 0x1007726ee
    virtual int Rotation() const = 0;                                       // +0x18
    // SC4ViewUtilities: the vec3 at +0x68 and the one at +0x74 (whose z component is the float at +0x7c).
    virtual void GetBillboardBasis(cS3DVector3& basisA, cS3DVector3& basisB) const = 0;
};

// The hosts. The game installs the adapters; the test installs its fakes.
IParticlesTerrain* ParticlesTerrain();
void SetParticlesTerrain(IParticlesTerrain* terrain);                     // Init takes a reference
IParticlesCity* ParticlesCity();
void SetParticlesCity(IParticlesCity* city);
IParticlesOccupants* ParticlesOccupants();
void SetParticlesOccupants(IParticlesOccupants* occupants);
IParticlesEffects* ParticlesEffects();
void SetParticlesEffects(IParticlesEffects* effects);
IParticlesTextures* ParticlesTextures();
void SetParticlesTextures(IParticlesTextures* textures);

// cSC4ParticlesDescription is refcounted in the original (SetDescription keeps a reference, Shutdown / the destructor drop
// it). The class itself is not recovered, so the host supplies the two bookkeeping halves; both default to no-ops.
void SetParticlesDescriptionLifecycle(void (*retain)(cSC4ParticlesDescription*),
                                      void (*release)(cSC4ParticlesDescription*));

// ---------------------------------------------------------------------------------------------
// The effect
// ---------------------------------------------------------------------------------------------
namespace sc4 {

class cSC4ParticlesEffect : public IParticlesEffectComponent, public cSCParticleSimulatorBase {
public:
    // The shared generator (cSC4ParticlesEffect::mRandom, seeded from the clock at start-up = 0x1005fa31a..0x1005fa423).
    // Tests seed it to stay deterministic.
    static cRZRandom& Random();

    cSC4ParticlesEffect();                                               // @ 0x1005f27e2
    cSC4ParticlesEffect(const cSC4ParticlesEffect&);                     // @ 0x1005f2a9c (C2 alias)
    ~cSC4ParticlesEffect() override;                                     // @ 0x1005e87ac

    using cSCParticleSimulatorBase::Start;    // the simulator's Start()/Stop(bool,bool) live in the second vtable
    using cSCParticleSimulatorBase::Stop;
    void Start(int transition) override;                                 // @ 0x1005f2c12
    void Stop(int transition) override;                                  // @ 0x1005f33ac
    bool IsStarted() const override;                                     // @ 0x1005f9dc0
    void SetTransforms(const cS3DTransform& effect, const cS3DTransform& source) override;   // @ 0x1005f33cc
    void ApplyEffect(float) override {}                          // @ 0x1005f3732 (empty in the original)
    bool SetFloatParams(int type, const float* values, int count) override;   // @ 0x1005f3738
    bool SetVoidParams(int type, void** values, int count) override;          // @ 0x1005f38da
    std::uint32_t AddRef() override;                                     // @ 0x1005ecaa0
    std::uint32_t Release() override;                                    // @ 0x1005f9ddc
    bool Init() override;                                                // @ 0x1005f2aa6
    bool Shutdown() override;                                            // @ 0x1005f2b94

    bool Update(float elapsed, IParticlesDrawContext* context);          // @ 0x1005f38f4
    void SetDescription(cSC4ParticlesDescription* description);          // @ 0x1005f4226
    void SetModel(IParticlesModelInstance* model);                       // @ 0x1005f48da
    void SetTarget(const cS3DVector3& target);                           // @ 0x1005f4922
    void SetGlobalParams(const cSC4GlobalEffectParams* params);          // @ 0x1005f4c9e
    void SuppressEmission();                                             // @ 0x1005f4cac
    void SetVertexBufferExtension(IParticlesVertexBuffer* extension);    // @ 0x1005f4cba
    cS3DBoundingBox BoundingBox() const;                                 // @ 0x1005f4d0e
    void PrepareForRendering(IParticlesDrawContext* context);            // @ 0x1005f4d34
    void GenerateNewParticles();                                         // @ 0x1005f4ea0
    void SetPrimitivePoolFormat(cSC4PrimitivePool* pool);                // @ 0x1005f91a4
    cSCParticle* CreateParticle() override;                              // @ 0x1005f9224
    void DestroyParticle(cSCParticle* particle) override;                // @ 0x1005f9d98

    virtual void RenderActiveParticles(IParticlesDrawContext* context);       // @ 0x1005f56f8
    virtual void RenderActiveParticlesVB(IParticlesDrawContext* context);    // @ 0x1005f6548
    virtual void UpdateActiveParticles();                                // @ 0x1005f7630
    virtual bool UpdateParticlePhysics(cSC4EffectsParticle* particle, float elapsed);   // @ 0x1005f78ba

    // Deterministic helpers, public so the test can drive them directly (they are all part of the binary class).
    void UpdateRandomWalk(float elapsed);                                // @ 0x1005f3060
    void UpdateAttractor();                                              // @ 0x1005f3d08
    void LaunchTimedEffects();                                           // @ 0x1005f413c
    void SetCitySideEmissionVolume(int volume);                          // @ 0x1005f3c10
    void SetDrawMode(std::uint32_t mode);                                // @ 0x1005f4602
    void SetTexture(std::uint32_t texture);                              // @ 0x1005f4808
    void MaintainParticles(float elapsed);                               // @ 0x1005f4f2c
    void InjectParticles(float elapsed);                                 // @ 0x1005f5000
    void AddRateCurveParticles(float elapsed);                           // @ 0x1005f5288
    void LaunchTimeZeroEffects(cSCParticle* particle);                   // @ 0x1005f564e
    void ProcessParticle(cSC4EffectsParticle* particle, int index, cS3DVector3& outCorner,
                         cS3DVector3& outRight, cS3DVector3& outUp, cS3DColorARGB8& outColor);   // @ 0x1005f5976
    void ProcessParticle(cSC4EffectsParticle* particle, int index, cS3DVector3& outCorner,
                         cS3DVector3& outRight, cS3DVector3& outUp, cS3DColorARGB32& outColor);  // @ 0x1005f6ab2
    bool UpdateParticlePhysicsExtra(cSC4EffectsParticle* particle, float elapsed);   // @ 0x1005f79bc
    void ApplyTractorForce(cSC4EffectsParticle* particle, float elapsed);            // @ 0x1005f7de6
    bool ApplyTerrainForces(cSC4EffectsParticle* particle, const cS3DVector3& position, float elapsed);   // @ 0x1005f83da
    void TestBuildingCollision(cSC4EffectsParticle* particle);                       // @ 0x1005f8d84
    void TriggerEffect(const char* suffix, const cS3DVector3& position);             // @ 0x1005f9018
    void InitEffectsParticle(cSC4EffectsParticle* particle);                         // @ 0x1005f9288
    bool DoHeightBasedEmit(cSC4EffectsParticle* particle);                           // @ 0x1005f9ac2

    // Inspection (the original reads these fields directly).
    const cSC4ParticlesDescription* Description() const { return description_; }
    const cS3DVector3& Position() const { return position_; }
    const cS3DVector3& WalkDirection() const { return walkDirection_; }
    const cS3DVector3& AttractorForce() const { return attractorForce_; }
    const cS3DBoundingBox& Box() const { return box_; }
    std::uint32_t SpawnCount() const { return spawnCounter_; }
    double SpawnAccumulator() const { return spawnAccumulator_; }
    double Elapsed() const { return timeAccumulator_; }
    const cS3DTransform& EffectTransform() const { return effectTransform_; }
    const cS3DTransform& SourceTransform() const { return sourceTransform_; }
    std::uint32_t TextureId() const { return textureId_; }
    float WorldWidth() const { return worldWidth_; }
    float WorldDepth() const { return worldDepth_; }
    float WaterLevel() const { return waterLevel_; }
    float SpawnDelay() const { return spawnDelay_; }
    float SpawnFraction() const { return spawnFraction_; }
    std::uint32_t StartTransition() const { return static_cast<std::uint32_t>(startTransition_); }
    std::uint32_t StopTransition() const { return static_cast<std::uint32_t>(stopTransition_); }
    bool Suppressed() const { return suppressEmission_; }
    bool HasTerrainEffect() const { return hasTerrainEffect_; }
    bool HasWaterEffect() const { return hasWaterEffect_; }

    // The two views of a particle record (see cSC4EffectsParticle).
    static cSC4EffectsParticle* EffectsParticle(cSCParticle* particle) {
        return reinterpret_cast<cSC4EffectsParticle*>(particle);
    }
    static const cSC4EffectsParticle* EffectsParticle(const cSCParticle* particle) {
        return reinterpret_cast<const cSC4EffectsParticle*>(particle);
    }
    static cSCParticle* SimulatorParticle(cSC4EffectsParticle* particle) {
        return reinterpret_cast<cSCParticle*>(particle);
    }

private:
    // The shared geometry / colour computation of the two ProcessParticle overloads; the only difference between them is
    // how the colour is written (4 bytes vs 4 floats).
    void ProcessParticleGeometry(cSC4EffectsParticle* particle, int index, cS3DVector3& outCorner,
                                 cS3DVector3& outRight, cS3DVector3& outUp,
                                 float& outRed, float& outGreen, float& outBlue, float& outAlpha);
    std::list<cSCParticle*>::iterator RemoveDeadParticle(std::list<cSCParticle*>::iterator it,
                                                          cSC4EffectsParticle* particle);

    cSC4PackedRenderState renderState_;                  // +0x3c
    [[maybe_unused]] std::uint32_t reserved44_ = 0;                       // +0x44
    cSC4ParticlesDescription* description_ = nullptr;    // +0x48
    cSC4PrimitivePool pool_;                             // +0x50
    std::uint32_t textureId_ = 0;                        // +0xb8
    [[maybe_unused]] std::uint32_t reservedBc_ = 0;                       // +0xbc
    IParticlesTextureBinding* texture_ = nullptr;        // +0xc0
    cS3DVector3 basisA_;                                 // +0xc8
    cS3DVector3 basisB_;                                 // +0xd4
    std::int32_t basisRotation_ = 0;                     // +0xe0
    cS3DTransform effectTransform_;                      // +0xe4
    cS3DTransform sourceTransform_;                      // +0x11c
    cS3DVector3 velocityOffset_;                         // +0x154
    float deltaTime_ = 0;                                // +0x160
    [[maybe_unused]] std::uint32_t reserved164_ = 0;                      // +0x164
    double timeAccumulator_ = 0;                         // +0x168
    bool firstUpdate_ = false;                           // +0x170
    [[maybe_unused]] std::uint8_t reserved171_[7] = {};                   // +0x171
    double spawnAccumulator_ = 0;                        // +0x178
    std::uint32_t spawnCounter_ = 0;                     // +0x180
    [[maybe_unused]] std::uint32_t reserved184_ = 0;                      // +0x184
    double inverseRate_ = -1.0;                          // +0x188
    float spawnFraction_ = 0;                            // +0x190
    cS3DVector3 position_;                               // +0x194
    float walkTimer_ = 0;                                // +0x1a0
    [[maybe_unused]] std::uint32_t reserved1a4_ = 0;                      // +0x1a4
    cS3DVector3 walkDirection_;                          // +0x1a8
    bool citySideEmission_ = false;                      // +0x1b4
    [[maybe_unused]] std::uint8_t reserved1b5_[3] = {};                   // +0x1b5
    cS3DVector3 walkBasis_;                              // +0x1b8
    cS3DVector3 attractorForce_;                         // +0x1c4
    cS3DVector3 attractorTarget_;                        // +0x1d0
    float attractorLength_ = 0;                          // +0x1dc
    std::int32_t startTransition_ = 1;                   // +0x1e0
    std::int32_t stopTransition_ = 1;                    // +0x1e4
    float spawnDelay_ = 0;                               // +0x1e8
    cS3DVector3 attractorOrigin_;                        // +0x1ec
    cS3DBoundingBox box_{};                              // +0x1f8
    [[maybe_unused]] std::uint32_t reserved210_ = 0;                      // +0x210
    [[maybe_unused]] std::uint32_t reserved214_ = 0;                      // +0x214
    [[maybe_unused]] std::uint32_t reserved218_ = 0;                      // +0x218
    float rateBias_ = 1.0f;                              // +0x21c
    float scaleBias_ = 1.0f;                             // +0x220
    float velocityBias_ = 1.0f;                          // +0x224
    float attractorScale_ = 1.0f;                        // +0x228
    cS3DVector3 colour_{1.0f, 1.0f, 1.0f};               // +0x22c
    float rateScale_ = 1.0f;                             // +0x238
    float sizeScale_ = 1.0f;                             // +0x23c
    const cSC4GlobalEffectParams* globalParams_ = nullptr;   // +0x240
    IParticlesVertexBuffer* vertexBuffer_ = nullptr;     // +0x248
    IParticlesTerrain* terrain_ = nullptr;               // +0x250
    float worldWidth_ = 0;                               // +0x258
    float worldDepth_ = 0;                               // +0x25c
    float waterLevel_ = 0;                               // +0x260
    cS3DBoundingBox emissionBox_{};                      // +0x264
    bool hasTerrainEffect_ = false;                      // +0x27c
    bool hasWaterEffect_ = false;                        // +0x27d
    bool suppressEmission_ = false;                      // +0x27e
    [[maybe_unused]] std::uint8_t reserved27f_ = 0;                       // +0x27f

    // Effect transforms are touched a lot through the original's change counter; kept where the original keeps them.
    static constexpr std::uint64_t kExtraPhysicsFlags = 0x18000d00;
    static constexpr std::uint64_t kSpecialEmit = 0x7c000;
    static constexpr std::uint64_t kFlagsLakeSea = 0x60000;
};

static_assert(sizeof(cSC4ParticlesDescription) == 0x2c0, "description layout must match the original record");
static_assert(sizeof(cSC4TimedEffectRecord) == 0x20, "timed effect records are 0x20 bytes");
static_assert(sizeof(cSC4TractorRecord) == 0x20, "tractor records are 0x20 bytes");
static_assert(sizeof(cSC4EffectsParticle) == 0x44, "particles are 0x44 bytes (sEffectsParticleAllocator)");
static_assert(sizeof(sc4::cSC4ParticlesEffect) == 0x280, "effect layout must match the original object");

}  // namespace sc4

using sc4::cSC4ParticlesEffect;
