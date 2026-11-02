// cSC4ResidentialSimulator (class id 0x4990c013) — residential population plus the two civic departments it
// owns, education (schools) and health (hospitals); the original object is ~0x650 bytes.
// Recovered here: the department model (tDepartmentData at +0x160 health / +0x200 education, 0xa0 bytes each),
// its tunables, the monthly strike machinery (chance, begin, end, per-occupant striking state), the occupant
// funding percentages and auto budget flags, building lists / counts, system ratings and the small queries.
// The population half follows residential.md (OpenSC4 specs): Init / Shutdown, the 16-state monthly cycle (proximity maps,
// DoPopulationChange, PostPopulationChange, history), the EQ / HQ / LE queries and the version-7 city record. Not yet: the
// education/health passes of states 0 and 5-8 (building lists, coverage, grading, ordinance effects).
//
// tDepartmentData layout: +0x00 minimum funding (0.6 default), +0x04 strike
// threshold (1.2), +0x08 (0.2), +0x0c strike length in days (10), +0x10 funding that ends a strike (0.9),
// +0x18 staff-efficiency curve, +0x38 strike chance curve, +0x58 third curve, +0x78 on strike, +0x7c strike
// start day, +0x90/+0x94 (0.25 / 100 defaults), +0x98 message id (health 0x77accd2, education 0x77accd0).
#pragma once
#include <array>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/RetainedReference.h"
#include "sc4/SimStreamOps.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/cSC4SimGrid.h"

class cIGZIStream;
class cIGZOStream;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4ResidentialSimulator = 0x4990c013;
constexpr std::uint32_t kGZIID_cISC4ResidentialSimulator = 0x077ac1ee;
constexpr std::uint32_t kMessageHealthStrike = 0x077accd2, kMessageEducationStrike = 0x077accd0;
constexpr std::uint32_t kResidentialStrikeStartDate = 0xeaa69f83;   // shared with the police simulator
constexpr std::uint32_t kOccupantAutoBudget = 0x6c05fc4d;

// A visual effect the population pass places (the "buzzards" over sick tracts): transform, Start(0), Release.
class IResidentialEffect {
public:
    virtual ~IResidentialEffect() = default;
    virtual void StartAt(float x, float y, float z) = 0;   // places, starts and releases the effect
};

class IResidentialOccupant {
public:
    virtual ~IResidentialOccupant() = default;
    virtual cISCPropertyHolder* PropertyHolder() = 0;
    virtual bool IsInGroup(std::uint32_t occupantGroup) = 0;     // vslot 0xb0
};

// Result of querying one school or hospital (the original starts with a cRZString for the building name).
struct ResidentialQueryData {
    std::string name;
    int field28 = 0, served = 0, field30 = 0, field34 = 0, field38 = 0;   // +0x28.. (served is +0x2c)
    float field3c = 0.0f;
};

// Department budget entry of an occupant (the budget simulator hands out a record per department).
class IBudgetRecord {
public:
    virtual ~IBudgetRecord() = default;
    virtual bool HasExplicitFunding(cISCPropertyHolder* holder, std::uint32_t propertyId) = 0;
    virtual float DefaultFunding() = 0;
    virtual void SetFunding(cISCPropertyHolder* holder, const SC4Percentage& value, std::uint32_t propertyId) = 0;
};

class IResidentialHost {
public:
    virtual ~IResidentialHost() = default;
    virtual std::uint32_t CurrentDay() = 0;
    // strike notification: begin carries the occupant, end carries the strike length in days
    virtual void PostStrikeMessage(std::uint32_t messageId, IResidentialOccupant* occupant, bool begin, std::uint32_t days) = 0;
    virtual void SetStrikerGroup(bool education, IResidentialOccupant* occupant, bool add) = 0;
    virtual bool CityHasOccupantManager() = 0;
    // does the budget simulator track (occupant, funding property)?
    virtual bool IsBudgeted(IResidentialOccupant* occupant, std::uint32_t propertyId) = 0;
    // the department record behind a budgeted property; may be null
    virtual IBudgetRecord* GetBudgetRecord(IResidentialOccupant* occupant, std::uint32_t propertyId) = 0;
    virtual void GetSchoolQueryData(IResidentialOccupant* occupant, ResidentialQueryData& out) = 0;
    virtual void GetHospitalQueryData(IResidentialOccupant* occupant, ResidentialQueryData& out) = 0;
    virtual void GetOccupantEfficiencyRatings(IResidentialOccupant* occupant, float& staff, bool education) = 0;
    virtual std::uint8_t CellWorkforce(unsigned x, unsigned z) = 0;      // vslot 0xe8
    virtual std::uint8_t GlobalWorkforce() = 0;                          // vslot 0x110
    virtual float CellLifeExpectancyBase(unsigned x, unsigned z) = 0;    // vslot 0x120
    // ---- stream seams: occupants / per-department grids reached through the host ----
    virtual void WriteOccupant(cIGZOStream&, IResidentialOccupant*) {}   // SC4WriteGZSerializable(stream, occupant)
    virtual bool ReadOccupant(cIGZIStream&, IResidentialOccupant**) { return false; }
    virtual void WriteDepartmentGrid(cIGZOStream&) {}                    // int16 grid (+0x80/+0x88 of tDepartmentData)
    virtual bool ReadDepartmentGrid(cIGZIStream&) { return false; }

    // ---- the population half (Init, the monthly cycle, the population pass) ----
    // Init steps 1-3: the city's services (BDS, occupant manager, developers, demand node 0x1000, message server, budget); false
    // stops Init as the original does without a demand node or a message server.
    virtual bool AcquireServices() { return false; }
    virtual void ReleaseServices() {}
    virtual void AddAgent(cIGZMessageTarget2*, int /*level*/, const char* /*name*/, std::uint32_t /*flags*/) {}
    virtual void RemoveAgent(cIGZMessageTarget2*) {}
    virtual void AddNotification(cIGZMessageTarget2*, std::uint32_t /*message*/) {}
    virtual void RemoveNotification(cIGZMessageTarget2*, std::uint32_t /*message*/) {}
    virtual void CreateHistory(std::uint32_t /*id*/, std::uint32_t /*length*/) {}
    virtual void RecordHistory(std::uint32_t /*id*/, std::int32_t /*value*/) {}    // message 0x89efa536 sent
    virtual const cISCPropertyHolder* SimulatorExemplar() { return nullptr; }   // {0x6534284a, 0xe7e2c2db, 0x69189c84}
    // growth developer 0x1010 / 0x1020 / 0x1030 (wealth 1..3) slot 15 GetPopulationMap
    virtual cSC4SimGrid<std::int16_t>* DeveloperPopulationMap(int /*wealth*/) { return nullptr; }
    virtual std::uint16_t PollutionValue(int /*type*/, int /*cellX*/, int /*cellZ*/) { return 0; }   // pollution slot 6
    virtual const cSC4SimGrid<std::uint8_t>* BuildingMap() { return nullptr; }  // BDS slot 41
    virtual std::uint8_t BuildingMapKey(int wealth) { return std::uint8_t((1 << 2) + wealth + 1); }   // BDS slot 42 (purpose 1)
    virtual int CurrentMonth() { return 1; }                                    // simulator GetSimDate, month only
    // effects manager slot 7. false = the create failed (one random draw); true with a null effect = created but nothing can show it.
    virtual bool CreateEffect(const char* /*name*/, IResidentialEffect** out) { *out = nullptr; return true; }
    virtual float TerrainAltitude(float /*x*/, float /*z*/) { return 0.0f; }
    virtual std::uint64_t BeginSlice(std::uint32_t /*milliseconds*/) { return 0; }
    virtual bool SliceExpired(std::uint64_t) { return false; }
};

struct DepartmentData {
    // zero-initialised like tDepartmentData::tDepartmentData; ReadSimulatorTunableValues installs the defaults
    float minimumFunding = 0;          // +0x00
    float strikeThreshold = 0;         // +0x04
    float value08 = 0;                 // +0x08
    std::uint32_t strikeDays = 0;      // +0x0c
    float strikeEndFunding = 0;        // +0x10
    cSC4ResponseCurve staffCurve;      // +0x18
    cSC4ResponseCurve strikeChance;    // +0x38
    cSC4ResponseCurve thirdCurve;      // +0x58
    bool onStrike = false;             // +0x78
    std::uint32_t strikeStartDay = 0;  // +0x7c
    float value90 = 0, value94 = 0;
    std::uint32_t messageId = 0;       // +0x98
};

// The primary view the city holds for this simulator (cISC4ResidentialSimulator, iid 0x077ac1ee).
class cISC4ResidentialSimulator : public cIGZUnknown {};

class cSC4ResidentialSimulator : public cISC4ResidentialSimulator,
                                 public cIGZMessageTarget2,
                                 public cIGZSerializable {
public:
    cSC4ResidentialSimulator(IResidentialHost* host, cRZRandom* random);

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4ResidentialSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    bool DoMessage(cIGZMessage2* message) override;

    // Loads defaults and then the simulator exemplar {0x6534284a, 0xe7e2c2db, 0x69189c84}; null = defaults.
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);

    DepartmentData& Health() { return health_; }       // +0x160
    DepartmentData& Education() { return education_; } // +0x200

    // ---- lists (7 school lists, 3 hospital lists, 2 extra lists, health-affecting lists) ----
    std::vector<std::list<IResidentialOccupant*>>& SchoolLists() { return schools_; }
    std::vector<std::list<IResidentialOccupant*>>& HospitalLists() { return hospitals_; }
    std::list<IResidentialOccupant*>& ExtraSchools() { return extraSchools_; }      // +0x2d0
    std::list<IResidentialOccupant*>& ExtraHospitals() { return extraHospitals_; }  // +0x2e8
    std::list<IResidentialOccupant*>& HealthAffecting1() { return healthAffecting1_; }  // +0x348
    std::list<IResidentialOccupant*>& HealthAffecting2() { return healthAffecting2_; }  // +0x300
    int GetSchoolBuildingCount() const;
    int GetHospitalBuildingCount() const;
    // filtered copy: with a filter only occupants in at least one of the groups are appended
    static void GetBuildings(const std::list<IResidentialOccupant*>& source, std::list<IResidentialOccupant*>& out,
                             const std::vector<std::uint32_t>* groups);
    void GetSchoolBuildings(std::list<IResidentialOccupant*>& out, const std::vector<std::uint32_t>* groups) const;
    void GetHospitalBuildings(std::list<IResidentialOccupant*>& out, const std::vector<std::uint32_t>* groups) const;
    void GetHealthAffectingBuildings(std::list<IResidentialOccupant*>& out) const;

    // ---- strikes ----
    bool SchoolIsOnStrike() const { return education_.onStrike; }
    bool HealthIsOnStrike() const { return health_.onStrike; }
    float GetChanceOfStrike(DepartmentData& dept, const std::list<IResidentialOccupant*>& buildings,
                            std::uint32_t fundingProperty, std::uint32_t groupFilter);
    float ChanceOfSchoolStrike();
    float ChanceOfHealthStrike();
    bool BeginStrike(DepartmentData& dept, IResidentialOccupant* occupant);
    bool EndStrike(DepartmentData& dept, std::uint32_t days);
    bool EndSchoolStrike();
    bool EndHealthStrike();
    // true when the occupant's striking state changed
    bool UpdateStrikingState(IResidentialOccupant* occupant, DepartmentData& dept, std::uint32_t strikingFundingProperty,
                             std::uint32_t idleFundingProperty);
    void DoMonthlyStrikeCheck(DepartmentData& dept, std::vector<std::list<IResidentialOccupant*>>& lists,
                              std::uint32_t strikingFundingProperty, std::uint32_t idleFundingProperty, std::uint32_t groupFilter);

    // ---- funding ----
    void SetOccupantFundingPercentages(IResidentialOccupant* occupant, const SC4Percentage& staff, const SC4Percentage& budget, bool education);
    void GetRawOccupantFundingPercentages(IResidentialOccupant* occupant, SC4Percentage& staff, SC4Percentage& budget, bool education);
    void GetOccupantFundingPercentages(IResidentialOccupant* occupant, SC4Percentage& staff, SC4Percentage& budget, bool education) {
        GetRawOccupantFundingPercentages(occupant, staff, budget, education);
    }
    float GetBaseStaffEfficiency(IResidentialOccupant* occupant, bool education);
    bool ReadCityRecord(cIGZIStream& stream);   // the version-7 record (residential.md 9.1)
    bool WriteCityRecord(cIGZOStream& stream) const;
    bool FundingChanged() const { return fundingChanged_; }
    static bool GetAutoBudget(const IResidentialOccupant* occupant);
    static void SetAutoBudget(IResidentialOccupant* occupant, bool value);
    bool GetGlobalAutoBudgetForSchools() const { return autoBudgetSchools_; }
    void SetGlobalAutoBudgetForSchools(bool v) { autoBudgetSchools_ = v; }
    bool GetGlobalAutoBudgetForHospitals() const { return autoBudgetHospitals_; }
    void SetGlobalAutoBudgetForHospitals(bool v) { autoBudgetHospitals_ = v; }

    // ---- system totals / ratings ----
    // Sums the query data of every building (optionally only those in one of `groups`); returns the count.
    int GetSystemTotals(const std::vector<std::list<IResidentialOccupant*>>& lists, ResidentialQueryData& totals, bool education,
                        const std::vector<std::uint32_t>& groups);
    int GetSchoolSystemTotals(ResidentialQueryData& totals, const std::vector<std::uint32_t>& groups) { return GetSystemTotals(schools_, totals, true, groups); }
    int GetHospitalSystemTotals(ResidentialQueryData& totals, const std::vector<std::uint32_t>& groups) { return GetSystemTotals(hospitals_, totals, false, groups); }
    float GetHealthSystemRating();
    float GetSchoolSystemRating();

    // ---- the population half (residential.md: Init 4.1, Shutdown 4.2, the monthly cycle 5, queries 6, tuning 7.1) ----
    bool Init();
    bool Shutdown();
    // One step of the 16-state monthly cycle; false when a time-sliced state ran out of time (the day is delivered again).
    bool DoDailySimulation(std::uint32_t milliseconds);
    std::uint32_t State() const { return state_; }
    cSC4SimGrid<std::uint8_t>* GetProximityMap(std::uint32_t wealth) const { return wealth - 1 < 3 ? proximity_[wealth - 1].get() : nullptr; }
    cSC4SimGrid<std::uint16_t>* GetPopulationGrid(int wealth) const { return populationGrids_[wealth].get(); }   // 0..2
    cSC4SimGrid<float>* GetEQGrid(int wealth) const { return eq_[wealth].get(); }
    cSC4SimGrid<float>* GetHQGrid(int wealth) const { return hq_[wealth].get(); }
    cSC4SimGrid<std::uint8_t>* GetAgeGrid() const { return ageGrid_.get(); }                                   // slot 41
    cSC4SimGrid<float>* GetGradeMap(bool school) const { return school ? schoolGrade_.get() : hospitalGrade_.get(); }   // 39 / 40
    float GetCellEQ(unsigned x, unsigned z) const;    // slots 35 / 36: cell coordinates
    float GetCellHQ(unsigned x, unsigned z) const;
    float GetCellEQByWealth(unsigned x, unsigned z, int wealth) const;
    float GetCellHQByWealth(unsigned x, unsigned z, int wealth) const;
    float GetGlobalEQ() const;
    float GetGlobalHQ() const;
    std::uint8_t GetCellLifeExpectancy(unsigned x, unsigned z);   // slot 29 (the tuning exemplar from the host)
    std::uint8_t GetGlobalLE();                                   // slot 34
    // The population tuning (residential.md 7.1).
    struct PopulationTuning {
        float eqDecay = 1.0f, hqDecay = 1.0f;
        std::int16_t newAge[3] = {25, 30, 35};
        std::uint8_t resetAge = 40;
        float eqReset = 0.7f, hqReset = 0.7f;
        float newEQ[3] = {4.0f, 6.0f, 8.0f}, newHQ[3] = {0.0f, 0.0f, 0.0f};
        float deathsDoorHQ = 0.0f, deathsDoorHeight = 0.0f, deathsDoorDelta = 0.0f, deathsDoorChance = 0.0f;
        cSC4ResponseCurve air, water, garbage;
    };
    void ReadTunablePopulationValues(const cISCPropertyHolder* exemplar);
    const PopulationTuning& Tuning() const { return tuning_; }
    void DoPopulationChange(int x, int z);   // one tract of state 4
    void PostPopulationChange();             // state 12
    std::uint64_t LifeExpectancySum() const { return leSum_; }
    int RunningPopulation() const { return runningPopulation_; }

    // ---- misc queries ----
    int GetPopulation() const { return population_; }
    void SetPopulation(int p) { population_ = p; }
    static constexpr int GetMaxEQ() { return 200; }
    static constexpr int GetMaxHQ() { return 200; }
    static constexpr int GetTotalCityEducationUpkeepCost() { return 0; }
    static constexpr int GetTotalCityHealthUpkeepCost() { return 0; }
    static std::uint8_t GetAverageNewAgeByWealth(const cISCPropertyHolder* exemplar, int wealth);
    float GetCellWorkforcePercent(const cISCPropertyHolder* exemplar, unsigned x, unsigned z);
    float GetGlobalWorkforcePercent(const cISCPropertyHolder* exemplar);
    std::uint32_t GetCellLifeExpectancy(const cISCPropertyHolder* exemplar, unsigned x, unsigned z);

    // ---- tunables kept outside the departments ----
    std::array<cSC4ResponseCurve, 9> wealthCurves;     // +0x400, props 0xea540621..29
    std::uint32_t value520 = 30, value524 = 30;        // 0x8a891421 / 0x8a89142a
    float value560 = 10.0f, value564 = 10.0f, value568 = 1.1f;
    std::vector<std::uint32_t> list528, list540;       // 0xec43826b / 0xac43826d

private:
    float RatingFor(const std::vector<std::list<IResidentialOccupant*>>& lists, bool education);
    static void SetDefaultCurves(DepartmentData& d, const std::vector<SC4Point>& a, const std::vector<SC4Point>& b);

    IResidentialHost* host_;
    cRZRandom* random_;
    std::uint32_t refCount_ = 0;   // +0x1c (COM identity)
    DepartmentData health_, education_;
    std::vector<std::list<IResidentialOccupant*>> schools_, hospitals_;
    std::list<IResidentialOccupant*> extraSchools_, extraHospitals_, healthAffecting1_, healthAffecting2_;
    int population_ = 0;
    bool fundingChanged_ = true;   // the record's dirty flag
    bool autoBudgetSchools_ = false, autoBudgetHospitals_ = false;

    // ---- population half ----
    void CreateGrids();
    void ReleaseGrids();
    void ResetIterator();
    bool NextTract(std::int32_t& x, std::int32_t& z);
    void SeedProximity(int wealth);
    bool ProximityPass();                         // one distance pass over the work grid; true when a value changed
    void CopyProximity(int wealth);
    float TractQuotient(const SimGridPtr<float> (&values)[3], int x, int z) const;
    float GlobalQuotient(const SimGridPtr<float> (&values)[3]) const;
    void RecordHistoryData(std::uint32_t id, std::int32_t value) { if (host_) host_->RecordHistory(id, value); }

    bool initialized_ = false, cityStarted_ = false, tuningLoaded_ = false;
    std::int32_t proximityTractSize_ = 1, populationTractSize_ = 4, eqTractSize_ = 4;
    SimGridPtr<std::uint8_t> proximity_[3];
    SimGridPtr<std::uint16_t> populationGrids_[3];
    SimGridPtr<std::uint8_t> ageGrid_;
    std::vector<SimGridPtr<std::uint8_t>> schoolCoverage_, hospitalCoverage_;   // 7 / 3 (education-health half)
    SimGridPtr<float> schoolGrade_, hospitalGrade_;
    SimGridPtr<float> eq_[3], hq_[3];
    SimGridPtr<std::uint8_t> work_;               // the temporary proximity grid (states 13 -> 3), not saved
    float unused98_ = 0.0f, unused9c_ = 0.0f;
    std::uint32_t state_ = 0;
    int runningPopulation_ = 0;
    std::uint64_t leSum_ = 0;
    ScanningIterator iterator_;
    std::int32_t ageCount_[20] = {}, ageEqSum_[20] = {};
    std::int32_t schoolCovered_ = 0, hospitalCovered_ = 0;
    PopulationTuning tuning_;
    // What a city file holds that the education/health half does not model yet: the departments' two references each and the
    // occupant lists (pending school, pending hospital, pending health-affecting; the school and hospital list vectors; the
    // health-affecting list), kept as read and written back.
    cRetainedReference departmentRefs_[4];
    std::vector<cRetainedReferences> fileLists_;
    std::uint32_t fileSchoolListCount_ = 7, fileHospitalListCount_ = 3;
};

}  // namespace sc4
