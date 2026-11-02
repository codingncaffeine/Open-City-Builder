#include "sc4/ResidentialSimulator.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"

namespace sc4 {

namespace {
// Building recognition, budget purposes, per-building properties and curves of the two services (REH 2, 4.1, 4.13).
struct Service {
    bool school;
    std::uint32_t staffPurpose, vehiclePurpose;
    std::uint32_t recognitionGroups[2];
    std::uint32_t served, effectiveCapacity, grade, staffRatio, capacity, radius, effect;
    std::uint32_t ageCurve, wealthCurve, distanceCurve;
    std::uint32_t strikeGroup;
    std::uint32_t groups[7];
    std::size_t groupCount;
    // ordinance effect properties: radius, distance curve, effect, capacity, age curve, decay
    std::uint32_t ordRadius, ordDistance, ordEffect, ordCapacity, ordAge, ordDecay;
};
constexpr Service kSchool{true, 0xea5654b6, 0x4a5654ba, {0x1503, 0x151b}, 0x09220161, 0xe9244b2e, 0x292453c5, 0x2a976c20,
                          0x8922041b, 0x691b42b3, 0xa92ae446, 0xc9299662, 0xc92d7ec0, 0xe921a936, 0x1a03,
                          {0x150f, 0x1510, 0x1514, 0x1504, 0x1505, 0x1506, 0x151b}, 7,
                          0xa91b3af4, 0xa91b3afa, 0xa92d9d7a, 0x892d9d02, 0xc91b3b02, 0x692ef65a};
constexpr Service kHospital{false, 0xca565486, 0xea56549e, {0x1507, 0x151c}, 0xc922015c, 0x49244b21, 0x092453cb, 0xaa976be7,
                            0x69220415, 0x091b42a2, 0xc9299659, 0xa92ae3bd, 0xa92d9fcd, 0x2921a9cc, 0x1a02,
                            {0x1512, 0x1513, 0x151c}, 3,
                            0x491b3ad5, 0x891b3ae6, 0xe91b3aee, 0x092d909b, 0xe92d9db4, 0xc92d9c7a};
const Service& Svc(bool school) { return school ? kSchool : kHospital; }

constexpr std::uint32_t kHealthAffectingA = 0x6a19f6b5, kHealthAffectingB = 0xaa19f6ea;
constexpr std::uint32_t kDepartmentGroup = 0x6a357b7f, kDepartmentNameGroup = 0x6a3ff01c;
constexpr std::uint32_t kNotGraded = 0xffffffffu;

// The code defaults of the department curves (REH 4.13).
const std::vector<SC4Point> kEfficiencyCurve = {{0.0f, 0.0f}, {100.0f, 100.0f}, {120.0f, 110.0f}};
const std::vector<SC4Point> kRisingCurve = {{0.0f, 20.0f}, {100.0f, 100.0f}};
const std::vector<SC4Point> kFlatCurve = {{0.0f, 100.0f}, {100.0f, 100.0f}};

// The property-pointer loader of a curve after a rebuild from its code default (foundations 4.1 "default" pattern).
void LoadCurve(const cISCPropertyHolder* holder, std::uint32_t id, cSC4ResponseCurve& curve, const std::vector<SC4Point>& fallback) {
    curve.SetResponseCurveData(fallback);
    std::vector<float> values;
    if (holder && holder->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(holder, id, values)) curve.SetResponseCurveData(values.data(), values.size());
}

template <class T> T PropertyOr(const cISCPropertyHolder* holder, std::uint32_t id, T fallback) {
    T value = fallback;
    if (!cSCPropertyHelp::GetPropertyValue(holder, id, value)) value = fallback;
    return value;
}

// _ftol into a u32 (the product is never negative in practice; REH 4.5 compat note)
std::uint32_t Trunc32(float v) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(v)); }

template <class T> T TractOr0(const cSC4SimGrid<T>* grid, std::int32_t x, std::int32_t z) {
    return grid && grid->TractIsInBounds(x, z) ? grid->GetTractValue(x, z) : T{};   // a smaller grid reads 0 (REH 4.6 choice)
}

// GetTractsInRadius (0x005e5200): x-major, the centre advanced by += w, the upper clamp unsigned.
void TractsInRadius(float px, float pz, float r, float w, std::int32_t countX, std::int32_t countZ,
                    cSC4ResidentialSimulator::TractList& out) {
    const float inv = 1.0f / w;
    std::int32_t x0 = static_cast<std::int32_t>((px - r) * inv), x1 = static_cast<std::int32_t>((px + r) * inv);
    std::int32_t z0 = static_cast<std::int32_t>((pz - r) * inv), z1 = static_cast<std::int32_t>((pz + r) * inv);
    if (x0 < 0) x0 = 0;
    if (static_cast<std::uint32_t>(x1) >= static_cast<std::uint32_t>(countX)) x1 = countX - 1;
    if (z0 < 0) z0 = 0;
    if (static_cast<std::uint32_t>(z1) >= static_cast<std::uint32_t>(countZ)) z1 = countZ - 1;
    const float rr = r * r;
    float cx = static_cast<float>(x0) * w + w * 0.5f;
    for (std::int32_t x = x0; x <= x1; ++x, cx += w) {
        float cz = static_cast<float>(z0) * w + w * 0.5f;
        for (std::int32_t z = z0; z <= z1; ++z, cz += w) {
            const float dx = cx - px, dz = cz - pz;
            const float dx2 = dx * dx;
            if (dz * dz + dx2 <= rr) out.emplace_back(x, z);
        }
    }
}
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
        ReleaseBuildingLists();
        delete this;
        return 0;
    }
    return --refCount_;
}

// ---- tuning (REH 4.13) ------------------------------------------------------------------------------------

void cSC4ResidentialSimulator::SetDefaultCurves(DepartmentData& d) {
    d.efficiencyCurve.SetResponseCurveData(kEfficiencyCurve);
    d.coverageCurve.SetResponseCurveData(kRisingCurve);
    d.strikeCurve.SetResponseCurveData(kRisingCurve);
    d.minimumFunding = 0.6f;
    d.initialFunding = 0.25f;
    d.maxFunding = 1.2f;
    d.strikeEfficiency = 0.2f;
    d.strikeDays = 10;
    d.minGradeToEnd = 0.9f;
    d.value94 = 100.0f;
    d.onStrike = false;
    d.strikeStartDay = 0;
}

void cSC4ResidentialSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    SetDefaultCurves(health_);
    health_.messageId = kMessageHealthStrike;
    SetDefaultCurves(education_);
    education_.messageId = kMessageEducationStrike;
    for (cSC4ResponseCurve& curve : wealthCurves) curve.SetResponseCurveData(kEfficiencyCurve);
    if (!exemplar) return;

    using H = cSCPropertyHelp;
    struct Ids { std::uint32_t obsolete, initMax, strikeEfficiency, duration, minGrade, efficiency, coverage, strike; };
    const auto department = [&](DepartmentData& d, const Ids& id, const std::vector<SC4Point>& coverageDefault) {
        float f = 0;
        if (H::GetPropertyValue(exemplar, id.obsolete, f)) d.minimumFunding = f * 0.01f;
        std::vector<float> pair;
        if (H::GetPropertyValue(exemplar, id.initMax, pair) && pair.size() > 1) {
            d.initialFunding = pair[0] * 0.01f;
            d.maxFunding = pair[1] * 0.01f;
        }
        if (H::GetPropertyValue(exemplar, id.strikeEfficiency, f)) d.strikeEfficiency = f * 0.01f;
        std::int32_t days = 0;
        if (H::GetPropertyValue(exemplar, id.duration, days)) d.strikeDays = static_cast<std::uint32_t>(days);
        SC4Percentage percentage;
        if (H::GetPropertyValue(exemplar, id.minGrade, percentage)) d.minGradeToEnd = percentage.value;
        LoadCurve(exemplar, id.efficiency, d.efficiencyCurve, kEfficiencyCurve);
        LoadCurve(exemplar, id.coverage, d.coverageCurve, coverageDefault);
        LoadCurve(exemplar, id.strike, d.strikeCurve, kRisingCurve);
    };
    department(health_, {0xe918946f, 0xa9189479, 0x89189482, 0x8918948b, 0x89189495, 0x2918949e, 0x0a53b2f9, 0x691894a4}, kRisingCurve);
    department(education_, {0xc91894a8, 0x891894ad, 0xe91894b3, 0xa91894bc, 0x691894c0, 0x691894c6, 0xea53b30f, 0xc91894cb}, kRisingCurve);

    for (std::size_t i = 0; i < wealthCurves.size(); ++i) LoadCurve(exemplar, 0xea540621u + std::uint32_t(i), wealthCurves[i], kEfficiencyCurve);
    // set only when the exemplar exists; the values here are that branch's code defaults
    schoolStaffDivisor = hospitalStaffDivisor = 30;
    H::GetPropertyValue(exemplar, 0x8a891421, schoolStaffDivisor);
    H::GetPropertyValue(exemplar, 0x8a89142a, hospitalStaffDivisor);
    coverageSlop = 1.1f;
    H::GetPropertyValue(exemplar, 0x6a9f6edd, coverageSlop);
    maxEQBoost = maxHQBoost = 10.0f;
    H::GetPropertyValue(exemplar, 0x4aa84161, maxEQBoost);
    H::GetPropertyValue(exemplar, 0x2aa84164, maxHQBoost);
    schoolChunks.clear();
    hospitalChunks.clear();
    H::GetPropertyValue(exemplar, 0xec43826b, schoolChunks);
    H::GetPropertyValue(exemplar, 0xac43826d, hospitalChunks);
}

// ---- departments and budget items (REH 4.2) -----------------------------------------------------------------

// budget.GetBudgetItemForPurpose; false without a budget simulator. The departments key locally funded objects by the purpose id,
// the ChangeBudgetItem* calls take the item's index (BudgetItem::purpose).
bool cSC4ResidentialSimulator::FindBudgetItem(cISCPropertyHolder* holder, std::uint32_t purpose, BudgetItem& item) const {
    cSC4BudgetSimulator* budget = host_ ? host_->BudgetSimulator() : nullptr;
    return budget && holder && budget->GetBudgetItemForPurpose(holder, purpose, item);
}

void cSC4ResidentialSimulator::CreateDepartments() {
    cSC4BudgetSimulator* budget = host_ ? host_->BudgetSimulator() : nullptr;
    if (!budget) return;
    struct Spec { std::uint32_t id, name; const DepartmentData* data; bool secondary; };
    const Spec specs[4] = {{0x09188f42, 0xca493a56, &health_, false}, {0xaa538cb3, 0xca538d98, &health_, true},
                           {0x09188f4c, 0x6a493a66, &education_, false}, {0x4a538cc6, 0x8a538dac, &education_, true}};
    for (int k = 0; k < 4; ++k) {
        if (departmentRefs_[k].get()) continue;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(specs[k].id);
        if (!department) {
            department = budget->CreateDepartmentBudget(specs[k].id, kDepartmentGroup);
            department->SetDepartmentName(kDepartmentNameGroup, specs[k].name);
            department->SetMaxAllowedFundingPercentage(specs[k].data->maxFunding);
            department->SetFundingPercentage(specs[k].secondary ? 1.0f : specs[k].data->initialFunding, 0);
            if (specs[k].secondary) department->CreateLineItem(1, true)->SetDisplayFlag(1, false);
        }
        departmentRefs_[k].reset(static_cast<cIGZSerializable*>(department));
    }
}

// The budget part of DoBuildingChanged for a building with the staff item (REH 4.1 step 1); false = a null department, which
// aborts the whole call.
bool cSC4ResidentialSimulator::RegisterWithDepartment(IResidentialOccupant* building, bool added, std::uint32_t staffPurpose,
                                                      std::uint32_t vehiclePurpose) {
    cSC4BudgetSimulator* budget = host_->BudgetSimulator();
    cISCPropertyHolder* holder = building->PropertyHolder();
    BudgetItem item;
    FindBudgetItem(holder, staffPurpose, item);
    std::uint32_t line = static_cast<std::uint32_t>(item.lineItemId);
    if (line == 0) {
        line = building->BuildingType();
        budget->ChangeBudgetItemLine(holder, static_cast<int>(item.purpose), line);
    }
    cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
    if (!department) return false;
    if (added) {
        if (!department->GetLineItem(line)) department->CreateLineItemForBuildingType(line, true)->SetDisplayFlag(2, true);
        department->AddLocallyFundedObject(holder, staffPurpose);
    } else {
        department->RemoveLocallyFundedObject(holder, staffPurpose);
    }
    // the line item's building count; the original fetches it with no null check on removal (choice: skip when missing)
    if (cSC4LineItem* lineItem = department->GetLineItem(line)) {
        const std::int64_t count = lineItem->GetSecondaryInfoField() + (added ? 1 : -1);
        lineItem->SetSecondaryInfoField(count);
        if (!added && count == 0) department->RemoveLineItem(line);
    }
    BudgetItem vehicle;
    if (FindBudgetItem(holder, vehiclePurpose, vehicle)) {
        cSC4DepartmentBudget* second = budget->GetDepartmentBudget(static_cast<std::uint32_t>(vehicle.departmentId));
        if (!second) return false;
        if (added) second->AddLocallyFundedObject(holder, vehiclePurpose);
        else second->RemoveLocallyFundedObject(holder, vehiclePurpose);
    }
    return true;
}

// slot 20: a = the building's local funding when it is locally funded, else the department's; b the same for the vehicle item.
// No staff item -> 1.0 / 1.0; an item whose department is missing leaves the outputs as the caller set them (RG10).
void cSC4ResidentialSimulator::GetOccupantFundingPercentages(IResidentialOccupant* occupant, float& staff, float& vehicle, bool school) {
    const Service& s = Svc(school);
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    BudgetItem item;
    if (!FindBudgetItem(holder, s.staffPurpose, item)) {
        staff = vehicle = 1.0f;
        return;
    }
    cSC4BudgetSimulator* budget = host_->BudgetSimulator();
    const auto funding = [&](const BudgetItem& it, std::uint32_t purpose, float& out) {
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(it.departmentId));
        if (!department) return false;
        out = department->IsLocallyFundedObjectInDepartment(holder, purpose) ? it.localFunding : department->GetFundingPercentage();
        return true;
    };
    if (!funding(item, s.staffPurpose, staff)) return;
    BudgetItem second;
    if (!FindBudgetItem(holder, s.vehiclePurpose, second)) {
        vehicle = 1.0f;
        return;
    }
    funding(second, s.vehiclePurpose, vehicle);
}

// slot 19: SetLocalFundingPercent for the staff item, then the vehicle item.
void cSC4ResidentialSimulator::SetOccupantFundingPercentages(IResidentialOccupant* occupant, float staff, float vehicle, bool school) {
    const Service& s = Svc(school);
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    for (auto [purpose, value] : {std::pair{s.staffPurpose, staff}, std::pair{s.vehiclePurpose, vehicle}}) {
        BudgetItem item;
        if (!FindBudgetItem(holder, purpose, item)) {
            fundingChanged_ = true;
            return;
        }
        cSC4DepartmentBudget* department = host_->BudgetSimulator()->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
        if (!department) return;
        department->SetLocalFundingPercent(holder, value, purpose);
    }
    fundingChanged_ = true;
}

void cSC4ResidentialSimulator::GetOccupantEfficiencyRatings(IResidentialOccupant* occupant, float& e1, float& e2, bool school,
                                                           bool ignoreStrike) {
    e1 = e2 = 1.0f;
    float a = 0.0f, b = 0.0f;
    GetOccupantFundingPercentages(occupant, a, b, school);
    BudgetItem item;
    if (!FindBudgetItem(occupant->PropertyHolder(), Svc(school).staffPurpose, item)) return;
    const DepartmentData& d = school ? education_ : health_;
    const std::uint32_t strike = PropertyOr<std::uint32_t>(occupant->PropertyHolder(), kResidentialStrikeStartDate, 0);
    if (!ignoreStrike && strike != 0) e1 = a * d.strikeEfficiency;
    else e1 = d.efficiencyCurve.GetOutputValue(a * 100.0f) * 0.01f;
    e2 = d.coverageCurve.GetOutputValue(b * 100.0f) * 0.01f;
}

float cSC4ResidentialSimulator::GetBaseStaffEfficiency(IResidentialOccupant* occupant, bool school) {
    float a = 0.0f, b = 0.0f;
    GetOccupantFundingPercentages(occupant, a, b, school);
    BudgetItem item;
    if (!FindBudgetItem(occupant->PropertyHolder(), Svc(school).staffPurpose, item)) return a;
    return (school ? education_ : health_).efficiencyCurve.GetOutputValue(a * 100.0f) * 0.01f;
}

bool cSC4ResidentialSimulator::GetAutoBudget(const IResidentialOccupant* occupant) {
    return PropertyOr<bool>(const_cast<IResidentialOccupant*>(occupant)->PropertyHolder(), kOccupantAutoBudget, false);
}

void cSC4ResidentialSimulator::SetAutoBudget(IResidentialOccupant* occupant, bool value) {
    cSCPropertyHelp::SetPropertyValue(occupant->PropertyHolder(), kOccupantAutoBudget, value);
}

// ---- buildings (REH 4.1, residential.md 5.8) -------------------------------------------------------------------

void cSC4ResidentialSimulator::DoBuildingChanged(IResidentialOccupant* building, bool added) {
    if (!building) return;
    cISCPropertyHolder* holder = building->PropertyHolder();
    if (!holder) return;
    const auto erase = [](OccupantList& list, IResidentialOccupant* o) {
        auto it = std::find(list.begin(), list.end(), o);
        if (it == list.end()) return false;
        list.erase(it);
        return true;
    };
    for (bool school : {false, true}) {   // the hospital test runs first; a building may pass both
        const Service& s = Svc(school);
        BudgetItem item;
        bool member;
        if (FindBudgetItem(holder, s.staffPurpose, item)) {
            if (!RegisterWithDepartment(building, added, s.staffPurpose, s.vehiclePurpose)) return;
            member = true;
        } else {
            member = building->IsInGroup(s.recognitionGroups[0]) || building->IsInGroup(s.recognitionGroups[1]);
        }
        if (!member) continue;
        OccupantList& pending = school ? pendingSchools_ : pendingHospitals_;
        static const bool trace = std::getenv("SC4_RESDEBUG") != nullptr;
        if (trace) std::fprintf(stderr, "residential: %s %s\n", school ? "school" : "hospital", added ? "added (pending)" : "removed");
        if (added) {
            cSCPropertyHelp::SetPropertyValue(holder, s.served, kNotGraded);
            cSCPropertyHelp::SetPropertyValue(holder, s.grade, 1.0f);
            cSCPropertyHelp::SetPropertyValue(holder, s.staffRatio, 1.0f);
            // a missing capacity writes whatever the stack held (quirk); the port writes 0
            cSCPropertyHelp::SetPropertyValue(holder, s.effectiveCapacity, PropertyOr<std::uint32_t>(holder, s.capacity, 0));
            building->AddRef();
            pending.push_back(building);
        } else {
            bool erased = false;
            for (OccupantList& list : school ? schools_ : hospitals_)
                if ((erased = erase(list, building))) break;
            if (!erased) erased = erase(pending, building);
            if (erased) building->Release();
        }
        fundingChanged_ = true;
    }
    float a = 0.0f, b = 0.0f;
    if (cSCPropertyHelp::GetPropertyValue(holder, kHealthAffectingA, a) && cSCPropertyHelp::GetPropertyValue(holder, kHealthAffectingB, b) &&
        a > 0.0f && b > 0.0f) {
        if (added) {
            building->AddRef();
            pendingHealthAffecting_.push_back(building);
        } else if (erase(healthAffecting_, building) || erase(pendingHealthAffecting_, building)) {
            building->Release();
        }
        fundingChanged_ = true;
    }
}

// State 0: each pending school / hospital goes to the first group it is in (none: dropped and leaked, the quirk); the pending
// health-affecting buildings move to the end of their list.
void cSC4ResidentialSimulator::DistributePendingBuildings() {
    for (bool school : {true, false}) {
        const Service& s = Svc(school);
        std::vector<OccupantList>& lists = school ? schools_ : hospitals_;
        OccupantList& pending = school ? pendingSchools_ : pendingHospitals_;
        for (IResidentialOccupant* building : pending) {
            for (std::size_t k = 0; k < s.groupCount && k < lists.size(); ++k) {
                if (building->IsInGroup(s.groups[k])) {
                    lists[k].push_back(building);
                    break;
                }
            }
        }
        pending.clear();
    }
    healthAffecting_.splice(healthAffecting_.end(), pendingHealthAffecting_);
}

int cSC4ResidentialSimulator::GetSchoolBuildingCount() const {
    int n = int(pendingSchools_.size());
    for (const auto& list : schools_) n += int(list.size());
    return n;
}

int cSC4ResidentialSimulator::GetHospitalBuildingCount() const {
    int n = int(pendingHospitals_.size());
    for (const auto& list : hospitals_) n += int(list.size());
    return n;
}

namespace {
// GetBuildings: the group lists whose group id is in `groups` (null = all, empty = none), AddRef each.
void AppendGroupLists(const std::vector<std::list<IResidentialOccupant*>>& lists, const std::uint32_t* ids, std::size_t idCount,
                      std::list<IResidentialOccupant*>& out, const std::vector<std::uint32_t>* groups) {
    for (std::size_t k = 0; k < lists.size(); ++k) {
        if (groups && (k >= idCount || std::find(groups->begin(), groups->end(), ids[k]) == groups->end())) continue;
        for (IResidentialOccupant* o : lists[k]) {
            o->AddRef();
            out.push_back(o);
        }
    }
}
void AppendList(const std::list<IResidentialOccupant*>& list, std::list<IResidentialOccupant*>& out) {
    for (IResidentialOccupant* o : list) {
        o->AddRef();
        out.push_back(o);
    }
}
}  // namespace

void cSC4ResidentialSimulator::GetSchoolBuildings(OccupantList& out, const std::vector<std::uint32_t>* groups) const {
    AppendGroupLists(schools_, kSchool.groups, kSchool.groupCount, out, groups);
    AppendList(pendingSchools_, out);
}

void cSC4ResidentialSimulator::GetHospitalBuildings(OccupantList& out, const std::vector<std::uint32_t>* groups) const {
    AppendGroupLists(hospitals_, kHospital.groups, kHospital.groupCount, out, groups);
    AppendList(pendingHospitals_, out);
}

void cSC4ResidentialSimulator::GetHealthAffectingBuildings(OccupantList& out) const {
    AppendList(healthAffecting_, out);
    AppendList(pendingHealthAffecting_, out);
}

void cSC4ResidentialSimulator::ReleaseBuildingLists() {
    const auto drop = [](OccupantList& list) {
        for (IResidentialOccupant* o : list) o->Release();
        list.clear();
    };
    for (OccupantList& list : schools_) drop(list);
    for (OccupantList& list : hospitals_) drop(list);
    drop(pendingSchools_);
    drop(pendingHospitals_);
    drop(healthAffecting_);
    drop(pendingHealthAffecting_);
}

// A loaded record's occupant references become the lists once the host can name the buildings (Init).
void cSC4ResidentialSimulator::AdoptFileLists() {
    if (fileLists_.empty() || !host_) return;
    std::vector<OccupantList*> targets = {&pendingSchools_, &pendingHospitals_, &pendingHealthAffecting_};
    for (OccupantList& list : schools_) targets.push_back(&list);
    for (OccupantList& list : hospitals_) targets.push_back(&list);
    targets.push_back(&healthAffecting_);
    for (std::size_t i = 0; i < fileLists_.size() && i < targets.size(); ++i) {
        for (const cRetainedReference& ref : fileLists_[i]) {
            IResidentialOccupant* o = host_->Occupant(ref.get());
            if (!o) continue;
            o->AddRef();
            targets[i]->push_back(o);
        }
    }
    fileLists_.clear();
    if (std::getenv("SC4_RESDEBUG"))
        std::fprintf(stderr, "residential: adopted %d schools, %d hospitals, %zu health-affecting\n", GetSchoolBuildingCount(),
                     GetHospitalBuildingCount(), healthAffecting_.size() + pendingHealthAffecting_.size());
}

// ---- ordinance effects (REH 4.3) --------------------------------------------------------------------------------

void cSC4ResidentialSimulator::GetOrdinanceEffects() {
    schoolEffects_.clear();
    healthEffects_.clear();
    if (!host_) return;
    std::vector<const cISCPropertyHolder*> ordinances;
    host_->EnactedOrdinances(ordinances);
    for (const cISCPropertyHolder* properties : ordinances) {
        if (!properties) continue;
        for (bool school : {true, false}) {
            const Service& s = Svc(school);
            OrdinanceEffect effect;
            bool present = false;
            const auto factor = [&](std::uint32_t id, float& out) {
                float v;
                if (cSCPropertyHelp::GetPropertyValue(properties, id, v)) {
                    out = v * 0.01f;
                    present = true;
                }
            };
            const auto curve = [&](std::uint32_t id, cSC4ResponseCurve& out) {
                LoadCurve(properties, id, out, kFlatCurve);
                present = present || properties->HasProperty(id);
            };
            factor(s.ordRadius, effect.radiusFactor);
            curve(s.ordDistance, effect.distanceCurve);
            factor(s.ordEffect, effect.effectFactor);
            factor(s.ordCapacity, effect.capacityFactor);
            curve(s.ordAge, effect.ageCurve);
            factor(s.ordDecay, effect.decayFactor);
            if (present) (school ? schoolEffects_ : healthEffects_).push_back(std::move(effect));
        }
    }
}

// m1 = product of the radius factors, m2 capacity, m3 effect (the group filter is never filled: every element applies).
void cSC4ResidentialSimulator::OrdinanceModifiers(bool school, float& radius, float& capacity, float& effect) const {
    radius = capacity = effect = 1.0f;
    for (const OrdinanceEffect& e : OrdinanceEffects(school)) {
        radius *= e.radiusFactor;
        capacity *= e.capacityFactor;
        effect *= e.effectFactor;
    }
}

// ---- coverage (REH 4.4) -------------------------------------------------------------------------------------------

// The shipped chunk arrays have one entry per group; a missing entry or 0 (the original indexes out of range or divides by zero)
// gives the building no coverage units.
std::uint32_t cSC4ResidentialSimulator::Chunk(bool school, std::size_t group) const {
    const std::vector<std::uint32_t>& chunks = school ? schoolChunks : hospitalChunks;
    return group < chunks.size() ? chunks[group] : 0;
}

// The geometry is the grid's: the coverage grid in states 5 / 7, the R$ population grid when grading (both tract size P).
void cSC4ResidentialSimulator::GetCoveredTractList(IResidentialOccupant* occupant, float radius, float tractWidth, std::int32_t countX,
                                                   std::int32_t countZ, TractList& tracts) const {
    float centre[3];
    occupant->LotCenter(centre);
    TractsInRadius(centre[0], centre[2], radius * coverageSlop, tractWidth, countX, countZ, tracts);
}

void cSC4ResidentialSimulator::UpdateCoverageMap(bool school, std::size_t group) {
    const Service& s = Svc(school);
    cSC4SimGrid<std::uint8_t>* grid = GetCoverageMap(school, group);
    const std::vector<OccupantList>& lists = school ? schools_ : hospitals_;
    if (!grid || group >= lists.size()) return;
    grid->SetTractValues(0);
    const std::uint32_t chunk = Chunk(school, group);
    std::int32_t& served = school ? schoolCovered_ : hospitalCovered_;
    for (IResidentialOccupant* occupant : lists[group]) {
        float e1, e2;
        GetOccupantEfficiencyRatings(occupant, e1, e2, school, false);
        float m = e2;
        for (const OrdinanceEffect& e : OrdinanceEffects(school)) m *= e.radiusFactor;
        // MarkBuildingCoverage
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        float radius;
        std::uint32_t capacity;
        if (!cSCPropertyHelp::GetPropertyValue(holder, s.radius, radius) || !cSCPropertyHelp::GetPropertyValue(holder, s.capacity, capacity)) continue;
        const float r = radius * m;
        if (!(r > 0.0f)) continue;
        const std::uint32_t units = chunk ? capacity / chunk : 0;
        TractList tracts;
        GetCoveredTractList(occupant, r, grid->GetTractWidthX(), grid->GetTractCountX(), grid->GetTractCountZ(), tracts);
        for (auto [x, z] : tracts) {
            const std::uint8_t old = grid->GetTractValue(x, z);
            grid->SetTractValue(x, z, ((old + units) & 0xffffu) > 255u ? 255 : static_cast<std::uint8_t>(old + units));
            if (old != 0) continue;
            bool lower = false;
            for (std::size_t j = 0; j < group && !lower; ++j) lower = TractOr0(GetCoverageMap(school, j), x, z) != 0;
            if (lower) continue;
            for (int w = 0; w < 3; ++w) served += TractOr0(populationGrids_[w].get(), x, z);
        }
    }
}

// ---- grading and the EQ / HQ boost (REH 4.5, 4.6) ---------------------------------------------------------------------

void cSC4ResidentialSimulator::LoadBuildingCurves(IResidentialOccupant* occupant, bool school, CurveSet& curves) const {
    const Service& s = Svc(school);
    const cISCPropertyHolder* holder = occupant->PropertyHolder();
    LoadCurve(holder, s.ageCurve, curves.age, kFlatCurve);
    LoadCurve(holder, s.wealthCurve, curves.wealth, kFlatCurve);
    LoadCurve(holder, s.distanceCurve, curves.distance, kFlatCurve);
}

std::uint32_t cSC4ResidentialSimulator::GetPopulationServed(IResidentialOccupant* occupant, float radius, std::uint32_t capacity,
                                                            const CurveSet& curves, bool school, const cSC4SimGrid<std::uint8_t>& coverage,
                                                            std::uint32_t chunk, const TractList& tracts) const {
    if (!(radius > 0.0f)) return 0;
    float position[3];
    occupant->Position(position);   // the occupant's own position, no lot lookup
    std::int32_t tx = 0, tz = 0;
    populationGrids_[0]->PositionToTract(position[0], position[2], tx, tz);
    const std::uint32_t units = chunk ? capacity / chunk : 0;
    float sum = 0.0f;
    for (auto [x, z] : tracts) {
        const std::uint8_t n = TractOr0(&coverage, x, z);
        if (n == 0) continue;
        const float dx = static_cast<float>(static_cast<std::uint32_t>(tx - x));
        const float dz2 = static_cast<float>(static_cast<std::uint32_t>((tz - z) * (tz - z)));
        const float d = std::sqrt(dz2 + dx * dx);
        const float xd = d / radius * 100.0f;
        float md = curves.distance.GetOutputValue(xd) * 0.01f;
        const float age = static_cast<float>(TractOr0(ageGrid_.get(), x, z));
        float ma = curves.age.GetOutputValue(age) * 0.01f;
        for (const OrdinanceEffect& e : OrdinanceEffects(school)) {
            md = (e.distanceCurve.GetOutputValue(xd) * md) * 0.01f;
            ma = (e.ageCurve.GetOutputValue(age) * ma) * 0.01f;
        }
        for (int w = 0; w < 3; ++w) {
            const float share = (static_cast<float>(units) / static_cast<float>(n)) * static_cast<float>(TractOr0(populationGrids_[w].get(), x, z));
            sum = sum + (((curves.wealth.GetOutputValue(static_cast<float>(w)) * 0.01f) * share) * ma) * md;
        }
    }
    return static_cast<std::uint32_t>(std::nearbyint(sum));   // round half to even (the + 12582912 trick)
}

// AddRadiusEffectWithCurves (0x005e48f0) on one wealth's EQ or HQ grid. The geometry is the target's; a tract with no population
// is skipped WITHOUT advancing the z centre (quirk), so the rest of that column uses a centre one tract too far back.
void cSC4ResidentialSimulator::AddRadiusEffect(cSC4SimGrid<float>& target, int wealth, const std::vector<const cSC4ResponseCurve*>& ageCurves,
                                               float px, float pz, float radius, const std::vector<const cSC4ResponseCurve*>& distanceCurves,
                                               float value, float maximum, std::uint32_t units, const cSC4SimGrid<std::uint8_t>& coverage,
                                               float& total) const {
    const float w = target.GetTractWidthX();
    const std::int32_t size = target.GetTractSize(), countX = target.GetTractCountX(), countZ = target.GetTractCountZ();
    const float inv = 1.0f / w;
    std::int32_t x0 = static_cast<std::int32_t>((px - radius) * inv), x1 = static_cast<std::int32_t>((px + radius) * inv);
    std::int32_t z0 = static_cast<std::int32_t>((pz - radius) * inv), z1 = static_cast<std::int32_t>((pz + radius) * inv);
    if (x0 < 0) x0 = 0;
    if (x1 >= countX) x1 = countX - 1;
    if (z0 < 0) z0 = 0;
    if (z1 >= countZ) z1 = countZ - 1;
    const cSC4SimGrid<std::uint16_t>* population = populationGrids_[wealth].get();
    const cSC4SimGrid<std::uint8_t>* age = ageGrid_.get();
    float cx = static_cast<float>(x0) * w + w * 0.5f;
    for (std::int32_t x = x0; x <= x1; ++x, cx += w) {
        float cz = static_cast<float>(z0) * w + w * 0.5f;
        for (std::int32_t z = z0; z <= z1; ++z) {
            const std::uint16_t pop = TractOr0(population, x, z);
            if (pop == 0) continue;
            const float dx = cx - px, dz = cz - pz;
            const float d = std::sqrt(dz * dz + dx * dx);
            if (!(d <= radius)) {
                cz += w;
                continue;
            }
            float mult = 1.0f;
            const float xd = 1.0f - (radius - d) / radius;
            for (const cSC4ResponseCurve* curve : distanceCurves) mult = (mult * curve->GetOutputValue(xd)) * 0.01f;
            if (age) {
                const std::int32_t ax = (x * size) >> age->GetTractShift(), az = (z * size) >> age->GetTractShift();
                const float a = static_cast<float>(TractOr0(age, ax, az));
                for (const cSC4ResponseCurve* curve : ageCurves) mult = (mult * curve->GetOutputValue(a)) * 0.01f;
            }
            float eff = mult * value;
            std::uint32_t people = pop;
            const std::uint8_t n = TractOr0(&coverage, x, z);
            if (n != 0) {
                const float k = static_cast<float>(units) / static_cast<float>(n);
                eff = eff * k;
                people = Trunc32(static_cast<float>(pop) * k);
            }
            total = total + static_cast<float>(people & 0xffffu) * eff;
            const float next = target.GetTractValue(x, z) + eff;
            target.SetTractValue(x, z, next > maximum ? maximum : next);
            cz += w;
        }
    }
}

void cSC4ResidentialSimulator::ComputeOccupantGrade(IResidentialOccupant* occupant, bool school, std::size_t group) {
    const Service& s = Svc(school);
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    cSC4SimGrid<std::uint8_t>* coverage = GetCoverageMap(school, group);
    cSC4SimGrid<float>* gradeGrid = GetGradeMap(school);
    if (!coverage || !gradeGrid || !populationGrids_[0]) return;
    float radius;
    if (!cSCPropertyHelp::GetPropertyValue(holder, s.radius, radius)) {
        cSCPropertyHelp::SetPropertyValue(holder, s.served, std::uint32_t{0});
        cSCPropertyHelp::SetPropertyValue(holder, s.effectiveCapacity, std::uint32_t{0});
        cSCPropertyHelp::SetPropertyValue(holder, s.grade, 0.0f);
        return;
    }
    // a missing capacity reads the radius property id from the stack (quirk); the port uses 0
    const std::uint32_t capacity = PropertyOr<std::uint32_t>(holder, s.capacity, 0);
    float e1, e2;
    GetOccupantEfficiencyRatings(occupant, e1, e2, school, false);
    const float base = GetBaseStaffEfficiency(occupant, school);
    CurveSet curves;
    LoadBuildingCurves(occupant, school, curves);
    float m1, m2, m3;
    OrdinanceModifiers(school, m1, m2, m3);
    const std::uint32_t chunk = Chunk(school, group);

    const float r = (e2 * m1) * radius;
    TractList tracts;
    const cSC4SimGrid<std::uint16_t>& geometry = *populationGrids_[0];
    GetCoveredTractList(occupant, r, geometry.GetTractWidthX(), geometry.GetTractCountX(), geometry.GetTractCountZ(), tracts);
    const std::uint32_t served = GetPopulationServed(occupant, r, capacity, curves, school, *coverage, chunk, tracts);
    const std::uint32_t staffCapacity = Trunc32((static_cast<float>(capacity) * base) * m2);
    const float staff = staffCapacity == 0 ? 0.0f : served == 0 ? 1.0f : std::min(1.0f, static_cast<float>(staffCapacity) / static_cast<float>(served));
    const std::uint32_t effective = Trunc32((static_cast<float>(capacity) * e1) * m2);
    const float grade = effective == 0 ? 0.0f : served == 0 ? 1.0f : std::min(1.0f, static_cast<float>(effective) / static_cast<float>(served));
    cSCPropertyHelp::SetPropertyValue(holder, s.staffRatio, staff);
    cSCPropertyHelp::SetPropertyValue(holder, s.grade, grade);
    cSCPropertyHelp::SetPropertyValue(holder, s.effectiveCapacity, effective);
    cSCPropertyHelp::SetPropertyValue(holder, s.served, served);

    // the shared grade grid: not a weighted mean (quirk); n = 0 only with units = 0, which stores NaN as the original does
    const std::uint32_t units = chunk ? capacity / chunk : 0;
    const float t = static_cast<float>(units) * grade;
    for (auto [x, z] : tracts) {
        if (!gradeGrid->TractIsInBounds(x, z)) continue;
        const std::uint8_t n = TractOr0(coverage, x, z);
        const float v = (n > 1 ? gradeGrid->GetTractValue(x, z) * static_cast<float>(n - 1) : 0.0f) + t / static_cast<float>(n);
        gradeGrid->SetTractValue(x, z, v > 1.0f ? 1.0f : v);
    }
    if (std::getenv("SC4_RESDEBUG")) {
        float a = -1.0f, b = -1.0f;
        GetOccupantFundingPercentages(occupant, a, b, school);
        std::fprintf(stderr, "residential: grade %s group %zu capacity %u served %u effective %u grade %.3f staff %.3f tracts %zu funding %.3f/%.3f\n",
                     school ? "school" : "hospital", group, capacity, served, effective, grade, staff, tracts.size(), a, b);
    }

    // 4.6 the EQ / HQ boost
    if (!(r > 0.0f)) return;
    const float multiplier = PropertyOr<float>(holder, s.effect, 1.0f);
    float centre[3];
    occupant->LotCenter(centre);
    std::vector<const cSC4ResponseCurve*> distanceCurves, ageCurves;
    for (const OrdinanceEffect& e : OrdinanceEffects(school)) {
        distanceCurves.push_back(&e.distanceCurve);
        ageCurves.push_back(&e.ageCurve);
    }
    distanceCurves.push_back(&curves.distance);
    ageCurves.push_back(&curves.age);
    float& total = school ? schoolGradeTotal_ : hospitalGradeTotal_;
    for (int w = 0; w < 3; ++w) {
        cSC4SimGrid<float>* target = school ? eq_[w].get() : hq_[w].get();
        if (!target) continue;
        const float value = ((grade * (curves.wealth.GetOutputValue(static_cast<float>(w)) * 0.01f)) * multiplier) * m3;
        AddRadiusEffect(*target, w, ageCurves, centre[0], centre[2], r * coverageSlop, distanceCurves, value, 200.0f, units, *coverage, total);
    }
}

// States 6 and 8: the grade grid restarts at 0; every building of every group gets its auto-budget step, then its grade.
void cSC4ResidentialSimulator::GradeBuildings(bool school) {
    if (cSC4SimGrid<float>* grid = GetGradeMap(school)) grid->SetTractValues(0.0f);
    const bool global = school ? autoBudgetSchools_ : autoBudgetHospitals_;
    std::vector<OccupantList>& lists = school ? schools_ : hospitals_;
    for (std::size_t k = 0; k < lists.size(); ++k) {
        for (IResidentialOccupant* occupant : lists[k]) {
            if (global || GetAutoBudget(occupant)) {
                float a = 0.0f, b = 0.0f;
                GetOccupantFundingPercentages(occupant, a, b, school);
                if (EstimateIdealFunding(occupant, a)) SetOccupantFundingPercentages(occupant, a, b, school);
            }
            ComputeOccupantGrade(occupant, school, k);
        }
    }
}

// ---- auto-budget, capacity, coverage geometry (REH 4.10, 4.11) ---------------------------------------------------------

bool cSC4ResidentialSimulator::EstimateIdealFunding(IResidentialOccupant* occupant, float& funding) {
    ResidentialQueryData query;
    bool school = true;
    if (!QueryData(occupant, true, query)) {
        if (!QueryData(occupant, false, query)) return false;
        school = false;
    }
    if (PropertyOr<std::uint32_t>(occupant->PropertyHolder(), Svc(school).served, kNotGraded) == kNotGraded) return false;
    float m1, m2, m3;
    OrdinanceModifiers(school, m1, m2, m3);
    const float r = static_cast<float>(query.served) / static_cast<float>(Trunc32(static_cast<float>(query.capacity) * m2));
    const DepartmentData& d = school ? education_ : health_;
    float f = r;
    float v = d.efficiencyCurve.GetOutputValue(f * 100.0f) * 0.01f;
    while (v > r) {
        f = f - 0.01f;
        v = d.efficiencyCurve.GetOutputValue(f * 100.0f) * 0.01f;
        if (f <= 0.0f) { f = 0.0f; break; }
    }
    while (v < r) {
        f = f + 0.01f;
        v = d.efficiencyCurve.GetOutputValue(f * 100.0f) * 0.01f;
        if (f > d.maxFunding) { f = d.maxFunding; break; }
    }
    funding = f;
    return true;
}

bool cSC4ResidentialSimulator::EstimateCurrentOccupantCapacity(IResidentialOccupant* occupant, std::uint32_t& capacity, std::uint32_t& staff) {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    std::uint32_t raw = 0;
    const bool school = cSCPropertyHelp::GetPropertyValue(holder, kSchool.capacity, raw);
    if (!school) raw = PropertyOr<std::uint32_t>(holder, kHospital.capacity, 0);
    float e1, e2, m1, m2, m3;
    GetOccupantEfficiencyRatings(occupant, e1, e2, school, true);
    OrdinanceModifiers(school, m1, m2, m3);
    capacity = Trunc32((static_cast<float>(raw) * e1) * m2);
    staff = schoolStaffDivisor ? (capacity + schoolStaffDivisor - 1) / schoolStaffDivisor : 0;   // the school divisor for both (quirk)
    return true;
}

bool cSC4ResidentialSimulator::GetOccupantCoverage(IResidentialOccupant* occupant, float level, float& distance, float& radius) {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    std::uint32_t probe;
    const bool school = cSCPropertyHelp::GetPropertyValue(holder, kSchool.capacity, probe);
    const bool hospital = cSCPropertyHelp::GetPropertyValue(holder, kHospital.capacity, probe);
    if (!school && !hospital) return false;
    const Service& s = hospital ? kHospital : kSchool;   // the hospital's radius and ordinances win; the funding flag stays the school's
    float e1, e2, m1, m2, m3;
    GetOccupantEfficiencyRatings(occupant, e1, e2, school, false);
    OrdinanceModifiers(!hospital, m1, m2, m3);
    radius = (e2 * m1) * PropertyOr<float>(holder, s.radius, 0.0f);
    CurveSet curves;
    LoadBuildingCurves(occupant, school, curves);
    std::vector<SC4Point> points;
    curves.distance.GetResponseCurveData(points);
    distance = 0.0f;
    const float limit = level * 100.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].y > limit) continue;
        const SC4Point& prev = points[i - 1];
        distance = (((prev.y - limit) / (prev.y - points[i].y)) * ((points[i].x - prev.x) * 0.01f)) * radius + prev.x * radius;
        break;
    }
    return true;
}

// ---- strikes (REH 4.9) -----------------------------------------------------------------------------------------------

float cSC4ResidentialSimulator::GetChanceOfStrike(DepartmentData& dept, const OccupantList& buildings, std::uint32_t gradeProperty,
                                                  std::uint32_t strikeGroup) {
    float q = 0.0f, g = 0.0f, c = 0.0f;
    for (IResidentialOccupant* occupant : buildings) {
        if (occupant->IsInGroup(strikeGroup)) {
            cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), gradeProperty, g);   // absent: g keeps its value
            c = dept.strikeCurve.GetOutputValue(g * 100.0f) * 0.01f;
        }
        q = (q + c) - q * c;   // a building outside the strike group reuses the previous c (quirk)
    }
    return q;
}

float cSC4ResidentialSimulator::ChanceOfSchoolStrike() {
    if (SchoolIsOnStrike()) return 1.0f;
    float p = 0.0f;
    for (const auto& list : schools_) {
        const float c = GetChanceOfStrike(education_, list, kSchool.grade, kSchool.strikeGroup);
        p = (c + p) - c * p;
    }
    return p;
}

float cSC4ResidentialSimulator::ChanceOfHealthStrike() {
    if (HealthIsOnStrike()) return 1.0f;
    float p = 0.0f;
    for (const auto& list : hospitals_) {
        const float c = GetChanceOfStrike(health_, list, kHospital.grade, kHospital.strikeGroup);
        p = (c + p) - c * p;
    }
    return p;
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

// A striking building stops after the duration (days) or once its staff ratio reaches the minimum grade; a working one rolls the
// strike curve at its grade, exactly one draw per check.
bool cSC4ResidentialSimulator::UpdateStrikingState(IResidentialOccupant* occupant, DepartmentData& dept, std::uint32_t staffProperty,
                                                   std::uint32_t gradeProperty) {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    const std::uint32_t today = host_->CurrentDay();
    const std::uint32_t start = PropertyOr<std::uint32_t>(holder, kResidentialStrikeStartDate, 0);
    std::uint32_t next = start;
    if (start != 0) {
        if (today - start > dept.strikeDays) {
            next = 0;
        } else {
            const float ratio = PropertyOr<float>(holder, staffProperty, 1.0f);
            if (ratio >= dept.minGradeToEnd) next = 0;   // a NaN ratio keeps the strike
        }
    } else {
        const float grade = PropertyOr<float>(holder, gradeProperty, 0.0f);
        const float p = dept.strikeCurve.GetOutputValue(grade * 100.0f);
        const double roll = random_->RandomDoubleRangeUniform(0.0, 100.0);
        if (roll < static_cast<double>(p)) next = today;
    }
    if (next == start) return false;
    cSCPropertyHelp::SetPropertyValue(holder, kResidentialStrikeStartDate, next);
    if (host_->CityHasOccupantManager()) host_->SetStrikerGroup(dept.messageId != kMessageHealthStrike, occupant, next != 0);
    return true;
}

void cSC4ResidentialSimulator::DoMonthlyStrikeCheck(DepartmentData& dept, std::vector<OccupantList>& lists, std::uint32_t staffProperty,
                                                    std::uint32_t gradeProperty, std::uint32_t strikeGroup) {
    bool changed = false;
    std::uint32_t count = 0;
    for (auto& list : lists) {
        for (IResidentialOccupant* occupant : list) {
            if (!occupant->IsInGroup(strikeGroup)) continue;
            changed = UpdateStrikingState(occupant, dept, staffProperty, gradeProperty) || changed;
            if (PropertyOr<std::uint32_t>(occupant->PropertyHolder(), kResidentialStrikeStartDate, 0) > 0) {
                ++count;
                if (changed) BeginStrike(dept, occupant);   // changed is sticky (quirk)
            }
        }
    }
    if (changed && dept.onStrike && count == 0) EndStrike(dept, host_->CurrentDay() - dept.strikeStartDay);
}

// ---- query data, totals, ratings (REH 4.11) ----------------------------------------------------------------------------

// Reads capacity, effective capacity, served (raw) and grade in that order, writing each as it goes; the hospital query then
// requires the staff item. Only a successful query fills the rest and maps served 0xffffffff to 0.
bool cSC4ResidentialSimulator::QueryData(IResidentialOccupant* occupant, bool school, ResidentialQueryData& out) {
    const Service& s = Svc(school);
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    if (!cSCPropertyHelp::GetPropertyValue(holder, s.capacity, out.capacity)) return false;
    if (!cSCPropertyHelp::GetPropertyValue(holder, s.effectiveCapacity, out.effectiveCapacity)) return false;
    if (!cSCPropertyHelp::GetPropertyValue(holder, s.served, out.served)) return false;
    float grade;
    if (!cSCPropertyHelp::GetPropertyValue(holder, s.grade, grade)) return false;
    BudgetItem staff;
    const bool hasStaff = FindBudgetItem(holder, s.staffPurpose, staff);
    if (!school && !hasStaff) return false;
    const std::uint32_t divisor = school ? schoolStaffDivisor : hospitalStaffDivisor;   // 0 divides by zero in the original
    out.staff = divisor ? out.capacity / divisor : 0;
    out.effectiveStaff = !divisor ? 0 : school ? (out.effectiveCapacity + divisor - 1) / divisor : out.effectiveCapacity / divisor;
    if (out.served == kNotGraded) out.served = 0;
    out.grade = grade;
    out.staffFunding = hasStaff ? staff.localFunding : 1.0f;
    out.staffCost = hasStaff ? static_cast<std::uint32_t>(staff.cost) : 0;
    BudgetItem vehicle;
    const bool hasVehicle = FindBudgetItem(holder, s.vehiclePurpose, vehicle);
    out.vehicleFunding = hasVehicle ? vehicle.localFunding : 1.0f;
    out.vehicleCost = hasVehicle ? static_cast<std::uint32_t>(vehicle.cost) : 0;
    return true;
}

// Totals: the caller's fields reset, then one record (never reset between buildings) summed for every group-list building in at
// least one of `groups`, whatever its query returns. Null groups (a crash in the original once a list holds a building) count none.
int cSC4ResidentialSimulator::SystemTotals(bool school, ResidentialQueryData& totals, const std::vector<std::uint32_t>* groups) {
    totals.capacity = totals.effectiveCapacity = totals.staff = totals.effectiveStaff = totals.served = 0;
    totals.grade = 0.0f;
    if (!groups) return 0;
    ResidentialQueryData record;
    int count = 0;
    for (const auto& list : school ? schools_ : hospitals_) {
        for (IResidentialOccupant* occupant : list) {
            bool matches = false;
            for (std::uint32_t group : *groups)
                if ((matches = occupant->IsInGroup(group))) break;
            if (!matches) continue;
            QueryData(occupant, school, record);
            totals.capacity += record.capacity;
            totals.effectiveCapacity += record.effectiveCapacity;
            totals.staff += record.staff;
            totals.effectiveStaff += record.effectiveStaff;
            totals.served += record.served;
            totals.grade += record.grade;
            ++count;
        }
    }
    return count;
}

// Rating: the summed effective capacity over the population, capped at 1 (a failed query re-adds the record's previous value).
float cSC4ResidentialSimulator::SystemRating(bool school) {
    ResidentialQueryData record;
    std::uint32_t sum = 0;
    for (const auto& list : school ? schools_ : hospitals_) {
        for (IResidentialOccupant* occupant : list) {
            QueryData(occupant, school, record);
            sum += record.effectiveCapacity;
        }
    }
    if (sum == 0) return 0.0f;
    if (population_ == 0) return 1.0f;
    return std::min(1.0f, static_cast<float>(sum) / static_cast<float>(population_));
}

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
constexpr std::uint32_t kSchoolStaffRatio = 0x2a976c20, kSchoolGrade = 0x292453c5, kHospitalStaffRatio = 0xaa976be7, kHospitalGrade = 0x092453cb;
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
        // The defaults overwrite a loaded record's department scalars, strikes included (9.3 quirk).
        ReadSimulatorTunableValues(host_->SimulatorExemplar());
        GetOrdinanceEffects();
    }
    CreateGrids();
    CreateDepartments();
    AdoptFileLists();
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
    ReleaseBuildingLists();
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
    // the ordinance decay factors (REH 4.3), multiplied in list order
    for (const OrdinanceEffect& e : schoolEffects_) t.eqDecay *= e.decayFactor;
    for (const OrdinanceEffect& e : healthEffects_) t.hqDecay *= e.decayFactor;
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
        schoolGradeTotal_ = hospitalGradeTotal_ = 0.0f;
        DoMonthlyStrikeCheck(education_, schools_, kSchoolStaffRatio, kSchoolGrade, 0x1a03);
        DoMonthlyStrikeCheck(health_, hospitals_, kHospitalStaffRatio, kHospitalGrade, 0x1a02);
        GetOrdinanceEffects();
        DistributePendingBuildings();
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
    case 5:
        schoolCovered_ = 0;
        for (std::size_t k = 0; k < schools_.size(); ++k) UpdateCoverageMap(true, k);
        state_ = 6;
        return true;
    case 6: GradeBuildings(true); state_ = 7; return true;
    case 7:
        hospitalCovered_ = 0;
        for (std::size_t k = 0; k < hospitals_.size(); ++k) UpdateCoverageMap(false, k);
        state_ = 8;
        return true;
    case 8: GradeBuildings(false); state_ = 9; return true;
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
    case kMessageOccupantInserted:
    case kMessageOccupantRemoved: {
        // data3 = 0x278128a0 (a building occupant); the host answers only for buildings (cISC4BuildingOccupant)
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        if (standard->GetData3() != 0x278128a0u || !host_) return true;
        if (IResidentialOccupant* building = host_->Occupant(standard->GetIGZUnknown()))   // void1 and IGZUnknown: the occupant
            DoBuildingChanged(building, message->GetType() == kMessageOccupantInserted);
        return true;
    }
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
    stream.SetFloat32(d.maxFunding);
    stream.SetFloat32(d.strikeEfficiency);
    stream.SetSint32(static_cast<std::int32_t>(d.strikeDays));
    stream.SetFloat32(d.minGradeToEnd);
    stream.SetUint8(d.onStrike ? 1 : 0);
    stream.SetSint32(static_cast<std::int32_t>(d.strikeStartDay));
    refs[0].Write(stream);
    refs[1].Write(stream);
    stream.SetFloat32(d.initialFunding);
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
    pct(d.maxFunding, version < 5);
    pct(d.strikeEfficiency, version < 5);
    std::int32_t days = 0;
    stream.GetSint32(days);
    d.strikeDays = static_cast<std::uint32_t>(days);
    pct(d.minGradeToEnd, version < 6);
    std::uint8_t onStrike = 0;
    stream.GetUint8(onStrike);
    d.onStrike = onStrike != 0;
    std::int32_t start = 0;
    stream.GetSint32(start);
    d.strikeStartDay = static_cast<std::uint32_t>(start);
    refs[0].Read(stream);
    refs[1].Read(stream);
    pct(d.initialFunding, version < 5);
    stream.GetFloat32(d.value94);
    stream.GetUint32(d.messageId);
}

void WriteOccupants(cIGZOStream& stream, const std::list<IResidentialOccupant*>& list) {
    stream.SetUint32(static_cast<std::uint32_t>(list.size()));
    for (IResidentialOccupant* o : list) SC4WriteGZSerializable(stream, o->Object());
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
    // 3 pending lists, the school lists, the hospital lists, the health-affecting list; a record read but not yet adopted by
    // Init writes its references back as read
    if (!fileLists_.empty()) {
        static const cRetainedReferences none;
        const auto list = [&](std::size_t i) -> const cRetainedReferences& { return i < fileLists_.size() ? fileLists_[i] : none; };
        std::size_t next = 0;
        for (int i = 0; i < 3; ++i) WriteList(stream, list(next++));
        stream.SetUint32(7);
        for (int i = 0; i < 7; ++i) WriteList(stream, list(next++));
        stream.SetUint32(3);
        for (int i = 0; i < 3; ++i) WriteList(stream, list(next++));
        WriteList(stream, list(next++));
    } else {
        WriteOccupants(stream, pendingSchools_);
        WriteOccupants(stream, pendingHospitals_);
        WriteOccupants(stream, pendingHealthAffecting_);
        stream.SetUint32(static_cast<std::uint32_t>(schools_.size()));
        for (const OccupantList& list : schools_) WriteOccupants(stream, list);
        stream.SetUint32(static_cast<std::uint32_t>(hospitals_.size()));
        for (const OccupantList& list : hospitals_) WriteOccupants(stream, list);
        WriteOccupants(stream, healthAffecting_);
    }
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
        std::uint32_t schoolLists = 0, hospitalLists = 0;
        stream.GetUint32(schoolLists);
        damaged = schoolLists != 7;
        for (std::uint32_t i = 0; !damaged && i < schoolLists; ++i) list();
        if (!damaged) {
            stream.GetUint32(hospitalLists);
            damaged = hospitalLists != 3;
            for (std::uint32_t i = 0; !damaged && i < hospitalLists; ++i) list();
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
        std::fprintf(stderr, "residential: read v%u null grids %d damaged=%d error=%d sizes %d/%d/%d state %u pop %d lists %zu grid0 %d\n", version, nullGrids, damaged,
                     int(stream.GetError()), proximityTractSize_, populationTractSize_, eqTractSize_, state_, population_, fileLists_.size(),
                     populationGrids_[0] ? populationGrids_[0]->GetTractSize() : -1);
    if (damaged) {
        ReleaseGrids();
        for (cRetainedReference& ref : departmentRefs_) ref = cRetainedReference();
        fileLists_.clear();
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
