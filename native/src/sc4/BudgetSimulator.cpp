#include "sc4/BudgetSimulator.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gz/StreamOps.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
IBudgetSimEnvironment* g_environment = nullptr;
IBudgetSimServices* g_services = nullptr;
constexpr std::uint32_t kInitMessages[4] = {0x26d31ec4, 0x29222f02, 0x08dac3e8, 0x08dac3e7};
std::string LowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

void cSC4BudgetSimulator::SetEnvironment(IBudgetSimEnvironment* environment) { g_environment = environment; }
IBudgetSimEnvironment* cSC4BudgetSimulator::Environment() { return g_environment; }

cSC4BudgetSimulator::cSC4BudgetSimulator() {
    const float points[] = {0.0f, 0.0f, 255.0f, 10.0f};       // land value 0..255 -> tax value 0..10
    landValueCurve_.SetResponseCurveData(points, 4);
}

cSC4BudgetSimulator::~cSC4BudgetSimulator() = default;

bool cSC4BudgetSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kSimulatorTypeBudget) {
        *out = static_cast<cIGZSerializable*>(this);
    } else if (iid == 0xe4fda3d4 || iid == 0x452294aa) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4BudgetSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

void cSC4BudgetSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    Tunables defaults;
    defaults.lvToResidentialTax = tunables_.lvToResidentialTax;
    defaults.lvToCommercialTax = tunables_.lvToCommercialTax;
    defaults.lvToIndustrialTax = tunables_.lvToIndustrialTax;
    defaults.minAverageLandValue = tunables_.minAverageLandValue;
    tunables_ = defaults;
    if (!exemplar) return;
    Tunables& t = tunables_;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x6a9c7718, t.startingFunds);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x890e0a99, t.taxScale);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x090e0a9f, t.minAllowableFunds);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x490e0aa6, t.bondIncrement);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x090e0aac, t.maxOutstandingBonds);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x890e0ab4, t.bondLimit);
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x8a3fb98d, t.loanInterest)) t.loanInterest *= 0.01f;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x0a3fb9a8, t.loanMonths);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x89170c6c, t.lvToResidentialTax);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x09170c74, t.lvToCommercialTax);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0xc9170c78, t.lvToIndustrialTax);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x49170c7c, t.minAverageLandValue);
    static const std::uint32_t kFactorProps[12] = {0xa9188ef5, 0xe918c920, 0x4918c925, 0xa918c929, 0x4918c92c, 0,
                                                   0x8918c937, 0x4918c930, 0x69a36e40, 0xa918c942, 0x8918c93e, 0xa9a36e47};
    for (int i = 0; i < 12; ++i)
        if (kFactorProps[i]) cSCPropertyHelp::GetPropertyValue(exemplar, kFactorProps[i], t.factors[i]);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4918c93a, t.factors[7]);       // overrides 0x4918c930
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x8a480df7, t.wealthWeights);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0xebff69e7, t.difficultyScale);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x0c032488, t.defaultTaxRate);
    std::vector<float> curve;
    if (exemplar->HasProperty(0x4ac519b8) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x4ac519b8, curve))
        landValueCurve_.SetResponseCurveData(curve.data(), curve.size());
}

// New-city initialisation of the money: starting funds of the difficulty, the default tax rate on every tax
// type, cleared monthly history and the year-to-date figures.
void cSC4BudgetSimulator::ApplyStartingFunds() {
    const int difficulty = g_environment ? g_environment->DifficultyLevel() : 0;
    if (static_cast<std::size_t>(difficulty) < tunables_.startingFunds.size())
        funds_ = monthStartFunds_ = tunables_.startingFunds[difficulty];
    PostFundsChangedMessage();
    for (TaxType& tax : taxTypes_) {
        tax.rate = static_cast<std::size_t>(difficulty) < tunables_.defaultTaxRate.size()
                       ? tunables_.defaultTaxRate[difficulty] : 0.0f;
        tax.field4 = tunables_.taxScale;
        std::fill(std::begin(tax.months), std::end(tax.months), 0);
        tax.value68 = tax.weightedAssessed = tax.income = 0;
    }
}

void cSC4BudgetSimulator::SetServices(IBudgetSimServices* services) { g_services = services; }

// 0x10021e330. Quirks kept: the cheat codes register before the funds are initialised; the start-of-game funds
// and tax reset only run for a city that was not loaded from a save (+0x20); history ids 0xaabaaa9f.. (12 tax
// types) are sized to 365 days and the funds history gets funds/1000. The original re-reads the tunables from
// the simulator itself; here the caller feeds the exemplar through ReadSimulatorTunableValues beforehand.
bool cSC4BudgetSimulator::Init() {
    if (initialised_) return true;
    initialised_ = true;
    if (g_services) {
        g_services->RegisterSimulatorNotifications(this);
        g_services->AcquireServices();
        for (std::uint32_t m : kInitMessages) g_services->SubscribeMessage(m);
        g_services->RegisterCheatCodes();
    }
    if (!loaded_) {
        ApplyStartingFunds();
        if (g_services)
            for (std::uint32_t i = 0; i < 12; ++i) g_services->ResetHistory(0xaabaaa9f + i, 365);
        if (g_environment) g_environment->AddHistory(kBudgetHistoryFunds, funds_ / 1000);
    }
    if (legacyTaxFlag_) {
        const int d = g_environment ? g_environment->DifficultyLevel() : 0;
        taxTypes_[8].rate = static_cast<std::size_t>(d) < tunables_.defaultTaxRate.size() ? tunables_.defaultTaxRate[d] : 0.0f;
    }
    if (g_services) g_services->RegisterGameDataNodes(this);
    return true;
}

// The department teardown is unconditional here (the original only runs it when initialised); the service
// teardown follows the original order.
void cSC4BudgetSimulator::Shutdown() {
    if (initialised_) {
        initialised_ = false;
        if (g_services) {
            g_services->UnregisterGameDataNodes(this);
            g_services->UnregisterCheatCodes();
            g_services->ReleaseServices();
            for (std::uint32_t m : kInitMessages) g_services->UnsubscribeMessage(m);
        }
    }
    for (auto& entry : departments_) {
        entry.second->Shutdown();
        entry.second->Release();
    }
    departments_.clear();
    if (g_services) g_services->UnregisterSimulator(this);
}

void cSC4BudgetSimulator::sExchangeFunds(cSC4BudgetSimulator* sim, std::int64_t& value, bool write) {
    if (!sim) return;
    if (write) sim->SetTotalFunds(value);
    else value = sim->GetTotalFunds();
}

void cSC4BudgetSimulator::sDepositFunds(cSC4BudgetSimulator* sim, const std::int64_t* amount) {
    if (sim && amount) sim->DepositFunds(*amount);
}

void cSC4BudgetSimulator::ShowBudgetWindow() {
    if (g_services) g_services->ShowBudgetDialog(expenseView);
}

bool cSC4BudgetSimulator::DoMessage(const BudgetMessage& m) {
    switch (static_cast<std::uint32_t>(m.type)) {
    case 0x230e27ac: {                                       // cheat string
        const std::uint32_t cheat = static_cast<std::uint32_t>(m.data1);
        if (cheat == 0x493f4fe6) {                            // "<cheat> <amount>": set the funds
            std::size_t sp = m.text.find(' ');
            if (sp == std::string::npos) return false;        // argc < 2
            while (sp < m.text.size() && m.text[sp] == ' ') ++sp;
            if (sp >= m.text.size()) return false;
            SetTotalFunds(std::strtoll(m.text.c_str() + sp, nullptr, 10));
            return true;
        }
        if (cheat == 0x4ab87766) { SetTotalFunds(GetTotalFunds() + 1000); return true; }
        if (cheat == 0xcbf31943) {                            // +10000 and an increasingly likely disaster
            if (g_services && g_services->CityCheatsEnabled()) {
                DepositFunds(10000);
                ++cheatCounter_;
                const double chance = static_cast<double>(1.0f - 1.0f / (static_cast<float>(cheatCounter_) * 0.2f + 1.0f));
                if (g_services->RandomDouble() < chance && g_services->HasDisasterLayer()) {
                    float w = 0, h = 0;
                    g_services->CityCellSize(w, h);
                    const double a = static_cast<double>(w * 0.125f * 3.0f);
                    const float x = static_cast<float>(static_cast<double>(w) * g_services->RandomDouble() * 0.25 + a);
                    const double b = static_cast<double>(h * 0.125f * 3.0f);
                    const int z = static_cast<int>(static_cast<double>(h) * g_services->RandomDouble() * 0.25 + b);
                    if (g_services->SpawnDisasterAt(static_cast<int>(x), z)) cheatCounter_ = 0;
                }
            }
            return true;
        }
        return false;
    }
    case 0x26d31ec4: {                                       // new city: funds and default tax rates only
        const int d = g_environment ? g_environment->DifficultyLevel() : 0;
        if (static_cast<std::size_t>(d) < tunables_.startingFunds.size())
            funds_ = monthStartFunds_ = tunables_.startingFunds[d];
        PostFundsChangedMessage();
        for (TaxType& tax : taxTypes_)
            tax.rate = static_cast<std::size_t>(d) < tunables_.defaultTaxRate.size() ? tunables_.defaultTaxRate[d] : 0.0f;
        return true;
    }
    case 0x29222f02:                                         // budget dialog announcements
        if (m.data1 == 0x892230f2) expenseView = true;
        else if (m.data1 == 0xc92230ee) expenseView = false;
        else return true;
        ShowBudgetWindow();
        return true;
    case 0x66956810: PostFundsChangedMessage(); return true;
    case 0x66956816: DoMonthlySimulation(); return true;
    case 0x66956814:                                         // idle: publish the tax rates (x10, rounded to even)
        if (g_environment)
            for (int i = 0; i < 12; ++i)
            {
                const float biased = taxTypes_[i].rate * 10.0f + 12582912.0f;   // magic-number round to nearest even
                std::int32_t bits;
                std::memcpy(&bits, &biased, sizeof bits);
                g_environment->AddHistory(0xaabaaa9f + i, bits - 0x4b400000);
            }
        return true;
    default: return true;
    }
}

// ------------------------------------------------------------------------------------------------ funds

void cSC4BudgetSimulator::SetTotalFunds(std::int64_t value) {
    if (funds_ == value) return;
    funds_ = value;
    monthStartFunds_ = value;
    PostFundsChangedMessage();
}

bool cSC4BudgetSimulator::DepositFunds(std::int64_t value) {
    if (value <= 0) return value < 0 ? WithdrawFunds(-value) : true;
    const std::int64_t current = funds_;
    std::int64_t result = INT64_MAX;
    if (value < INT64_MAX - current) result = current + value;          // saturates for positive balances
    if (current < 0) result = current + value;
    funds_ = monthStartFunds_ = result;
    PostFundsChangedMessage();
    return true;
}

bool cSC4BudgetSimulator::WithdrawFunds(std::int64_t value) {
    if (value <= 0) return value < 0 ? DepositFunds(-value) : true;
    if (funds_ < value) return false;
    funds_ -= value;
    monthStartFunds_ = funds_;
    PostFundsChangedMessage();
    return true;
}

std::int64_t cSC4BudgetSimulator::GetEstIncome() {
    int year = 0, month = 0;
    if (g_environment) g_environment->SimulatorDate(year, month);
    return GetTotalMonthlyIncome() * (13 - month) + ytdIncome_;
}

std::int64_t cSC4BudgetSimulator::GetEstExpenses() {
    int year = 0, month = 0;
    if (g_environment) g_environment->SimulatorDate(year, month);
    return GetTotalMonthlyExpense() * (13 - month) + ytdExpenses_;
}

namespace {
// One x87 multiplication stored to float32: the operands enter exactly (a 64-bit integer too), the product rounds once.
inline float MulF(long double a, float b) { return static_cast<float>(a * static_cast<long double>(b)); }
constexpr float kInv1200 = 1.0f / 1200.0f;   // the float32 nearest 1/1200; the original multiplies by it
}  // namespace

std::int64_t cSC4BudgetSimulator::GetTaxIncome(int taxType) const {
    const int difficulty = g_environment ? g_environment->DifficultyLevel() : 0;
    const float scale = static_cast<std::size_t>(difficulty) < tunables_.difficultyScale.size()
                            ? tunables_.difficultyScale[difficulty] : 0.0f;
    if (taxType < 0 || taxType >= 12) return 0;   // (budget.md: the original does not check)
    return static_cast<std::int64_t>(MulF(MulF(MulF(static_cast<long double>(taxTypes_[taxType].weightedAssessed), scale), taxTypes_[taxType].rate), kInv1200));
}

std::int64_t cSC4BudgetSimulator::GetTotalMonthlyIncome() {
    std::int64_t total = 0;
    for (int i = 0; i < 12; ++i) total += GetTaxIncome(i);
    if (g_environment) total += g_environment->OrdinanceMonthlyIncome() + g_environment->NeighborsIncome();
    for (auto& entry : departments_) total += entry.second->GetTotalIncome();
    return total;
}

std::int64_t cSC4BudgetSimulator::GetTotalMonthlyExpense() {
    std::int64_t total = 0;
    if (g_environment) total += g_environment->OrdinanceMonthlyExpense() + g_environment->NeighborsExpense();
    for (auto& entry : departments_) total += entry.second->GetTotalExpenses();
    for (const Bond& bond : bonds_) total += bond.monthlyPayment;
    return total;
}

// ----------------------------------------------------------------------------------------------- taxes

// @ i386 0x00691e20 (budget.md §5.2): for every taxable lot, in the lot manager's order,
//   land     = roundHalfEven(LVCurve(lv) * sizeX * sizeZ)
//   assessed = trunc(float(land + value) * WaningWealth[bWealth - lWealth] * OccWeight[t])
//   lotTax   = float(assessed) * DifficultyTaxMultiplier * rate[t] * INV1200
// each product left to right with one float32 rounding, the lot's tax added to the total first and then to its group, both as
// float32 running sums; the month's group taxes and the total are truncated to 64 bits.
std::int64_t cSC4BudgetSimulator::ComputeTax() {
    for (TaxType& tax : taxTypes_) tax.weightedAssessed = 0;
    float perType[12] = {};
    float total = 0.0f;
    const int difficulty = g_environment ? g_environment->DifficultyLevel() : 0;
    const float scale = static_cast<std::size_t>(difficulty) < tunables_.difficultyScale.size() ? tunables_.difficultyScale[difficulty] : 0.0f;
    if (g_environment) {
        g_environment->ForEachTaxableLot([&](const LotTaxInfo& lot) {
            if (lot.taxType < 0 || lot.taxType >= 12) return;
            const float valueCurve = landValueCurve_.GetOutputValue(static_cast<float>(lot.landValue));
            const float landArea = MulF(MulF(valueCurve, static_cast<float>(lot.lotSizeX)), static_cast<float>(lot.lotSizeZ));
            const std::int64_t land = static_cast<std::int64_t>(std::nearbyint(landArea));
            const std::size_t weightIndex = static_cast<std::size_t>(lot.wealthDelta);
            // the original indexes past the table for a delta outside 0..2 (budget.md §5.2 choice: 1.0)
            const float waning = weightIndex < tunables_.wealthWeights.size() ? tunables_.wealthWeights[weightIndex] : 1.0f;
            const float weighted = MulF(MulF(static_cast<long double>(land + lot.assessedValue), waning), tunables_.factors[lot.taxType]);
            const std::int64_t assessed = static_cast<std::int64_t>(weighted);
            taxTypes_[lot.taxType].weightedAssessed += assessed;
            const float tax = MulF(MulF(MulF(static_cast<long double>(assessed), scale), GetTaxRate(lot.taxType)), kInv1200);
            total = total + tax;
            perType[lot.taxType] = perType[lot.taxType] + tax;
        });
    }
    for (int i = 0; i < 12; ++i) taxTypes_[i].income = static_cast<std::int64_t>(perType[i]);
    if (static const bool trace = std::getenv("SC4_TAXDEBUG") != nullptr; trace) {
        static int months = 0;
        if (months++ < 3) {
            std::fprintf(stderr, "tax: month %d total %lld (difficulty %d scale %.3f) weights", months, static_cast<long long>(total), difficulty, scale);
            for (float f : tunables_.factors) std::fprintf(stderr, " %.3f", f);
            std::fprintf(stderr, " waning");
            for (float f : tunables_.wealthWeights) std::fprintf(stderr, " %.3f", f);
            std::fprintf(stderr, "\n");
            for (int i = 0; i < 12; ++i)
                if (taxTypes_[i].weightedAssessed || loadedTax_[i].first)
                    std::fprintf(stderr, "tax:   type %2d rate %.2f assessed %lld income %lld | file assessed %lld income %lld\n", i,
                                 taxTypes_[i].rate, static_cast<long long>(taxTypes_[i].weightedAssessed),
                                 static_cast<long long>(taxTypes_[i].income), static_cast<long long>(loadedTax_[i].first),
                                 static_cast<long long>(loadedTax_[i].second));
        }
    }
    return static_cast<std::int64_t>(total);
}

// ------------------------------------------------------------------------------------------------ bonds

std::int64_t cSC4BudgetSimulator::GetMonthlyPaymentForLoan(std::int64_t principal) const {
    const double monthly = static_cast<double>(tunables_.loanInterest) / 12.0;
    const double growth = std::pow(monthly + 1.0, -static_cast<double>(tunables_.loanMonths));
    return static_cast<std::int64_t>((static_cast<double>(principal) * monthly) / (1.0 - growth));
}

std::int64_t cSC4BudgetSimulator::GetFullCostOfLoan(std::int64_t principal) const {
    return GetMonthlyPaymentForLoan(principal) * static_cast<std::int64_t>(tunables_.loanMonths);
}

std::int64_t cSC4BudgetSimulator::GetTotalMonthlyBondPayments() const {
    std::int64_t total = 0;
    for (const Bond& bond : bonds_) total += bond.monthlyPayment;
    return total;
}

bool cSC4BudgetSimulator::IssueBond(std::uint32_t amount) {
    if (amount == 0 || outstandingBonds_ >= tunables_.maxOutstandingBonds) return false;
    const std::uint64_t increment = static_cast<std::uint64_t>(tunables_.bondIncrement);
    std::uint64_t principal = (amount / increment) * increment;
    if (static_cast<std::int32_t>(principal) == 0) principal = increment;
    principal &= 0xffffffffull;
    if (static_cast<std::int64_t>(principal) > tunables_.bondLimit) return false;
    if (borrowed_ + static_cast<std::int64_t>(principal) > GetCurrentBorrowingLimit()) return false;
    const std::int32_t fullCost = static_cast<std::int32_t>(GetFullCostOfLoan(static_cast<std::int64_t>(principal)));
    const std::int64_t monthly = GetMonthlyPaymentForLoan(static_cast<std::int64_t>(principal));
    int year = 0, month = 0;
    if (g_environment) g_environment->SimulatorDate(year, month);
    const std::uint32_t extraMonths = tunables_.loanMonths % 12;
    int dueMonth = month + static_cast<int>(extraMonths);
    int wrappedMonth = month - 12 + static_cast<int>(extraMonths);
    if (dueMonth < 13) wrappedMonth = dueMonth;
    ++outstandingBonds_;
    Bond bond;
    bond.month = static_cast<std::uint32_t>(wrappedMonth);
    bond.year = static_cast<std::uint32_t>((dueMonth > 12 ? 1 : 0) + year + static_cast<int>(tunables_.loanMonths / 12));
    bond.principal = static_cast<std::int64_t>(principal);
    bond.fullCost = fullCost;
    bond.remaining = fullCost;
    bond.paid = 0;
    bond.monthlyPayment = monthly;
    bond.remainingPrincipal = static_cast<std::int64_t>(principal);
    bonds_.push_back(bond);
    PostBondMessage(kBudgetMessageBondIssued);
    borrowed_ += static_cast<std::int64_t>(principal);
    DepositFunds(static_cast<std::int64_t>(principal));
    return true;
}

void cSC4BudgetSimulator::GetAllLoans(std::vector<LoanInfo>& out) const {
    out.clear();
    int year = 0, month = 0;
    for (const Bond& bond : bonds_) {
        if (g_environment) g_environment->SimulatorDate(year, month);
        LoanInfo info;
        const int monthDiff = static_cast<int>(bond.month) - month;
        info.monthsLeft = monthDiff < 0 ? monthDiff + 12 : monthDiff;
        info.yearsLeft = (monthDiff >> 31) + (static_cast<int>(bond.year) - year);
        info.principal = bond.principal;
        info.remainingPrincipal = bond.remainingPrincipal;
        info.monthlyPayment = bond.monthlyPayment;
        out.push_back(info);
    }
}

// --------------------------------------------------------------------------------- departments / groups

cSC4DepartmentBudget* cSC4BudgetSimulator::CreateDepartmentBudget(std::uint32_t id, std::uint32_t group) {
    auto it = departments_.find(id);
    if (it != departments_.end()) return it->second;
    auto* department = new cSC4DepartmentBudget();
    department->AddRef();
    department->Init();
    departments_[id] = department;
    department->SetDepartmentID(id);
    department->SetFundingPercentage(1.0f, 0, true);
    GetBudgetGroup(group).departments.insert(id);
    department->SetBudgetGroup(group);
    DoBudgetGroupChanged(group);
    return department;
}

cSC4BudgetSimulator::BudgetGroup& cSC4BudgetSimulator::GetBudgetGroup(std::uint32_t id) {
    auto it = groups_.find(id);
    if (it != groups_.end()) return it->second;
    BudgetGroup& group = groups_[id];
    group.id = id;
    group.expense = group.income = 0;
    if (g_environment) group.name = g_environment->LoadLocalizedString(kBudgetGroupNameTable, id);
    return group;
}

bool cSC4BudgetSimulator::RemoveDepartmentBudget(std::uint32_t id) {
    auto it = departments_.find(id);
    if (it == departments_.end()) return false;
    cSC4DepartmentBudget* department = it->second;
    const std::uint32_t groupId = department->GetBudgetGroup();
    auto group = groups_.find(groupId);
    if (group != groups_.end()) {
        group->second.departments.erase(id);
        DoBudgetGroupChanged(group->second.id);
    }
    departments_.erase(it);
    department->Shutdown();
    department->Release();
    return true;
}

cSC4DepartmentBudget* cSC4BudgetSimulator::GetDepartmentBudget(std::uint32_t id) {
    auto it = departments_.find(id);
    return it == departments_.end() ? nullptr : it->second;
}

// matches a department whose lower-cased name starts with the (lower-cased) text
cSC4DepartmentBudget* cSC4BudgetSimulator::GetDepartmentBudget(const std::string& name) {
    const std::string wanted = LowerCase(name);
    for (auto& entry : departments_) {
        cRZString departmentName;
        entry.second->GetDepartmentName(departmentName);
        const std::string candidate = LowerCase(departmentName.ToChar());
        if (wanted.empty() || candidate.find(wanted) == 0) return entry.second;
    }
    return nullptr;
}

void cSC4BudgetSimulator::GetAllGroups(std::vector<BudgetGroupInfo>& out) {
    out.clear();
    for (auto& entry : groups_) out.push_back({entry.second.id, entry.second.name, entry.second.expense, entry.second.income});
}

void cSC4BudgetSimulator::GetDepartmentBudgetsInGroup(std::uint32_t group, std::vector<cSC4DepartmentBudget*>& out) {
    out.clear();
    auto it = groups_.find(group);
    if (it == groups_.end()) return;
    for (std::uint32_t id : it->second.departments) {
        auto department = departments_.find(id);
        if (department == departments_.end()) continue;
        department->second->AddRef();
        out.push_back(department->second);
    }
}

void cSC4BudgetSimulator::DoBudgetGroupChanged(std::uint32_t group) {
    auto it = groups_.find(group);
    if (it == groups_.end()) return;
    std::int64_t expense = 0, income = 0;
    for (std::uint32_t id : it->second.departments) {
        cSC4DepartmentBudget* department = GetDepartmentBudget(id);
        if (!department) continue;
        expense += department->GetTotalExpenses();
        income += department->GetTotalIncome();
    }
    if (expense != it->second.expense || income != it->second.income) {
        it->second.expense = expense;
        it->second.income = income;
        if (g_environment) g_environment->PostMessage(kBudgetMessageGroupChanged, group, expense, income);
    }
}

void cSC4BudgetSimulator::NeededFundingChanged(cSC4DepartmentBudget* department) {
    if (department) DoBudgetGroupChanged(department->GetBudgetGroup());
}

void cSC4BudgetSimulator::FundingPercentageChanged(cSC4DepartmentBudget* department) {
    if (department) DoBudgetGroupChanged(department->GetBudgetGroup());
}

void cSC4BudgetSimulator::PostBudgetMessage(std::uint32_t type, cSC4DepartmentBudget* department, std::uint32_t id,
                                            std::uint32_t extra) {
    if (g_environment) g_environment->PostBudgetMessage(type, department, id, extra);
}

void cSC4BudgetSimulator::NotifyLocalFundingChanged(cISCPropertyHolder* holder) {
    if (g_environment) g_environment->NotifyLocalFundingChanged(holder);
}

std::string cSC4BudgetSimulator::LoadLocalizedString(std::uint32_t group, std::uint32_t instance) {
    return g_environment ? g_environment->LoadLocalizedString(group, instance) : std::string();
}

std::string cSC4BudgetSimulator::BuildingName(std::uint32_t buildingType) {
    return g_environment ? g_environment->BuildingName(buildingType) : std::string();
}

// ------------------------------------------------------------------------------------------ budget items

// The object's budget items are parallel property arrays (department, line item, reserved, cost, local funding
// in percent); a missing local funding entry defaults to 100 %.
bool cSC4BudgetSimulator::GetBudgetItemInfo(cISCPropertyHolder* holder, std::vector<BudgetItem>& items) {
    std::vector<std::uint32_t> departments, lineItems, reserved;
    std::vector<std::int64_t> costs;
    std::vector<float> funding;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropDepartments, departments)) return false;
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropLineItems, lineItems);
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropReserved, reserved);
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropCosts, costs);
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropLocalFunding, funding);
    if (departments.empty()) return false;
    items.assign(departments.size(), BudgetItem());
    for (std::size_t i = 0; i < departments.size(); ++i) {
        BudgetItem& item = items[i];
        item.departmentId = static_cast<std::int32_t>(departments[i]);
        item.lineItemId = i < lineItems.size() ? static_cast<std::int32_t>(lineItems[i]) : 0;
        item.reserved8 = i < reserved.size() ? static_cast<std::int32_t>(reserved[i]) : 0;
        item.cost = i < costs.size() ? costs[i] : 0;
        item.purpose = static_cast<std::uint32_t>(i);
        item.localFunding = (i < funding.size() ? funding[i] : 100.0f) * 0.01f;
    }
    return true;
}

// The first item whose purpose id (property 0xea54d285) matches (budget.md 7). The departments key their locally funded objects
// by that id too, as a city file stores them.
bool cSC4BudgetSimulator::GetBudgetItemForPurpose(cISCPropertyHolder* holder, std::uint32_t purpose, BudgetItem& item) {
    std::vector<BudgetItem> items;
    if (!GetBudgetItemInfo(holder, items)) return false;
    for (const BudgetItem& candidate : items) {
        if (static_cast<std::uint32_t>(candidate.reserved8) == purpose) {
            item = candidate;
            return true;
        }
    }
    return false;
}

void cSC4BudgetSimulator::ChangeBudgetItemLine(cISCPropertyHolder* holder, int purpose, std::uint32_t lineItemId) {
    std::vector<std::uint32_t> lineItems;
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropLineItems, lineItems);
    if (static_cast<std::size_t>(purpose) < lineItems.size()) {
        lineItems[purpose] = lineItemId;
        cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropLineItems, lineItems);
    }
}

void cSC4BudgetSimulator::ChangeBudgetItemCost(cISCPropertyHolder* holder, std::uint32_t purpose, std::int64_t cost) {
    std::vector<std::int64_t> costs;
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropCosts, costs);
    if (purpose < costs.size()) {
        costs[purpose] = cost;
        cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropCosts, costs);
    }
}

void cSC4BudgetSimulator::ChangeBudgetItemLocalFunding(cISCPropertyHolder* holder, std::uint32_t purpose, float funding) {
    std::vector<float> values;
    cSCPropertyHelp::GetPropertyValue(holder, kBudgetItemPropLocalFunding, values);
    if (values.size() <= purpose) values.resize(purpose + 1, 0.0f);
    values[purpose] = funding * 100.0f;
    cSCPropertyHelp::SetPropertyValue(holder, kBudgetItemPropLocalFunding, values);
}

// AddBudgetItemToBudget / RemoveBudgetItemFromBudget(holder, index): the item at `index` adds (removes) its cost to its line's full
// expenses when its department and line exist; nothing is created (budget.md 7).
void cSC4BudgetSimulator::AddBudgetItemToBudget(cISCPropertyHolder* holder, int index) {
    std::vector<BudgetItem> items;
    if (index < 0 || !GetBudgetItemInfo(holder, items) || static_cast<std::size_t>(index) >= items.size()) return;
    if (cSC4LineItem* line = GetLineItemFromBudgetItem(items[static_cast<std::size_t>(index)])) line->AddToFullExpenses(items[static_cast<std::size_t>(index)].cost);
}

void cSC4BudgetSimulator::RemoveBudgetItemFromBudget(cISCPropertyHolder* holder, int index) {
    std::vector<BudgetItem> items;
    if (index < 0 || !GetBudgetItemInfo(holder, items) || static_cast<std::size_t>(index) >= items.size()) return;
    if (cSC4LineItem* line = GetLineItemFromBudgetItem(items[static_cast<std::size_t>(index)])) line->AddToFullExpenses(-items[static_cast<std::size_t>(index)].cost);
}

cSC4LineItem* cSC4BudgetSimulator::GetLineItemFromBudgetItem(const BudgetItem& item) {
    cSC4DepartmentBudget* department = GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
    return department ? department->GetLineItem(static_cast<std::uint32_t>(item.lineItemId)) : nullptr;
}

void cSC4BudgetSimulator::CopyBudgetItemProperties(cISCPropertyHolder* from, cISCPropertyHolder* to) {
    std::vector<std::uint32_t> values;
    for (std::uint32_t id : {kBudgetItemPropDepartments, kBudgetItemPropLineItems, kBudgetItemPropReserved}) {
        if (cSCPropertyHelp::GetPropertyValue(from, id, values)) cSCPropertyHelp::SetPropertyValue(to, id, values);
    }
    std::vector<std::int64_t> costs;
    if (cSCPropertyHelp::GetPropertyValue(from, kBudgetItemPropCosts, costs))
        cSCPropertyHelp::SetPropertyValue(to, kBudgetItemPropCosts, costs);
    std::vector<float> funding;
    if (cSCPropertyHelp::GetPropertyValue(from, kBudgetItemPropLocalFunding, funding))
        cSCPropertyHelp::SetPropertyValue(to, kBudgetItemPropLocalFunding, funding);
}

// ------------------------------------------------------------------------------------- monthly cycle

void cSC4BudgetSimulator::PostFundsChangedMessage() {
    if (g_environment) g_environment->PostMessage(kBudgetMessageFundsChanged, 0, funds_, 0);
}

void cSC4BudgetSimulator::PostBondMessage(std::uint32_t type) {
    if (g_environment) g_environment->PostMessage(type, 0, 0, 0);
}

// daily tick: the tax rates (x10, rounded) go to the history warehouse
void cSC4BudgetSimulator::DoDailyHistory() {
    if (!g_environment) return;
    for (int i = 0; i < 12; ++i)
        g_environment->AddHistory(0xaabaaa9f + static_cast<std::uint32_t>(i),
                                  static_cast<std::int32_t>(std::nearbyint(taxTypes_[i].rate * 10.0f)));
}

// The month that just ended (the simulator runs two months behind the calendar): taxes, ordinance and neighbour
// money, department spending and bond payments settle into the treasury and the year-to-date figures.
void cSC4BudgetSimulator::DoMonthlySimulation() {
    if (funds_ != monthStartFunds_) funds_ = monthStartFunds_;
    int year = 0, month = 0;
    if (g_environment) g_environment->SimulatorDate(year, month);
    const int calendarMonth = month;
    const int calendarYear = year;
    int closedMonth, closedYear = year;
    if (month < 2) {
        closedMonth = 11;
        closedYear = year - 1;
    } else {
        closedMonth = month - 2;
    }
    (void)closedYear;
    const std::int64_t tax = ComputeTax();
    taxYtd_ += tax;
    monthlyTax_ = tax;
    const std::int64_t ordinanceIncome = g_environment ? g_environment->OrdinanceMonthlyIncome() : 0;
    ordinanceIncomeByMonth_[closedMonth] = ordinanceIncome;
    ordinanceIncomeYtd_ += ordinanceIncome;
    std::uint64_t income = static_cast<std::uint64_t>((g_environment ? g_environment->NeighborsIncome() : 0) +
                                                      ordinanceIncome + tax);
    for (auto& entry : departments_) income += static_cast<std::uint64_t>(entry.second->GetTotalIncome());
    const std::int64_t ordinanceExpense = g_environment ? g_environment->OrdinanceMonthlyExpense() : 0;
    ordinanceExpenseByMonth_[closedMonth] = ordinanceExpense;
    ordinanceExpenseYtd_ += ordinanceExpense;
    std::uint64_t expense = static_cast<std::uint64_t>((g_environment ? g_environment->NeighborsExpense() : 0) +
                                                       ordinanceExpense);
    for (auto& entry : departments_) {
        cSC4DepartmentBudget* department = entry.second;
        department->RecalculateAllLineItemCurrentExpenses();
        const std::int64_t spent = department->GetTotalExpenses();
        department->SetTotalSpending(department->GetTotalSpending() + spent);
        expense += static_cast<std::uint64_t>(spent);
    }
    for (auto it = bonds_.begin(); it != bonds_.end();) {
        Bond& bond = *it;
        const int monthDiff = static_cast<int>(bond.month) - calendarMonth;
        const int monthsLeft = monthDiff < 0 ? monthDiff + 12 : monthDiff;
        const int yearsLeft = (monthDiff >> 31) + (static_cast<int>(bond.year) - calendarYear);
        const std::int64_t payment = bond.monthlyPayment;
        bondPaymentsYtd_ += payment;
        bond.remaining -= payment;
        bond.paid += payment;
        if (yearsLeft == 0 && monthsLeft == 0) {
            borrowed_ -= bond.principal;
            it = bonds_.erase(it);
            --outstandingBonds_;
            PostBondMessage(kBudgetMessageBondRepaid);
        } else {
            const std::int64_t next = static_cast<std::int64_t>((tunables_.loanInterest / 12.0f) *
                                                                  static_cast<float>(bond.remainingPrincipal)) -
                                      payment + bond.remainingPrincipal;
            bond.remainingPrincipal = next;
            if (yearsLeft == 0 && monthsLeft == 1) bond.monthlyPayment = next;
            ++it;
        }
        expense += static_cast<std::uint64_t>(payment);
    }
    SetTotalFunds(static_cast<std::int64_t>(income - expense) + funds_);
    ytdExpenses_ += static_cast<std::int64_t>(expense);
    ytdIncome_ += static_cast<std::int64_t>(income);
    monthlyIncome_ = static_cast<std::int64_t>(income);
    monthlyExpense_ = static_cast<std::int64_t>(expense);
    if (g_environment) {
        auto clamp32 = [](std::int64_t v) -> std::int64_t { return v > 0xfffffffell ? 0xffffffffll : (v & 0xffffffffll); };
        g_environment->AddHistory(kBudgetHistoryIncome, clamp32(static_cast<std::int64_t>(income)));
        g_environment->AddHistory(kBudgetHistoryExpense, clamp32(static_cast<std::int64_t>(expense)));
        g_environment->AddHistory(kBudgetHistoryFunds, static_cast<std::int32_t>(funds_ / 1000));
    }
    monthStartFunds_ = funds_;
    if (closedMonth == 11) {                         // year end: reset the year-to-date counters
        ordinanceIncomeYtd_ = 0;
        ytdIncome_ = 0;
        ytdExpenses_ = 0;
        misc798_ = misc790_ = taxYtd_ = 0;
        misc7b0_ = misc7a8_ = 0;
        for (auto& entry : departments_) entry.second->SetTotalSpending(0);
        ordinanceExpenseYtd_ = 0;
        misc7a0_ = 0;
        bondPaymentsYtd_ = 0;
    }
    if (funds_ <= tunables_.minAllowableFunds && g_environment)
        g_environment->PostMessage(kBudgetMessageFundsLow, 0, 0, 0);
}

// ------------------------------------------------------------------------------------------ persistence

bool cSC4BudgetSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(5);
    stream.SetSint64(funds_);
    stream.SetSint64(monthStartFunds_);
    stream.SetSint64(borrowed_);
    stream.SetSint64(monthlyTax_);
    stream.SetUint32(static_cast<std::uint32_t>(departments_.size()));
    for (const auto& entry : departments_) {
        stream.SetUint32(entry.first);
        SC4WriteGZSerializable(stream, static_cast<cIGZSerializable*>(entry.second));
    }
    for (const TaxType& tax : taxTypes_) {
        stream.SetFloat32(tax.rate);
        stream.SetFloat32(tax.field4);
        for (std::int64_t v : tax.months) stream.SetSint64(v);
        stream.SetSint64(tax.value68);
        stream.SetSint64(tax.weightedAssessed);
        stream.SetSint64(tax.income);
    }
    stream.SetSint64(ordinanceExpenseYtd_);
    stream.SetSint64(ordinanceIncomeYtd_);
    for (std::int64_t v : ordinanceExpenseByMonth_) stream.SetSint64(v);
    for (std::int64_t v : ordinanceIncomeByMonth_) stream.SetSint64(v);
    stream.SetSint64(taxYtd_);
    stream.SetSint64(misc790_);
    stream.SetSint64(misc798_);
    stream.SetSint64(misc7a0_);
    stream.SetSint64(misc7a8_);
    stream.SetSint64(misc7b0_);
    stream.SetUint32(static_cast<std::uint32_t>(bonds_.size()));
    for (const Bond& bond : bonds_) {
        stream.SetSint32(static_cast<std::int32_t>(bond.month));
        stream.SetSint32(static_cast<std::int32_t>(bond.year));
        stream.SetSint64(bond.principal);
        stream.SetSint64(bond.fullCost);
        stream.SetSint64(bond.remaining);
        stream.SetSint64(bond.paid);
        stream.SetSint64(bond.monthlyPayment);
        stream.SetSint64(bond.remainingPrincipal);
    }
    stream.SetSint64(bondPaymentsYtd_);
    stream.SetSint64(monthlyIncome_);
    stream.SetSint64(yearlyIncome_);
    stream.SetSint64(monthlyExpense_);
    stream.SetSint64(yearlyExpense_);
    stream.SetUint32(static_cast<std::uint32_t>(groups_.size()));
    for (const auto& entry : groups_) {
        const BudgetGroup& group = entry.second;
        stream.SetUint32(entry.first);
        stream.SetUint32(group.id);
        stream.SetGZStr(cRZString(group.name.c_str()));
        stream.SetSint64(group.expense);
        stream.SetSint64(group.income);
        stream.SetUint32(static_cast<std::uint32_t>(group.departments.size()));
        for (std::uint32_t id : group.departments) stream.SetUint32(id);
    }
    return !stream.GetError();
}

bool cSC4BudgetSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 4) return false;
    loaded_ = true;
    stream.GetSint64(funds_);
    stream.GetSint64(monthStartFunds_);
    stream.GetSint64(borrowed_);
    stream.GetSint64(monthlyTax_);
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        stream.GetUint32(id);
        void* department = nullptr;
        SC4ReadGZSerializable(stream, kGZCLSID_cSC4DepartmentBudget, &department, false);
        if (department)
            // QueryInterface(classId) answers the cISC4DepartmentBudget sub-object (+8); downcast through that type so
            // the compiler subtracts the offset (a void/cIGZUnknown chain kept the +8 address; see Budget.cpp).
            departments_[id] = static_cast<cSC4DepartmentBudget*>(static_cast<cISC4DepartmentBudget*>(department));
    }
    for (TaxType& tax : taxTypes_) {
        stream.GetFloat32(tax.rate);
        stream.GetFloat32(tax.field4);
        for (std::int64_t& v : tax.months) stream.GetSint64(v);
        stream.GetSint64(tax.value68);
        stream.GetSint64(tax.weightedAssessed);
        stream.GetSint64(tax.income);
        loadedTax_[std::size_t(&tax - &taxTypes_[0])] = {tax.weightedAssessed, tax.income};   // SC4_TAXDEBUG
    }
    stream.GetSint64(ordinanceExpenseYtd_);
    stream.GetSint64(ordinanceIncomeYtd_);
    for (std::int64_t& v : ordinanceExpenseByMonth_) stream.GetSint64(v);
    for (std::int64_t& v : ordinanceIncomeByMonth_) stream.GetSint64(v);
    stream.GetSint64(taxYtd_);
    stream.GetSint64(misc790_);
    stream.GetSint64(misc798_);
    stream.GetSint64(misc7a0_);
    stream.GetSint64(misc7a8_);
    stream.GetSint64(misc7b0_);
    std::uint32_t bondCount = 0;
    stream.GetUint32(bondCount);
    outstandingBonds_ = static_cast<std::int32_t>(bondCount);
    bonds_.clear();
    for (std::uint32_t i = 0; i < bondCount; ++i) {
        Bond bond;
        std::int32_t v = 0;
        stream.GetSint32(v);
        bond.month = static_cast<std::uint32_t>(v);
        stream.GetSint32(v);
        bond.year = static_cast<std::uint32_t>(v);
        stream.GetSint64(bond.principal);
        stream.GetSint64(bond.fullCost);
        stream.GetSint64(bond.remaining);
        stream.GetSint64(bond.paid);
        stream.GetSint64(bond.monthlyPayment);
        stream.GetSint64(bond.remainingPrincipal);
        bonds_.push_back(bond);
    }
    stream.GetSint64(bondPaymentsYtd_);
    stream.GetSint64(monthlyIncome_);
    stream.GetSint64(yearlyIncome_);
    stream.GetSint64(monthlyExpense_);
    stream.GetSint64(yearlyExpense_);
    std::uint32_t groupCount = 0;
    stream.GetUint32(groupCount);
    for (std::uint32_t i = 0; i < groupCount; ++i) {
        std::uint32_t key = 0;
        stream.GetUint32(key);
        BudgetGroup group;
        stream.GetUint32(group.id);
        cRZString name;
        stream.GetGZStr(name);
        group.name = g_environment ? g_environment->LoadLocalizedString(kBudgetGroupNameTable, group.id) : name.ToChar();
        stream.GetSint64(group.expense);
        stream.GetSint64(group.income);
        std::uint32_t members = 0;
        stream.GetUint32(members);
        for (std::uint32_t m = 0; m < members; ++m) {
            std::uint32_t id = 0;
            stream.GetUint32(id);
            group.departments.insert(id);
        }
        groups_[key] = group;
    }
    return !stream.GetError();
}

}  // namespace sc4
