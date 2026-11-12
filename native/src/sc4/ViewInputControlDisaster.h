// The two cursor tools the disaster menu hands out (cSC4View3DWin::InvokeDisaster, Mac i386 0x00521612):
//   cSC4ViewInputControlTorch (0x002b6fa8..0x002b771a, 0x74 bytes): the fire torch. While the left button is down the cell under
//     the cursor is set alight, again on every animation tick; a small effect follows the cursor ("torch_inactive", "torch_active"
//     while burning).
//   cSC4ViewInputControlEffectDisaster (0x002d3d60..0x002d410a, 0xa8 bytes): an effect tool (the cursor carries a visual effect)
//     that starts a disaster. Without a manager of its own, starting the terrain-controlled effect makes an effect disaster instance
//     of it (meteor, lightning, tornado ...); with one (volcano, earthquake) releasing the button asks that manager to start a
//     disaster at the picked point.
#pragma once
#include <cstdint>

#include "gz/cIGZMessage2.h"
#include "rz/cRZTimer.h"
#include "sc4/ViewInputControl.h"
#include "sc4/ViewInputControlEffect.h"

namespace sc4 {

constexpr std::uint32_t kDisasterTorchId = 0xa9de8784;              // the menu item that is the torch (every other is an exemplar)
constexpr std::uint32_t kTorchCursor = 0xe9c24ac4, kTorchCursorBurning = 0x0a08c54e, kDisasterToolId = 0x4a08c57e;
constexpr std::uint32_t kEffectDisasterCursor = 0x6a40222a;
constexpr std::uint32_t kTorchTickInterval = 0xc0;
constexpr std::uint32_t kTorchAudioInit = 0x2a106113, kTorchAudioShutdown = 0x8a10611a, kTorchAudioDown = 0x8a10611d,
                        kTorchAudioUp = 0x0a106122, kTorchAudioMove = 0x4a106126, kTorchAudioActive = 0xaa96fe8f;
constexpr std::uint32_t kEffectDisasterManagerDefault = 0x6a7b4397;   // the effect disaster manager
// A disaster's exemplar {0x6534284a, 0x29b6c670, id}: the effect's name, or its effect group, and the manager that runs it.
constexpr std::uint32_t kDisasterPropertyEffectName = 0x4a08c700, kDisasterPropertyManager = 0x4a08c701,
                        kDisasterPropertyEffectGroup = 0x4a08c702;

// What the two tools ask the city.
class IDisasterToolHost {
public:
    virtual ~IDisasterToolHost() = default;
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;                    // SL::spCity slot 0x17c
    // the fire disaster manager (the disaster layer's manager 0x621cda33, asked for 0x49b43f57): its IgniteCell and SetCause
    virtual bool IgniteCell(int cellX, int cellZ, bool again, bool force, std::uint32_t value) = 0;
    virtual void SetFireCause(std::uint32_t cause) = 0;
    virtual void StartTick(cIGZMessageTarget2* target, std::uint32_t interval) = 0;              // SL::ATM() slot 0x14
    virtual void StopTick(cIGZMessageTarget2* target, std::uint32_t interval) = 0;               // SL::ATM() slot 0x18
    // SL::spSS: a sound message of `type` (class 0x5b523ba4) with three data words, sent at once
    virtual void SendToolAudio(std::uint32_t type, std::uint32_t a, std::uint32_t b, std::uint32_t c) = 0;
    virtual bool HasDisasterManager(std::uint32_t id) = 0;                                        // the disaster layer's slot 0x1c
    virtual void AttemptDisasterStart(std::uint32_t id, int x, int z) = 0;                        // that manager's slot 0x1c
    // the manager asked for 0xca7b3f3c, its slot 0xc StartDisasterEffect(effect, position, id): the instance, counted; null
    // when the manager does not run effects
    virtual cIGZUnknown* StartDisasterEffect(std::uint32_t managerId, IToolVisualEffect* effect, const cS3DVector3& position,
                                             std::uint32_t id) = 0;
    virtual void StopDisasterEffect(cIGZUnknown* instance) = 0;                                   // the instance's slot 0x20
};
IDisasterToolHost* DisasterToolHost();
void SetDisasterToolHost(IDisasterToolHost* host);

class cSC4ViewInputControlTorch final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    cSC4ViewInputControlTorch() = default;                                 // @ 0x002b7116
    bool QueryInterface(std::uint32_t iid, void** out) override { return cSC4ViewInputControl::QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return cSC4ViewInputControl::AddRef(); }
    std::uint32_t Release() override { return cSC4ViewInputControl::Release(); }

    bool Init() override;                                                  // @ 0x002b771a
    bool Shutdown() override;                                              // @ 0x002b7684
    bool OnKeyDown(std::uint32_t key, std::uint32_t mods) override { return !IsKeyToBePassed(key, mods); }   // @ 0x002b6fcc
    bool OnMouseDownL(int x, int y, std::uint32_t mods) override;          // @ 0x002b7554
    bool OnMouseUpL(int x, int y, std::uint32_t mods) override;            // @ 0x002b74d0
    bool OnMouseMove(int x, int y, std::uint32_t mods) override;           // @ 0x002b73ea
    bool OnMouseExit() override;                                           // @ 0x002b72da
    bool DoMessage(cIGZMessage2* message) override;                        // @ 0x002b7042

    bool IsBurning() const { return active_; }
    bool HasEffect() const { return effect_ != nullptr; }
    int CellX() const { return cellX_; }
    int CellZ() const { return cellZ_; }

private:
    void InitTorchEffect();                                                // @ 0x002b6ff2
    void UpdateTorchEffect();                                              // @ 0x002b70be
    void ShutdownTorchEffect();                                            // @ 0x002b7266
    void ActivateTorch(bool on);                                           // @ 0x002b72f2
    void SendAudio(std::uint32_t type, std::uint32_t a, std::uint32_t b, std::uint32_t c);   // @ 0x002b71ae

    IToolVisualEffect* effect_ = nullptr;      // +0x30
    cS3DVector3 pos_{0.0f, 0.0f, 0.0f};        // +0x34
    int cellX_ = 0, cellZ_ = 0;                // +0x40, +0x44
    bool active_ = false;                      // +0x5c
    cRZTimer timer_{cRZTimer::kMilliseconds};  // +0x60 (unit 4)
};

class cSC4ViewInputControlEffectDisaster final : public cSC4ViewInputControlEffect {
public:
    // `pick` is the exemplar's property 0x4a08c702 read as a bool (kept only when the exemplar has it).
    explicit cSC4ViewInputControlEffectDisaster(bool hasPickProperty = false, bool pick = false);   // @ 0x002d404c

    bool Init() override;                                                  // @ 0x002d3fde
    bool Shutdown() override;                                              // @ 0x002d3efc
    bool OnMouseUpL(int x, int y, std::uint32_t mods) override;            // @ 0x002d3f70
    void SetTerrainControlledEffectState(bool on) override;                // @ 0x002d3dd8
    void SetDisasterManager(std::uint32_t id);                             // @ 0x002d3e98

    std::uint32_t ManagerId() const { return managerId_; }
    bool HasOwnManager() const { return ownManager_; }
    cIGZUnknown* Instance() const { return instance_; }

private:
    bool ownManager_ = false;                  // +0x98
    std::uint32_t managerId_ = 0;              // +0x9c
    bool managerFound_ = false;                // +0xa0 (the original holds the manager)
    cIGZUnknown* instance_ = nullptr;          // +0xa4
};

}  // namespace sc4
