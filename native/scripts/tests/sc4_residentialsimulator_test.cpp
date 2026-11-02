// The residential simulator's education and health half (residential-education-health.md, OpenSC4 specs): building recognition
// and the department registration (4.1), funding and efficiency (4.2), ordinance effects (4.3), coverage (4.4), grading (4.5), the
// EQ / HQ boost with its z-centre quirk (4.6), strikes (4.9), auto-budget (4.10), query data, ratings and totals (4.11). The budget
// is the real cSC4BudgetSimulator.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>

#include "sc4/BudgetSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/SCProperty.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

class FakeCity : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 16; }
    std::int32_t CitySizeZInCells() const override { return 16; }
    float CellSizeXMeters() const override { return 16.0f; }   // P = 4: 4 x 4 tracts of 64 m
    float CellSizeZMeters() const override { return 16.0f; }
};

class Env final : public IBudgetSimEnvironment {
public:
    void SimulatorDate(int& y, int& m) override { y = 1; m = 1; }
    std::int64_t OrdinanceMonthlyIncome() override { return 0; }
    std::int64_t OrdinanceMonthlyExpense() override { return 0; }
    std::int64_t NeighborsIncome() override { return 0; }
    std::int64_t NeighborsExpense() override { return 0; }
    int DifficultyLevel() override { return 0; }
    void ForEachTaxableLot(const std::function<void(const LotTaxInfo&)>&) override {}
    void PostMessage(std::uint32_t, std::uint32_t, std::int64_t, std::int64_t) override {}
    void AddHistory(std::uint32_t, std::int64_t) override {}
    void PostBudgetMessage(std::uint32_t, cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override {}
    void NotifyLocalFundingChanged(cISCPropertyHolder*) override {}
    std::string LoadLocalizedString(std::uint32_t, std::uint32_t) override { return "name"; }
    std::string BuildingName(std::uint32_t) override { return "building"; }
};

struct Building : IResidentialOccupant {
    cSCPropertyHolder* props = new cSCPropertyHolder();
    std::vector<std::uint32_t> groups;
    std::uint32_t type = 0x7777;
    float position[3] = {0, 0, 0}, centre[3] = {0, 0, 0};
    int refs = 0;
    Building() { props->AddRef(); }
    ~Building() override { props->Release(); }
    cISCPropertyHolder* PropertyHolder() override { return props; }
    bool IsInGroup(std::uint32_t g) override { return std::find(groups.begin(), groups.end(), g) != groups.end(); }
    std::uint32_t BuildingType() override { return type; }
    void Position(float xyz[3]) override { std::copy(position, position + 3, xyz); }
    void LotCenter(float xyz[3]) override { std::copy(centre, centre + 3, xyz); }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void At(float x, float z) { position[0] = centre[0] = x; position[2] = centre[2] = z; }
    template <class T> void Set(std::uint32_t id, T v) { cSCPropertyHelp::SetPropertyValue(props, id, v); }
    template <class T> T Get(std::uint32_t id, T fallback = T{}) { T v = fallback; cSCPropertyHelp::GetPropertyValue(props, id, v); return v; }
    // budget items: departments, lines, purpose ids, costs
    void Items(std::vector<std::uint32_t> departments, std::vector<std::uint32_t> lines, std::vector<std::uint32_t> purposes,
               std::vector<std::int64_t> costs) {
        Set(kBudgetItemPropDepartments, departments);
        Set(kBudgetItemPropLineItems, lines);
        Set(kBudgetItemPropReserved, purposes);
        Set(kBudgetItemPropCosts, costs);
    }
};

struct Host : IResidentialHost {
    cSC4BudgetSimulator* budget = nullptr;
    cSCPropertyHolder* exemplar = nullptr;
    std::vector<const cISCPropertyHolder*> ordinances;
    std::uint32_t day = 100;
    struct Msg { std::uint32_t id; bool begin; std::uint32_t days; };
    std::vector<Msg> messages;
    std::vector<std::array<int, 2>> strikers;   // {education, add}
    std::uint32_t CurrentDay() override { return day; }
    void PostStrikeMessage(std::uint32_t id, IResidentialOccupant*, bool begin, std::uint32_t days) override { messages.push_back({id, begin, days}); }
    void SetStrikerGroup(bool education, IResidentialOccupant*, bool add) override { strikers.push_back({education, add}); }
    bool CityHasOccupantManager() override { return true; }
    std::uint8_t CellWorkforce(unsigned, unsigned) override { return 0; }
    std::uint8_t GlobalWorkforce() override { return 0; }
    float CellLifeExpectancyBase(unsigned, unsigned) override { return 0.0f; }
    cSC4BudgetSimulator* BudgetSimulator() override { return budget; }
    void EnactedOrdinances(std::vector<const cISCPropertyHolder*>& out) override { out = ordinances; }
    bool AcquireServices() override { return true; }
    const cISCPropertyHolder* SimulatorExemplar() override { return exemplar; }
};

constexpr std::uint32_t kHealth = 0x09188f42, kAmbulances = 0xaa538cb3, kEducation = 0x09188f4c, kBuses = 0x4a538cc6;
constexpr std::uint32_t kHealthStaff = 0xca565486, kAmbulance = 0xea56549e, kSchoolStaff = 0xea5654b6, kBus = 0x4a5654ba;
constexpr std::uint32_t kSchoolCapacity = 0x8922041b, kSchoolRadius = 0x691b42b3, kSchoolServed = 0x09220161, kSchoolEffCap = 0xe9244b2e,
                        kSchoolGrade = 0x292453c5, kSchoolStaffRatio = 0x2a976c20, kSchoolEffect = 0xa92ae446;
constexpr std::uint32_t kHospitalCapacity = 0x69220415, kHospitalRadius = 0x091b42a2, kHospitalServed = 0xc922015c,
                        kHospitalEffCap = 0x49244b21, kHospitalGrade = 0x092453cb, kHospitalStaffRatio = 0xaa976be7;
constexpr std::uint32_t kStrikeDay = 0xeaa69f83;

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// The shipped tuning values the spec's examples use (REH 4.13).
void ShippedTuning(cSCPropertyHolder& ex) {
    cSCPropertyHelp::SetPropertyValue(&ex, 0x891894ad, std::vector<float>{25.0f, 120.0f});
    cSCPropertyHelp::SetPropertyValue(&ex, 0xa9189479, std::vector<float>{25.0f, 120.0f});
    cSCPropertyHelp::SetPropertyValue(&ex, 0xe91894b3, 20.0f);
    cSCPropertyHelp::SetPropertyValue(&ex, 0xa91894bc, std::int32_t(500));
    cSCPropertyHelp::SetPropertyValue(&ex, 0xea53b30f, std::vector<float>{0, 50, 100, 100, 120, 110});   // school funding -> coverage
    cSCPropertyHelp::SetPropertyValue(&ex, 0x0a53b2f9, std::vector<float>{0, 50, 100, 100});             // health funding -> coverage
    cSCPropertyHelp::SetPropertyValue(&ex, 0xec43826b, std::vector<std::uint32_t>{500, 750, 1000, 1000, 10000, 300, 1200});
    cSCPropertyHelp::SetPropertyValue(&ex, 0xac43826d, std::vector<std::uint32_t>{500, 500, 2000});
    cSCPropertyHelp::SetPropertyValue(&ex, 0x8a891421, std::uint32_t(30));
    cSCPropertyHelp::SetPropertyValue(&ex, 0x8a89142a, std::uint32_t(30));
    cSCPropertyHelp::SetPropertyValue(&ex, 0x6a9f6edd, 1.1f);
}

struct World {
    FakeCity city;
    Env env;
    Host host;
    cRZRandom random{777};
    cSCPropertyHolder exemplar;
    cSC4ResidentialSimulator* sim = nullptr;
    World() {
        SetSimGridCity(&city);
        cSC4BudgetSimulator::SetEnvironment(&env);
        host.budget = new cSC4BudgetSimulator();
        host.budget->AddRef();
        SetBudgetHost(host.budget);
        exemplar.AddRef();
        ShippedTuning(exemplar);
        host.exemplar = &exemplar;
        sim = new cSC4ResidentialSimulator(&host, &random);
        sim->AddRef();
        sim->Init();
    }
};

void TestTuning() {
    Host host;
    cRZRandom random(1);
    auto* sim = new cSC4ResidentialSimulator(&host, &random);
    sim->AddRef();
    // code defaults (REH 4.13)
    CHECK(sim->Health().minimumFunding == 0.6f && sim->Health().initialFunding == 0.25f && sim->Health().maxFunding == 1.2f);
    CHECK(sim->Health().strikeEfficiency == 0.2f && sim->Health().strikeDays == 10 && sim->Health().minGradeToEnd == 0.9f);
    CHECK(sim->Health().efficiencyCurve.GetOutputValue(120.0f) == 110.0f && sim->Health().coverageCurve.GetOutputValue(0.0f) == 20.0f);
    CHECK(sim->Health().strikeCurve.GetOutputValue(0.0f) == 20.0f && sim->Education().messageId == 0x077accd0);
    cSCPropertyHolder ex;
    ex.AddRef();
    ShippedTuning(ex);
    cSCPropertyHelp::SetPropertyValue(&ex, 0xc91894cb, std::vector<float>{0, 100, 80, 0});   // school strike response
    sim->ReadSimulatorTunableValues(&ex);
    CHECK(Near(sim->Education().initialFunding, 0.25f) && Near(sim->Education().maxFunding, 1.2f) && sim->Education().strikeDays == 500);
    CHECK(sim->Education().coverageCurve.GetOutputValue(0.0f) == 50.0f && sim->Health().coverageCurve.GetOutputValue(0.0f) == 50.0f);
    CHECK(sim->Education().strikeCurve.GetOutputValue(80.0f) == 0.0f && sim->Health().strikeCurve.GetOutputValue(0.0f) == 20.0f);
    CHECK(sim->schoolChunks.size() == 7 && sim->schoolChunks[4] == 10000 && sim->hospitalChunks[2] == 2000);
    CHECK(sim->schoolStaffDivisor == 30 && Near(sim->coverageSlop, 1.1f));
    sim->Release();
}

// 4.1: a hospital with the staff item registers with the health department (line := building type, count, local funding at the
// department's percentage), gets its starting properties, waits in the pending list and joins its group in state 0.
void TestRecognition() {
    World w;
    cSC4ResidentialSimulator& sim = *w.sim;
    cSC4BudgetSimulator& budget = *w.host.budget;
    cSC4DepartmentBudget* health = budget.GetDepartmentBudget(kHealth);
    CHECK(health && budget.GetDepartmentBudget(kAmbulances) && budget.GetDepartmentBudget(kEducation) && budget.GetDepartmentBudget(kBuses));
    CHECK(health && Near(health->GetFundingPercentage(), 0.25f) && Near(health->GetMaxAllowedFundingPercentage(), 1.2f));
    CHECK(budget.GetDepartmentBudget(kAmbulances)->GetLineItem(1) != nullptr);

    Building h;
    h.groups = {0x1512};
    h.Items({kHealth, kAmbulances}, {0, 1}, {kHealthStaff, kAmbulance}, {400, 50});
    h.Set(kHospitalCapacity, std::uint32_t(1000));
    h.Set(kHospitalRadius, 200.0f);
    sim.DoBuildingChanged(&h, true);
    std::vector<std::uint32_t> lines;
    cSCPropertyHelp::GetPropertyValue(h.props, kBudgetItemPropLineItems, lines);
    CHECK(lines.size() == 2 && lines[0] == 0x7777 && lines[1] == 1);
    cSC4LineItem* line = health->GetLineItem(0x7777);
    CHECK(line && line->GetSecondaryInfoField() == 1 && (line->GetDisplayFlags() & 2));
    CHECK(health->IsLocallyFundedObjectInDepartment(h.props, kHealthStaff));
    CHECK(budget.GetDepartmentBudget(kAmbulances)->IsLocallyFundedObjectInDepartment(h.props, kAmbulance));
    CHECK(h.Get<std::uint32_t>(kHospitalServed) == 0xffffffffu && h.Get<float>(kHospitalGrade) == 1.0f && h.Get<float>(kHospitalStaffRatio) == 1.0f);
    CHECK(h.Get<std::uint32_t>(kHospitalEffCap) == 1000 && h.refs == 1);
    CHECK(sim.PendingHospitals().size() == 1 && sim.PendingSchools().empty() && sim.GetHospitalBuildingCount() == 1);

    // funding: the building is locally funded at the department's 25 %; the ambulance department is at 100 %
    float a = 0, b = 0;
    sim.GetOccupantFundingPercentages(&h, a, b, false);
    CHECK(Near(a, 0.25f) && Near(b, 1.0f));
    float e1, e2;
    sim.GetOccupantEfficiencyRatings(&h, e1, e2, false, false);
    CHECK(Near(e1, 0.25f) && Near(e2, 1.0f));

    sim.DistributePendingBuildings();
    CHECK(sim.PendingHospitals().empty() && sim.HospitalLists()[0].size() == 1);

    // a building found through its group only (the farmer's market shape), a health-affecting one, one in no listed group
    Building market, clinic, stray;
    market.groups = {0x151c};
    clinic.Set(0x6a19f6b5, 2.0f);
    clinic.Set(0xaa19f6ea, 3.0f);
    stray.groups = {0x1507};   // recognised as a hospital, but in none of 0x1512 / 0x1513 / 0x151c
    sim.DoBuildingChanged(&market, true);
    sim.DoBuildingChanged(&clinic, true);
    sim.DoBuildingChanged(&stray, true);
    CHECK(sim.PendingHospitals().size() == 2 && sim.PendingHealthAffecting().size() == 1 && market.Get<std::uint32_t>(kHospitalEffCap, 9) == 0);
    sim.DistributePendingBuildings();
    CHECK(sim.HospitalLists()[2].size() == 1 && sim.HealthAffecting().size() == 1 && sim.PendingHealthAffecting().empty());
    CHECK(stray.refs == 1 && sim.GetHospitalBuildingCount() == 2);   // dropped without Release (the quirk)

    // removal: out of its list, released, the line item gone with its last building
    sim.DoBuildingChanged(&h, false);
    CHECK(h.refs == 0 && sim.HospitalLists()[0].empty() && !health->GetLineItem(0x7777));
    CHECK(!health->IsLocallyFundedObjectInDepartment(h.props, kHealthStaff));
    sim.DoBuildingChanged(&clinic, false);
    CHECK(clinic.refs == 0 && sim.HealthAffecting().empty());

    // a staff item whose department does not exist aborts the call: no list change
    Building orphan;
    orphan.groups = {0x1512};
    orphan.Items({0x999}, {0}, {kHealthStaff}, {10});
    sim.DoBuildingChanged(&orphan, true);
    CHECK(orphan.refs == 0 && sim.PendingHospitals().empty());
    // and its funding: the outputs stay as preset (0, 0), so e1 = effCurve(0) = 0 and e2 = coverageCurve(0) = 0.5 (4.2 example)
    sim.GetOccupantEfficiencyRatings(&orphan, e1, e2, false, false);
    CHECK(e1 == 0.0f && Near(e2, 0.5f) && sim.GetBaseStaffEfficiency(&orphan, false) == 0.0f);
    // no staff item at all: 1.0 / 1.0
    sim.GetOccupantFundingPercentages(&market, a, b, false);
    CHECK(a == 1.0f && b == 1.0f);
    sim.DoBuildingChanged(&market, false);
    sim.Release();
}

// 4.4 / 4.5 / 4.6 on a 4 x 4 grid of 64 m tracts. A school at the centre of tract (1,1), radius 60 m: with the 1.1 slop the covered
// tracts are the cross (0,1), (1,0), (1,1), (1,2), (2,1).
void TestCoverageAndGrade() {
    World w;
    cSC4ResidentialSimulator& sim = *w.sim;
    sim.GetPopulationGrid(0)->SetTractValue(1, 1, 100);
    sim.GetPopulationGrid(1)->SetTractValue(2, 1, 50);
    sim.GetPopulationGrid(0)->SetTractValue(3, 3, 999);   // outside
    Building a, b;
    a.groups = {0x1503, 0x150f};   // no budget items: recognised as a school through 0x1503, filed under K-6 0x150f
    a.At(96.0f, 96.0f);
    a.Set(kSchoolCapacity, std::uint32_t(1000));   // chunk 500 -> 2 units
    a.Set(kSchoolRadius, 60.0f);
    b.groups = {0x1503, 0x1510};
    b.At(96.0f, 96.0f);
    b.Set(kSchoolCapacity, std::uint32_t(200000)); // chunk 750 -> 266 units: saturates at 255
    b.Set(kSchoolRadius, 60.0f);
    sim.DoBuildingChanged(&a, true);
    sim.DoBuildingChanged(&b, true);
    sim.DistributePendingBuildings();
    CHECK(sim.SchoolLists()[0].size() == 1 && sim.SchoolLists()[1].size() == 1);
    sim.UpdateCoverageMap(true, 0);
    sim.UpdateCoverageMap(true, 1);
    cSC4SimGrid<std::uint8_t>* c0 = sim.GetCoverageMap(true, 0);
    cSC4SimGrid<std::uint8_t>* c1 = sim.GetCoverageMap(true, 1);
    int covered = 0;
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) covered += c0->GetTractValue(x, z) != 0;
    CHECK(covered == 5 && c0->GetTractValue(1, 1) == 2 && c0->GetTractValue(0, 1) == 2 && c0->GetTractValue(2, 1) == 2 && c0->GetTractValue(2, 2) == 0);
    CHECK(c1->GetTractValue(1, 2) == 255);
    CHECK(sim.ServedCount(true) == 150);   // group 1 adds nothing: group 0 already covers those tracts

    // grading (4.5): served 150 (the +x tract reads its distance curve at the far end, a flat curve here), grade 1, staff 1
    sim.ComputeOccupantGrade(&a, true, 0);
    CHECK(a.Get<std::uint32_t>(kSchoolServed) == 150 && a.Get<std::uint32_t>(kSchoolEffCap) == 1000);
    CHECK(a.Get<float>(kSchoolGrade) == 1.0f && a.Get<float>(kSchoolStaffRatio) == 1.0f);
    cSC4SimGrid<float>* grade = sim.GetGradeMap(true);
    CHECK(grade->GetTractValue(1, 1) == 1.0f && grade->GetTractValue(0, 1) == 1.0f && grade->GetTractValue(2, 2) == 0.0f);
    // the boost (4.6): EQ R$ at (1,1) +1. EQ R$$ at (2,1) gets nothing: in column x = 2 the empty tract (2,0) does not advance the
    // z centre, so (2,1) is measured from z = 32 m, 90.5 m away (> 66 m)
    CHECK(Near(sim.GetEQGrid(0)->GetTractValue(1, 1), 1.0f));
    CHECK(sim.GetEQGrid(1)->GetTractValue(2, 1) == 0.0f);

    // 4.11 query data, rating, totals
    ResidentialQueryData q;
    CHECK(sim.GetSchoolQueryData(&a, q) && q.capacity == 1000 && q.effectiveCapacity == 1000 && q.staff == 33 && q.effectiveStaff == 34);
    CHECK(q.served == 150 && q.grade == 1.0f && q.staffFunding == 1.0f && q.staffCost == 0);
    CHECK(!sim.GetHospitalQueryData(&a, q));
    sim.SetPopulation(400000);
    b.Set(kSchoolEffCap, std::uint32_t(199000));
    CHECK(Near(sim.GetSchoolSystemRating(), 200000.0f / 400000.0f));
    ResidentialQueryData totals;
    std::vector<std::uint32_t> only{0x1510};
    CHECK(sim.GetSchoolSystemTotals(totals, &only) == 1 && totals.capacity == 200000);
    CHECK(sim.GetSchoolSystemTotals(totals, nullptr) == 0 && totals.capacity == 0);
    sim.Release();
}

// The 4.6 step-7 table: value 1.5, population 120, units 3 at the building's own tract with coverage n = 0, 3, 6.
void TestBoostTable() {
    for (int n : {0, 3, 6}) {
        World w;
        cSC4ResidentialSimulator& sim = *w.sim;
        sim.GetPopulationGrid(0)->SetTractValue(1, 0, 120);
        Building s;
        s.groups = {0x150f};
        s.At(96.0f, 32.0f);                              // tract (1,0): the first of its column, no centre shift
        s.Set(kSchoolCapacity, std::uint32_t(1500));     // chunk 500 -> 3 units
        s.Set(kSchoolRadius, 60.0f);
        s.Set(kSchoolEffect, 1.5f);
        sim.DoBuildingChanged(&s, true);
        sim.DistributePendingBuildings();
        sim.GetCoverageMap(true, 0)->SetTractValue(1, 0, std::uint8_t(n));
        sim.ComputeOccupantGrade(&s, true, 0);
        const float expected = n == 6 ? 0.75f : 1.5f;
        CHECK(Near(sim.GetEQGrid(0)->GetTractValue(1, 0), expected));
        sim.Release();
    }
}

void TestOrdinances() {
    World w;
    cSCPropertyHolder ordinance, unrelated, tuning;
    ordinance.AddRef();
    unrelated.AddRef();
    tuning.AddRef();
    cSCPropertyHelp::SetPropertyValue(&ordinance, 0xa92d9d7a, 110.0f);   // school effect factor
    cSCPropertyHelp::SetPropertyValue(&ordinance, 0x692ef65a, 50.0f);    // school decay factor
    cSCPropertyHelp::SetPropertyValue(&unrelated, 0x12345678, 1.0f);
    w.host.ordinances = {&ordinance, &unrelated};
    w.sim->GetOrdinanceEffects();
    CHECK(w.sim->OrdinanceEffects(true).size() == 1 && w.sim->OrdinanceEffects(false).empty());
    const OrdinanceEffect& e = w.sim->OrdinanceEffects(true)[0];
    CHECK(Near(e.effectFactor, 1.1f) && e.radiusFactor == 1.0f && Near(e.decayFactor, 0.5f) && e.distanceCurve.GetOutputValue(50.0f) == 100.0f);
    cSCPropertyHelp::SetPropertyValue(&tuning, 0xc929c433, 0.2f);
    w.sim->ReadTunablePopulationValues(&tuning);
    CHECK(Near(w.sim->Tuning().eqDecay, 0.1f) && w.sim->Tuning().hqDecay == 1.0f);
    w.sim->Release();
}

void TestStrikes() {
    World w;
    cSC4ResidentialSimulator& sim = *w.sim;
    Host& host = w.host;
    Building a, b;
    a.groups = b.groups = {0x1a02};
    a.Set(kHospitalGrade, 0.0f);
    b.Set(kHospitalGrade, 0.0f);
    std::vector<std::list<IResidentialOccupant*>> lists(2);
    lists[0] = {&a};
    lists[1] = {&b};
    sim.Health().strikeCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1000.0f}, {100.0f, 1000.0f}});   // always strikes
    host.day = 400;
    sim.DoMonthlyStrikeCheck(sim.Health(), lists, kHospitalStaffRatio, kHospitalGrade, 0x1a02);
    CHECK(sim.HealthIsOnStrike() && sim.Health().strikeStartDay == 400 && a.Get<std::uint32_t>(kStrikeDay) == 400);
    CHECK(host.messages.size() == 1 && host.messages[0].begin && host.messages[0].id == 0x077accd2);
    CHECK(host.strikers.size() == 2 && host.strikers[0][0] == 0 && host.strikers[0][1] == 1);
    // the staff ratio reaching the minimum grade ends each building's strike; then the city strike ends
    a.Set(kHospitalStaffRatio, 0.95f);
    b.Set(kHospitalStaffRatio, 0.95f);
    host.day = 405;
    sim.DoMonthlyStrikeCheck(sim.Health(), lists, kHospitalStaffRatio, kHospitalGrade, 0x1a02);
    CHECK(!sim.HealthIsOnStrike() && host.messages.size() == 2 && !host.messages[1].begin && host.messages[1].days == 5);
    // a strike past its duration (10 days for health here) ends whatever the ratio
    a.Set(kStrikeDay, std::uint32_t(100));
    a.Set(kHospitalStaffRatio, 0.1f);
    host.day = 110;
    CHECK(!sim.UpdateStrikingState(&a, sim.Health(), kHospitalStaffRatio, kHospitalGrade));
    host.day = 111;
    CHECK(sim.UpdateStrikingState(&a, sim.Health(), kHospitalStaffRatio, kHospitalGrade) && a.Get<std::uint32_t>(kStrikeDay, 7) == 0);
    // chance on the shipped response (0,100)(80,0): grade 0.4 -> 50 %; a building outside the strike group reuses the previous c
    sim.Health().strikeCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 100.0f}, {80.0f, 0.0f}});
    a.Set(kHospitalGrade, 0.4f);
    Building outside;
    std::list<IResidentialOccupant*> mixed = {&a, &outside};
    CHECK(Near(sim.GetChanceOfStrike(sim.Health(), mixed, kHospitalGrade, 0x1a02), 0.75f));
    sim.Release();
}

// 4.10: served 1150 of 1000 -> r = 1.15; the efficiency curve never reaches it below the 120 % ceiling, so f stops at 1.2.
void TestAutoBudget() {
    World w;
    cSC4ResidentialSimulator& sim = *w.sim;
    Building s;
    s.groups = {0x150f};
    s.Items({kEducation, kBuses}, {0, 1}, {kSchoolStaff, kBus}, {300, 20});
    s.Set(kSchoolCapacity, std::uint32_t(1000));
    sim.DoBuildingChanged(&s, true);
    float f = -1.0f;
    CHECK(!sim.EstimateIdealFunding(&s, f));   // not graded yet (served 0xffffffff)
    s.Set(kSchoolServed, std::uint32_t(1150));
    CHECK(sim.EstimateIdealFunding(&s, f) && Near(f, 1.2f));
    s.Set(kSchoolServed, std::uint32_t(500));  // r = 0.5: the curve gives exactly 0.5 there
    CHECK(sim.EstimateIdealFunding(&s, f) && Near(f, 0.5f, 1e-6f));
    sim.SetOccupantFundingPercentages(&s, f, 1.0f, true);
    float a = 0, b = 0;
    sim.GetOccupantFundingPercentages(&s, a, b, true);
    CHECK(Near(a, 0.5f) && Near(b, 1.0f));
    sim.DoBuildingChanged(&s, false);
    sim.Release();
}

}  // namespace

int main() {
    TestTuning();
    TestRecognition();
    TestCoverageAndGrade();
    TestBoostTable();
    TestOrdinances();
    TestStrikes();
    TestAutoBudget();
    if (failures) return 1;
    std::printf("sc4_residentialsimulator: all passed\n");
    return 0;
}
