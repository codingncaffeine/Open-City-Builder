// Visual-effect components: the six component descriptions an effects resource is made of (camera, attractor,
// sound, sequence, brush, scrubber; each is a small serializable record that instantiates its runtime effect)
// and the camera and sound effects they create. Description layouts are given per field.
//
//   type 3 brush      +0x14 u32 (7), +0x18 f (1), +0x1c f, +0x20 u8, +0x24 u32 (-1), +0x28/+0x30 vec2 (1,1), +0x38 f (100)
//   type 4 attractor  +0x18 name, +0x30 flag
//   type 5 scrubber   +0x18 u32, +0x20/+0x24 u32, +0x28 u32 (1), +0x2c/+0x30 f, +0x34/+0x38/+0x3c/+0x40/+0x44/+0x48 u32,
//                     +0x4c f (16), +0x50 vec2, +0x58/+0x5c f
//   type 6 sequence   +0x18 vector<{vec2 range, string name}>, +0x30 u32
//   type 7 sound      +0x18 u32, +0x20 sound id, +0x24 repeat interval f (0.5), +0x28 duration f
//   type 8 camera     +0x18 flags (1 zoom, 2 rotate, 4 move, 8 follow), +0x20 zoom u8, +0x21 rotation u8, +0x24 radius f
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"

namespace sc4 {

enum class EffectComponentType : std::uint32_t { Brush = 3, Attractor = 4, Scrubber = 5, Sequence = 6, Sound = 7, Camera = 8 };

constexpr std::uint32_t kMessageCameraMoved = 0xa6b79611;       // the user moved the camera
constexpr std::uint32_t kSoundEventPlay = 0xea451903;
constexpr std::uint32_t kSoundEventStop = 0x6a451908;
constexpr std::uint32_t kSoundEventMove = 0xca45190f;

class cISC4VisualEffectComponent {
public:
    virtual ~cISC4VisualEffectComponent() = default;
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
    virtual bool IsStarted() const = 0;
    virtual void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) = 0;
    virtual void ApplyEffect(float elapsed) = 0;
    virtual bool SetFloatParams(int type, const float* values, int count) = 0;
    virtual bool SetVoidParams(int type, void** values, int count) = 0;
    // The particle system behind a particles component (the port wraps the systems as components); null for the others.
    virtual const class cSC4ParticlesEffect* ParticleSystem() const { return nullptr; }
    void AddRef() { ++refs_; }
    void Release() { if (refs_ <= 1) delete this; else --refs_; }
protected:
    std::uint32_t refs_ = 0;
};

class cISC4ComponentDescription {
public:
    virtual ~cISC4ComponentDescription() = default;
    virtual std::uint32_t Type() const = 0;
    virtual cISC4VisualEffectComponent* Instantiate() = 0;
    virtual void Read(cIGZIStream& stream) = 0;
    virtual void Write(cIGZOStream& stream) const = 0;
    void AddRef() { ++refs_; }
    void Release() { if (refs_ <= 1) delete this; else --refs_; }
private:
    std::uint32_t refs_ = 0;
};

// Runtime effect classes that live elsewhere register their constructors here (cSC4AttractorEffect,
// cSC4SequenceEffect, cSC4BrushEffect, cSC4ScrubberEffect); instantiation returns null until they do.
using EffectComponentFactory = std::function<cISC4VisualEffectComponent*(cISC4ComponentDescription*)>;
void SetEffectComponentFactory(EffectComponentType type, EffectComponentFactory factory);

// ---- services -------------------------------------------------------------------------------------
class ICameraControl {
public:
    virtual ~ICameraControl() = default;
    virtual int ZoomLevel() = 0;                                // +0x110
    virtual void SetZoom(float zoom) = 0;
    virtual void SetRotation(std::uint32_t rotation) = 0;
    virtual void SetViewTargetPosition(const cS3DVector3& position) = 0;
};
class IEffectsHost {
public:
    virtual ~IEffectsHost() = default;
    virtual ICameraControl* Camera() = 0;                       // SL::spRenderer vslot 0x80
    virtual bool CameraEffectsEnabled() = 0;                    // render properties +0x978
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;      // SL::spMS2
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;
    virtual void PostSoundEvent(std::uint32_t event, std::uint32_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) = 0;   // SL::spSS vslot 0x50
    virtual bool HasSoundSystem() = 0;
};
IEffectsHost* EffectsHost();
void SetEffectsHost(IEffectsHost* host);

// ---- descriptions ---------------------------------------------------------------------------------
class cSC4CameraDescription final : public cISC4ComponentDescription {
public:
    std::uint32_t Type() const override { return 8; }
    cISC4VisualEffectComponent* Instantiate() override;
    void Read(cIGZIStream& stream) override;
    void Write(cIGZOStream& stream) const override;
    std::uint32_t flags = 0;       // +0x18
    std::uint8_t zoom = 0;         // +0x20
    std::uint8_t rotation = 0;     // +0x21
    float radius = 0.0f;           // +0x24
};

class cSC4AttractorDescription final : public cISC4ComponentDescription {
public:
    std::uint32_t Type() const override { return 4; }
    cISC4VisualEffectComponent* Instantiate() override;
    void Read(cIGZIStream& stream) override;
    void Write(cIGZOStream& stream) const override;
    std::string name;              // +0x18
    bool flag = false;             // +0x30
};

class cSC4SoundDescription final : public cISC4ComponentDescription {
public:
    std::uint32_t Type() const override { return 7; }
    cISC4VisualEffectComponent* Instantiate() override;
    void Read(cIGZIStream& stream) override;
    void Write(cIGZOStream& stream) const override;
    std::uint32_t flags = 0;       // +0x18
    std::uint32_t soundId = 0;     // +0x20
    float interval = 0.5f;         // +0x24
    float duration = 0.0f;         // +0x28
};

class cSC4SequenceDescription final : public cISC4ComponentDescription {
public:
    struct cItemRec {
        cS3DVector2 range;         // +0
        std::string name;          // +8
    };
    std::uint32_t Type() const override { return 6; }
    cISC4VisualEffectComponent* Instantiate() override;
    void Read(cIGZIStream& stream) override;
    void Write(cIGZOStream& stream) const override;
    std::vector<cItemRec> items;   // +0x18
    std::uint32_t value = 0;       // +0x30
};

class cSC4BrushDescription final : public cISC4ComponentDescription {
public:
    std::uint32_t Type() const override { return 3; }
    cISC4VisualEffectComponent* Instantiate() override;
    void Read(cIGZIStream& stream) override;
    void Write(cIGZOStream& stream) const override;
    std::uint32_t id = 7;          // +0x14
    float f18 = 1.0f, f1c = 0.0f;
    std::uint8_t flag = 0;         // +0x20
    std::uint32_t u24 = 0xffffffffu;
    cS3DVector2 v28{1.0f, 1.0f}, v30{1.0f, 1.0f};
    float f38 = 100.0f;
};

class cSC4ScrubberDescription final : public cISC4ComponentDescription {
public:
    std::uint32_t Type() const override { return 5; }
    cISC4VisualEffectComponent* Instantiate() override;
    void Read(cIGZIStream& stream) override;
    void Write(cIGZOStream& stream) const override;
    std::uint32_t u18 = 0, u20 = 0, u24 = 0, u28 = 1;
    float f2c = 0.0f, f30 = 0.0f;
    std::uint32_t u34 = 0, u38 = 0, u3c = 0, u40 = 0, u44 = 0, u48 = 0;
    float f4c = 16.0f;
    cS3DVector2 v50{};
    float f58 = 0.0f, f5c = 0.0f;
};

// ---- runtime effects ------------------------------------------------------------------------------
// Moves / zooms / rotates the camera when started and, when a follow flag or radius is set, keeps the camera on
// the effect while it moves (until the user takes the camera).
class cSC4CameraEffect final : public cISC4VisualEffectComponent, public cIGZMessageTarget2 {
public:
    explicit cSC4CameraEffect(cSC4CameraDescription* description);
    ~cSC4CameraEffect() override;
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { ++refs_; return refs_; }
    std::uint32_t Release() override { if (refs_ <= 1) { delete this; return 0; } return --refs_; }
    void Start(int transition) override;
    void Stop(int transition) override;
    bool IsStarted() const override { return started_; }
    void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) override;
    void ApplyEffect(float elapsed) override;
    bool SetFloatParams(int type, const float* values, int count) override;
    bool SetVoidParams(int, void**, int) override { return false; }
    bool DoMessage(cIGZMessage2* message) override;
    const cS3DVector3& Position() const { return position_; }
    bool Following() const { return follow_; }

private:
    cSC4CameraDescription* description_;   // +0x20 (reference held)
    cS3DVector3 position_;                 // +0x28
    bool started_ = false;                 // +0x34
    bool applying_ = false;                // +0x35
    bool follow_ = false;                  // +0x36
    bool registered_ = false;              // +0x37
};

// Plays a sound at the effect's position: a "play" event when started, "move" events every interval while it
// plays, and stops itself when the duration has passed.
class cSC4SoundEffect final : public cISC4VisualEffectComponent {
public:
    explicit cSC4SoundEffect(cSC4SoundDescription* description);
    ~cSC4SoundEffect() override;
    void Start(int transition) override;
    void Stop(int transition) override;
    bool IsStarted() const override { return started_; }
    void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) override;
    void ApplyEffect(float elapsed) override;
    bool SetFloatParams(int, const float*, int) override { return false; }
    bool SetVoidParams(int, void**, int) override { return false; }
    std::uint32_t InstanceId() const { return instanceId_; }

private:
    cSC4SoundDescription* description_;    // +0x18
    cS3DVector3 position_;                 // +0x20
    bool started_ = false;                 // +0x2c
    std::uint32_t instanceId_ = 0;         // +0x30
    float elapsed_ = 0.0f;                 // +0x34
    float next_ = 0.0f;                    // +0x38
    bool soundSystem_ = false;             // +0x40
};

// ---- brush effect ---------------------------------------------------------------------------------
// The terrain brush service of the effects (cISTETerrain vslots 0x1c8 / 0x2b8 / 0x2c0) and the city's cell test.
struct TerrainBrushParams {
    std::uint32_t u24 = 0;         // description +0x24
    std::int32_t last = 0;         // 1 on the final application of a frame
    float randomA = 0.0f, randomB = 0.0f;
    std::int32_t mode = 3;
    float amount = 0.0f;           // description +0x38
    cS3DVector2 from, to;          // previous / current position
};
class ITerrainBrush {
public:
    virtual ~ITerrainBrush() = default;
    virtual void BeginBrush() = 0;                                                    // vslot 0x2b8
    virtual void EndBrush() = 0;                                                      // vslot 0x2c0
    virtual void ApplyBrush(std::uint32_t brushId, const TerrainBrushParams& params) = 0;   // vslot 0x1c8
    virtual bool PositionInCity(float x, float z) = 0;                                // city vslot 0x310
    virtual double RandomRange(double low, double high) = 0;                          // cRZRandom mRandom
};
void SetTerrainBrush(ITerrainBrush* brush);
ITerrainBrush* TerrainBrush();

// Paints the terrain along its path: a one-shot description applies the brush once, otherwise brush strokes are
// applied at a steady rate (fractions carry over) until the description's duration has passed.
class cSC4BrushEffect final : public cISC4VisualEffectComponent {
public:
    cSC4BrushEffect() = default;
    ~cSC4BrushEffect() override;
    void SetDescription(cSC4BrushDescription* description);
    void Start(int transition) override;
    void Stop(int transition) override;
    bool IsStarted() const override { return started_; }
    void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) override;
    void ApplyEffect(float elapsed) override;
    bool SetFloatParams(int, const float*, int) override { return false; }
    bool SetVoidParams(int, void**, int) override { return false; }
    void ApplyBrush(int count);

private:
    cSC4BrushDescription* description_ = nullptr;   // +0x18
    cS3DVector2 position_, previous_;               // +0x28, +0x30
    bool started_ = false;                          // +0x38
    bool brushBegun_ = false;                       // +0x39
    float accumulator_ = 0.0f;                      // +0x3c
    float elapsed_ = 0.0f;                          // +0x40
    float randomA_ = 0.0f, randomB_ = 0.0f;         // +0x44, +0x48
};

// the translation of (parent * local): the local rotation / scale applied to the parent's translation
cS3DVector3 ComposeTranslation(const cS3DTransform& parent, const cS3DTransform& local);

}  // namespace sc4
