// cSC4PropOccupantTerrainDecal — a prop that is a texture laid on the terrain (the scorch under a burnt lot or settling lava):
// class 0x2977aa48, interface cISC4PropOccupantTerrainDecal 0xe982078a. Mac i386 0x00102b44..0x00106208 (16 methods), spec
// prop-occupant.md and lot-developer.md 7.2 (LD14), record lot-records.md 10.5.
//
// Original layout (complete object): +0x00 cISC4PropOccupantTerrainDecal vptr, +0x04 cSC4PropOccupant (its message target at
// +0x08, occupant at +0x0c), +0x6c overlay id (-1: none), +0x70 texture key, +0x7c size, +0x80 position x / z, +0x88 opacity,
// +0x8c fade value, +0x90 fade target, +0x94 fade rate per second, +0x98 completion action, +0x99 holds a reference on itself
// (action 2), +0x9a registered with the animation tick manager, +0x9c the overlay manager (one reference).
//
// The decal reaches the terrain only while the view flags the occupant (flag 2): SetAllFlags then adds it to overlay manager
// 2 (land) when its fade rate is at least 0.1 per second, else to 0 (dynamic land), and enables or disables it afterwards.
// The fade runs on the animation tick (priority 0xc0); at its end action 1 removes the overlay, action 2 takes the prop out
// of the occupant manager and drops its own reference.
#pragma once
#include <cstdint>

#include "gz/cGZPersistResourceKey.h"
#include "sc4/PropOccupant.h"

namespace sc4 {

class cSTEOverlayManager;

constexpr std::uint32_t kGZCLSID_cSC4PropOccupantTerrainDecal = 0x2977aa48;
constexpr std::uint32_t kGZIID_cISC4PropOccupantTerrainDecal = 0xe982078a;
constexpr std::uint32_t kTerrainDecalTickMessage = 0x86ad10ee;   // the animation tick (its float: elapsed milliseconds)
constexpr std::uint16_t kTerrainDecalRecordVersion = 3;

// What the decal reaches outside itself: SL::TerrainView() vslot 0x48 (the overlay manager of an index) and SL::ATM()
// vslots 0x14 / 0x18 (the animation tick manager, priority 0xc0).
class ITerrainDecalHost {
public:
    virtual ~ITerrainDecalHost() = default;
    virtual cSTEOverlayManager* OverlayManager(int index) = 0;
    virtual bool AddTick(cIGZMessageTarget2* target) = 0;
    virtual void RemoveTick(cIGZMessageTarget2* target) = 0;
};
void SetTerrainDecalHost(ITerrainDecalHost* host);
ITerrainDecalHost* TerrainDecalHost();

class cISC4PropOccupantTerrainDecal : public cIGZUnknown {
public:
    enum tCompletionAction : std::uint8_t { kKeepDecal = 0, kRemoveDecal = 1, kRemoveOccupant = 2 };
    virtual void SetDecalTexture(const cGZPersistResourceKey& key, float size) = 0;                       // [3]
    virtual void SetOpacity(float opacity) = 0;                                                            // [4]
    virtual void SetOpacityFade(float seconds, tCompletionAction action, float from, float to) = 0;       // [5]
    virtual void SetDecalSize(float size) = 0;                                                             // [6]
};

class cSC4PropOccupantTerrainDecal final : public cISC4PropOccupantTerrainDecal, public cSC4PropOccupant {
public:
    cSC4PropOccupantTerrainDecal();                                    // @ 0x0010607a
    ~cSC4PropOccupantTerrainDecal() override;                          // @ 0x00106192

    // cIGZUnknown (both interfaces share them)
    bool QueryInterface(std::uint32_t iid, void** out) override;       // @ 0x001059fe
    std::uint32_t AddRef() override { return cSC4PropOccupant::AddRef(); }     // @ 0x00102b44
    std::uint32_t Release() override { return cSC4PropOccupant::Release(); }   // @ 0x00102b52

    // cISC4PropOccupantTerrainDecal
    void SetDecalTexture(const cGZPersistResourceKey& key, float size) override;                       // @ 0x00103146
    void SetOpacity(float opacity) override;                                                            // @ 0x00103714
    void SetOpacityFade(float seconds, tCompletionAction action, float from, float to) override;       // @ 0x00102b60
    void SetDecalSize(float size) override;                                                             // @ 0x00103750

    // cSC4PropOccupant / cSC4Occupant
    bool Init() override;                                              // @ 0x00103b84
    bool Shutdown() override;                                          // @ 0x00103650
    bool Write(cIGZOStream& stream) const override;                    // @ 0x00105064
    bool Read(cIGZIStream& stream) override;                           // @ 0x001054b0
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PropOccupantTerrainDecal; }   // @ 0x0065e584
    bool DoMessage(cIGZMessage2* message) override;                    // @ 0x00104b8e
    void SetPosition(const float* xyz) override;                       // @ 0x00104356
    void SetAllFlags(std::uint32_t flags) override;                    // @ 0x00102e78

    // test / report access
    std::uint32_t OverlayId() const { return overlayId_; }
    float Opacity() const { return opacity_; }
    float Size() const { return size_; }
    const cGZPersistResourceKey& TextureKey() const { return key_; }
    bool Ticking() const { return ticking_; }

private:
    cIGZMessageTarget2* TickTarget() { return static_cast<cIGZMessageTarget2*>(this); }
    void DropOverlay();

    std::uint32_t overlayId_ = 0xffffffffu;   // +0x6c
    cGZPersistResourceKey key_{0, 0, 0};       // +0x70
    float size_ = 0.0f;                        // +0x7c
    float posX_ = 0.0f, posZ_ = 0.0f;          // +0x80
    float opacity_ = 1.0f;                     // +0x88
    float fadeValue_ = 1.0f;                   // +0x8c
    float fadeTarget_ = 0.0f;                  // +0x90
    float fadeRate_ = 1.0f;                    // +0x94
    std::uint8_t action_ = kKeepDecal;         // +0x98
    bool selfReference_ = false;               // +0x99
    bool ticking_ = false;                     // +0x9a
    cSTEOverlayManager* manager_ = nullptr;    // +0x9c (one reference)
};

}  // namespace sc4
