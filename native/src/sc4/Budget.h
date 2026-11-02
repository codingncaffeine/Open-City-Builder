// Budget line items and department budgets (cSC4LineItem 32 functions 0x100245924..0x100245edc, class id
// 0xaa313c9f; cSC4DepartmentBudget 54 functions 0x10024347a..0x100245e42, class id 0xe990bffc). A department
// budget owns its line items and tracks the "locally funded" objects (building, purpose) whose own funding
// percentage feeds the department's. The budget simulator (spBudgetSim) is reached through IBudgetHost.
//
// cSC4LineItem layout: +0x10 owner reference held, +0x18 name (cRZString), +0x40 name instance, +0x44 name
// group, +0x48 name needs reload, +0x50 secondary info, +0x58 full expenses, +0x60 current expenses, +0x68
// income, +0x70 refcount, +0x74 id, +0x78 display flags (default 5), +0x7c local-funding item, +0x80 department,
// +0x88 type. cSC4DepartmentBudget layout: +0x10 initialised, +0x14 refcount, +0x18 id, +0x20 name, +0x48/+0x4c
// name instance/group, +0x50 budget group, +0x54 funding %, +0x58 maximum funding % (default 1), +0x60 total
// spending, +0x68 fixed funding, +0x70 line item map, +0x88 locally funded set.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZString.h"
#include "sc4/SCProperty.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4LineItem = 0xaa313c9f;
constexpr std::uint32_t kGZCLSID_cSC4DepartmentBudget = 0xe990bffc;
constexpr std::uint32_t kGZIID_cISC4DepartmentBudget = 0x223e2b67;
constexpr std::uint32_t kBudgetMessageFundingPercentageChanged = 0x08dac3e7;
constexpr std::uint32_t kBudgetMessageNeededFundingChanged = 0x08dac3e8;
constexpr std::uint32_t kBudgetMessageLocalFundingChanged = 0x6a5fd3fd;
constexpr std::uint32_t kGZIID_cISCPropertyHolderNotify = 0x26bb767e;   // interface asked of a funded object

class cSC4DepartmentBudget;

// One budget item of an object: which department / line it feeds and how it is funded (cISC4BudgetSimulator::BudgetItem).
struct BudgetItem {
    std::int32_t departmentId = 0;      // +0x00
    std::int32_t lineItemId = 0;        // +0x04
    std::int32_t reserved8 = 0;         // +0x08 the purpose id (property 0xea54d285) GetBudgetItemForPurpose matches
    std::int32_t reservedC = 0;         // +0x0c
    std::int64_t cost = 0;              // +0x10
    std::uint32_t purpose = 0;          // +0x18 the item's INDEX in the arrays (what ChangeBudgetItem* take)
    float localFunding = 0.0f;          // +0x1c
};

class IBudgetHost {
public:
    virtual ~IBudgetHost() = default;
    virtual bool GetBudgetItemForPurpose(cISCPropertyHolder* holder, std::uint32_t purpose, BudgetItem& item) = 0;   // sim 0x198
    virtual bool GetBudgetItemInfo(cISCPropertyHolder* holder, std::vector<BudgetItem>& items) = 0;                 // sim 0x190
    virtual void ChangeBudgetItemCost(cISCPropertyHolder* holder, std::uint32_t purpose, std::int64_t cost) = 0;     // sim 0x1a8
    virtual void ChangeBudgetItemLocalFunding(cISCPropertyHolder* holder, std::uint32_t purpose, float funding) = 0; // sim 0x1b0
    virtual void NeededFundingChanged(cSC4DepartmentBudget* department) = 0;                                          // sim 0x100
    virtual void FundingPercentageChanged(cSC4DepartmentBudget* department) = 0;                                      // sim 0x108
    // message server: a standard message of `type` from the department with its id as data, `extra` as second data
    virtual void PostBudgetMessage(std::uint32_t type, cSC4DepartmentBudget* department, std::uint32_t id, std::uint32_t extra) = 0;
    // a funded object that answers interface 0x26bb767e is told its local funding changed (message 0x6a5fd3fd)
    virtual void NotifyLocalFundingChanged(cISCPropertyHolder* holder) = 0;
    virtual std::string LoadLocalizedString(std::uint32_t group, std::uint32_t instance) = 0;                         // cSLString
    virtual std::string BuildingName(std::uint32_t buildingType) = 0;                                                 // building dev sim 0x130
};
IBudgetHost* BudgetHost();
void SetBudgetHost(IBudgetHost* host);

class cISC4LineItem : public cIGZUnknown {
public:
    virtual void SetName(std::uint32_t group, std::uint32_t instance) = 0;
    virtual void GetName(cRZString& out) = 0;
    virtual std::uint32_t GetID() const = 0;
    virtual std::uint32_t GetType() const = 0;
    virtual void SetType(std::uint32_t type) = 0;
    virtual std::int64_t GetSecondaryInfoField() const = 0;
    virtual void SetSecondaryInfoField(std::int64_t value) = 0;
    virtual std::int64_t GetFullExpenses() const = 0;
    virtual void SetFullExpenses(std::int64_t value) = 0;
    virtual void AddToFullExpenses(std::int64_t value) = 0;
    virtual std::int64_t GetCurrentExpenses() const = 0;
    virtual std::int64_t GetIncome() const = 0;
    virtual void SetIncome(std::int64_t value) = 0;
    virtual void AddToIncome(std::int64_t value) = 0;
    virtual std::uint32_t GetDisplayFlags() const = 0;
    virtual void SetDisplayFlag(std::uint32_t flag, bool on) = 0;
    virtual bool IsLocalFundingItem() const = 0;
};

class cSC4LineItem : public cIGZSerializable, public cISC4LineItem {
public:
    cSC4LineItem(cSC4DepartmentBudget* department, std::uint32_t id, bool isLocalFunding);
    ~cSC4LineItem();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    void SetFullExpenses(std::int64_t value) override;
    void AddToFullExpenses(std::int64_t value) override { SetFullExpenses(fullExpenses_ + value); }
    void SetIncome(std::int64_t value) override;
    void AddToIncome(std::int64_t value) override;
    void SetName(std::uint32_t group, std::uint32_t instance) override;
    void GetName(cRZString& out) override;
    std::uint32_t GetID() const override { return id_; }
    std::uint32_t GetType() const override { return type_; }
    void SetType(std::uint32_t type) override { type_ = type; }
    std::int64_t GetSecondaryInfoField() const override { return secondaryInfo_; }
    void SetSecondaryInfoField(std::int64_t value) override { secondaryInfo_ = value; }
    std::int64_t GetFullExpenses() const override { return fullExpenses_; }
    std::int64_t GetCurrentExpenses() const override { return currentExpenses_; }
    std::int64_t GetIncome() const override { return income_; }
    std::uint32_t GetDisplayFlags() const override { return displayFlags_; }
    void SetDisplayFlag(std::uint32_t flag, bool on) override { displayFlags_ = on ? (displayFlags_ | flag) : (displayFlags_ & ~flag); }
    bool IsLocalFundingItem() const override { return isLocalFunding_; }

    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4LineItem; }

    // direct field access used by the owning department
    void SetCurrentExpenses(std::int64_t value) { currentExpenses_ = value; }
    bool ownerReferenceHeld = true;                          // +0x10
    void DropOwnerReference();                               // releases the department reference once

private:
    cRZString name_;                                         // +0x18
    std::uint32_t nameInstance_ = 0, nameGroup_ = 0;         // +0x40, +0x44
    bool nameNeedsReload_ = false;                           // +0x48
    std::int64_t secondaryInfo_ = 0;                         // +0x50
    std::int64_t fullExpenses_ = 0;                          // +0x58
    std::int64_t currentExpenses_ = 0;                       // +0x60
    std::int64_t income_ = 0;                                // +0x68
    std::uint32_t refCount_ = 1;                             // +0x70
    std::uint32_t id_ = 0;                                   // +0x74
    std::uint32_t displayFlags_ = 5;                         // +0x78
    bool isLocalFunding_ = false;                            // +0x7c
    cSC4DepartmentBudget* department_ = nullptr;             // +0x80 (reference counted)
    std::uint32_t type_ = 1;                                 // +0x88
};

class cISC4DepartmentBudget : public cIGZUnknown {
public:
    virtual std::uint32_t GetDepartmentID() const = 0;
    virtual const float& GetFundingPercentage() const = 0;
};

class cSC4DepartmentBudget : public cIGZSerializable, public cISC4DepartmentBudget {
public:
    cSC4DepartmentBudget() = default;
    ~cSC4DepartmentBudget();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init();
    bool Shutdown();
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4DepartmentBudget; }

    std::uint32_t GetDepartmentID() const override { return departmentId_; }
    void SetDepartmentID(std::uint32_t id) { departmentId_ = id; }
    void GetDepartmentName(cRZString& out) const { out = name_; }
    void SetDepartmentName(std::uint32_t group, std::uint32_t instance);
    const float& GetFundingPercentage() const override { return fundingPercentage_; }
    void SetFundingPercentage(float percentage, std::uint32_t messageData, bool recalculate = true);
    const float& GetMaxAllowedFundingPercentage() const { return maxAllowedFundingPercentage_; }
    void SetMaxAllowedFundingPercentage(float percentage) { maxAllowedFundingPercentage_ = percentage; }
    std::int64_t GetIdealMonthlyFunding() const;       // sum of the line items' full expenses
    std::int64_t GetTotalExpenses() const;             // sum of the line items' current expenses
    std::int64_t GetTotalIncome() const;
    std::int64_t GetTotalSpending() const { return totalSpending_; }
    void SetTotalSpending(std::int64_t value) { totalSpending_ = value; }
    std::uint32_t GetBudgetGroup() const { return budgetGroup_; }
    void SetBudgetGroup(std::uint32_t group) { budgetGroup_ = group; }
    bool GetIsFixedFunding() const { return fixedFunding_; }
    void SetFixedFunding(bool fixed) { fixedFunding_ = fixed; }

    cSC4LineItem* CreateLineItem(std::uint32_t id, bool isLocalFunding);
    cSC4LineItem* CreateLineItemForBuildingType(std::uint32_t buildingType, bool isLocalFunding);
    void RemoveLineItem(std::uint32_t id);
    cSC4LineItem* GetLineItem(std::uint32_t id);
    void GetAllLineItems(std::vector<cSC4LineItem*>& out);   // each returned with a reference

    // locally funded objects (building, purpose)
    void AddLocallyFundedObject(cISCPropertyHolder* object, std::uint32_t purpose);
    void RemoveLocallyFundedObject(cISCPropertyHolder* object, std::uint32_t purpose);
    bool IsLocallyFundedObjectInDepartment(cISCPropertyHolder* object, std::uint32_t purpose) const;
    void SetLocalFundingPercent(cISCPropertyHolder* object, float percent, std::uint32_t purpose);
    float GetLocalFundingPercent(cISCPropertyHolder* object, std::uint32_t purpose) const;
    void SetLocalFullFunding(cISCPropertyHolder* object, std::int64_t cost, std::uint32_t purpose);
    std::int64_t GetLocalFullFunding(cISCPropertyHolder* object, std::uint32_t purpose) const;
    void GetLocallyFundedItemsByPurpose(std::uint32_t purpose, std::vector<cISCPropertyHolder*>& out);
    bool HasLocallyFundedItemsByPurpose(std::uint32_t purpose) const;

    void RecalculateLineItemCurrentExpenses(std::uint32_t lineItemId);
    void RecalculateAllLineItemCurrentExpenses();
    void DoDepartmentFundingChanged();
    void SendBudgetChangeMessage(std::uint32_t type, std::uint32_t extra);

private:
    struct ObjectFunding {                       // 0x40 bytes in the original
        cISCPropertyHolder* object = nullptr;
        BudgetItem item;
        bool done = false;
        std::int32_t cost = 0;
        std::int32_t maximum = 0;
        std::int32_t amount = 0;
        float localFunding = 0.0f;
    };

    bool initialised_ = false;                    // +0x10
    std::uint32_t refCount_ = 0;                  // +0x14
    std::uint32_t departmentId_ = 0;              // +0x18
    cRZString name_;                              // +0x20
    std::uint32_t nameInstance_ = 0, nameGroup_ = 0;   // +0x48, +0x4c
    std::uint32_t budgetGroup_ = 0;               // +0x50
    float fundingPercentage_ = 0.0f;              // +0x54
    float maxAllowedFundingPercentage_ = 1.0f;    // +0x58
    std::int64_t totalSpending_ = 0;              // +0x60
    bool fixedFunding_ = false;                   // +0x68
    std::map<std::uint32_t, cSC4LineItem*> lineItems_;                          // +0x70
    std::set<std::pair<cISCPropertyHolder*, std::uint32_t>> locallyFunded_;     // +0x88
};

}  // namespace sc4
