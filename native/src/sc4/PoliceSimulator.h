// cSC4PoliceSimulator — police coverage, the criminal population, jails, strikes and jailbreaks (114 functions,
// 0x1000a237e..0x1000a5c1e, class id 0x6990c04f, interfaces 0x452294aa message target, 0xc9c00435, 0xe4fda3d4
// serializable). It extends cSC4ProtectionSimulator (the grids and station bookkeeping live in
// ProtectionSimulator.h; the original embeds it at +8) with the crime model:
//   * criminals (+0x150) pile up from crimes and are only held by jail capacity; jails (buildings with
//     exemplar property 0x48d71ee0 = capacity) lock up criminals in proportion capacity * jail efficiency, the
//     funding-driven efficiency curve (+0x1a8).
//   * every month: rehabilitation removes criminals * rehab rate, the month's crimes are folded in, station
//     crime stats roll over, strikes start/end (funding too low), jailbreaks may start (chance curve of the
//     jailed fraction) and end (once the jails empty below a threshold).
// Station exemplar properties: 0x48d71ed0/ed1 station effect peak/edge, ed2 radius (also the "is a police
// station" test), ed5 dispatch count, ed7 base call number, ed8 availability mask, ed9..edb dispatch effect,
// ee0 jail capacity / ee1 jailed count, ee4, 0x8a70a0b9 crime stats uint32[4] {crimes, arrests, thisMonth
// crimes, thisMonth arrests}, 0xeaa69f83 strike start date, funding property 0xa567baa (stations) and
// 0xea56768a (jails). Original layout: +0xec protection scale, +0xf0 on strike, +0xf4 strike start, +0x130 jails
// (+0x140 count), +0x148 jail capacity, +0x14c jail population, +0x150 criminals, +0x154/+0x158 last month's
// arrests/crimes, +0x15c/+0x160 this month's, +0x164 unjailed criminals added, +0x168 funded jails,
// +0x180 unjailed fraction, curves +0x188/+0x1a8/+0x1e0/+0x208, +0x238 jailbreak buildings.
// Stream Read/Write, Init/Shutdown wiring and DoMessage routing stay unrecovered (service locator).
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

constexpr std::uint32_t kGZCLSID_cSC4PoliceSimulator = 0x6990c04f;
constexpr std::uint32_t kGZIID_cISC4PoliceSimulator = 0x67659895;

// exemplar properties
constexpr std::uint32_t kPoliceStationEffectPeak = 0x48d71ed0, kPoliceStationEffectEdge = 0x48d71ed1;
constexpr std::uint32_t kPoliceStationRadius = 0x48d71ed2;
constexpr std::uint32_t kPoliceStationDispatchCount = 0x48d71ed5;
constexpr std::uint32_t kPoliceStationBaseCallNumber = 0x48d71ed7;
constexpr std::uint32_t kPoliceStationAvailability = 0x48d71ed8;
constexpr std::uint32_t kPoliceDispatchPeak = 0x48d71ed9, kPoliceDispatchEdge = 0x48d71eda, kPoliceDispatchRadius = 0x48d71edb;
constexpr std::uint32_t kPoliceJailCapacity = 0x48d71ee0, kPoliceJailPopulation = 0x48d71ee1;
constexpr std::uint32_t kPoliceStationCrimeStats = 0x8a70a0b9;
constexpr std::uint32_t kPoliceStrikeStartDate = 0xeaa69f83;
constexpr std::uint32_t kPoliceStationFundingProperty = 0x0a567baa, kPoliceJailFundingProperty = 0xea56768a;
// messages / history
constexpr std::uint32_t kPoliceMessageStrikeBegin = 0xa8daf511, kPoliceMessageStrikeEnd = 0xa8daf512;
constexpr std::uint32_t kPoliceMessageCoverage = 0xa8daf510, kPoliceMessageStationsChanged = 0xa949569a;
constexpr std::uint32_t kPoliceMessageJailbreakBegin = 0x2a614891, kPoliceMessageJailbreakEnd = 0x2a614892;
constexpr std::uint32_t kPoliceHistoryCrimes = 0x0a5cc155, kPoliceHistoryArrests = 0x0a5cc156;
constexpr std::uint32_t kPoliceHistoryPercentCoverage = 0x4a11d354, kPoliceHistoryAverageCoverage = 0x4a11d355;

class IPoliceOccupant : public IProtectionStation {
public:
    virtual cISCPropertyHolder* PropertyHolder() = 0;      // vslot 0x30
    virtual bool UsesDepartmentBudget() = 0;               // QueryInterface(0xe9793a65)
};

class IPoliceHost {
public:
    virtual ~IPoliceHost() = default;
    virtual int CurrentDay() = 0;                          // city date -> day number
    virtual void PostMessage(std::uint32_t message, IPoliceOccupant* occupant) = 0;
    virtual void AddHistory(std::uint32_t historyId, std::int64_t value) = 0;
    // budget department record for an occupant: false when it has none
    virtual bool FundingFraction(IPoliceOccupant* occupant, std::uint32_t propertyId, float& fraction) = 0;
    // the same record's cost (budget simulator vslot 0x198 on the occupant's property holder, record +0x10)
    virtual bool FundingInfo(IPoliceOccupant* occupant, std::uint32_t propertyId, int& cost, float& fraction) = 0;
    virtual float DepartmentBudgetFraction() = 0;          // police department +0x50
    virtual void SetStrikerGroup(IPoliceOccupant* occupant, bool add) = 0;
    virtual void SetJailbreakGroup(IPoliceOccupant* occupant, bool add) = 0;
    virtual void RegisterFundedStation(IPoliceOccupant* occupant, std::uint32_t propertyId, bool add) = 0;
    // services forwarded to the protection base
    virtual int AvailableDispatchCount(std::uint32_t purpose) = 0;
    virtual std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) = 0;
    virtual bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) = 0;
    virtual bool LotAllowsDispatch(IProtectionStation* station) = 0;
    virtual bool IsAboveWater(float x, float z) = 0;
    virtual bool IsZonedAt(int x, int z) = 0;
    virtual void TakeStationDispatch(IProtectionStation* station) = 0;
    // ---- stream seams: jail/station occupants are reached through the host ----
    virtual void WriteOccupant(cIGZOStream&, IPoliceOccupant*) {}        // SC4WriteGZSerializable(stream, occupant)
    virtual bool ReadOccupant(cIGZIStream&, IPoliceOccupant**) { return false; }
};

struct PoliceTunables {
    float unjailedFraction = 1.1f;      // +0x180 (0x3f8ccccd)
    float value1c8 = 1.2f;              // +0x1c8 (0x3f99999a)
    float rehabilitationRate = 0.01f;   // +0x1cc
    float strikeEfficiency = 0.5f;      // +0x1d0
    std::uint32_t strikeMinDays = 10;   // +0x1d4
    float strikeEndFunding = 0.9f;      // +0x1d8 (0x3f666666)
    int value200 = 16;                  // +0x200
    float jailbreakMin = 1.0f, jailbreakMax = 5.0f;   // +0x228/+0x22c, ReadSimulatorTunableValues stores x0.01
    float jailbreakEndThreshold = 0.5f; // +0x230
};

// The primary view the city holds for this simulator (cISC4PoliceSimulator, iid 0x67659895).
class cISC4PoliceSimulator : public cIGZUnknown {};

class cSC4PoliceSimulator : public cISC4PoliceSimulator,
                            public cIGZMessageTarget2,
                            public cISC4ProtectionDispatchSimulator,
                            public cIGZSerializable,
                            private IProtectionHost {
public:
    explicit cSC4PoliceSimulator(IPoliceHost* host);

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PoliceSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    bool DoMessage(cIGZMessage2* message) override;

    // tunables from the simulator exemplar {type 0x6534284a, group 0xe7e2c2db, instance 0x28dfcf89}; null = defaults
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    const PoliceTunables& Tunables() const { return tunables_; }
    cSC4ResponseCurve& StationEfficiencyCurve() { return stationEfficiency_; }   // +0x188
    cSC4ResponseCurve& JailEfficiencyCurve() { return jailEfficiency_; }         // +0x1a8
    cSC4ResponseCurve& StrikeCurve() { return strikeChance_; }                   // +0x1e0
    cSC4ResponseCurve& JailbreakCurve() { return jailbreakChance_; }             // +0x208
    cSC4ProtectionSimulator& Protection() { return protection_; }

    // efficiency
    float GetStationEfficiency(IPoliceOccupant* station);
    float CalculateJailEfficiency(IPoliceOccupant* jail);
    bool GetStationDispatchRadius(IPoliceOccupant* station, float& radius);
    bool GetOccupantCoverage(IPoliceOccupant* station, const SC4Percentage& share, float& covered, float& strength);

    // criminals
    void RecalculateCriminalStatistics();
    void DoMonthlyCriminalRehabilitation();
    void ProcessMonthlyCrimeStats();
    enum UpdateCrimeType { kCrime = 0, kArrest = 1, kRollover = 2 };
    static void UpdatePoliceStationCrimeStats(IPoliceOccupant* station, UpdateCrimeType type);
    int ResetUnjailedCriminals();
    int GetJailCapacity(IPoliceOccupant* jail);
    long GetJailCurrentUpkeepCost(IPoliceOccupant* jail, long maintenance);
    float GetJailedPercentage() const;
    int GetJailCapacity() const { return jailCapacity_; }
    int GetJailPopulation() const { return jailPopulation_; }
    int GetCriminalCount() const { return criminals_; }
    int GetArrestCount() const { return arrestsPrev_; }
    int GetCrimeCount() const { return crimesPrev_; }
    void AddCrimes(int crimes) { crimesThisMonth_ += crimes; }
    void AddArrests(int arrests) { arrestsThisMonth_ += arrests; }
    void AddUnjailedCriminals(int count) { unjailedAdded_ += count; }
    void SetCriminalCount(int count) { criminals_ = count; }
    static int GetPoliceStationArrestCount(IPoliceOccupant* station);
    static int GetPoliceStationCrimeCount(IPoliceOccupant* station);

    // strikes
    bool UpdateStrikingState(IPoliceOccupant* station);
    bool IsOnStrike() const { return onStrike_; }
    static bool IsStationOnStrike(IPoliceOccupant* station);
    float GetChanceOfStrike();
    bool BeginStrike(IPoliceOccupant* station);
    bool EndStrike();
    int GetStrikeStartDate() const { return strikeStart_; }
    int GetStrikeLength();
    int GetStrikingStations(std::list<IPoliceOccupant*>& out);
    void CheckForStrikeStartOrEnd();

    // jailbreaks
    float GetChanceOfJailbreak(IPoliceOccupant* jail);
    bool BeginJailbreak(IPoliceOccupant* jail);
    bool EndJailbreak(IPoliceOccupant* jail);
    bool IsInJailbreak(IPoliceOccupant* jail) const;
    void CheckForJailbreakStart();
    void CheckForJailbreakEnd();
    int GetJailbreakBuildings(std::list<IPoliceOccupant*>& out) const;

    // buildings
    static bool IsPoliceStation(IPoliceOccupant* occupant);
    static bool IsJail(IPoliceOccupant* occupant);
    void DoOccupantInserted(IPoliceOccupant* occupant);
    void DoOccupantRemoved(IPoliceOccupant* occupant);
    void DoOccupantChanged(IPoliceOccupant* occupant);
    bool ProtectionGetStationFunding(IPoliceOccupant* station, int* cost) const;
    IPoliceOccupant* GetNearestStation(float x, float z);
    int GetTotalDispatchCount();
    static bool GetDispatchesAtStation(IPoliceOccupant* station, std::uint32_t* mask, std::uint32_t* baseCall,
                                       std::uint32_t* count);
    static bool CanHandleDispatchType(std::uint32_t type) { return type == 0; }
    std::size_t GetPoliceStationCount() const { return protection_.Stations().size(); }
    std::size_t GetJailCount() const { return jails_.size(); }
    const std::list<IPoliceOccupant*>& Jails() const { return jails_; }
    void ProtectionProcessCell(int, int, int) {}                      // @ 0x1000a3c04 (the original returns at once)
    int GetPoliceStations(std::list<IProtectionStation*>& out) { return cSC4ProtectionSimulator::CopyOccupantList(out, protection_.Stations()); }   // @ 0x1000a46a2
    int GetJails(std::list<IPoliceOccupant*>& out) { return cSC4ProtectionSimulator::CopyOccupantList(out, jails_); }          // @ 0x1000a46c6
    // @ 0x1000a3f2e / 0x1000a5bfa: the original returns the 8-byte values at +0x128 and +0xb0 (the stream code treats
    // +0x128 as a serialised grid pointer; neither producer is recovered), kept as raw values.
    std::uint64_t GetJailDepartmentBudget() const { return value128_; }
    std::uint64_t GetPolicePowerGrid() const { return value0b0_; }
    void SetPoliceRawValues(std::uint64_t at128, std::uint64_t at0b0) { value128_ = at128; value0b0_ = at0b0; }
    float GetAverageCoverage() const { return protection_.AverageProtection(); }
    float GetPercentCoverage() const { return protection_.CoverageShare(); }

    // protection hooks (called through the base in the original)
    bool ProtectionGetStationEffect(IPoliceOccupant* station, ProtectionEffect& effect);
    bool ProtectionGetDispatchEffect(IPoliceOccupant* station, ProtectionEffect& effect);
    void ProtectionEndOfCycle();

    void DoMonthlySimulation();
    void DoDailySimulation();
    cRZRandom& Random() { return random_; }

private:
    // IProtectionHost (the base's vslot hooks)
    bool GetStationEffect(IProtectionStation* station, ProtectionEffect& effect) override;
    void OnAdjustedCell(int x, int z, int radius) override { ProtectionProcessCell(x, z, radius); }
    void OnPassFinished() override { ProtectionEndOfCycle(); }
    void SetStationDispatchMask(IProtectionStation* station, std::uint32_t mask) override;
    void SetStationDispatchOffset(IProtectionStation* station, std::uint32_t offset) override;
    void GetStationInfo(IProtectionStation* station, int* available, std::uint32_t* offset, std::uint32_t* count) override;
    int MinimumDispatchesForService() override { return GetTotalDispatchCount(); }
    std::uint32_t DispatchPurpose() override { return 0; }
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

    IPoliceHost* host_;
    cRZRandom random_{0xffffffff};
    std::uint32_t refCount_ = 0;   // +0x110 (COM identity)
    PoliceTunables tunables_;
    cSC4ProtectionSimulator protection_;
    cSC4ResponseCurve stationEfficiency_, jailEfficiency_, strikeChance_, jailbreakChance_;
    std::list<IPoliceOccupant*> jails_, fundedJails_, jailbreakBuildings_;
    // a city file's references (power grid +0x128, the jails, dispatch helper +0x118): not resolved yet, kept as read
    cRetainedReference filePowerGrid_, fileDispatch_;
    cRetainedReferences fileFundedJails_, fileJails_;
    int jailCapacity_ = 0, jailPopulation_ = 0, criminals_ = 0;
    int arrestsPrev_ = 0, crimesPrev_ = 0, arrestsThisMonth_ = 0, crimesThisMonth_ = 0, unjailedAdded_ = 0;
    bool onStrike_ = false;
    int strikeStart_ = 0;
    std::uint64_t value128_ = 0, value0b0_ = 0;   // +0x128, +0xb0
};

}  // namespace sc4
