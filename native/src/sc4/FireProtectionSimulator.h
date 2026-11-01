// cSC4FireProtectionSimulator — fire station coverage and fire-department strikes (79 functions,
// 0x1002fa4ce..0x1002fc1f2, class id 0x2990c05a, interfaces 0x88f54c7b / 0x452294aa message target,
// 0xc9c00435 / 0xe4fda3d4 serializable). It extends cSC4ProtectionSimulator (tract size 2, cell divisor 50;
// the base lives in ProtectionSimulator.h, embedded at +8 in the original) and, unlike the police simulator,
// has no population model: it only turns station funding into efficiency and handles strikes.
//
// Station exemplar properties: 0x8f55dc0/dc1 station effect peak/edge, dc2 radius (also the "is a fire
// station" test), dc5 dispatch count, 0x69b425dd base call number, 0x69b425de availability mask,
// 0x29233e9a/9b/9c dispatch effect peak/edge/radius, 0xeaa69f83 strike start date, funding property
// 0xea567bc3. Original layout: +0xe4/+0xe8 average protection / coverage share (the base's), +0xf0 on strike,
// +0xf4 strike start date, +0xfc funding property id, +0x128 efficiency curve, +0x148..+0x17c tunables,
// +0x158 strike chance curve.
// Init/Shutdown wiring, Read/Write and DoMessage routing stay unrecovered (service locator and streams).
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/ProtectionSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"

class cIGZIStream;
class cIGZOStream;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4FireProtectionSimulator = 0x2990c05a;
constexpr std::uint32_t kGZIID_cISC4FireProtectionSimulator = 0x88f54c7b;

constexpr std::uint32_t kFireStationEffectPeak = 0x08f55dc0, kFireStationEffectEdge = 0x08f55dc1;
constexpr std::uint32_t kFireStationRadius = 0x08f55dc2;
constexpr std::uint32_t kFireStationDispatchCount = 0x08f55dc5;
constexpr std::uint32_t kFireStationBaseCallNumber = 0x69b425dd, kFireStationAvailability = 0x69b425de;
constexpr std::uint32_t kFireDispatchPeak = 0x29233e9a, kFireDispatchEdge = 0x29233e9b, kFireDispatchRadius = 0x29233e9c;
constexpr std::uint32_t kFireStrikeStartDate = 0xeaa69f83;
constexpr std::uint32_t kFireFundingProperty = 0xea567bc3;
constexpr std::uint32_t kFireMessageCoverage = 0x68f54ce0, kFireMessageStrikeBegin = 0x68f54ce1, kFireMessageStrikeEnd = 0x68f54ce2;
constexpr std::uint32_t kFireHistoryPercentCoverage = 0x2a11d203, kFireHistoryAverageCoverage = 0x2a11d204;
constexpr std::uint32_t kFireHistoryCombinedCoverage = 0xaaad4d87;   // mean of fire and police coverage shares

class IFireOccupant : public IProtectionStation {
public:
    virtual cISCPropertyHolder* PropertyHolder() = 0;      // vslot 0x30
    virtual bool UsesDepartmentBudget() = 0;               // QueryInterface(0xe9793a65)
};

class IFireHost {
public:
    virtual ~IFireHost() = default;
    virtual int CurrentDay() = 0;
    virtual void PostMessage(std::uint32_t message, IFireOccupant* occupant) = 0;
    virtual void AddHistory(std::uint32_t historyId, std::int64_t value) = 0;
    // budget record of an occupant for a funding property: false when it has none
    virtual bool FundingFraction(IFireOccupant* occupant, std::uint32_t propertyId, float& fraction) = 0;
    virtual bool FundingInfo(IFireOccupant* occupant, std::uint32_t propertyId, int& cost, float& fraction) = 0;
    virtual bool SetFundingFraction(IFireOccupant* occupant, std::uint32_t propertyId, const SC4Percentage& fraction) = 0;
    virtual float DepartmentBudgetFraction() = 0;          // fire department +0x50
    virtual void* DepartmentBudget() = 0;
    virtual void SetStrikerGroup(IFireOccupant* occupant, bool add) = 0;
    virtual void RegisterFundedStation(IFireOccupant* occupant, std::uint32_t propertyId, bool add) = 0;
    virtual float PoliceCoverageShare() = 0;               // police simulator GetPercentCoverage
    virtual int AvailableDispatchCount(std::uint32_t purpose) = 0;
    virtual std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) = 0;
    virtual bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) = 0;
    virtual bool LotAllowsDispatch(IProtectionStation* station) = 0;   // true when the occupant has no lot
    virtual bool IsAboveWater(float x, float z) = 0;
    virtual bool IsZonedAt(int x, int z) = 0;
    virtual void TakeStationDispatch(IProtectionStation* station) = 0;
    // ---- stream seams: station occupants are reached through the host ----
    virtual void WriteOccupant(cIGZOStream&, IFireOccupant*) {}       // SC4WriteGZSerializable(stream, occupant)
    virtual bool ReadOccupant(cIGZIStream&, IFireOccupant**) { return false; }
};

struct FireTunables {
    float value148 = 1.2f;              // +0x148 (0x3f99999a)
    float strikeEfficiency = 0.5f;      // +0x14c
    float strikeEndFunding = 0.9f;      // +0x150 (0x3f666666)
    float noRoadFactor = 0.8f;          // +0x178 (0x3f4ccccd): efficiency factor for lots the dispatcher can't use
    int strikeMinDays = 60;             // +0x17c
};

struct FireResourceKey { std::uint32_t type = 0, group = 0, instance = 0; };

// The primary view the city holds for this simulator (cISC4FireProtectionSimulator, iid 0x88f54c7b).
class cISC4FireProtectionSimulator : public cIGZUnknown {};

class cSC4FireProtectionSimulator : public cISC4FireProtectionSimulator,
                                    public cIGZMessageTarget2,
                                    public cISC4ProtectionDispatchSimulator,
                                    public cIGZSerializable,
                                    private IProtectionHost {
public:
    explicit cSC4FireProtectionSimulator(IFireHost* host);

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4FireProtectionSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    bool DoMessage(cIGZMessage2* message) override;

    // tunables from the simulator exemplar {type 0x6534284a, group 0xe7e2c2db, instance 0x68f5524a}; null = defaults
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    const FireTunables& Tunables() const { return tunables_; }
    cSC4ResponseCurve& StationEfficiencyCurve() { return stationEfficiency_; }   // +0x128
    cSC4ResponseCurve& StrikeCurve() { return strikeChance_; }                   // +0x158
    cSC4ProtectionSimulator& Protection() { return protection_; }
    cSC4SimGrid<std::uint8_t>* GetFireProtectionGrid() const { return protection_.AdjustedGrid(); }
    void* GetDepartmentBudget() const { return host_->DepartmentBudget(); }

    // stations and efficiency
    bool IsFireStation(IFireOccupant* occupant) const;
    float GetStationEfficiency(IFireOccupant* station);
    bool GetStationFunding(IFireOccupant* station, int* cost, SC4Percentage* fraction) const;
    bool SetStationFunding(IFireOccupant* station, const SC4Percentage& fraction);
    float GetStationDispatchRadius(IFireOccupant* station);
    bool GetOccupantCoverage(IFireOccupant* station, const SC4Percentage& share, float& covered, float& strength);
    int GetTotalDispatchCount();
    static bool GetDispatchesAtStation(IFireOccupant* station, std::uint32_t* mask, std::uint32_t* baseCall,
                                       std::uint32_t* count);
    static bool CanHandleDispatchType(std::uint32_t type) { return type == 1; }
    static bool GetDispatchAutomataProperties(const void* occupant, std::uint32_t& automatonType,
                                              FireResourceKey& modelKey, FireResourceKey& secondKey);
    std::size_t GetFireStationCount() const { return protection_.Stations().size(); }
    int GetFireStations(std::list<IFireOccupant*>& out) const;
    float GetAverageCoverage() const { return protection_.AverageProtection(); }
    float GetPercentCoverage() const { return protection_.CoverageShare(); }
    void DoOccupantInserted(IFireOccupant* occupant) { protection_.DoOccupantInserted(occupant); }
    void DoOccupantRemoved(IFireOccupant* occupant) { protection_.DoOccupantRemoved(occupant); }
    void DoOccupantChanged(IFireOccupant* occupant) { protection_.DoOccupantChanged(occupant); }

    // strikes
    bool UpdateStrikingState(IFireOccupant* station);
    bool IsOnStrike() const { return onStrike_; }
    static bool IsStationOnStrike(IFireOccupant* station);
    float GetChanceOfStrike();
    bool BeginStrike(IFireOccupant* station);
    bool EndStrike();
    int GetStrikeStartDate() const { return strikeStart_; }
    int GetStrikeLength();
    int GetStrikingStations(std::list<IFireOccupant*>& out);

    // protection hooks (reached through the base's vslots in the original)
    bool ProtectionGetStationEffect(IFireOccupant* station, ProtectionEffect& effect);
    bool ProtectionGetDispatchEffect(IFireOccupant* station, ProtectionEffect& effect);
    bool ProtectionGetStationFunding(IFireOccupant* station, int* cost) const;
    void ProtectionSetStationAvailabilityFlags(IFireOccupant* station, std::uint32_t flags);
    void ProtectionSetStationBaseCallNumber(IFireOccupant* station, std::uint32_t number);
    static std::uint32_t ProtectionGetDispatchType() { return 1; }
    void ProtectionEndOfCycle();

    void DoMonthlySimulation();
    void DoDailySimulation();
    cRZRandom& Random() { return random_; }

private:
    bool GetStationEffect(IProtectionStation* station, ProtectionEffect& effect) override;
    void OnAdjustedCell(int, int, int) override {}
    void OnPassFinished() override { ProtectionEndOfCycle(); }
    void SetStationDispatchMask(IProtectionStation* station, std::uint32_t mask) override;
    void SetStationDispatchOffset(IProtectionStation* station, std::uint32_t offset) override;
    void GetStationInfo(IProtectionStation* station, int* available, std::uint32_t* offset, std::uint32_t* count) override;
    int MinimumDispatchesForService() override { return GetTotalDispatchCount(); }
    std::uint32_t DispatchPurpose() override { return 1; }
    int AvailableDispatchCount(std::uint32_t purpose) override { return host_->AvailableDispatchCount(purpose); }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) override { return host_->ActiveDispatches(purpose); }
    bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) override { return host_->GetDispatch(id, info); }
    bool LotAllowsDispatch(IProtectionStation* station) override { return host_->LotAllowsDispatch(station); }
    bool IsAboveWater(float x, float z) override { return host_->IsAboveWater(x, z); }
    bool IsZonedAt(int x, int z) override { return host_->IsZonedAt(x, z); }
    void RegisterStationProperty(IProtectionStation* station, bool add) override;
    void GetDispatchEffect(const ProtectionDispatchInfo& info, ProtectionEffect& effect) override;
    void TakeStationDispatch(IProtectionStation* station) override { host_->TakeStationDispatch(station); }
    // IProtectionHost stream seams (the base serializes its grids/stations through these).
    void WriteGridInt16(cIGZOStream& stream, cSC4SimGrid<std::int16_t>* grid) override;
    bool ReadGridInt16(cIGZIStream& stream, cSC4SimGrid<std::int16_t>** out) override;
    void WriteGridUint8(cIGZOStream& stream, cSC4SimGrid<std::uint8_t>* grid) override;
    bool ReadGridUint8(cIGZIStream& stream, cSC4SimGrid<std::uint8_t>** out) override;
    void WriteStation(cIGZOStream& stream, IProtectionStation* station) override;
    bool ReadStation(cIGZIStream& stream, IProtectionStation** out) override;

    IFireHost* host_;
    cRZRandom random_{0xffffffff};
    std::uint32_t refCount_ = 0;   // +0x110 (COM identity)
    FireTunables tunables_;
    cSC4ProtectionSimulator protection_;
    cRetainedReference fileDispatch_;   // +0x120 the dispatch helper a city file named (kept as read)
    cSC4ResponseCurve stationEfficiency_, strikeChance_;
    bool onStrike_ = false;
    int strikeStart_ = 0;
};

}  // namespace sc4
