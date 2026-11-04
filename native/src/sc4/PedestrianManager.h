// cSC4PedestrianManager — the per-kind automata manager that owns the city's pedestrians (22 symbols,
// 0x1000f0cc0..0x1000f3bf0; primary vtable 0x100a71ae0). It derives from cSC4TrafficAutomataManager in the original
// (its constructor calls the traffic manager constructor at 0x100111b96) and adds the pedestrian-specific bookkeeping:
//   * the default path/network types (2/0), the three populate layers (1, 7, 8) and a crime-layer multiplier of 1.0;
//   * the crowd-reaction event (message 0xa9e3b6e2 -> HandleCrowdReactionEvent, which fans the event out to every nearby
//     pedestrian and posts message 0xa9e3b6e3);
//   * the pedestrian exemplar/group defaults (exemplar group 0x17, default group 0x4001, fallback group
//     0x896e75af) and the two per-layer tuning overrides (GetEdgeDensityCurve / GetMaxAutosPerSecond for layers 1/7);
//   * the commotion multiplier that adds the clock percent when the automata are loaded;
//   * the concrete automaton factory (AddAutomaton creates a cSC4Pedestrian).
//
// Class id 0xc98f9d17; the primary vtable carries the four cSC4AutomataManager views (0x98f9bf4 primary,
// 1 cIGZMessageTarget, 0xe4fda3d4 cIGZSerializable, 0x452294aa cIGZMessageTarget2) plus the class id itself.
// QueryInterface answers 0xc98f9d17 in addition to the base ids.
//
// Quirks kept (asserted by the test, with the address):
//  * HandleCrowdReactionEvent measures the pedestrian-to-occupant distance in three dimensions and calls the pedestrian
//    only when it does not already carry flag 0x4000 (0x1000f0fd2).
//  * GetCommotionMultiplier divides the clock percent by 50 (not -100 like the traffic base) and adds 1, with no clamp
//    (0x1000f1554).
//  * GetEdgeDensityCurve / GetMaxAutosPerSecond answer the base curves for every layer except 1 and 7, which use the
//    tuning params' sixth curve (0x100b21980, i.e. +0x100) and its trailing float (0x100b219a0).
//  * HandleTuningParametersChanged reads the u32 at 0x100b21894 for the max count and the byte at 0x100b219d5 for the
//    min zoom.
//  * HandleRenderPropertiesChanged re-derives the max count from the render properties density factor (offset 0x3e0) and
//    re-reads the min zoom (offset 0x428).
//
// Seams: the pedestrian manager reached SL::spMS2 / SL::spRenderProperties / SL::spCity through IPedestrianManagerHost
// below; the traffic half is the recovered cSC4TrafficAutomataManager / cSC4AutomataManager base.
#pragma once
#include <cstdint>

#include "sc4/TrafficAutomataManager.h"

namespace sc4 {

class cSC4PedestrianManager;

constexpr std::uint32_t kGZCLSID_cSC4PedestrianManager = 0xc98f9d17;
constexpr std::uint32_t kGZIID_cISC4PedestrianManager = 0xc98f9d17;   // QueryInterface answers the class id

constexpr std::uint32_t kPedestrianMessageCrowdReaction = 0xa9e3b6e2;   // registered in Init
constexpr std::uint32_t kPedestrianMessageCrowdReactionResult = 0xa9e3b6e3;   // posted by HandleCrowdReactionEvent

constexpr std::uint32_t kPedestrianExemplarGroup = 0x17;   // i386 constructor: +0xa50 = 0x17 (the 64-bit store also writes the next word)
constexpr std::uint32_t kPedestrianDefaultGroup = 0x4001;
constexpr std::uint32_t kPedestrianFallbackGroup = 0x896e75af;   // the cSC4Pedestrian class id

// ---- seams -------------------------------------------------------------------------------------------------------------
class IPedestrianManagerHost : public ITrafficAutomataManagerHost {
public:
    ~IPedestrianManagerHost() override = default;
    virtual bool HasRenderProperties() const = 0;                 // SL::spRenderProperties != 0
    virtual float AutomataDensityFactor() = 0;                    // spRenderProperties +0x48 -> +0x3e0
    virtual int AutomataMinZoom() = 0;                            // spRenderProperties +0x30 -> +0x428
    virtual bool PedestrianCrowdReaction(IAutomataAutomaton* automaton, const cS3DVector3& position,
                                         std::uint32_t event) = 0;   // cSC4Pedestrian::HandleCrowdReactionEvent
    virtual void PostCrowdReactionMessage(cISC4Occupant* occupant, std::uint32_t event, std::uint32_t count) = 0;
    virtual IAutomataAutomaton* CreatePedestrianAutomaton() = 0;  // new cSC4Pedestrian(this, ...) (refcount 0)
};

class cSC4PedestrianManager : public cSC4TrafficAutomataManager {
public:
    explicit cSC4PedestrianManager(IPedestrianManagerHost* host = nullptr);   // @ 0x1000f0cc0
    ~cSC4PedestrianManager() override;                                        // @ 0x1000f3bac

    bool QueryInterface(std::uint32_t iid, void** out) override;              // @ 0x1000f158c

    bool Init() override;                                                     // @ 0x1000f0e46
    bool Shutdown() override;                                                 // @ 0x1000f0e9c
    bool DoMessage(cIGZMessage2* message) override;                           // @ 0x1000f0eda

    bool AddAutomaton(IAutomataAutomaton* automaton) override;                // @ 0x1000f1368
    bool HandleTuningParametersChanged() override;                            // @ 0x1000f1434
    bool HandleRenderPropertiesChanged() override;                            // @ 0x1000f1466
    cSC4ResponseCurve& GetEdgeDensityCurve(PopulateLayer layer) override;     // @ 0x1000f150c
    float GetMaxAutosPerSecond(PopulateLayer layer) override;                 // @ 0x1000f152e
    float GetCommotionMultiplier(ITrafficNetworkMapOccupant* occupant, PopulateLayer layer) override;   // @ 0x1000f1554
    bool HandleNewBuilding(IAutomataBuildingOccupant* building) override;     // @ 0x1000f121c

    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PedestrianManager; }   // @ 0x1000f3be4

    // The crowd-reaction fan-out (message 0xa9e3b6e2).
    bool HandleCrowdReactionEvent(cISC4Occupant* occupant, std::uint32_t event, std::uint32_t radius);   // @ 0x1000f0fd2

    IPedestrianManagerHost* PedestrianHost() const { return pedestrianHost_; }
    void SetPedestrianHost(IPedestrianManagerHost* host) { pedestrianHost_ = host; }

protected:
    IPedestrianManagerHost* PedestrianHostOrGlobal() const;

    IPedestrianManagerHost* pedestrianHost_ = nullptr;   // port seam (not in the original)
};

}  // namespace sc4
