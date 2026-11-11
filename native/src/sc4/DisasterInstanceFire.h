// cSC4DisasterInstanceFire — the city's one fire disaster (Mac i386 0x0033bc90..0x0033ebd2, class size 0x108): every burning
// occupant has a cSC4FireOccupant in its list. A fire is a counter that climbs once per update (one second of the instance's
// timer) through five stage thresholds; fire protection coverage and hosing bring it down. Reaching the target's maximum stage
// burns the target down (rubble through the demolition service, flora killed); each update a fire may also ignite a cell near
// it (Gaussian around the target, widened by the stage radius and the wind). Fires started from one ignition share a "family":
// the disaster layer gets one camera location per family.
//
// Phases: 0 idle, 1 burning (the layer counts a running disaster, the instance ticks with the animation tick manager at
// priority 0xc0), 3 ended. The instance is created by the fire manager and lives for the city's life.
//
// The simulators, the effects, the lots and the view are the IFireInstanceHost seam (the original reaches them through SL::sp*).
#pragma once
#include <cstdint>
#include <list>
#include <set>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZRandom.h"
#include "rz/cRZTimer.h"
#include "sc4/DisasterManagers.h"
#include "sc4/FireOccupant.h"
#include "sc4/OccupantFilters2.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kMessageFireIgnition = 0x49e3db1d;        // data1 = the fire occupant, data2 = 1 lit / 0 out
constexpr std::uint32_t kMessageFireSeverity = 0x49d15b72;        // data1 = severity
constexpr std::uint32_t kMessageFireIgnitedCounts = 0x49d15b73;   // data1 = buildings, data2 = flora ignited since the last update
constexpr std::uint32_t kMessageDisasterPhase = 0xa9c7cab6;       // data1 = manager id, data2 = phase
constexpr std::uint32_t kHistoryFiresEnded = 0x49d15b74;
constexpr std::uint32_t kMessageViewLevelChanged = 0x06cdb65b, kMessageViewChanged = 0xa6b79602;
constexpr std::uint32_t kMessageSimTick = 0x66956813, kMessageAnimationTick = 0x86ad10ee;
constexpr std::uint32_t kPropertyFlammability = 0x29244db5;       // nonzero: the occupant can burn
constexpr std::uint32_t kPropertyRubbleFlags = 0x6a43150f;        // demolition effect flags of the burnt lot
constexpr std::uint32_t kPropertyDemolishLotWhenBurnt = 0x2a89b677;
constexpr std::uint32_t kPropertyRadiationAmount = 0xaa5c04c8, kPropertyRadiationRadius = 0xaa5c04c5;
constexpr std::uint32_t kFireTargetTypeFlora = 0x74758926, kFireTargetTypeProp = 0xa823821e;
constexpr std::uint32_t kFireTickPriority = 0xc0;
constexpr int kLotConditionBurning = 5, kLotConditionBurntOut = 6;

class IFireInstanceHost {
public:
    virtual ~IFireInstanceHost() = default;
    virtual cIGZMessageServer2* MessageServer() = 0;                                 // SL::spMS2
    virtual cISC4DisasterLayer* DisasterLayer() = 0;                                 // city vslot 0x120
    virtual cISC4OccupantManager* Occupants() = 0;                                   // SL::spOccupantManager
    virtual bool HasCity() = 0;
    virtual bool HasSimulator() = 0;                                                 // SL::spSimulator
    virtual bool SimulatorPaused() = 0;                                              // simulator vslot 0x3c
    virtual bool SimulatorEmergencyPaused() = 0;                                     // simulator vslot 0x44
    virtual void RemoveSimulatorAgent(cIGZMessageTarget2* target) = 0;               // simulator vslot 0x54
    virtual int CellCountX() = 0;                                                    // city vslot 0x174
    virtual int CellCountZ() = 0;                                                    // city vslot 0x178
    virtual float CellWidthX() = 0;                                                  // city vslot 0x16c
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;       // city vslot 0x17c
    virtual void CellCenterToPosition(int cellX, int cellZ, float& x, float& z) = 0; // city vslot 0x184
    virtual bool TutorialRunning() = 0;                                              // city vslot 0x148 -> vslot 0x20
    virtual std::uint8_t Flammability(int cellX, int cellZ) = 0;                     // flammability grid vslot 0x1c
    virtual std::uint8_t FireProtection(int cellX, int cellZ, int width, int height) = 0;   // protection grid vslot 0x20 (average)
    virtual void AmbientWind(float& x, float& z) = 0;                                // weather simulator vslot 0x58
    virtual void AddTransientAuraEffect(float x, float z, int effect) = 0;           // aura simulator vslot 0x1c
    virtual void StartEffectOnOccupant(const char* name, cISC4Occupant* occupant) = 0;   // effects manager: model from the occupant
    virtual void StartEffectAt(const char* name, const float position[3]) = 0;
    virtual void KillFloraOccupant(cISC4Occupant* occupant) = 0;                     // flora simulator vslot 0x5c
    virtual bool OccupantLotRect(cISC4Occupant* occupant, SC4CellRect& rect) = 0;    // lot manager vslot 0x38 + lot vslot 0x38
    virtual void SetOccupantLotCondition(cISC4Occupant* occupant, int condition) = 0;   // lot vslot 0x90
    virtual void DemolishRegion(const SC4CellRect& rect, std::uint32_t flags) = 0;   // demolition vslot 0x18 (commit, privilege 0)
    virtual void AddRadiation(const float position[3], float radius, float amount) = 0;   // pollution simulator vslot 0x108
    virtual void RecordHistory(std::uint32_t id, std::int32_t value) = 0;            // history warehouse vslot 0x30
    virtual void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;      // SL::ATM() vslot 0x14
    virtual void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;   // vslot 0x18
    virtual int CurrentViewLevel() = 0;                                              // 3D view vslot 0x58 (1 = the surface)
    // The tuning exemplar {0x6534284a, 0x29b6c670, 0xc9b6b73d}; null when the data has none.
    virtual const cISCPropertyHolder* TuningExemplar() = 0;
    virtual bool LoadTuningCurve(cSC4ResponseCurve& curve, std::uint32_t property) = 0;
};
IFireInstanceHost* FireInstanceHost();
void SetFireInstanceHost(IFireInstanceHost* host);

// Fire occupants that still burn (the hose and the extinguish calls look for these).
class cSC4ActiveFireOccupantFilter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type == kOccupantTypeFire; }
};

class cSC4DisasterInstanceFire final : public cISC4DisasterInstanceFire, public cIGZMessageTarget2 {
public:
    cSC4DisasterInstanceFire();
    ~cSC4DisasterInstanceFire();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;                                   // @ 0x0033bc9a: the last reference shuts down first

    // cISC4DisasterInstance
    bool Shutdown() override;                                           // @ 0x0033cf36
    cISC4DisasterManager* GetManager() const override;                  // @ 0x0033be56
    std::uint32_t GetID() const override { return kDisasterManagerIdFire; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return phase_; }

    // cISC4DisasterInstanceFire
    bool Init() override;                                               // @ 0x0033deaa
    void Update() override;                                             // @ 0x0033ebd2
    bool Ignite(cISC4Occupant* occupant, bool force, std::uint32_t family, bool jump) override;                // @ 0x0033d0b0
    bool IgniteCell(std::uint32_t x, std::uint32_t z, bool roll, bool force, std::uint32_t family) override;   // @ 0x0033cab2
    std::uint32_t ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) override;                       // @ 0x0033c950
    bool QuickBurnCell(std::uint32_t x, std::uint32_t z) override;      // @ 0x0033c8a0
    bool IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t inner, std::uint32_t outer) override;   // @ 0x0033cc4a
    bool Extinguish(cISC4Occupant* target) override;                    // @ 0x0033c3c2
    bool BurnDown(cISC4Occupant* target) override;                      // @ 0x0033c390
    std::uint32_t SetCause(std::uint32_t cause) override;               // @ 0x0033bd1a: only the first cause sticks
    std::uint32_t GetCause() override { return cause_; }
    int GetFireCountInCellRect(const SC4CellRect& rect) override;       // @ 0x0033c218
    int GetFireCount() override { return static_cast<int>(fires_.size()); }

    bool DoMessage(cIGZMessage2* message) override;                     // @ 0x0033c298

    bool ExtinguishFire(cSC4FireOccupant* fire);                        // @ 0x0033bdf2 (vslot 0x68)
    bool BurnDownFire(cSC4FireOccupant* fire);                          // @ 0x0033d8f6 (vslot 0x6c)
    int GetDamagedOccupantsCount() const { return damaged_; }
    int GetCost() const { return cost_; }
    std::uint32_t Severity() const { return severity_; }
    const std::list<cSC4FireOccupant*>& Fires() const { return fires_; }
    std::size_t ToastCount() const { return toast_.size(); }
    cRZRandom& Random() { return random_; }
    // Tests: one update of every fire without waiting for the timer.
    void UpdateAllFires();                                              // @ 0x0033eb4a

private:
    void SendIgnitionMessage(cISC4Occupant* fire, bool lit);            // @ 0x0033bd4a
    void ChooseSpreadCell(cSC4FireOccupant* fire, int& x, int& z);      // @ 0x0033beb4
    void SetPhase(int phase);                                           // @ 0x0033c01e
    void SetSeverity(std::uint32_t severity);                           // @ 0x0033c0d8
    cSC4FireOccupant* LookupFireFromTarget(cISC4Occupant* target);      // @ 0x0033c354
    void AddToToastList(cISC4Occupant* occupant);                       // @ 0x0033c3f4
    void KillAllFires();                                                // @ 0x0033c4ae
    void KillFire(cSC4FireOccupant* fire);                              // @ 0x0033c590
    void UpdatePhase();                                                 // @ 0x0033c73a
    void ToastOccupant(cISC4Occupant* occupant);                        // @ 0x0033d536
    void ProcessToastList();                                            // @ 0x0033d96c
    void ReadTunableValues();                                           // @ 0x0033db68
    void UpdateFire(cSC4FireOccupant* fire);                            // @ 0x0033e6d6
    int ProtectionStep(cISC4Occupant* target);                          // the rounded protection curve over the target's cells
    void Post(std::uint32_t type, std::uint64_t data1, std::uint64_t data2, bool withData2);

    std::uint32_t refCount_ = 0;
    cRZRandom random_{0xffffffffu};                    // +0x18
    cRZTimer timer_{cRZTimer::Units(4)};               // +0x2c
    cSC4OccupantTypesFilter* filter_ = nullptr;        // +0x40 flora, buildings, props
    int cellCountX_ = 0, cellCountZ_ = 0;              // +0x44, +0x48
    bool tutorial_ = false;                            // +0x4c nothing burns down while a tutorial runs
    std::vector<std::uint32_t> stageThresholds_;       // +0x50 counter value where each stage ends (running sums)
    std::vector<std::uint32_t> spreadOdds_;            // +0x5c per stage: a spread attempt once in this many updates
    std::vector<std::uint32_t> spreadRadius_;          // +0x68 per stage, metres
    float windFactor_ = 0.0f;                          // +0x74
    std::uint32_t updateInterval_ = 0;                 // +0x78 timer units between updates
    cSC4ResponseCurve topCurve_, sideCurve_;           // +0x7c, +0x8c flame counts (cSC4FireOccupant reads them)
    cSC4ResponseCurve protectionCurve_;                // +0x9c coverage -> counter step down
    cSC4ResponseCurve flammabilityCurve_;              // +0xac flammability -> ignition chance in percent
    std::uint8_t locationZoom_ = 0;                    // +0xbc
    bool initialised_ = false;                         // +0xbd
    std::uint32_t nextUpdate_ = 0;                     // +0xc0
    int phase_ = 0;                                    // +0xc4
    std::uint32_t buildingsIgnited_ = 0;               // +0xc8
    std::uint32_t floraIgnited_ = 0;                   // +0xcc
    std::uint32_t severity_ = 0;                       // +0xd0
    std::uint32_t cause_ = 0;                          // +0xd4
    std::list<cSC4FireOccupant*> fires_;               // +0xd8 (a reference each)
    int damaged_ = 0;                                  // +0xe0
    int cost_ = 0;                                     // +0xe4
    std::list<cISC4Occupant*> toast_;                  // +0xe8 occupants waiting to burn down (a reference each)
    std::set<std::uint32_t> families_;                 // +0xf0
};

}  // namespace sc4
