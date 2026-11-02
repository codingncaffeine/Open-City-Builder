// cSC4ResidentialSimulator (class id 0x4990c013) — residential population plus the two civic departments it
// owns, education (schools) and health (hospitals); the original object is ~0x650 bytes.
// The population half follows residential.md (OpenSC4 specs): Init / Shutdown, the 16-state monthly cycle (proximity maps,
// DoPopulationChange, PostPopulationChange, history), the EQ / HQ / LE queries and the version-7 city record. The education and
// health half follows residential-education-health.md (REH): building recognition and the group lists (4.1), departments and
// funding (4.2), ordinance effects (4.3), coverage maps (4.4), grading (4.5), the EQ / HQ boost (4.6), strikes (4.9), auto-budget
// (4.10), query data, ratings and totals (4.11) and the coverage history (4.12).
//
// tDepartmentData (REH 4.9, residential.md 9.1 department block): +0x00 the obsolete minimum funding (0.6, never used), +0x04 max
// funding (1.2), +0x08 strike efficiency (0.2), +0x0c strike duration in days (10), +0x10 min grade to end a strike (0.9, compared
// with the staff ratio), +0x18 funding to efficiency curve, +0x38 strike response curve, +0x58 funding to coverage curve, +0x78 on
// strike, +0x7c strike start day, +0x90 initial funding (0.25), +0x94 the 100.0 field, +0x98 message id (health 0x77accd2,
// education 0x77accd0).
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

class cSC4BudgetSimulator;
struct BudgetItem;

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

// A building the education/health half keeps in its lists: the building's cISC4Occupant, whose pointer is its identity in every
// list, message and saved reference (REH 4.1 RG15). The host hands out one adapter per occupant, so adapters compare as occupants.
class IResidentialOccupant {
public:
    virtual ~IResidentialOccupant() = default;
    virtual cISCPropertyHolder* PropertyHolder() = 0;
    virtual bool IsInGroup(std::uint32_t occupantGroup) = 0;     // IsOccupantGroup
    virtual std::uint32_t BuildingType() { return 0; }           // cISC4BuildingOccupant::GetBuildingType
    virtual void Position(float xyz[3]) { xyz[0] = xyz[1] = xyz[2] = 0.0f; }   // occupant slot 8 GetPosition
    // lot manager slot 14 GetOccupantLot -> lot slot 15 GetCenter (x, z = 8 (x1 + x2 + 1)); without a lot, Position (REH 4.4 RG5)
    virtual void LotCenter(float xyz[3]) { Position(xyz); }
    virtual void AddRef() {}                                     // the list's reference on the occupant
    virtual void Release() {}
    virtual cIGZUnknown* Object() { return nullptr; }            // what a saved list reference names
};

// The record the school / hospital query fills (REH 4.11; the original begins with a cRZString the queries do not fill).
struct ResidentialQueryData {
    std::uint32_t capacity = 0;            // +0x14
    std::uint32_t effectiveCapacity = 0;   // +0x18
    std::uint32_t staff = 0;               // +0x1c capacity / divisor
    std::uint32_t effectiveStaff = 0;      // +0x20 effective capacity / divisor (ceil for schools, floor for hospitals)
    std::uint32_t served = 0;              // +0x24 (0xffffffff -> 0 on success)
    float grade = 0.0f;                    // +0x28
    float staffFunding = 0.0f;             // +0x2c
    std::uint32_t staffCost = 0;           // +0x30 low 32 bits of the item's cost
    float vehicleFunding = 0.0f;           // +0x34
    std::uint32_t vehicleCost = 0;         // +0x38
};

class IResidentialHost {
public:
    virtual ~IResidentialHost() = default;
    virtual std::uint32_t CurrentDay() = 0;   // city.GetSimulator().GetSimDate().DayNumber()
    // strike messages (REH 5): begin carries the occupant, end carries the strike length in days
    virtual void PostStrikeMessage(std::uint32_t messageId, IResidentialOccupant* occupant, bool begin, std::uint32_t days) = 0;
    // automata controller manager: AddControllersFromOccupantGroup / RemoveControllers ("striking_education" / "striking_health")
    virtual void SetStrikerGroup(bool education, IResidentialOccupant* occupant, bool add) = 0;
    virtual bool CityHasOccupantManager() = 0;
    virtual std::uint8_t CellWorkforce(unsigned x, unsigned z) = 0;      // vslot 0xe8
    virtual std::uint8_t GlobalWorkforce() = 0;                          // vslot 0x110
    virtual float CellLifeExpectancyBase(unsigned x, unsigned z) = 0;    // vslot 0x120

    // ---- the education/health half ----
    // The building behind an occupant-inserted / removed message's void1 or a saved list entry; null when it is no building.
    virtual IResidentialOccupant* Occupant(cIGZUnknown* /*object*/) { return nullptr; }
    virtual cSC4BudgetSimulator* BudgetSimulator() { return nullptr; }   // city.GetBudgetSimulator()
    // GetMiscProperties() of every enacted ordinance, in ascending id order (GetOrdinanceOnArray + GetOrdinanceByID)
    virtual void EnactedOrdinances(std::vector<const cISCPropertyHolder*>& /*out*/) {}

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
    float minimumFunding = 0;          // +0x00 (OBS) min acceptable funding, loaded and never used
    float maxFunding = 0;              // +0x04
    float strikeEfficiency = 0;        // +0x08
    std::uint32_t strikeDays = 0;      // +0x0c strike duration, compared in days
    float minGradeToEnd = 0;           // +0x10 compared with the staff ratio
    cSC4ResponseCurve efficiencyCurve; // +0x18 funding to efficiency
    cSC4ResponseCurve strikeCurve;     // +0x38 strike response (grade -> chance in percent)
    cSC4ResponseCurve coverageCurve;   // +0x58 funding to coverage
    bool onStrike = false;             // +0x78
    std::uint32_t strikeStartDay = 0;  // +0x7c
    float initialFunding = 0, value94 = 0;
    std::uint32_t messageId = 0;       // +0x98
};

// One ordinance's effect on a service (REH 4.3): the factors default to 1.0 and the curves to (0,100)(100,100).
struct OrdinanceEffect {
    float radiusFactor = 1.0f, effectFactor = 1.0f, capacityFactor = 1.0f, decayFactor = 1.0f;
    cSC4ResponseCurve distanceCurve, ageCurve;
};

// The primary view the city holds for this simulator (cISC4ResidentialSimulator, iid 0x077ac1ee).
class cISC4ResidentialSimulator : public cIGZUnknown {};

class cSC4ResidentialSimulator : public cISC4ResidentialSimulator,
                                 public cIGZMessageTarget2,
                                 public cIGZSerializable {
public:
    using OccupantList = std::list<IResidentialOccupant*>;
    using TractList = std::vector<std::pair<std::int32_t, std::int32_t>>;

    cSC4ResidentialSimulator(IResidentialHost* host, cRZRandom* random);

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4ResidentialSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    bool DoMessage(cIGZMessage2* message) override;

    // Loads defaults and then the simulator exemplar {0x6534284a, 0xe7e2c2db, 0x69189c84}; null = defaults (REH 4.13).
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);

    DepartmentData& Health() { return health_; }       // +0x160
    DepartmentData& Education() { return education_; } // +0x200

    // ---- buildings (REH 4.1, residential.md 5.8) ----
    void DoBuildingChanged(IResidentialOccupant* building, bool added);
    void DistributePendingBuildings();   // the list moves of state 0
    std::vector<OccupantList>& SchoolLists() { return schools_; }
    std::vector<OccupantList>& HospitalLists() { return hospitals_; }
    OccupantList& PendingSchools() { return pendingSchools_; }
    OccupantList& PendingHospitals() { return pendingHospitals_; }
    OccupantList& HealthAffecting() { return healthAffecting_; }
    OccupantList& PendingHealthAffecting() { return pendingHealthAffecting_; }
    int GetSchoolBuildingCount() const;     // slot 46
    int GetHospitalBuildingCount() const;   // slot 48
    void GetSchoolBuildings(OccupantList& out, const std::vector<std::uint32_t>* groups) const;     // AddRef each
    void GetHospitalBuildings(OccupantList& out, const std::vector<std::uint32_t>* groups) const;
    void GetHealthAffectingBuildings(OccupantList& out) const;   // slot 61

    // ---- strikes (REH 4.9) ----
    bool SchoolIsOnStrike() const { return education_.onStrike; }
    bool HealthIsOnStrike() const { return health_.onStrike; }
    float GetChanceOfStrike(DepartmentData& dept, const OccupantList& buildings, std::uint32_t gradeProperty, std::uint32_t strikeGroup);
    float ChanceOfSchoolStrike();
    float ChanceOfHealthStrike();
    bool BeginStrike(DepartmentData& dept, IResidentialOccupant* occupant);
    bool EndStrike(DepartmentData& dept, std::uint32_t days);
    bool EndSchoolStrike();
    bool EndHealthStrike();
    // true when the occupant's striking state changed
    bool UpdateStrikingState(IResidentialOccupant* occupant, DepartmentData& dept, std::uint32_t staffProperty, std::uint32_t gradeProperty);
    void DoMonthlyStrikeCheck(DepartmentData& dept, std::vector<OccupantList>& lists, std::uint32_t staffProperty,
                              std::uint32_t gradeProperty, std::uint32_t strikeGroup);

    // ---- funding (REH 4.2, 4.10, 4.11) ----
    void SetOccupantFundingPercentages(IResidentialOccupant* occupant, float staff, float vehicle, bool school);   // slot 19
    void GetOccupantFundingPercentages(IResidentialOccupant* occupant, float& staff, float& vehicle, bool school);  // slot 20
    void GetOccupantEfficiencyRatings(IResidentialOccupant* occupant, float& e1, float& e2, bool school, bool ignoreStrike);
    float GetBaseStaffEfficiency(IResidentialOccupant* occupant, bool school);
    bool EstimateIdealFunding(IResidentialOccupant* occupant, float& funding);   // slot 58
    bool EstimateCurrentOccupantCapacity(IResidentialOccupant* occupant, std::uint32_t& capacity, std::uint32_t& staff);   // slot 28
    bool GetOccupantCoverage(IResidentialOccupant* occupant, float level, float& distance, float& radius);   // slot 45
    bool FundingChanged() const { return fundingChanged_; }
    static bool GetAutoBudget(const IResidentialOccupant* occupant);
    static void SetAutoBudget(IResidentialOccupant* occupant, bool value);
    bool GetGlobalAutoBudgetForSchools() const { return autoBudgetSchools_; }
    void SetGlobalAutoBudgetForSchools(bool v) { autoBudgetSchools_ = v; }
    bool GetGlobalAutoBudgetForHospitals() const { return autoBudgetHospitals_; }
    void SetGlobalAutoBudgetForHospitals(bool v) { autoBudgetHospitals_ = v; }

    // ---- coverage and grading (REH 4.3 - 4.6) ----
    void GetOrdinanceEffects();
    const std::vector<OrdinanceEffect>& OrdinanceEffects(bool school) const { return school ? schoolEffects_ : healthEffects_; }
    void UpdateCoverageMap(bool school, std::size_t group);    // states 5 and 7, one group
    void GradeBuildings(bool school);                          // states 6 and 8
    void ComputeOccupantGrade(IResidentialOccupant* occupant, bool school, std::size_t group);
    cSC4SimGrid<std::uint8_t>* GetCoverageMap(bool school, std::size_t group) const {
        const auto& v = school ? schoolCoverage_ : hospitalCoverage_;
        return group < v.size() ? v[group].get() : nullptr;
    }
    std::int32_t ServedCount(bool school) const { return school ? schoolCovered_ : hospitalCovered_; }

    // ---- query data, totals, ratings (REH 4.11) ----
    bool GetSchoolQueryData(IResidentialOccupant* occupant, ResidentialQueryData& out) { return QueryData(occupant, true, out); }     // 26
    bool GetHospitalQueryData(IResidentialOccupant* occupant, ResidentialQueryData& out) { return QueryData(occupant, false, out); }  // 27
    // Sums the query data of every group-list building in at least one of `groups` (null = none) and returns the count.
    int GetSchoolSystemTotals(ResidentialQueryData& totals, const std::vector<std::uint32_t>* groups) { return SystemTotals(true, totals, groups); }
    int GetHospitalSystemTotals(ResidentialQueryData& totals, const std::vector<std::uint32_t>* groups) { return SystemTotals(false, totals, groups); }
    float GetSchoolSystemRating() { return SystemRating(true); }    // slot 12
    float GetHealthSystemRating() { return SystemRating(false); }   // slot 13

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
        float newEQ[3] = {4.0f, 6.0f, 8.0f}, newHQ[3] = {20.0f, 40.0f, 60.0f};   // newHQ: the shipped values (REH 4.13 choice)
        float deathsDoorHQ = 0.0f, deathsDoorHeight = 0.0f, deathsDoorDelta = 0.0f, deathsDoorChance = 0.0f;
        cSC4ResponseCurve air, water, garbage;
    };
    void ReadTunablePopulationValues(const cISCPropertyHolder* exemplar);
    const PopulationTuning& Tuning() const { return tuning_; }
    void DoPopulationChange(int x, int z);   // one tract of state 4
    void PostPopulationChange();             // state 12
    std::uint64_t LifeExpectancySum() const { return leSum_; }
    int RunningPopulation() const { return runningPopulation_; }
    bool ReadCityRecord(cIGZIStream& stream);   // the version-7 record (residential.md 9.1)
    bool WriteCityRecord(cIGZOStream& stream) const;

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

    // ---- tunables kept outside the departments (REH 4.13) ----
    std::array<cSC4ResponseCurve, 9> wealthCurves;     // +0x400, props 0xea540621..29
    std::uint32_t schoolStaffDivisor = 30, hospitalStaffDivisor = 30;   // 0x8a891421 / 0x8a89142a
    float maxEQBoost = 10.0f, maxHQBoost = 10.0f;      // 0x4aa84161 / 0x2aa84164, loaded and never read
    float coverageSlop = 1.1f;                         // 0x6a9f6edd
    std::vector<std::uint32_t> schoolChunks, hospitalChunks;   // 0xec43826b / 0xac43826d

private:
    struct CurveSet { cSC4ResponseCurve age, wealth, distance; };
    void LoadBuildingCurves(IResidentialOccupant* occupant, bool school, CurveSet& curves) const;
    void OrdinanceModifiers(bool school, float& radius, float& capacity, float& effect) const;
    std::uint32_t Chunk(bool school, std::size_t group) const;
    void GetCoveredTractList(IResidentialOccupant* occupant, float radius, float tractWidth, std::int32_t countX, std::int32_t countZ,
                             TractList& tracts) const;
    std::uint32_t GetPopulationServed(IResidentialOccupant* occupant, float radius, std::uint32_t capacity, const CurveSet& curves,
                                      bool school, const cSC4SimGrid<std::uint8_t>& coverage, std::uint32_t chunk, const TractList& tracts) const;
    void AddRadiusEffect(cSC4SimGrid<float>& target, int wealth, const std::vector<const cSC4ResponseCurve*>& ageCurves, float px, float pz,
                         float radius, const std::vector<const cSC4ResponseCurve*>& distanceCurves, float value, float maximum,
                         std::uint32_t units, const cSC4SimGrid<std::uint8_t>& coverage, float& total) const;
    bool FindBudgetItem(cISCPropertyHolder* holder, std::uint32_t purpose, BudgetItem& item) const;
    bool RegisterWithDepartment(IResidentialOccupant* building, bool added, std::uint32_t staffPurpose, std::uint32_t vehiclePurpose);
    bool QueryData(IResidentialOccupant* occupant, bool school, ResidentialQueryData& out);
    int SystemTotals(bool school, ResidentialQueryData& totals, const std::vector<std::uint32_t>* groups);
    float SystemRating(bool school);
    void CreateDepartments();
    void AdoptFileLists();
    void ReleaseBuildingLists();
    static void SetDefaultCurves(DepartmentData& d);

    IResidentialHost* host_;
    cRZRandom* random_;
    std::uint32_t refCount_ = 0;   // +0x1c (COM identity)
    DepartmentData health_, education_;
    std::vector<OccupantList> schools_, hospitals_;
    OccupantList pendingSchools_, pendingHospitals_, healthAffecting_, pendingHealthAffecting_;
    std::vector<OrdinanceEffect> schoolEffects_, healthEffects_;
    float schoolGradeTotal_ = 0.0f, hospitalGradeTotal_ = 0.0f;   // +0x314 / +0x318, never read
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
    std::vector<SimGridPtr<std::uint8_t>> schoolCoverage_, hospitalCoverage_;   // 7 / 3
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
    // The departments' two references each (health 0x09188f42 / 0xaa538cb3, education 0x09188f4c / 0x4a538cc6): read from the
    // record, then the budget simulator's own departments from Init on.
    cRetainedReference departmentRefs_[4];
    // The occupant lists as a record holds them until Init adopts them (pending school, pending hospital, pending
    // health-affecting; the 7 school lists; the 3 hospital lists; the health-affecting list).
    std::vector<cRetainedReferences> fileLists_;
};

}  // namespace sc4
