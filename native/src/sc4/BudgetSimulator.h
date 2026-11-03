// cSC4BudgetSimulator — the city treasury (87 functions, 0x10021dd6e..0x100223078, class id 0xe990be01,
// simulator type 0x6772bd23): total funds, per-tax-type rates and assessed values, department budgets grouped
// into budget groups, bonds, monthly income / expense roll-up and the per-object budget item properties.
// The simulator doubles as the IBudgetHost the department budgets talk to (spBudgetSim in the original).
// The monthly simulation, tax computation, persistence and tunables are recovered; Init (message / registry /
// cheat-code wiring), the cheat handlers and the budget window need the framework and stay partial.
//
// Layout: +0x48 total funds, +0x50 funds at the start of the month, +0x58 total borrowed, +0x60 YTD expenses,
// +0x68 YTD income, +0x78 department map, +0x90 group map, +0xa8 12 tax types (stride 0x80), +0x6a8 monthly tax,
// +0x6b8.. ordinance / neighbour figures, +0x7b8 bonds, +0x7d0 outstanding bonds, +0x7e0 starting funds per
// difficulty, +0x7f8.. tunables, +0x828 land value -> tax factors, +0x858/+0x890 wealth / difficulty tables,
// +0x870 land value curve, +0x8a8 default tax rate per difficulty, +0x8c0.. totals, +0x8e0 expense view flag.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "sc4/Budget.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4BudgetSimulator = 0xe990be01;
constexpr std::uint32_t kSimulatorTypeBudget = 0x6772bd23;
constexpr std::uint32_t kBudgetTaxTypeCount = 12;

constexpr std::uint32_t kBudgetItemPropDepartments = 0xea54d283;     // uint32[]
constexpr std::uint32_t kBudgetItemPropLineItems = 0xea54d284;       // uint32[]
constexpr std::uint32_t kBudgetItemPropReserved = 0xea54d285;        // uint32[]
constexpr std::uint32_t kBudgetItemPropCosts = 0xea54d286;           // int64[]
constexpr std::uint32_t kBudgetItemPropLocalFunding = 0xea54d287;    // float[] (percent)

constexpr std::uint32_t kBudgetMessageFundsChanged = 0x0772fad4;
constexpr std::uint32_t kBudgetMessageGroupChanged = 0x0a3e7624;
constexpr std::uint32_t kBudgetMessageBondIssued = 0xc772fada;
constexpr std::uint32_t kBudgetMessageBondRepaid = 0xca3fbdce;
constexpr std::uint32_t kBudgetMessageFundsLow = 0xaa5cf1e3;
constexpr std::uint32_t kBudgetHistoryIncome = 0x6a51178b;
constexpr std::uint32_t kBudgetHistoryExpense = 0xca5ba3fb;
constexpr std::uint32_t kBudgetHistoryFunds = 0x4a5ba3d0;
constexpr std::uint32_t kBudgetGroupNameTable = 0x6a3ff01c;

struct LotTaxInfo {
    int taxType = -1;                 // kTaxTypeByPurposeAndWealth[purpose][wealth]; -1 = untaxed
    int lotSizeX = 0, lotSizeZ = 0;
    std::uint8_t landValue = 0;       // land value simulator sample at the lot
    std::int64_t assessedValue = 0;   // building property 0xe91a0b5f
    int wealthDelta = 0;              // profile wealth - lot wealth index, selects the weighting entry
};

class IBudgetSimEnvironment {
public:
    virtual ~IBudgetSimEnvironment() = default;
    virtual void SimulatorDate(int& year, int& month) = 0;               // simulator vslot 0x38
    virtual std::int64_t OrdinanceMonthlyIncome() = 0;                   // ordinance simulator 0xb8
    virtual std::int64_t OrdinanceMonthlyExpense() = 0;                  // ordinance simulator 0xb0
    virtual std::int64_t NeighborsIncome() = 0;                          // neighbours simulator 0x100
    virtual std::int64_t NeighborsExpense() = 0;                         // neighbours simulator 0x108
    virtual int DifficultyLevel() = 0;                                   // city vslot 0xc0
    virtual void ForEachTaxableLot(const std::function<void(const LotTaxInfo&)>& visit) = 0;
    virtual void PostMessage(std::uint32_t type, std::uint32_t a, std::int64_t b, std::int64_t c) = 0;
    virtual void AddHistory(std::uint32_t id, std::int64_t value) = 0;   // history warehouse vslot 0x60
    // IBudgetHost passthroughs
    virtual void PostBudgetMessage(std::uint32_t type, cSC4DepartmentBudget* department, std::uint32_t id, std::uint32_t extra) = 0;
    virtual void NotifyLocalFundingChanged(cISCPropertyHolder* holder) = 0;
    virtual std::string LoadLocalizedString(std::uint32_t group, std::uint32_t instance) = 0;
    virtual std::string BuildingName(std::uint32_t buildingType) = 0;
};

class cSC4BudgetSimulator;

// Framework services used by Init / Shutdown / DoMessage / ShowBudgetWindow (new in the second port pass; the
// original reaches them through SL::spSimulator, spMS2, spCity, spLandValueSim, spOrdinanceSim, spCCM, spSC4,
// spDisasterLayer and the game data registry). Cheat registration is behind this seam so the macOS build can no-op it.
class IBudgetSimServices {
public:
    virtual ~IBudgetSimServices() = default;
    virtual void RegisterSimulatorNotifications(void* owner) = 0;     // spSimulator vslot 0x98 with types 0, 6 ("cSC4BudgetSimulator") and 4
    virtual void UnregisterSimulator(void* owner) = 0;                // city vslot 0x190 object, vslot 0xa8
    virtual void SubscribeMessage(std::uint32_t message) = 0;         // spMS2 vslot 0x28
    virtual void UnsubscribeMessage(std::uint32_t message) = 0;       // spMS2 vslot 0x30
    virtual void AcquireServices() = 0;                               // AddRef city, land value sim, ordinance sim, MS2
    virtual void ReleaseServices() = 0;
    virtual void RegisterCheatCodes() = 0;                            // 0xcbf31943 and 0x4ab87766; 0x493f4fe6 only when cheats are allowed
    virtual void UnregisterCheatCodes() = 0;
    virtual void RegisterGameDataNodes(cSC4BudgetSimulator* sim) = 0; // "budget" node, "total_funds" value, "deposit_funds" command
    virtual void UnregisterGameDataNodes(cSC4BudgetSimulator* sim) = 0;
    virtual void ResetHistory(std::uint32_t id, std::uint32_t days) = 0;   // history warehouse vslot 0x68
    virtual bool CityCheatsEnabled() = 0;                             // city vslot 0xb0
    virtual double RandomDouble() = 0;                                // sRandom.RandomDoubleUniform
    virtual void CityCellSize(float& x, float& z) = 0;                // city vslots 0x2c8 / 0x2d0
    virtual bool SpawnDisasterAt(int x, int z) = 0;                   // disaster layer object 0x42963812 vslot 0x38
    virtual bool HasDisasterLayer() = 0;
    virtual void ShowBudgetDialog(bool expense) = 0;                  // new cSC4BudgetUIExpenseWinProc (0x40) / IncomeWinProc (0x78), vslots 0x8, 0x10
};

// What DoMessage reads from a cIGZMessage2 (vslots 0x20 type, 0x48 data1, 0x78 -> 0x38 text).
struct BudgetMessage {
    std::int32_t type = 0;
    std::int64_t data1 = 0;
    std::string text;
};

class cSC4BudgetSimulator : public cIGZSerializable, public IBudgetHost {
public:
    struct Bond {
        std::uint32_t month = 0, year = 0;              // +0x10, +0x14 maturity
        std::int64_t principal = 0;                     // +0x18
        std::int64_t fullCost = 0;                      // +0x20
        std::int64_t remaining = 0;                     // +0x28
        std::int64_t paid = 0;                          // +0x30
        std::int64_t monthlyPayment = 0;                // +0x38
        std::int64_t remainingPrincipal = 0;            // +0x40
    };
    struct BudgetGroup {
        std::uint32_t id = 0;
        std::string name;
        std::int64_t expense = 0, income = 0;
        std::set<std::uint32_t> departments;
    };
    struct BudgetGroupInfo { std::uint32_t id; std::string name; std::int64_t expense, income; };
    struct LoanInfo {
        std::int32_t monthsLeft = 0, yearsLeft = 0;
        std::int64_t principal = 0, remainingPrincipal = 0, monthlyPayment = 0;
    };
    struct TaxType {                                    // 0x80 bytes
        float rate = 0.0f;                              // +0x00
        float field4 = 0.0f;                            // +0x04
        std::int64_t months[12] = {};                   // +0x08
        std::int64_t value68 = 0;                       // +0x68
        std::int64_t weightedAssessed = 0;              // +0x70
        std::int64_t income = 0;                        // +0x78
    };
    struct Tunables {
        float taxScale = 25.0f;                         // +0x7f8
        std::int64_t minAllowableFunds = -100000;       // +0x800
        std::int64_t bondIncrement = 1000;              // +0x808
        std::int32_t maxOutstandingBonds = 5;           // +0x810
        std::int64_t bondLimit = 10000;                 // +0x818
        float loanInterest = 0.085f;                    // +0x820 (annual)
        std::uint32_t loanMonths = 120;                 // +0x824
        float factors[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};                         // +0x828 land value -> tax factors
        float lvToResidentialTax = 0, lvToCommercialTax = 0, lvToIndustrialTax = 0;   // static tunables
        std::uint8_t minAverageLandValue = 0;
        std::vector<float> wealthWeights{1.0f, 1.0f, 1.0f};         // +0x858
        std::vector<float> difficultyScale{100000.0f, 100000.0f, 100000.0f};   // +0x890
        std::vector<float> defaultTaxRate;              // +0x8a8
        std::vector<std::int64_t> startingFunds;        // +0x7e0
    };

    cSC4BudgetSimulator();
    ~cSC4BudgetSimulator();
    static void SetEnvironment(IBudgetSimEnvironment* environment);
    static IBudgetSimEnvironment* Environment();

    // COM
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetSimulatorType() const { return kSimulatorTypeBudget; }
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4BudgetSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;

    // tunables (ReadSimulatorTunableValues): from the simulator exemplar when it exists
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    Tunables& GetTunables() { return tunables_; }
    cSC4ResponseCurve& LandValueCurve() { return landValueCurve_; }
    void ApplyStartingFunds();                           // message 0x26d31ec4: funds and tax rates for the difficulty
    void Shutdown();
    bool Init();                                         // 0x10021e330
    bool DoMessage(const BudgetMessage& message);        // 0x1002201fc
    void ShowBudgetWindow();                             // 0x100222fa4
    static void SetServices(IBudgetSimServices* services);
    // game data registry callbacks (0x10021f010 / 0x10021f186); `sim` is the owner the registry resolves
    static void sExchangeFunds(cSC4BudgetSimulator* sim, std::int64_t& value, bool write);
    static void sDepositFunds(cSC4BudgetSimulator* sim, const std::int64_t* amount);   // null parameter = no-op
    void SetLegacyTaxFlag(bool v) { legacyTaxFlag_ = v; }

    // funds
    void SetTotalFunds(std::int64_t value);
    std::int64_t GetTotalFunds() const { return funds_; }
    bool DepositFunds(std::int64_t value);               // negative values withdraw
    bool WithdrawFunds(std::int64_t value);              // false when it would overdraw; negatives deposit
    std::int64_t GetMinAllowableFunds() const { return tunables_.minAllowableFunds; }
    std::int64_t GetYTDIncome() const { return ytdIncome_; }
    std::int64_t GetYTDExpenses() const { return ytdExpenses_; }
    std::int64_t GetEstIncome();
    std::int64_t GetEstExpenses();
    std::int64_t GetTotalMonthlyIncome();
    std::int64_t GetTotalMonthlyExpense();
    std::int64_t GetTotalYearlyIncome() const { return yearlyIncome_; }
    std::int64_t GetTotalYearlyExpense() const { return yearlyExpense_; }
    void SetTotalMonthlyIncome(std::int64_t v) { monthlyIncome_ = v; }
    void SetTotalMonthlyExpense(std::int64_t v) { monthlyExpense_ = v; }
    void SetTotalYearlyIncome(std::int64_t v) { yearlyIncome_ = v; }
    void SetTotalYearlyExpense(std::int64_t v) { yearlyExpense_ = v; }

    // taxes
    std::int64_t GetTaxIncome() const { return monthlyTax_; }
    std::int64_t GetTaxIncome(int taxType) const;
    float GetTaxRate(int taxType) const { return taxType < 12 ? taxTypes_[taxType].rate : 0.0f; }
    void SetTaxRate(int taxType, float rate) { if (taxType < 12) taxTypes_[taxType].rate = rate; }
    std::int64_t GetWeightedAssessedTaxValue(int taxType) const { return taxType < 12 ? taxTypes_[taxType].weightedAssessed : 0; }
    std::int64_t ComputeTax();

    // bonds
    std::int64_t GetBondIncrement() const { return tunables_.bondIncrement; }
    bool IssueBond(std::uint32_t amount);
    std::int64_t GetTotalBorrowed() const { return borrowed_; }
    std::int64_t GetCurrentBorrowingLimit() const { return static_cast<std::int64_t>(tunables_.maxOutstandingBonds) * tunables_.bondLimit; }
    std::int64_t GetCurrentBondLimit() const { return tunables_.bondLimit; }
    std::int64_t GetCurrentMaxOutstandingBondsLimit() const { return tunables_.maxOutstandingBonds; }
    void GetAllLoans(std::vector<LoanInfo>& out) const;
    std::int64_t GetMonthlyPaymentForLoan(std::int64_t principal) const;
    std::int64_t GetFullCostOfLoan(std::int64_t principal) const;
    std::int64_t GetTotalMonthlyBondPayments() const;
    std::uint32_t GetLoanTimeInMonths() const { return tunables_.loanMonths; }
    const std::list<Bond>& Bonds() const { return bonds_; }

    // departments and groups
    cSC4DepartmentBudget* CreateDepartmentBudget(std::uint32_t id, std::uint32_t group);
    BudgetGroup& GetBudgetGroup(std::uint32_t id);
    bool RemoveDepartmentBudget(std::uint32_t id);
    cSC4DepartmentBudget* GetDepartmentBudget(std::uint32_t id);
    cSC4DepartmentBudget* GetDepartmentBudget(const std::string& name);   // case-insensitive "starts with" match
    void GetAllGroups(std::vector<BudgetGroupInfo>& out);
    void SetGroupName(std::uint32_t id, const std::string& name) { GetBudgetGroup(id).name = name; }
    void GetDepartmentBudgetsInGroup(std::uint32_t group, std::vector<cSC4DepartmentBudget*>& out);
    void DoBudgetGroupChanged(std::uint32_t group);
    float GetFunding(std::uint32_t) const { return 0.0f; }

    // monthly cycle
    void DoMonthlySimulation();
    void DoDailyHistory();
    void PostFundsChangedMessage();
    void PostBondMessage(std::uint32_t type);

    // IBudgetHost (spBudgetSim facing the department budgets); `purpose` indexes the object's budget item arrays
    bool GetBudgetItemForPurpose(cISCPropertyHolder* holder, std::uint32_t purpose, BudgetItem& item) override;
    bool GetBudgetItemInfo(cISCPropertyHolder* holder, std::vector<BudgetItem>& items) override;
    void ChangeBudgetItemLine(cISCPropertyHolder* holder, int purpose, std::uint32_t lineItemId);
    void ChangeBudgetItemCost(cISCPropertyHolder* holder, std::uint32_t purpose, std::int64_t cost) override;
    void ChangeBudgetItemLocalFunding(cISCPropertyHolder* holder, std::uint32_t purpose, float funding) override;
    void AddBudgetItemToBudget(cISCPropertyHolder* holder, int purpose);
    void RemoveBudgetItemFromBudget(cISCPropertyHolder* holder, int purpose);
    cSC4LineItem* GetLineItemFromBudgetItem(const BudgetItem& item);
    static void CopyBudgetItemProperties(cISCPropertyHolder* from, cISCPropertyHolder* to);
    void NeededFundingChanged(cSC4DepartmentBudget* department) override;
    void FundingPercentageChanged(cSC4DepartmentBudget* department) override;
    void PostBudgetMessage(std::uint32_t type, cSC4DepartmentBudget* department, std::uint32_t id, std::uint32_t extra) override;
    void NotifyLocalFundingChanged(cISCPropertyHolder* holder) override;
    std::string LoadLocalizedString(std::uint32_t group, std::uint32_t instance) override;
    std::string BuildingName(std::uint32_t buildingType) override;

    bool expenseView = false;                           // +0x8e0 (income / expense budget window)

private:
    std::uint32_t refCount_ = 0;                        // +0x1c
    bool initialised_ = false;                          // +0x18
    std::int32_t cheatCounter_ = 0;                     // +0x70 (in the original's gap before the department map)
    bool legacyTaxFlag_ = false;                        // +0x6b0: when set Init resets tax type 8's rate to the default
    bool loaded_ = false;                               // +0x20
    std::int64_t funds_ = 0, monthStartFunds_ = 0;      // +0x48, +0x50
    std::int64_t borrowed_ = 0;                         // +0x58
    std::int64_t ytdExpenses_ = 0, ytdIncome_ = 0;      // +0x60, +0x68
    std::map<std::uint32_t, cSC4DepartmentBudget*> departments_;   // +0x78
    std::map<std::uint32_t, BudgetGroup> groups_;       // +0x90
    TaxType taxTypes_[12];                              // +0xa8
    std::pair<std::int64_t, std::int64_t> loadedTax_[12] = {};   // the city file's assessed / income per tax type (SC4_TAXDEBUG)
    std::int64_t monthlyTax_ = 0;                       // +0x6a8
    std::int64_t ordinanceExpenseYtd_ = 0, ordinanceIncomeYtd_ = 0;      // +0x6b8, +0x6c0
    std::int64_t ordinanceExpenseByMonth_[12] = {};     // +0x6c8
    std::int64_t ordinanceIncomeByMonth_[12] = {};      // +0x728
    std::int64_t taxYtd_ = 0, misc790_ = 0, misc798_ = 0, misc7a0_ = 0, misc7a8_ = 0, misc7b0_ = 0;   // +0x788..
    std::list<Bond> bonds_;                             // +0x7b8
    std::int32_t outstandingBonds_ = 0;                 // +0x7d0
    std::int64_t bondPaymentsYtd_ = 0;                  // +0x7d8
    Tunables tunables_;
    cSC4ResponseCurve landValueCurve_;                  // +0x870
    std::int64_t monthlyIncome_ = 0, yearlyIncome_ = 0, monthlyExpense_ = 0, yearlyExpense_ = 0;   // +0x8c0..+0x8d8
};

}  // namespace sc4
