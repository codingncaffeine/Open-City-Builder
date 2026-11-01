#include "sc4/ResidentialSimulator.h"
#include "sc4/DBSegment.h"

#include <algorithm>

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
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4ResidentialSimulator::DoMessage(cIGZMessage2*) { return true; }   // routing unrecovered

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
    return curve.GetOutputValue(float(host_->CellWorkforce(x, z))) * 0.01f;
}

float cSC4ResidentialSimulator::GetGlobalWorkforcePercent(const cISCPropertyHolder* exemplar) {
    if (!exemplar) return 0.0f;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {200.0f, 100.0f}});
    std::vector<float> values;
    if (exemplar->HasProperty(0x4941a92b) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x4941a92b, values)) curve.SetResponseCurveData(values.data(), values.size());
    return curve.GetOutputValue(float(host_->GlobalWorkforce())) * 0.01f;
}

std::uint32_t cSC4ResidentialSimulator::GetCellLifeExpectancy(const cISCPropertyHolder* exemplar, unsigned x, unsigned z) {
    if (!exemplar) return 0;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {200.0f, 200.0f}});
    std::vector<float> values;
    if (exemplar->HasProperty(0x6941a67f) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x6941a67f, values)) curve.SetResponseCurveData(values.data(), values.size());
    return std::uint32_t(curve.GetOutputValue(host_->CellLifeExpectancyBase(x, z))) & 0xff;
}

// ---- streams ------------------------------------------------------------------------------------------------------

namespace {

// tDepartmentData stream format (WriteDepartmentData @ 0x100204b72 / ReadDepartmentData @ 0x100205126). The
// response curves (+0x18/+0x38/+0x58) are tunables rebuilt from the exemplar and are not serialized; the two
// int16 grids at +0x80/+0x88 are reached through the host.
void WriteDepartmentData(cIGZOStream& stream, IResidentialHost* host, const DepartmentData& d) {
    stream.SetFloat32(d.minimumFunding);
    stream.SetFloat32(d.strikeThreshold);
    stream.SetFloat32(d.value08);
    stream.SetSint32(static_cast<std::int32_t>(d.strikeDays));
    stream.SetFloat32(d.strikeEndFunding);
    stream.SetUint8(d.onStrike ? 1 : 0);
    stream.SetSint32(static_cast<std::int32_t>(d.strikeStartDay));
    host->WriteDepartmentGrid(stream);                       // +0x80
    host->WriteDepartmentGrid(stream);                       // +0x88
    stream.SetFloat32(d.value90);
    stream.SetFloat32(d.value94);
    stream.SetUint32(d.messageId);
}

void ReadDepartmentData(cIGZIStream& stream, IResidentialHost* host, DepartmentData& d, std::uint16_t version) {
    if (version >= 5) {
        stream.GetFloat32(d.minimumFunding);
        stream.GetFloat32(d.strikeThreshold);
        stream.GetFloat32(d.value08);
    } else {
        float v = 0;
        stream.GetFloat32(v); d.minimumFunding = v * 0.01f;
        stream.GetFloat32(v); d.strikeThreshold = v * 0.01f;
        stream.GetFloat32(v); d.value08 = v * 0.01f;
    }
    std::int32_t days = 0;
    stream.GetSint32(days); d.strikeDays = static_cast<std::uint32_t>(days);
    if (version >= 6) {
        stream.GetFloat32(d.strikeEndFunding);
    } else {
        float v = 0;
        stream.GetFloat32(v); d.strikeEndFunding = v * 0.01f;
    }
    std::uint8_t onStrike = 0;
    stream.GetUint8(onStrike); d.onStrike = onStrike != 0;
    std::int32_t start = 0;
    stream.GetSint32(start); d.strikeStartDay = static_cast<std::uint32_t>(start);
    host->ReadDepartmentGrid(stream);                        // +0x80
    host->ReadDepartmentGrid(stream);                        // +0x88
    if (version >= 5) {
        stream.GetFloat32(d.value90);
    } else {
        float v = 0;
        stream.GetFloat32(v); d.value90 = v * 0.01f;
    }
    stream.GetFloat32(d.value94);
    stream.GetUint32(d.messageId);
}

template <class List>
void WriteOccupantList(cIGZOStream& stream, IResidentialHost* host, const List& list) {
    stream.SetUint32(std::uint32_t(list.size()));
    for (IResidentialOccupant* occupant : list) host->WriteOccupant(stream, occupant);
}

// The record holds far fewer occupants than this; a larger count means the reader is out of step with the stream
// (the streamed layout is only partly recovered), and trusting it would allocate without bound.
constexpr std::uint32_t kMaxStreamedOccupants = 1u << 20;

template <class List>
bool ReadOccupantList(cIGZIStream& stream, IResidentialHost* host, List& list) {
    std::uint32_t count = 0;
    stream.GetUint32(count);
    list.clear();
    if (count > kMaxStreamedOccupants) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        IResidentialOccupant* occupant = nullptr;
        host->ReadOccupant(stream, &occupant);
        if (occupant) list.push_back(occupant);
    }
    return !stream.GetError();
}

}  // namespace

bool cSC4ResidentialSimulator::Write(cIGZOStream& stream) const {
    // a record read from a city file is saved in the city-file layout (RecordTape.h)
    void* dbStream = nullptr;
    if (!cityRecord_.Empty() && stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return cityRecord_.Replay(stream);
    }
    // PARTIAL: the ~30 coverage/workforce/EQ/HQ grids and the ScanningIterator that make up most of the
    // original record belong to the unrecovered population model and are not streamed (see notes/ledger_pending).
    stream.SetUint16(7);
    stream.SetSint32(population_);                            // +0x60
    stream.SetUint8(fundingChanged_ ? 1 : 0);                 // +0x12c
    WriteDepartmentData(stream, host_, health_);              // +0x160
    WriteDepartmentData(stream, host_, education_);           // +0x200
    WriteOccupantList(stream, host_, extraSchools_);          // +0x2d0
    WriteOccupantList(stream, host_, extraHospitals_);        // +0x2e8
    WriteOccupantList(stream, host_, healthAffecting2_);      // +0x300
    stream.SetUint32(std::uint32_t(schools_.size()));         // +0x318 (7 lists)
    for (const auto& list : schools_) WriteOccupantList(stream, host_, list);
    stream.SetUint32(std::uint32_t(hospitals_.size()));       // +0x330 (3 lists)
    for (const auto& list : hospitals_) WriteOccupantList(stream, host_, list);
    WriteOccupantList(stream, host_, healthAffecting1_);      // +0x348
    stream.SetUint8(autoBudgetSchools_ ? 1 : 0);              // +0x648
    stream.SetUint8(autoBudgetHospitals_ ? 1 : 0);            // +0x649
    return !stream.GetError();
}

// 0x100204c4e: the record of a city file. Most of it is object references to the coverage / workforce grids (the unrecovered population
// model keeps them) and to the occupants of the school / hospital lists; those are consumed here, the scalar state and the department data
// are kept. A stream that is not a city-file segment stream (the unit tests) carries the port's own reduced format, read below.
bool cSC4ResidentialSimulator::ReadCityRecord(cIGZIStream& stream) {
    constexpr std::uint32_t kMaxCount = 1u << 22;
    cRecordTape& t = cityRecord_;
    t.Clear();
    const std::uint16_t version = t.U16(stream);
    if (version > 7) return false;
    const auto occupantList = [&]() {
        const std::uint32_t count = t.U32(stream);
        if (count < 10000000u)
            for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) t.Ref(stream);   // the list clears first and ignores a larger count
    };
    const auto department = [&](DepartmentData& d) {
        const bool scaled = version < 5;
        for (float* field : {&d.minimumFunding, &d.strikeThreshold, &d.value08}) {
            const float v = t.F32(stream, scaled ? nullptr : field);
            if (scaled) *field = v * 0.01f;
        }
        t.S32(stream, reinterpret_cast<std::int32_t*>(&d.strikeDays));
        const float end = t.F32(stream, version < 6 ? nullptr : &d.strikeEndFunding);
        if (version < 6) d.strikeEndFunding = end * 0.01f;
        t.Bool(stream, &d.onStrike);
        t.S32(stream, reinterpret_cast<std::int32_t*>(&d.strikeStartDay));
        t.Ref(stream);
        t.Ref(stream);
        const float v90 = t.F32(stream, scaled ? nullptr : &d.value90);
        if (scaled) d.value90 = v90 * 0.01f;
        t.F32(stream, &d.value94);
        t.U32(stream, &d.messageId);
    };
    static_assert(sizeof(population_) == sizeof(std::int32_t));
    t.S32(stream, reinterpret_cast<std::int32_t*>(&population_));   // +0x60
    for (int i = 0; i < 3; ++i) t.Ref(stream);
    t.S32(stream);
    for (int i = 0; i < 3; ++i) t.Ref(stream);
    t.Ref(stream);
    for (int list = 0; list < 2; ++list) {                    // two vectors of grids
        const std::uint32_t count = t.U32(stream);
        if (count > kMaxCount) return false;
        for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) t.Ref(stream);
    }
    t.Ref(stream);
    t.Ref(stream);
    t.S32(stream);
    for (int i = 0; i < 6; ++i) t.Ref(stream);
    t.F32(stream);
    t.F32(stream);
    t.U32(stream);
    t.Bool(stream, &fundingChanged_);                         // +0x12c
    t.S32(stream);
    department(health_);                                      // +0x160
    department(education_);                                   // +0x200
    for (int i = 0; i < 3; ++i) occupantList();               // +0x2d0 / +0x2e8 / +0x300
    for (int group = 0; group < 2; ++group) {                 // the school lists, then the hospital lists
        const std::uint32_t count = t.U32(stream);
        if (count > kMaxCount) return false;
        for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) occupantList();
    }
    occupantList();                                           // +0x348
    if (version >= 3) {
        t.S8(stream);                                         // ScanningIterator
        t.S32(stream); t.S32(stream); t.U32(stream);
        for (int i = 0; i < 4; ++i) t.S32(stream);
        t.U64(stream);
        if (version > 3) t.S32(stream);
        if (version > 6) {
            t.Bool(stream, &autoBudgetSchools_);              // +0x648
            t.Bool(stream, &autoBudgetHospitals_);            // +0x649
        }
    }
    return !stream.GetError();
}

bool cSC4ResidentialSimulator::Read(cIGZIStream& stream) {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return ReadCityRecord(stream);
    }
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 7) return false;
    stream.GetSint32(population_);                            // +0x60
    std::uint8_t flag = 0;
    stream.GetUint8(flag); fundingChanged_ = flag != 0;       // +0x12c
    ReadDepartmentData(stream, host_, health_, version);      // +0x160
    ReadDepartmentData(stream, host_, education_, version);   // +0x200
    if (!ReadOccupantList(stream, host_, extraSchools_)) return false;           // +0x2d0
    if (!ReadOccupantList(stream, host_, extraHospitals_)) return false;         // +0x2e8
    if (!ReadOccupantList(stream, host_, healthAffecting2_)) return false;       // +0x300
    std::uint32_t schoolLists = 0;
    stream.GetUint32(schoolLists);
    if (schoolLists > kMaxStreamedOccupants) return false;
    schools_.clear();
    for (std::uint32_t i = 0; i < schoolLists; ++i) {
        std::list<IResidentialOccupant*> list;
        if (!ReadOccupantList(stream, host_, list)) return false;
        schools_.push_back(list);
    }
    std::uint32_t hospitalLists = 0;
    stream.GetUint32(hospitalLists);
    if (hospitalLists > kMaxStreamedOccupants) return false;
    hospitals_.clear();
    for (std::uint32_t i = 0; i < hospitalLists; ++i) {
        std::list<IResidentialOccupant*> list;
        if (!ReadOccupantList(stream, host_, list)) return false;
        hospitals_.push_back(list);
    }
    if (!ReadOccupantList(stream, host_, healthAffecting1_)) return false;       // +0x348
    stream.GetUint8(flag); autoBudgetSchools_ = flag != 0;    // +0x648
    stream.GetUint8(flag); autoBudgetHospitals_ = flag != 0;  // +0x649
    return !stream.GetError();
}

}  // namespace sc4
