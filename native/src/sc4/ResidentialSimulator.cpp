#include "sc4/ResidentialSimulator.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kEducationOccupantGroupName = 0;   // groups are addressed through the host
constexpr std::uint32_t kPropertyStaffEfficiencyEducation = 0xea5654b6, kPropertyStaffEfficiencyHealth = 0xca565486;
constexpr std::uint32_t kPropertyBudgetEducation = 0x4a5654ba, kPropertyBudgetHealth = 0xea56549e;

// The three-point curves of the original: staff efficiency saturates at 110% for 120% funding; the other
// curves rise from 20% at no funding.
const std::vector<SC4Point> kCurveA = {{0.0f, 0.0f}, {100.0f, 100.0f}, {120.0f, 110.0f}};
const std::vector<SC4Point> kCurveB = {{0.0f, 20.0f}, {100.0f, 100.0f}};

bool GetResidentialFloat(const cISCPropertyHolder* e, std::uint32_t id, float& out) { return cSCPropertyHelp::GetPropertyValue(e, id, out); }
}  // namespace

cSC4ResidentialSimulator::cSC4ResidentialSimulator(IResidentialHost* host, cRZRandom* random)
    : host_(host), random_(random), schools_(7), hospitals_(3) {
    ReadSimulatorTunableValues(nullptr);
}

// ---- identity -------------------------------------------------------------------------------------------

bool cSC4ResidentialSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);         // +0x10
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);  // +0x8
    else if (iid == 1 || iid == kGZIID_cISC4ResidentialSimulator) *out = static_cast<cISC4ResidentialSimulator*>(this);  // +0
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4ResidentialSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}


void cSC4ResidentialSimulator::SetDefaultCurves(DepartmentData& d, const std::vector<SC4Point>& a, const std::vector<SC4Point>& b) {
    d.staffCurve.SetResponseCurveData(a);
    d.strikeChance.SetResponseCurveData(b);
    d.thirdCurve.SetResponseCurveData(b);
    d.minimumFunding = 0.6f;
    d.strikeThreshold = 1.2f;
    d.value08 = 0.2f;
    d.strikeDays = 10;
    d.strikeEndFunding = 0.9f;
    d.value90 = 0.25f;
    d.value94 = 100.0f;
    d.onStrike = false;
    d.strikeStartDay = 0;
}

void cSC4ResidentialSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    SetDefaultCurves(health_, kCurveA, kCurveB);
    health_.messageId = kMessageHealthStrike;
    SetDefaultCurves(education_, kCurveA, kCurveB);
    education_.messageId = kMessageEducationStrike;
    for (cSC4ResponseCurve& curve : wealthCurves) curve.SetResponseCurveData(kCurveA);
    if (!exemplar) return;

    using H = cSCPropertyHelp;
    const auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id) {
        std::vector<float> values;
        if (exemplar->HasProperty(id) && H::GetPropertyValue(exemplar, id, values)) curve.SetResponseCurveData(values.data(), values.size());
    };
    float f = 0;
    std::vector<float> pair;
    SC4Percentage percentage;

    // health department
    if (GetResidentialFloat(exemplar, 0xe918946f, f)) health_.minimumFunding = f * 0.01f;
    loadCurve(health_.staffCurve, 0x2918949e);
    loadCurve(health_.thirdCurve, 0x0a53b2f9);
    if (H::GetPropertyValue(exemplar, 0xa9189479, pair) && pair.size() > 1) {
        health_.value90 = pair[0] * 0.01f;
        health_.strikeThreshold = pair[1] * 0.01f;
    }
    if (GetResidentialFloat(exemplar, 0x89189482, f)) health_.value08 = f * 0.01f;
    std::int32_t integer = 0;
    if (H::GetPropertyValue(exemplar, 0x8918948b, integer)) health_.strikeDays = static_cast<std::uint32_t>(integer);
    if (H::GetPropertyValue(exemplar, 0x89189495, percentage)) health_.strikeEndFunding = percentage.value;
    loadCurve(health_.strikeChance, 0x691894a4);

    // education department
    if (GetResidentialFloat(exemplar, 0xc91894a8, f)) education_.minimumFunding = f * 0.01f;
    loadCurve(education_.staffCurve, 0x691894c6);
    loadCurve(education_.thirdCurve, 0xea53b30f);
    if (H::GetPropertyValue(exemplar, 0x891894ad, pair) && pair.size() > 1) {
        education_.value90 = pair[0] * 0.01f;
        education_.strikeThreshold = pair[1] * 0.01f;
    }
    if (GetResidentialFloat(exemplar, 0xe91894b3, f)) education_.value08 = f * 0.01f;
    if (H::GetPropertyValue(exemplar, 0xa91894bc, integer)) education_.strikeDays = static_cast<std::uint32_t>(integer);
    if (H::GetPropertyValue(exemplar, 0x691894c0, percentage)) education_.strikeEndFunding = percentage.value;
    loadCurve(education_.strikeChance, 0xc91894cb);

    for (std::size_t i = 0; i < wealthCurves.size(); ++i) loadCurve(wealthCurves[i], 0xea540621u + std::uint32_t(i));
    value520 = value524 = 30;
    H::GetPropertyValue(exemplar, 0x8a891421, value520);
    H::GetPropertyValue(exemplar, 0x8a89142a, value524);
    value568 = 1.1f;
    H::GetPropertyValue(exemplar, 0x6a9f6edd, value568);
    value560 = value564 = 10.0f;
    H::GetPropertyValue(exemplar, 0x4aa84161, value560);
    H::GetPropertyValue(exemplar, 0x2aa84164, value564);
    H::GetPropertyValue(exemplar, 0xec43826b, list528);
    H::GetPropertyValue(exemplar, 0xac43826d, list540);
}

// ---- building lists --------------------------------------------------------------------------------------

int cSC4ResidentialSimulator::GetSchoolBuildingCount() const {
    int n = int(extraSchools_.size());
    for (const auto& list : schools_) n += int(list.size());
    return n;
}

int cSC4ResidentialSimulator::GetHospitalBuildingCount() const {
    int n = int(extraHospitals_.size());
    for (const auto& list : hospitals_) n += int(list.size());
    return n;
}

void cSC4ResidentialSimulator::GetBuildings(const std::list<IResidentialOccupant*>& source, std::list<IResidentialOccupant*>& out,
                                            const std::vector<std::uint32_t>* groups) {
    for (IResidentialOccupant* occupant : source) {
        if (!groups) {
            out.push_back(occupant);
            continue;
        }
        for (std::uint32_t group : *groups) {
            if (occupant->IsInGroup(group)) {
                out.push_back(occupant);
                break;
            }
        }
    }
}

void cSC4ResidentialSimulator::GetSchoolBuildings(std::list<IResidentialOccupant*>& out, const std::vector<std::uint32_t>* groups) const {
    for (const auto& list : schools_) GetBuildings(list, out, groups);
    GetBuildings(extraSchools_, out, groups);
}

void cSC4ResidentialSimulator::GetHospitalBuildings(std::list<IResidentialOccupant*>& out, const std::vector<std::uint32_t>* groups) const {
    for (const auto& list : hospitals_) GetBuildings(list, out, groups);
    GetBuildings(extraHospitals_, out, groups);
}

void cSC4ResidentialSimulator::GetHealthAffectingBuildings(std::list<IResidentialOccupant*>& out) const {
    for (IResidentialOccupant* o : healthAffecting1_) out.push_back(o);
    for (IResidentialOccupant* o : healthAffecting2_) out.push_back(o);
}

// ---- strikes -----------------------------------------------------------------------------------------------

float cSC4ResidentialSimulator::GetChanceOfStrike(DepartmentData& dept, const std::list<IResidentialOccupant*>& buildings,
                                                  std::uint32_t fundingProperty, std::uint32_t groupFilter) {
    if (dept.onStrike || buildings.empty()) return 0.0f;
    float funding = 0.0f, combined = 0.0f, chance = 0.0f;
    for (IResidentialOccupant* occupant : buildings) {
        if (occupant->IsInGroup(groupFilter)) {
            float value;
            if (cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), fundingProperty, value)) funding = value;
            chance = dept.strikeChance.GetOutputValue(funding * 100.0f) * 0.01f;
        }
        // an occupant outside the group re-applies the previous chance, as the original does
        combined = (combined + chance) - combined * chance;
    }
    return combined;
}

float cSC4ResidentialSimulator::ChanceOfSchoolStrike() {
    if (SchoolIsOnStrike()) return 1.0f;
    float total = 0.0f;
    for (const auto& list : schools_) {
        const float c = GetChanceOfStrike(education_, list, 0x292453c5, 0x1a03);
        total = (total + c) - total * c;
    }
    return total;
}

float cSC4ResidentialSimulator::ChanceOfHealthStrike() {
    if (HealthIsOnStrike()) return 1.0f;
    float total = 0.0f;
    for (const auto& list : hospitals_) {
        const float c = GetChanceOfStrike(health_, list, 0x092453cb, 0x1a02);
        total = (total + c) - total * c;
    }
    return total;
}

bool cSC4ResidentialSimulator::BeginStrike(DepartmentData& dept, IResidentialOccupant* occupant) {
    if (dept.onStrike) return false;
    dept.onStrike = true;
    dept.strikeStartDay = host_->CurrentDay();
    host_->PostStrikeMessage(dept.messageId, occupant, true, 0);
    return true;
}

bool cSC4ResidentialSimulator::EndStrike(DepartmentData& dept, std::uint32_t days) {
    if (!dept.onStrike) return false;
    dept.onStrike = false;
    dept.strikeStartDay = 0;
    host_->PostStrikeMessage(dept.messageId, nullptr, false, days);
    return true;
}

bool cSC4ResidentialSimulator::EndSchoolStrike() {
    if (!SchoolIsOnStrike()) return false;
    return EndStrike(education_, host_->CurrentDay() - education_.strikeStartDay);
}

bool cSC4ResidentialSimulator::EndHealthStrike() {
    if (!HealthIsOnStrike()) return false;
    return EndStrike(health_, host_->CurrentDay() - health_.strikeStartDay);
}

bool cSC4ResidentialSimulator::UpdateStrikingState(IResidentialOccupant* occupant, DepartmentData& dept,
                                                   std::uint32_t strikingFundingProperty, std::uint32_t idleFundingProperty) {
    const std::uint32_t today = host_->CurrentDay();
    std::uint32_t start = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kResidentialStrikeStartDate, start);
    std::uint32_t next;
    if (start == 0) {
        float funding;
        if (!cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), idleFundingProperty, funding)) funding = 0.0f;
        else funding *= 100.0f;
        const float chance = dept.strikeChance.GetOutputValue(funding);
        next = (float(random_->RandomDoubleRangeUniform(0.0, 100.0)) < chance) ? today : 0;
    } else {
        float funding;
        const bool found = cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), strikingFundingProperty, funding);
        next = 0;
        if (today - start <= dept.strikeDays) {
            if (!found) funding = 1.0f;
            next = dept.strikeEndFunding <= funding ? 0 : start;
        }
    }
    if (next == start) return false;
    cSCPropertyHelp::SetPropertyValue(occupant->PropertyHolder(), kResidentialStrikeStartDate, next);
    if (host_->CityHasOccupantManager()) {
        const bool education = dept.messageId != kMessageHealthStrike;
        host_->SetStrikerGroup(education, occupant, next != 0);
    }
    return true;
}

void cSC4ResidentialSimulator::DoMonthlyStrikeCheck(DepartmentData& dept, std::vector<std::list<IResidentialOccupant*>>& lists,
                                                    std::uint32_t strikingFundingProperty, std::uint32_t idleFundingProperty,
                                                    std::uint32_t groupFilter) {
    if (lists.empty()) return;
    bool fromEarlierChange = false;
    std::uint32_t strikers = 0;
    for (auto& list : lists) {
        for (IResidentialOccupant* occupant : list) {
            if (!occupant->IsInGroup(groupFilter)) continue;
            const bool changed = UpdateStrikingState(occupant, dept, strikingFundingProperty, idleFundingProperty);
            const bool begin = changed ? true : fromEarlierChange;
            std::uint32_t start = 0;
            cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kResidentialStrikeStartDate, start);
            if (start != 0) {
                ++strikers;
                if (begin) BeginStrike(dept, occupant);
            }
            fromEarlierChange = begin;
        }
    }
    if (fromEarlierChange && strikers == 0 && dept.onStrike) EndStrike(dept, host_->CurrentDay() - dept.strikeStartDay);
}

// ---- funding ------------------------------------------------------------------------------------------------

void cSC4ResidentialSimulator::SetOccupantFundingPercentages(IResidentialOccupant* occupant, const SC4Percentage& staff,
                                                             const SC4Percentage& budget, bool education) {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    const std::uint32_t staffProp = education ? kPropertyStaffEfficiencyEducation : kPropertyStaffEfficiencyHealth;
    const std::uint32_t budgetProp = education ? kPropertyBudgetEducation : kPropertyBudgetHealth;
    if (host_->IsBudgeted(occupant, staffProp)) {
        IBudgetRecord* record = host_->GetBudgetRecord(occupant, staffProp);
        if (!record) return;
        record->SetFunding(holder, staff, staffProp);
        if (host_->IsBudgeted(occupant, budgetProp)) {
            IBudgetRecord* second = host_->GetBudgetRecord(occupant, budgetProp);
            if (!second) return;
            second->SetFunding(holder, budget, budgetProp);
        }
    }
    fundingChanged_ = true;
}

void cSC4ResidentialSimulator::GetRawOccupantFundingPercentages(IResidentialOccupant* occupant, SC4Percentage& staff,
                                                                SC4Percentage& budget, bool education) {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    const std::uint32_t staffProp = education ? kPropertyStaffEfficiencyEducation : kPropertyStaffEfficiencyHealth;
    const std::uint32_t budgetProp = education ? kPropertyBudgetEducation : kPropertyBudgetHealth;
    if (!host_->IsBudgeted(occupant, staffProp)) {
        staff.value = 1.0f;
        budget.value = 1.0f;
        return;
    }
    IBudgetRecord* record = host_->GetBudgetRecord(occupant, staffProp);
    if (!record) return;    // outputs stay untouched, as in the original
    staff.value = record->HasExplicitFunding(holder, staffProp) ? 0.0f : record->DefaultFunding();
    if (!host_->IsBudgeted(occupant, budgetProp)) {
        budget.value = 1.0f;
        return;
    }
    IBudgetRecord* second = host_->GetBudgetRecord(occupant, budgetProp);
    if (!second) return;
    budget.value = second->HasExplicitFunding(holder, budgetProp) ? 0.0f : second->DefaultFunding();
}

float cSC4ResidentialSimulator::GetBaseStaffEfficiency(IResidentialOccupant* occupant, bool education) {
    float staff = 0.0f;
    host_->GetOccupantEfficiencyRatings(occupant, staff, education);
    const std::uint32_t prop = education ? kPropertyStaffEfficiencyEducation : kPropertyStaffEfficiencyHealth;
    if (!host_->IsBudgeted(occupant, prop)) return staff;
    cSC4ResponseCurve& curve = education ? education_.staffCurve : health_.staffCurve;
    return curve.GetOutputValue(staff * 100.0f) * 0.01f;
}

bool cSC4ResidentialSimulator::GetAutoBudget(const IResidentialOccupant* occupant) {
    bool value = false;
    cSCPropertyHelp::GetPropertyValue(const_cast<IResidentialOccupant*>(occupant)->PropertyHolder(), kOccupantAutoBudget, value);
    return value;
}

void cSC4ResidentialSimulator::SetAutoBudget(IResidentialOccupant* occupant, bool value) {
    cSCPropertyHelp::SetPropertyValue(occupant->PropertyHolder(), kOccupantAutoBudget, value);
}

// ---- totals and ratings ---------------------------------------------------------------------------------------

int cSC4ResidentialSimulator::GetSystemTotals(const std::vector<std::list<IResidentialOccupant*>>& lists, ResidentialQueryData& totals,
                                              bool education, const std::vector<std::uint32_t>& groups) {
    totals.field28 = totals.served = totals.field30 = totals.field34 = totals.field38 = 0;   // +0x28..+0x3c are cleared
    totals.field3c = 0.0f;
    int count = 0;
    for (const auto& list : lists) {
        for (IResidentialOccupant* occupant : list) {
            bool matches = false;
            for (std::uint32_t group : groups) {
                if (occupant->IsInGroup(group)) { matches = true; break; }
            }
            if (!matches) continue;
            ResidentialQueryData one;
            if (education) host_->GetSchoolQueryData(occupant, one);
            else host_->GetHospitalQueryData(occupant, one);
            totals.field28 += one.field28;
            totals.served += one.served;
            totals.field30 += one.field30;
            totals.field34 += one.field34;
            totals.field38 += one.field38;
            totals.field3c += one.field3c;
            ++count;
        }
    }
    return count;
}

float cSC4ResidentialSimulator::RatingFor(const std::vector<std::list<IResidentialOccupant*>>& lists, bool education) {
    if (lists.empty()) return 0.0f;
    unsigned served = 0;
    for (const auto& list : lists) {
        for (IResidentialOccupant* occupant : list) {
            ResidentialQueryData data;
            if (education) host_->GetSchoolQueryData(occupant, data);
            else host_->GetHospitalQueryData(occupant, data);
            served += unsigned(data.served);
        }
    }
    if (served == 0) return 0.0f;
    if (population_ == 0) return 1.0f;
    const float ratio = float(served) / float(population_);
    return ratio <= 1.0f ? ratio : 1.0f;
}

float cSC4ResidentialSimulator::GetHealthSystemRating() { return RatingFor(hospitals_, false); }
float cSC4ResidentialSimulator::GetSchoolSystemRating() { return RatingFor(schools_, true); }

// ---- exemplar-driven queries ------------------------------------------------------------------------------------

std::uint8_t cSC4ResidentialSimulator::GetAverageNewAgeByWealth(const cISCPropertyHolder* exemplar, int wealth) {
    std::uint8_t ages[3] = {25, 30, 25};
    if (exemplar) {
        cSCPropertyHelp::GetPropertyValue(exemplar, 0xc929c652, ages[0]);
        cSCPropertyHelp::GetPropertyValue(exemplar, 0x0929cc30, ages[1]);
        cSCPropertyHelp::GetPropertyValue(exemplar, 0x0929cc36, ages[2]);
    }
    return ages[std::uint8_t(wealth - 1) % 3];
}

float cSC4ResidentialSimulator::GetCellWorkforcePercent(const cISCPropertyHolder* exemplar, unsigned x, unsigned z) {
    if (!exemplar) return 0.0f;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {200.0f, 100.0f}});
    std::vector<float> values;
    if (exemplar->HasProperty(0x4941a92b) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x4941a92b, values)) curve.SetResponseCurveData(values.data(), values.size());
    // slot 29 at the cell; the host seam stands in while the grids do not exist (the unit tests)
    const float le = hq_[0] ? float(std::uint8_t(GetCellLifeExpectancy(exemplar, x, z))) : float(host_->CellWorkforce(x, z));
    return curve.GetOutputValue(le) * 0.01f;
}

float cSC4ResidentialSimulator::GetGlobalWorkforcePercent(const cISCPropertyHolder* exemplar) {
    if (!exemplar) return 0.0f;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {200.0f, 100.0f}});
    std::vector<float> values;
    if (exemplar->HasProperty(0x4941a92b) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x4941a92b, values)) curve.SetResponseCurveData(values.data(), values.size());
    const float le = hq_[0] ? float(GetGlobalLE()) : float(host_->GlobalWorkforce());   // slot 34
    return curve.GetOutputValue(le) * 0.01f;
}

std::uint32_t cSC4ResidentialSimulator::GetCellLifeExpectancy(const cISCPropertyHolder* exemplar, unsigned x, unsigned z) {
    if (!exemplar) return 0;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {200.0f, 200.0f}});
    std::vector<float> values;
    if (exemplar->HasProperty(0x6941a67f) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x6941a67f, values)) curve.SetResponseCurveData(values.data(), values.size());
    const float hq = hq_[0] ? GetCellHQ(x, z) : host_->CellLifeExpectancyBase(x, z);   // slot 36 (the host seam without grids)
    return std::uint32_t(curve.GetOutputValue(hq)) & 0xff;
}

// ---- the population half (residential.md) -------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kMessageSimNewDay = 0x66956814, kMessageSimNewMonth = 0x66956816;
constexpr std::uint32_t kMessageOccupantInserted = 0x99ef1142, kMessageOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kProximityIds[3] = {0x49d5bc96, 0x49d5bca6, 0x49d5bcb7};
constexpr std::uint32_t kPopulationIds[3] = {0x69d5c2a0, 0x69d5c2a1, 0x69d5c2a2};
constexpr std::uint32_t kAgeGridId = 0x69d5c303;
constexpr std::uint32_t kSchoolCoverageIds[7] = {0xaa8e1dc1, 0x8a8df4b6, 0xca8e449e, 0xaa8df4bd, 0x8a8df4c4, 0xaa8df4d4, 0x69d5c30d};
constexpr std::uint32_t kHospitalCoverageIds[3] = {0xaa8e1de4, 0x8a8df6fc, 0x69d5c317};
constexpr std::uint32_t kSchoolGradeId = 0x69d5c324, kHospitalGradeId = 0x69d5c330;
constexpr std::uint32_t kEQIds[3] = {0x69d5c2d0, 0x69d5c2d1, 0x69d5c2d2}, kHQIds[3] = {0x69d5c2e0, 0x69d5c2e1, 0x69d5c2e2};
constexpr std::uint32_t kHistoryLifeExpectancy = 0x2a5e7009, kHistoryAgeGroupPopulation = 0xca545cab, kHistoryAgeGroupEQ = 0xca545cca;
constexpr std::uint32_t kHistoryGlobalEQ = 0xaa1a2864, kHistoryGlobalHQ = 0xaa1a2866, kHistoryPopulation = 0xaa1a2cca;
constexpr std::uint32_t kHistoryHospitalCoverage = 0x4a8a3fe2, kHistorySchoolCoverage = 0x4a8a3fe5;
constexpr const char* kAgentName = "cSC4ResidentialSimulator";
constexpr std::uint32_t kStateIdle = 10;

template <class T> SimGridPtr<T> NewGrid(std::int32_t tractSize, std::uint32_t id) {
    SimGridPtr<T> grid(new cSC4SimGrid<T>());
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(tractSize);
    grid->SetInstanceID(id);
    return grid;
}

// rhe: the float rounded half to even (the original adds 12582912 and takes the low bits)
std::int32_t RoundHalfEven(float v) { return static_cast<std::int32_t>(std::nearbyint(v)); }
}  // namespace

// 4.1: services, agents, history, the new-city part, the tuning, every absent grid, the iterator.
bool cSC4ResidentialSimulator::Init() {
    if (!host_ || !host_->AcquireServices()) return false;
    if (!initialized_) {
        host_->AddNotification(this, kMessageOccupantInserted);
        host_->AddNotification(this, kMessageOccupantRemoved);
        for (int level : {0, 10, 6, 4}) host_->AddAgent(this, level, kAgentName, 2);
    }
    for (std::uint32_t g = 0; g < 9; ++g) {
        host_->CreateHistory(kHistoryAgeGroupPopulation + g, 2);
        host_->CreateHistory(kHistoryAgeGroupEQ + g, 2);
    }
    host_->CreateHistory(kHistoryHospitalCoverage, 2);
    host_->CreateHistory(kHistorySchoolCoverage, 2);
    if (!cityStarted_) {
        cSC4SimGrid<std::int16_t>* developerMap = host_->DeveloperPopulationMap(1);
        populationTractSize_ = eqTractSize_ = developerMap ? developerMap->GetTractSize() : 4;
        population_ = 0;
        runningPopulation_ = 0;
        state_ = 0;
        cityStarted_ = true;
    }
    if (!initialized_) {
        // The defaults overwrite a loaded record's department scalars, strikes included (9.3 quirk). PARTIAL: the ordinance-effect
        // lists are not built (education/health half).
        ReadSimulatorTunableValues(host_->SimulatorExemplar());
    }
    CreateGrids();
    ResetIterator();
    ReadTunablePopulationValues(host_->SimulatorExemplar());
    leSum_ = 0;
    initialized_ = true;
    return true;
}

// 4.2
bool cSC4ResidentialSimulator::Shutdown() {
    if (std::getenv("SC4_RESDEBUG")) std::fprintf(stderr, "residential: shutdown (initialized %d, refs %u)\n", int(initialized_), refCount_);
    if (!initialized_) return true;
    initialized_ = false;
    if (host_) {
        host_->RemoveAgent(this);
        host_->RemoveNotification(this, kMessageOccupantInserted);
        host_->RemoveNotification(this, kMessageOccupantRemoved);
        host_->ReleaseServices();
    }
    ReleaseGrids();
    for (cRetainedReference& ref : departmentRefs_) ref = cRetainedReference();
    fileLists_.clear();
    return true;
}

void cSC4ResidentialSimulator::CreateGrids() {
    for (int w = 0; w < 3; ++w) {
        if (!proximity_[w]) proximity_[w] = NewGrid<std::uint8_t>(proximityTractSize_, kProximityIds[w]);
        if (!populationGrids_[w]) populationGrids_[w] = NewGrid<std::uint16_t>(populationTractSize_, kPopulationIds[w]);
        if (!eq_[w]) eq_[w] = NewGrid<float>(eqTractSize_, kEQIds[w]);
        if (!hq_[w]) hq_[w] = NewGrid<float>(eqTractSize_, kHQIds[w]);
    }
    if (!ageGrid_) ageGrid_ = NewGrid<std::uint8_t>(populationTractSize_, kAgeGridId);
    if (schoolCoverage_.size() != 7) schoolCoverage_.resize(7);
    if (!schoolCoverage_[0])
        for (std::size_t k = 0; k < 7; ++k) schoolCoverage_[k] = NewGrid<std::uint8_t>(populationTractSize_, kSchoolCoverageIds[k]);
    if (hospitalCoverage_.size() != 3) hospitalCoverage_.resize(3);
    if (!hospitalCoverage_[0])
        for (std::size_t k = 0; k < 3; ++k) hospitalCoverage_[k] = NewGrid<std::uint8_t>(populationTractSize_, kHospitalCoverageIds[k]);
    if (!schoolGrade_) schoolGrade_ = NewGrid<float>(populationTractSize_, kSchoolGradeId);
    if (!hospitalGrade_) hospitalGrade_ = NewGrid<float>(populationTractSize_, kHospitalGradeId);
}

void cSC4ResidentialSimulator::ReleaseGrids() {
    for (int w = 0; w < 3; ++w) {
        proximity_[w].reset();
        populationGrids_[w].reset();
        eq_[w].reset();
        hq_[w].reset();
    }
    ageGrid_.reset();
    for (auto& grid : schoolCoverage_) grid.reset();
    for (auto& grid : hospitalCoverage_) grid.reset();
    schoolGrade_.reset();
    hospitalGrade_.reset();
    work_.reset();
}

// The scan of state 4 (simgrid 7.4): the R$ population grid's tract range, step 1, Reset(0, 0).
void cSC4ResidentialSimulator::ResetIterator() {
    const cSC4SimGrid<std::uint16_t>* grid = populationGrids_[0].get();
    const std::int32_t countX = grid ? grid->GetTractCountX() : 0, countZ = grid ? grid->GetTractCountZ() : 0;
    iterator_.rect.left = 0;
    iterator_.rect.top = 0;
    iterator_.rect.right = countX > 0 ? countX - 1 : 0;
    iterator_.rect.bottom = countZ > 0 ? countZ - 1 : 0;
    iterator_.remaining = 1;
    iterator_.x = 0;
    iterator_.z = 0;
    iterator_.state = 0;
}

bool cSC4ResidentialSimulator::NextTract(std::int32_t& x, std::int32_t& z) {
    ScanningIterator& it = iterator_;
    if (it.state == 2) return false;
    if (it.state == 0) {
        it.state = 1;
    } else {
        const std::int32_t step = it.remaining ? static_cast<std::int32_t>(it.remaining) : 1;
        it.x += step;
        if (it.x > it.rect.right) {
            it.x = it.rect.left;
            it.z += step;
            if (it.z > it.rect.bottom) {
                it.state = 2;
                return false;
            }
        }
    }
    x = it.x;
    z = it.z;
    return true;
}

// 5.3 step 1 (with the original's seed defect: only element 0 of each row is written). No building map seeds nothing.
void cSC4ResidentialSimulator::SeedProximity(int wealth) {
    if (!work_) work_ = NewGrid<std::uint8_t>(proximityTractSize_, 0);
    const cSC4SimGrid<std::uint8_t>* map = host_ ? host_->BuildingMap() : nullptr;
    if (!map) return;
    const std::uint8_t key = host_->BuildingMapKey(wealth);
    auto& grid = work_->GetGridData();
    const auto& source = map->GetGridData();
    const std::int32_t countX = work_->GetTractCountX(), countZ = work_->GetTractCountZ();
    const std::int32_t mapX = map->GetTractCountX(), mapZ = map->GetTractCountZ();
    const std::int32_t shift = work_->GetTractShift(), mapShift = map->GetTractShift();
    for (std::int32_t x = 0; x < countX; ++x) {
        for (std::int32_t z = 0; z < countZ; ++z) {
            std::uint8_t value = 0xff;
            if (shift == mapShift) {
                if (x < mapX && mapZ > 0 && source[x][0] == key) value = 0;
            } else if (shift < mapShift) {
                const std::int32_t d = mapShift - shift, sx = x >> d, sz = z >> d;
                if (sx < mapX && sz < mapZ && source[sx][sz] == key) value = 0;
            } else {
                const std::int32_t d = shift - mapShift, n = 1 << d;
                for (std::int32_t i = 0; i < n && value; ++i)
                    for (std::int32_t j = 0; j < n && value; ++j) {
                        const std::int32_t sx = (x << d) + i, sz = (z << d) + j;
                        if (sx < mapX && sz < mapZ && source[sx][sz] == key) value = 0;
                    }
            }
            if (!grid[x].empty()) grid[x][0] = value;
        }
    }
}

// 5.3 step 2: forward and backward sweeps along every row, then along every column; d starts at 255 for each sweep.
bool cSC4ResidentialSimulator::ProximityPass() {
    if (!work_) return false;
    auto& grid = work_->GetGridData();
    const std::int32_t countX = work_->GetTractCountX(), countZ = work_->GetTractCountZ();
    std::uint32_t changed = 0;
    const auto visit = [&](std::uint8_t& cell, std::int32_t& d) {
        d = std::min(d + 1, static_cast<std::int32_t>(cell));
        changed |= cell ^ static_cast<std::uint8_t>(d);
        cell = static_cast<std::uint8_t>(d);
    };
    for (std::int32_t x = 0; x < countX; ++x) {
        std::int32_t d = 255;
        for (std::int32_t z = 0; z < countZ; ++z) visit(grid[x][z], d);
        d = 255;
        for (std::int32_t z = countZ - 1; z >= 0; --z) visit(grid[x][z], d);
    }
    for (std::int32_t z = 0; z < countZ; ++z) {
        std::int32_t d = 255;
        for (std::int32_t x = 0; x < countX; ++x) visit(grid[x][z], d);
        d = 255;
        for (std::int32_t x = countX - 1; x >= 0; --x) visit(grid[x][z], d);
    }
    return changed != 0;
}

void cSC4ResidentialSimulator::CopyProximity(int wealth) {
    cSC4SimGrid<std::uint8_t>* target = proximity_[wealth - 1].get();
    if (!target || !work_) return;
    auto& out = target->GetGridData();
    const auto& in = work_->GetGridData();
    const std::size_t countX = std::min(out.size(), in.size());
    for (std::size_t x = 0; x < countX; ++x) {
        const std::size_t countZ = std::min(out[x].size(), in[x].size());
        for (std::size_t z = 0; z < countZ; ++z) out[x][z] = in[x][z];
    }
}

// 7.1. PARTIAL: the ordinance decay products (education/health half) are 1.
void cSC4ResidentialSimulator::ReadTunablePopulationValues(const cISCPropertyHolder* exemplar) {
    PopulationTuning& t = tuning_;
    t.eqDecay = 1.0f;
    t.hqDecay = 1.0f;
    t.newAge[0] = 25; t.newAge[1] = 30; t.newAge[2] = 35;
    t.resetAge = 40;
    t.eqReset = t.hqReset = 0.7f;
    t.newEQ[0] = 4.0f; t.newEQ[1] = 6.0f; t.newEQ[2] = 8.0f;
    t.deathsDoorHQ = t.deathsDoorHeight = t.deathsDoorDelta = t.deathsDoorChance = 0.0f;
    if (exemplar) {
        using H = cSCPropertyHelp;
        const std::vector<SC4Point> pollutionDefault = {{0.0f, 0.0f}, {32000.0f, 100.0f}};
        for (auto [curve, id] : {std::pair{&t.air, 0x6941b022u}, std::pair{&t.water, 0xa941b028u}, std::pair{&t.garbage, 0xc941b02eu}}) {
            curve->SetResponseCurveData(pollutionDefault);
            std::vector<float> values;
            if (exemplar->HasProperty(id) && H::GetPropertyValue(exemplar, id, values)) curve->SetResponseCurveData(values.data(), values.size());
        }
        H::GetPropertyValue(exemplar, 0xc929c433, t.eqDecay);
        H::GetPropertyValue(exemplar, 0x0929c439, t.hqDecay);
        H::GetPropertyValue(exemplar, 0xc929c652, t.newAge[0]);
        H::GetPropertyValue(exemplar, 0x0929cc30, t.newAge[1]);
        H::GetPropertyValue(exemplar, 0x0929cc36, t.newAge[2]);
        H::GetPropertyValue(exemplar, 0xca027307, t.resetAge);
        SC4Percentage percentage;
        if (H::GetPropertyValue(exemplar, 0xca027325, percentage)) t.eqReset = percentage.value;
        if (H::GetPropertyValue(exemplar, 0xca027328, percentage)) t.hqReset = percentage.value;
        H::GetPropertyValue(exemplar, 0xc929cc58, t.newEQ[0]);
        H::GetPropertyValue(exemplar, 0x0929cc65, t.newEQ[1]);
        H::GetPropertyValue(exemplar, 0x2929cc6b, t.newEQ[2]);
        H::GetPropertyValue(exemplar, 0xc929cc8d, t.newHQ[0]);
        H::GetPropertyValue(exemplar, 0x6929cc92, t.newHQ[1]);
        H::GetPropertyValue(exemplar, 0x4929cc97, t.newHQ[2]);
        H::GetPropertyValue(exemplar, 0x6ab70da7, t.deathsDoorHQ);
        H::GetPropertyValue(exemplar, 0x6ab71654, t.deathsDoorHeight);
        H::GetPropertyValue(exemplar, 0x4ab71659, t.deathsDoorDelta);
        H::GetPropertyValue(exemplar, 0x4ab70dac, t.deathsDoorChance);
    }
    tuningLoaded_ = true;
}

// 5.5 / education-health 4.7: one tract of state 4.
void cSC4ResidentialSimulator::DoPopulationChange(int x, int z) {
    const PopulationTuning& t = tuning_;
    const std::int32_t S = populationGrids_[0]->GetTractSize();
    if (S < 1 || S > 256) return;   // a damaged grid record (9.2 choice: the original hangs or faults)
    // the code assumes P = E (2): a record whose grids disagree in size would index out of range
    if (!ageGrid_->TractIsInBounds(x, z)) return;
    for (int w = 0; w < 3; ++w)
        if (!populationGrids_[w]->TractIsInBounds(x, z) || !eq_[w]->TractIsInBounds(x, z) || !hq_[w]->TractIsInBounds(x, z)) return;
    // 1. pollution: the tract's mean of each source grid, low byte only
    std::uint32_t p[3] = {};
    for (int q = 0; q < 3; ++q) {
        std::uint32_t sum = 0;
        const std::uint32_t x0 = static_cast<std::uint32_t>(S * x), z0 = static_cast<std::uint32_t>(S * z);
        for (std::uint32_t i = x0; i < x0 + static_cast<std::uint32_t>(S); ++i)
            for (std::uint32_t j = z0; j < z0 + static_cast<std::uint32_t>(S); ++j)
                sum += host_ ? host_->PollutionValue(q, static_cast<int>(i), static_cast<int>(j)) : 0;
        const std::uint32_t area = static_cast<std::uint32_t>(S * S);
        p[q] = (area ? sum / area : 0) & 0xff;
    }
    float hqLoss = t.hqDecay;
    hqLoss = t.air.GetOutputValue(float(p[0])) + hqLoss;
    hqLoss = t.water.GetOutputValue(float(p[1])) + hqLoss;
    hqLoss = t.garbage.GetOutputValue(float(p[2])) + hqLoss;

    // 2. decay, and the buzzards over a sick tract
    bool buzzard = false;
    for (int w = 0; w < 3; ++w) {
        float eq = eq_[w]->GetTractValue(x, z) - t.eqDecay;
        if (eq < 0.0f) eq = 0.0f;
        eq_[w]->SetTractValue(x, z, eq);
        float hq = hq_[w]->GetTractValue(x, z) - hqLoss;
        if (hq < 0.0f) hq = 0.0f;
        hq_[w]->SetTractValue(x, z, hq);
        if (hq < t.deathsDoorHQ && !buzzard && populationGrids_[w]->GetTractValue(x, z) != 0 && random_) {
            const double r = random_->RandomDoubleRangeUniform(0.0, 100.0);
            IResidentialEffect* effect = nullptr;
            if (r < t.deathsDoorChance && host_ && host_->CreateEffect("buzzards", &effect)) {
                const float widthX = eq_[w]->GetTractWidthX(), widthZ = eq_[w]->GetTractWidthZ();
                const float wx = widthX * float(x) + widthX * 0.5f, wz = widthZ * float(z) + widthZ * 0.5f;
                const float altitude = effect ? host_->TerrainAltitude(wx, wz) : 0.0f;
                const float y = static_cast<float>(double(altitude + t.deathsDoorHeight) +
                                                   random_->RandomDoubleRangeUniform(-t.deathsDoorDelta, t.deathsDoorDelta));
                if (effect) effect->StartAt(wx, y, wz);
                buzzard = true;
            }
        }
    }

    // 3. the developers' population, all three read before any write
    std::uint32_t fresh[3] = {};
    for (int w = 0; w < 3; ++w) {
        const cSC4SimGrid<std::int16_t>* map = host_ ? host_->DeveloperPopulationMap(w + 1) : nullptr;
        if (map && map->TractIsInBounds(x, z)) fresh[w] = static_cast<std::uint16_t>(map->GetTractValue(x, z));
    }
    // 4. population, newcomers' EQ / HQ
    std::uint32_t total = 0, old[3] = {}, arrivals[3] = {};
    bool arrived = false;
    for (int w = 0; w < 3; ++w) {
        total += fresh[w];
        old[w] = populationGrids_[w]->GetTractValue(x, z);
        populationGrids_[w]->SetTractValue(x, z, static_cast<std::uint16_t>(fresh[w]));
        runningPopulation_ += static_cast<std::int32_t>(fresh[w]);
        if (old[w] < fresh[w]) {
            arrivals[w] = static_cast<std::uint16_t>(fresh[w] - old[w]);
            const std::int32_t delta = static_cast<std::int16_t>(fresh[w] - old[w]);
            const auto blend = [&](SimGridPtr<float>& grid, float bonus) {
                const float q = grid->GetTractValue(x, z);
                const float m = q > 0.0f ? 2.0f : 1.0f;
                const float blended = ((((q + bonus) / m) * float(delta)) + float(old[w]) * q) / float(fresh[w]);
                grid->SetTractValue(x, z, std::min(200.0f, blended));
            };
            blend(eq_[w], t.newEQ[w]);
            blend(hq_[w], t.newHQ[w]);
            arrived = true;
        }
        if (fresh[w] == 0) {
            eq_[w]->SetTractValue(x, z, 0.0f);
            hq_[w]->SetTractValue(x, z, 0.0f);
        }
    }
    // 5. ageing (the month is read per tract)
    std::int32_t age = ageGrid_->GetTractValue(x, z);
    if (total != 0) {
        if (host_ && host_->CurrentMonth() == 12) age = (age + 1) & 0xff;
    } else {
        age = 0;
    }
    // 6. the newcomers' age (the numerator keeps the old populations of every level)
    if (arrived) {
        const std::int32_t sumNew = static_cast<std::int32_t>(fresh[0] + fresh[1] + fresh[2]);
        if (sumNew != 0) {
            std::int32_t numerator = 0;
            for (int w = 0; w < 3; ++w)
                numerator += static_cast<std::int32_t>(old[w]) * age + static_cast<std::int32_t>(arrivals[w]) * t.newAge[w];
            age = numerator / sumNew;
        } else {
            age = 0;
        }
    }
    // 7. 8. life expectancy after the EQ/HQ updates; the generational reset
    const std::int32_t lifeExpectancy = GetCellLifeExpectancy(static_cast<unsigned>(S * x), static_cast<unsigned>(S * z));
    if (age > lifeExpectancy) {
        for (int w = 0; w < 3; ++w) {
            eq_[w]->SetTractValue(x, z, eq_[w]->GetTractValue(x, z) * t.eqReset);
            hq_[w]->SetTractValue(x, z, hq_[w]->GetTractValue(x, z) * t.hqReset);
        }
        age = t.resetAge;
    }
    // 9. 10. 11.
    ageGrid_->SetTractValue(x, z, static_cast<std::uint8_t>(age));
    leSum_ += static_cast<std::uint64_t>(static_cast<std::int64_t>(lifeExpectancy * static_cast<std::int32_t>(total)));
    std::int32_t bucket = (age - 20) / 3;
    if (static_cast<std::uint32_t>(bucket) >= 20) bucket = 19;
    for (int w = 0; w < 3; ++w) {
        const std::int32_t pop = populationGrids_[w]->GetTractValue(x, z);
        ageCount_[bucket] += pop;
        ageEqSum_[bucket] += static_cast<std::int32_t>(eq_[w]->GetTractValue(x, z)) * pop;
    }
}

// 5.6
void cSC4ResidentialSimulator::PostPopulationChange() {
    if (std::getenv("SC4_RESDEBUG")) {
        long sums[3] = {};
        int sizes[3] = {};
        for (int w = 0; w < 3; ++w) {
            const cSC4SimGrid<std::int16_t>* map = host_ ? host_->DeveloperPopulationMap(w + 1) : nullptr;
            sizes[w] = map ? map->GetTractSize() : -1;
            if (map) for (const auto& column : map->GetGridData()) for (std::int16_t v : column) sums[w] += v;
        }
        std::fprintf(stderr, "residential: population %d -> %d (developer maps %ld/%ld/%ld, tract sizes %d/%d/%d)\n", population_,
                     runningPopulation_, sums[0], sums[1], sums[2], sizes[0], sizes[1], sizes[2]);
    }
    population_ = runningPopulation_;
    const std::int64_t divisor = population_;
    RecordHistoryData(kHistoryLifeExpectancy,
                      population_ != 0 ? static_cast<std::int32_t>(leSum_ / static_cast<std::uint64_t>(divisor)) : 0);
    float average[20];
    for (int b = 0; b < 20; ++b)
        average[b] = ageCount_[b] != 0 ? float(static_cast<std::uint32_t>(ageEqSum_[b])) / float(static_cast<std::uint32_t>(ageCount_[b])) : 0.0f;
    for (std::uint32_t g = 0; g < 9; ++g) {
        std::int32_t popSum = 0, eqSum = 0;
        for (int b = 0; b < 20; ++b) {
            if (ageCount_[b] <= 0) continue;
            const float scaled = wealthCurves[g].GetOutputValue(float(21 + 3 * b)) * float(ageCount_[b]);
            const std::int32_t n = static_cast<std::int32_t>(scaled * 0.01f);
            popSum += n;
            eqSum = static_cast<std::int32_t>(float(n) * average[b] + float(eqSum));
        }
        RecordHistoryData(kHistoryAgeGroupPopulation + g, popSum);
        RecordHistoryData(kHistoryAgeGroupEQ + g, popSum > 0 ? eqSum / popSum : 0);
    }
}

// 5.1: one state per call, 0 -> 13 -> 1 -> 14 -> 2 -> 15 -> 3 -> 11 -> 4 -> 12 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10.
bool cSC4ResidentialSimulator::DoDailySimulation(std::uint32_t milliseconds) {
    if (!initialized_) {
        static int shown = 0;
        if (std::getenv("SC4_RESDEBUG") && shown++ < 3) std::fprintf(stderr, "residential: day before Init\n");
        return true;
    }
    const std::uint64_t slice = host_ ? host_->BeginSlice(milliseconds) : 0;
    const auto expired = [&]() { return host_ && host_->SliceExpired(slice); };
    const auto passes = [&]() {   // until a pass changes nothing; false when time ran out between passes
        while (ProximityPass())
            if (expired()) return false;
        return true;
    };
    switch (state_) {
    case 0:
        // PARTIAL (education/health half): grade totals, strike checks, ordinance effects, pending building lists.
        state_ = 13;
        return true;
    case 13: SeedProximity(1); state_ = 1; return true;
    case 1:
        if (!passes()) return false;
        CopyProximity(1);
        state_ = 14;
        return true;
    case 14: SeedProximity(2); state_ = 2; return true;
    case 2:
        if (!passes()) return false;
        CopyProximity(2);
        state_ = 15;
        return true;
    case 15:
        SeedProximity(3);
        while (ProximityPass()) {}
        state_ = 3;
        return true;
    case 3:
        if (!passes()) return false;
        CopyProximity(3);
        work_.reset();
        state_ = 11;
        return true;
    case 11:
        std::fill(std::begin(ageCount_), std::end(ageCount_), 0);
        std::fill(std::begin(ageEqSum_), std::end(ageEqSum_), 0);
        leSum_ = 0;
        runningPopulation_ = 0;
        tuningLoaded_ = false;
        state_ = 4;
        return true;
    case 4: {
        if (!tuningLoaded_) ReadTunablePopulationValues(host_ ? host_->SimulatorExemplar() : nullptr);
        std::int32_t x = 0, z = 0;
        for (int n = 1; NextTract(x, z); ++n) {
            DoPopulationChange(x, z);
            if ((n & 15) == 0 && expired()) return false;
        }
        state_ = 12;
        return true;
    }
    case 12:
        ResetIterator();
        PostPopulationChange();
        state_ = 5;
        return true;
    // PARTIAL (education/health half): school coverage and grading, hospital coverage and grading.
    case 5: schoolCovered_ = 0; state_ = 6; return true;
    case 6: state_ = 7; return true;
    case 7: hospitalCovered_ = 0; state_ = 8; return true;
    case 8: state_ = 9; return true;
    case 9:
        RecordHistoryData(kHistoryGlobalEQ, static_cast<std::int32_t>(GetGlobalEQ()));
        RecordHistoryData(kHistoryGlobalHQ, static_cast<std::int32_t>(GetGlobalHQ()));
        RecordHistoryData(kHistoryPopulation, population_);
        if (population_ > 0) {
            const float hospital = float(hospitalCovered_) / float(population_);
            RecordHistoryData(kHistorySchoolCoverage, RoundHalfEven(float(schoolCovered_) / float(population_) * 1000.0f));
            RecordHistoryData(kHistoryHospitalCoverage, RoundHalfEven(hospital * 1000.0f));
        } else {
            RecordHistoryData(kHistoryHospitalCoverage, 0);
            RecordHistoryData(kHistorySchoolCoverage, 0);
        }
        state_ = kStateIdle;
        return true;
    default:   // 10 and anything above 15: waits for the month
        return true;
    }
}

bool cSC4ResidentialSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    switch (message->GetType()) {
    case kMessageSimNewDay: {
        const std::uint32_t before = state_;
        const bool finished = DoDailySimulation(5);
        static const bool trace = std::getenv("SC4_RESDEBUG") != nullptr;
        if (trace && (before != state_ || !finished)) std::fprintf(stderr, "residential: day: state %u -> %u%s\n", before, state_, finished ? "" : " (out of time)");
        return finished;
    }
    case kMessageSimNewMonth:
        if (state_ == kStateIdle) {
            state_ = 0;
            fundingChanged_ = false;
        } else {
            fundingChanged_ = true;
        }
        return true;
    // PARTIAL (education/health half): occupant inserted / removed -> DoBuildingChanged (school, hospital, health-affecting lists).
    default: return true;
    }
}

// ---- population-half queries (6.1, education-health 4.8) ----

float cSC4ResidentialSimulator::TractQuotient(const SimGridPtr<float> (&values)[3], int x, int z) const {
    if (!populationGrids_[0] || !values[0] || !populationGrids_[0]->TractIsInBounds(x, z)) return 0.0f;
    const std::uint32_t p0 = populationGrids_[0]->GetTractValue(x, z), p1 = populationGrids_[1]->GetTractValue(x, z),
                        p2 = populationGrids_[2]->GetTractValue(x, z);
    const std::uint16_t sum = static_cast<std::uint16_t>(p0 + p1 + p2);
    if (sum == 0) return 0.0f;
    float s = values[2]->GetTractValue(x, z) * float(p2);
    s = values[1]->GetTractValue(x, z) * float(p1) + s;
    return (values[0]->GetTractValue(x, z) * float(p0) + s) / float(sum);
}

float cSC4ResidentialSimulator::GetCellEQ(unsigned x, unsigned z) const {
    if (!eq_[0]) return 0.0f;
    const unsigned T = static_cast<unsigned>(eq_[0]->GetTractSize());
    return T ? TractQuotient(eq_, static_cast<int>(x / T), static_cast<int>(z / T)) : 0.0f;
}

float cSC4ResidentialSimulator::GetCellHQ(unsigned x, unsigned z) const {
    if (!hq_[0]) return 0.0f;
    const unsigned T = static_cast<unsigned>(hq_[0]->GetTractSize());
    return T ? TractQuotient(hq_, static_cast<int>(x / T), static_cast<int>(z / T)) : 0.0f;
}

float cSC4ResidentialSimulator::GetCellEQByWealth(unsigned x, unsigned z, int wealth) const {
    if (wealth < 1 || wealth > 3 || !eq_[0]) return 0.0f;   // the original reads out of bounds
    const unsigned T = static_cast<unsigned>(eq_[0]->GetTractSize());
    const int tx = static_cast<int>(x / T), tz = static_cast<int>(z / T);
    return eq_[wealth - 1]->TractIsInBounds(tx, tz) ? eq_[wealth - 1]->GetTractValue(tx, tz) : 0.0f;
}

float cSC4ResidentialSimulator::GetCellHQByWealth(unsigned x, unsigned z, int wealth) const {
    if (wealth < 1 || wealth > 3 || !hq_[0]) return 0.0f;
    const unsigned T = static_cast<unsigned>(hq_[0]->GetTractSize());
    const int tx = static_cast<int>(x / T), tz = static_cast<int>(z / T);
    return hq_[wealth - 1]->TractIsInBounds(tx, tz) ? hq_[wealth - 1]->GetTractValue(tx, tz) : 0.0f;
}

float cSC4ResidentialSimulator::GlobalQuotient(const SimGridPtr<float> (&values)[3]) const {
    if (population_ == 0 || !values[0] || !populationGrids_[0]) return 0.0f;
    float sum = 0.0f;
    const std::int32_t countX = values[0]->GetTractCountX(), countZ = values[0]->GetTractCountZ();
    for (std::int32_t x = 0; x < countX; ++x) {
        for (std::int32_t z = 0; z < countZ; ++z) {
            if (!populationGrids_[0]->TractIsInBounds(x, z)) continue;
            const float a = values[0]->GetTractValue(x, z) * float(populationGrids_[0]->GetTractValue(x, z));
            const float b = values[1]->GetTractValue(x, z) * float(populationGrids_[1]->GetTractValue(x, z)) + a;
            const float c = values[2]->GetTractValue(x, z) * float(populationGrids_[2]->GetTractValue(x, z)) + b;
            sum = c + sum;
        }
    }
    return sum / float(population_);
}

float cSC4ResidentialSimulator::GetGlobalEQ() const { return GlobalQuotient(eq_); }
float cSC4ResidentialSimulator::GetGlobalHQ() const { return GlobalQuotient(hq_); }

std::uint8_t cSC4ResidentialSimulator::GetCellLifeExpectancy(unsigned x, unsigned z) {
    return static_cast<std::uint8_t>(GetCellLifeExpectancy(host_ ? host_->SimulatorExemplar() : nullptr, x, z));
}

std::uint8_t cSC4ResidentialSimulator::GetGlobalLE() {
    const cISCPropertyHolder* exemplar = host_ ? host_->SimulatorExemplar() : nullptr;
    if (!exemplar) return 0;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {200.0f, 200.0f}});
    std::vector<float> values;
    if (exemplar->HasProperty(0x6941a67f) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x6941a67f, values)) curve.SetResponseCurveData(values.data(), values.size());
    return static_cast<std::uint8_t>(static_cast<std::uint32_t>(curve.GetOutputValue(GetGlobalHQ())));
}

// ---- streams (residential.md 9) ------------------------------------------------------------------------------------

namespace {
template <class T> void ReadGrid(cIGZIStream& stream, SimGridPtr<T>& grid) {
    void* raw = nullptr;
    grid.reset();
    if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) grid.reset(SimGridCast<T>(raw));
}
template <class T> void WriteGrid(cIGZOStream& stream, const SimGridPtr<T>& grid) {
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(grid.get()));
}

// The department block: f0, max funding, strike efficiency, strike days, min grade, strike flag, strike start, two references,
// init funding, the 100.0 field, the message id. Version < 5 stores the percentages unscaled, version < 6 the min grade too.
void WriteDepartment(cIGZOStream& stream, const DepartmentData& d, const cRetainedReference* refs) {
    stream.SetFloat32(d.minimumFunding);
    stream.SetFloat32(d.strikeThreshold);
    stream.SetFloat32(d.value08);
    stream.SetSint32(static_cast<std::int32_t>(d.strikeDays));
    stream.SetFloat32(d.strikeEndFunding);
    stream.SetUint8(d.onStrike ? 1 : 0);
    stream.SetSint32(static_cast<std::int32_t>(d.strikeStartDay));
    refs[0].Write(stream);
    refs[1].Write(stream);
    stream.SetFloat32(d.value90);
    stream.SetFloat32(d.value94);
    stream.SetUint32(d.messageId);
}

void ReadDepartment(cIGZIStream& stream, DepartmentData& d, cRetainedReference* refs, std::uint16_t version) {
    const auto pct = [&](float& field, bool scaled) {
        float v = 0.0f;
        stream.GetFloat32(v);
        field = scaled ? v * 0.01f : v;
    };
    pct(d.minimumFunding, version < 5);
    pct(d.strikeThreshold, version < 5);
    pct(d.value08, version < 5);
    std::int32_t days = 0;
    stream.GetSint32(days);
    d.strikeDays = static_cast<std::uint32_t>(days);
    pct(d.strikeEndFunding, version < 6);
    std::uint8_t onStrike = 0;
    stream.GetUint8(onStrike);
    d.onStrike = onStrike != 0;
    std::int32_t start = 0;
    stream.GetSint32(start);
    d.strikeStartDay = static_cast<std::uint32_t>(start);
    refs[0].Read(stream);
    refs[1].Read(stream);
    pct(d.value90, version < 5);
    stream.GetFloat32(d.value94);
    stream.GetUint32(d.messageId);
}

void WriteList(cIGZOStream& stream, const cRetainedReferences& list) {
    stream.SetUint32(static_cast<std::uint32_t>(list.size()));
    for (const cRetainedReference& ref : list) ref.Write(stream);
}

// a count of 0 or >= 10,000,000 reads nothing more; null entries are dropped (9.2 RS12 choice)
void ReadList(cIGZIStream& stream, cRetainedReferences& list) {
    list.clear();
    std::uint32_t count = 0;
    stream.GetUint32(count);
    if (count >= 10000000u) return;
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) {
        cRetainedReference ref;
        ref.Read(stream);
        if (ref.get()) list.push_back(std::move(ref));
    }
}
}  // namespace

// 9.1: version 7.
bool cSC4ResidentialSimulator::WriteCityRecord(cIGZOStream& stream) const {
    stream.SetUint16(7);
    stream.SetSint32(proximityTractSize_);
    for (const auto& grid : proximity_) WriteGrid(stream, grid);
    stream.SetSint32(populationTractSize_);
    for (const auto& grid : populationGrids_) WriteGrid(stream, grid);
    WriteGrid(stream, ageGrid_);
    stream.SetUint32(static_cast<std::uint32_t>(schoolCoverage_.size()));
    for (const auto& grid : schoolCoverage_) WriteGrid(stream, grid);
    stream.SetUint32(static_cast<std::uint32_t>(hospitalCoverage_.size()));
    for (const auto& grid : hospitalCoverage_) WriteGrid(stream, grid);
    WriteGrid(stream, schoolGrade_);
    WriteGrid(stream, hospitalGrade_);
    stream.SetSint32(eqTractSize_);
    for (const auto& grid : eq_) WriteGrid(stream, grid);
    for (const auto& grid : hq_) WriteGrid(stream, grid);
    stream.SetFloat32(unused98_);
    stream.SetFloat32(unused9c_);
    stream.SetUint32(state_);
    stream.SetUint8(fundingChanged_ || state_ != kStateIdle ? 1 : 0);
    stream.SetSint32(population_);
    WriteDepartment(stream, health_, &departmentRefs_[0]);
    WriteDepartment(stream, education_, &departmentRefs_[2]);
    // the occupant lists as read: 3 pending lists, the school lists, the hospital lists, the health-affecting list
    static const cRetainedReferences none;
    const auto list = [&](std::size_t i) -> const cRetainedReferences& { return i < fileLists_.size() ? fileLists_[i] : none; };
    std::size_t next = 0;
    for (int i = 0; i < 3; ++i) WriteList(stream, list(next++));
    stream.SetUint32(fileSchoolListCount_);
    for (std::uint32_t i = 0; i < fileSchoolListCount_; ++i) WriteList(stream, list(next++));
    stream.SetUint32(fileHospitalListCount_);
    for (std::uint32_t i = 0; i < fileHospitalListCount_; ++i) WriteList(stream, list(next++));
    WriteList(stream, list(next++));
    stream << iterator_;
    stream.SetUint64(leSum_);
    stream.SetSint32(runningPopulation_);
    stream.SetUint8(autoBudgetSchools_ ? 1 : 0);
    stream.SetUint8(autoBudgetHospitals_ ? 1 : 0);
    return true;
}

// 9.2. A damaged record (stream error, coverage or list counts other than 7 / 3, tract sizes outside 1..256, a state above 15)
// leaves the simulator as constructed, so the next Init sets the city up as new.
bool cSC4ResidentialSimulator::ReadCityRecord(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    stream.GetSint32(proximityTractSize_);
    for (auto& grid : proximity_) ReadGrid(stream, grid);
    stream.GetSint32(populationTractSize_);
    for (auto& grid : populationGrids_) ReadGrid(stream, grid);
    ReadGrid(stream, ageGrid_);
    std::uint32_t schoolCoverage = 0, hospitalCoverage = 0;
    stream.GetUint32(schoolCoverage);
    bool damaged = schoolCoverage != 7;
    schoolCoverage_.resize(damaged ? 0 : 7);
    for (auto& grid : schoolCoverage_) ReadGrid(stream, grid);
    stream.GetUint32(hospitalCoverage);
    damaged = damaged || hospitalCoverage != 3;
    hospitalCoverage_.resize(damaged ? 0 : 3);
    for (auto& grid : hospitalCoverage_) ReadGrid(stream, grid);
    if (!damaged) {
        ReadGrid(stream, schoolGrade_);
        ReadGrid(stream, hospitalGrade_);
        stream.GetSint32(eqTractSize_);
        for (auto& grid : eq_) ReadGrid(stream, grid);
        for (auto& grid : hq_) ReadGrid(stream, grid);
        stream.GetFloat32(unused98_);
        stream.GetFloat32(unused9c_);
        stream.GetUint32(state_);
        std::uint8_t dirty = 0;
        stream.GetUint8(dirty);
        fundingChanged_ = dirty != 0;
        stream.GetSint32(population_);
        ReadDepartment(stream, health_, &departmentRefs_[0], version);
        ReadDepartment(stream, education_, &departmentRefs_[2], version);
        fileLists_.clear();
        const auto list = [&]() { fileLists_.emplace_back(); ReadList(stream, fileLists_.back()); };
        for (int i = 0; i < 3; ++i) list();
        stream.GetUint32(fileSchoolListCount_);
        damaged = fileSchoolListCount_ != 7;
        for (std::uint32_t i = 0; !damaged && i < fileSchoolListCount_; ++i) list();
        if (!damaged) {
            stream.GetUint32(fileHospitalListCount_);
            damaged = fileHospitalListCount_ != 3;
            for (std::uint32_t i = 0; !damaged && i < fileHospitalListCount_; ++i) list();
        }
        if (!damaged) list();
    }
    if (!damaged) {
        if (version >= 3) {
            stream >> iterator_;
            stream.GetUint64(leSum_);
            if (version >= 4) stream.GetSint32(runningPopulation_);
        } else {
            ResetIterator();
            leSum_ = 0;
            runningPopulation_ = 0;
            if (state_ == 4) state_ = 11;
        }
        std::uint8_t a = 0, b = 0;
        if (version >= 7) {
            stream.GetUint8(a);
            stream.GetUint8(b);
        }
        autoBudgetSchools_ = a != 0;
        autoBudgetHospitals_ = b != 0;
    }
    const auto sizeOk = [](std::int32_t s) { return s >= 1 && s <= 256; };
    damaged = damaged || stream.GetError() || !sizeOk(proximityTractSize_) || !sizeOk(populationTractSize_) || !sizeOk(eqTractSize_) ||
              state_ > 15;   // the grids are references the load fills in later: their own sizes are checked where state 4 uses them
    int nullGrids = 0;
    for (int w = 0; w < 3; ++w) nullGrids += !proximity_[w] + !populationGrids_[w] + !eq_[w] + !hq_[w];
    nullGrids += !ageGrid_ + !schoolGrade_ + !hospitalGrade_;
    for (const auto& grid : schoolCoverage_) nullGrids += !grid;
    for (const auto& grid : hospitalCoverage_) nullGrids += !grid;
    if (std::getenv("SC4_RESDEBUG"))
        std::fprintf(stderr, "residential: read v%u null grids %d damaged=%d error=%d sizes %d/%d/%d state %u pop %d lists %u/%u grid0 %d\n", version, nullGrids, damaged,
                     int(stream.GetError()), proximityTractSize_, populationTractSize_, eqTractSize_, state_, population_, fileSchoolListCount_,
                     fileHospitalListCount_, populationGrids_[0] ? populationGrids_[0]->GetTractSize() : -1);
    if (damaged) {
        ReleaseGrids();
        for (cRetainedReference& ref : departmentRefs_) ref = cRetainedReference();
        fileLists_.clear();
        fileSchoolListCount_ = 7;
        fileHospitalListCount_ = 3;
        proximityTractSize_ = 1;
        populationTractSize_ = eqTractSize_ = 4;
        state_ = 0;
        population_ = runningPopulation_ = 0;
        leSum_ = 0;
        unused98_ = unused9c_ = 0.0f;
        iterator_ = ScanningIterator();
        ReadSimulatorTunableValues(nullptr);
        cityStarted_ = false;
        return false;
    }
    if (state_ == 1) state_ = 13;
    else if (state_ == 2) state_ = 14;
    else if (state_ == 3) state_ = 15;
    tuningLoaded_ = false;
    cityStarted_ = true;
    return true;
}

bool cSC4ResidentialSimulator::Write(cIGZOStream& stream) const { return WriteCityRecord(stream); }
bool cSC4ResidentialSimulator::Read(cIGZIStream& stream) { return ReadCityRecord(stream); }

}  // namespace sc4
