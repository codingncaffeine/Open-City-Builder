// cSC4VehicleManager — the per-kind automata manager that owns the city's road vehicles (27 symbols,
// 0x1000d9c40..0x1000e24dc; primary vtable 0x100a70770). It derives from cSC4TrafficAutomataManager in the original (the
// cSC4VehicleManager constructor calls the traffic manager constructor at 0x100111b96) and adds the vehicle-specific
// bookkeeping:
//   * the default path/network types (1/1), the ten populate layers (1,2,3,5,4,7,6,8,9,10) and the crime-layer
//     density multiplier (0.2);
//   * the headlights sweep (day/night and the two headlight messages 0xc9da96e8/0xc9da96e9);
//   * the tuning reload (message 0xc53d10aa, data1 0xa9918d0b) and the funds-changed sweep
//     (message 0x0772fad4 -> HandleFundsChanged);
//   * the vehicle exemplar/group defaults (exemplar group 0x16, default group 0x4000, fallback group
//     0x896e714a) and the two per-layer tuning overrides (GetEdgeDensityCurve / GetMaxAutosPerSecond for layers 1/7);
//   * the concrete automaton factory (AddAutomaton creates a cSC4Vehicle);
//   * the automata-frequency marked-cell view (the +0xb98 pointer, created elsewhere, freed by Shutdown).
//
// Class id 0xa98f9d3b; the primary vtable carries the four cSC4AutomataManager views (0x98f9bf4 primary,
// 1 cIGZMessageTarget, 0xe4fda3d4 cIGZSerializable, 0x452294aa cIGZMessageTarget2) plus the class id itself.
// QueryInterface answers 0xa98f9d3b in addition to the base ids.
//
// Quirks kept (asserted by the test, with the address):
//  * The constructor leaves the +0xb98 marked-cell view null; no recovered code creates it, so Shutdown only ever
//    clears a null pointer (0x1000d9c99 / 0x1000da046).
//  * GetEdgeDensityCurve / GetMaxAutosPerSecond answer the base curves for every layer except 1 and 7, which use the
//    tuning table's first curve / first float (0x100b218b0 / 0x100b218d0).
//  * HandleTuningParametersChanged reads the tuning params' first field as a raw 32-bit word (300 by default) for the
//    max count, and the byte at 0x100b219d4 for the min zoom.
//  * HandleRenderPropertiesChanged re-derives the max count from the render properties density factor and re-reads the
//    min zoom from the render properties object (0x398 / 0x3e0 offsets), then passes the zoom with a -1 rotation.
//  * DoMessage's headlights sweep and the funds-changed sweep walk the base automata list through the host seams below
//    (the original calls the cSC4Vehicle's 0x5b8 slot and the script system directly).
//
// Seams: everything the vehicle manager reached through SL::spMS2 / SL::spCity / SL::spRenderProperties /
// SL::sp24HourClock / SL::spViewUtilities and the cSC4Vehicle it creates goes through IVehicleManagerHost below
// (each method names the original slot/address it replaces). The traffic half is the recovered
// cSC4TrafficAutomataManager / cSC4AutomataManager base.
#pragma once
#include <cstdint>

#include "sc4/TrafficAutomataManager.h"

namespace sc4 {

class cSC4VehicleManager;

constexpr std::uint32_t kGZCLSID_cSC4VehicleManager = 0xa98f9d3b;
constexpr std::uint32_t kGZIID_cISC4VehicleManager = 0xa98f9d3b;   // QueryInterface answers the class id

// Messages registered in Init (removed in Shutdown).
constexpr std::uint32_t kVehicleMessageHeadlights = 0xc53d10aa;   // data1 0xa9918d0b = "automata tuning changed"
constexpr std::uint32_t kVehicleMessageCameraTarget1 = 0xa6b79601;
constexpr std::uint32_t kVehicleMessageCameraTarget2 = 0xa6b79611;
constexpr std::uint32_t kVehicleMessageHeadlightsOff = 0xc9da96e8;
constexpr std::uint32_t kVehicleMessageHeadlightsOn = 0xc9da96e9;
constexpr std::uint32_t kVehicleMessageHourPassed = 0x44778234;
constexpr std::uint32_t kVehicleMessageFundsChanged = 0x0772fad4;

// The value of data1 of message 0xc53d10aa that triggers the tuning reload.
constexpr std::uint32_t kVehicleTuningChangedToken = 0xa9918d0b;

// The default exemplar/group ids the constructor writes (0x1000d9ca4 / 0x1000d9cb5).
constexpr std::uint32_t kVehicleExemplarGroup = 0x16;   // i386 constructor: +0xa50 = 0x16 (the 64-bit store also writes the next word)
constexpr std::uint32_t kVehicleDefaultGroup = 0x4000;
constexpr std::uint32_t kVehicleFallbackGroup = 0x896e714a;      // the cISC4Vehicle class id

// ---- seams -------------------------------------------------------------------------------------------------------------
// Everything the vehicle manager reached through an engine global or the cSC4Vehicle it creates.
class IVehicleManagerHost : public ITrafficAutomataManagerHost {
public:
    ~IVehicleManagerHost() override = default;
    virtual bool HasRenderProperties() const = 0;                 // SL::spRenderProperties != 0
    virtual float AutomataDensityFactor() = 0;                    // spRenderProperties +0x48 -> +0x398
    virtual int AutomataMinZoom() = 0;                            // spRenderProperties +0x30 -> +0x3e0
    virtual int TimeOfDay() = 0;                                  // SL::sp24HourClock vslot 0x30 (0 day / 1 night / 2)
    virtual void ShowVehicleHeadlights(IAutomataAutomaton* automaton, bool on, bool instant) = 0;   // cSC4Vehicle 0x5b8
    virtual IAutomataAutomaton* CreateVehicleAutomaton() = 0;     // new cSC4Vehicle(this, ...) (refcount 0)
    // After ReadTuningParameters on message 0xc53d10aa data1 0xa9918d0b: tell the four per-kind managers
    // (city 0x158/0x160/0x168/0x170, vslot 0xc0), the group budget (0x188 vslot 0xf8) and the network manager
    // (this+0xaf0 vslot 0x28) to re-read the tuning.
    virtual void NotifyKindManagersTuningChanged() = 0;
    // The funds-changed sweep: for every building occupant, stamp the "ambulance tone" / "bus service" flags through the
    // script system (cSC4AutomataScriptSystem vslots 0x88/0x90). Ported as one seam because the original reaches the
    // city's two occupant lists (0x220/0x1a0) and the script system directly.
    virtual void HandleFundsChangedSweep() = 0;
};

class cSC4VehicleManager : public cSC4TrafficAutomataManager {
public:
    explicit cSC4VehicleManager(IVehicleManagerHost* host = nullptr);   // @ 0x1000d9c40
    ~cSC4VehicleManager() override;                                     // @ 0x1000e2498

    // ---- cIGZUnknown (four views, one count; the class id is answered here) ----
    bool QueryInterface(std::uint32_t iid, void** out) override;        // @ 0x1000dac68

    // ---- lifecycle ----
    bool Init() override;                                               // @ 0x1000d9f76
    bool Shutdown() override;                                           // @ 0x1000da046
    bool DoMessage(cGZMessage& message) override;                       // @ 0x1000da126
    bool DoMessage(cIGZMessage2* message) override;                     // @ 0x1000da13e

    // ---- per-kind overrides ----
    bool AddAutomaton(IAutomataAutomaton* automaton) override;          // @ 0x1000dab6e
    bool HandleTuningParametersChanged() override;                      // @ 0x1000da884
    bool HandleRenderPropertiesChanged() override;                      // @ 0x1000da8b6
    cSC4ResponseCurve& GetEdgeDensityCurve(PopulateLayer layer) override;   // @ 0x1000da95c
    float GetMaxAutosPerSecond(PopulateLayer layer) override;           // @ 0x1000da97e
    bool HandleNewBuilding(IAutomataBuildingOccupant* building) override;   // @ 0x1000da9a0
    void DoAnimationTick(std::uint32_t data, float deltaMilliseconds) override;   // @ 0x1000dac3a

    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4VehicleManager; }   // @ 0x1000e24d0

    // ---- the funds-changed sweep (message 0x0772fad4) ----
    void HandleFundsChanged();                                          // @ 0x1000da428

    // ---- test / engine access ----
    IVehicleManagerHost* VehicleHost() const { return vehicleHost_; }
    void SetVehicleHost(IVehicleManagerHost* host) { vehicleHost_ = host; }
    void SetMarkedCellView(cIGZUnknown* view) { markedCellView_ = view; }
    cIGZUnknown* MarkedCellView() const { return markedCellView_; }

protected:
    IVehicleManagerHost* VehicleHostOrGlobal() const;

    IVehicleManagerHost* vehicleHost_ = nullptr;   // port seam (not in the original)
    cIGZUnknown* markedCellView_ = nullptr;        // +0xb98 (the automata-frequency marked-cell view)
};

}  // namespace sc4
