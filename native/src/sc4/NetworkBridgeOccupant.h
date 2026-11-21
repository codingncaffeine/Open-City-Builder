// cSC4NetworkBridgeOccupant (GZCOM class 0x49cc1bcd): one piece of a bridge, a network piece with a pre-built model (the
// deck, the pylons and the ramps come from the bridge type's exemplars). Recovered from the Mac i386 build
// (0x00156a1e..0x00156bc2, CanBeLit 0x00687dd6). Its record is its base's (it has no Read / Write of its own).
#pragma once
#include <cstdint>

#include "sc4/NetworkOccupantWithPreBuiltModel.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4NetworkBridgeOccupant = 0x49cc1bcd;

// The model registration effects a bridge piece starts over deep water (cSC4ModelRegEffects at i386 +0x1c8 in the
// original): the app makes them on the piece's model.
class IBridgeEffectsHost {
public:
    virtual ~IBridgeEffectsHost() = default;
    // UpdateBaseModel: the old model's effects stop; with a model they are made for it, and started (StartAllEffects(0)
    // + Update) when the water at the piece is more than 12 m above the terrain (SL::spTerrain GetLerpedWaterLevel -
    // GetAltitude at the piece's x, z).
    virtual void* UpdateEffects(void* effects, s3d::cIS3DModelInstance* model, const float* position) = 0;
    virtual void ReleaseEffects(void* effects) = 0;   // Shutdown: stopped and the model cleared
};
IBridgeEffectsHost* BridgeEffectsHost();
void SetBridgeEffectsHost(IBridgeEffectsHost* host);

class cSC4NetworkBridgeOccupant : public cSC4NetworkOccupantWithPreBuiltModel {
public:
    cSC4NetworkBridgeOccupant();                                    // i386 0x00156bc2
    ~cSC4NetworkBridgeOccupant() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;  // i386 0x00156a1e: answers its own class id
    bool Init() override;                                          // i386 0x00156a6e
    bool Shutdown() override;                                      // i386 0x00156b68
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NetworkBridgeOccupant; }
    bool CanBeLit() const override { return true; }                // i386 0x00687dd6
    void UpdateBaseModel(s3d::cIS3DModelInstance* model) override;  // i386 0x00156aa0

private:
    void* effects_ = nullptr;   // the cSC4ModelRegEffects (i386 +0x1c8), made by the app
};

}  // namespace sc4
