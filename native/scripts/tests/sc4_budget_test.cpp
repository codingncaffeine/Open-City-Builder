#include <cmath>
#include <cstdio>
#include <map>

#include "rz/PortMemoryStreams.h"
#include "sc4/Budget.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Host final : public IBudgetHost {
public:
    std::map<std::pair<cISCPropertyHolder*, std::uint32_t>, BudgetItem> items;
    int neededChanged = 0, fundingChanged = 0, notified = 0;
    std::vector<std::uint32_t> messages;
    bool GetBudgetItemForPurpose(cISCPropertyHolder* h, std::uint32_t purpose, BudgetItem& item) override {
        auto it = items.find({h, purpose});
        if (it == items.end()) return false;
        item = it->second;
        return true;
    }
    bool GetBudgetItemInfo(cISCPropertyHolder* h, std::vector<BudgetItem>& out) override {
        for (auto& e : items) if (e.first.first == h) out.push_back(e.second);
        return !out.empty();
    }
    void ChangeBudgetItemCost(cISCPropertyHolder* h, std::uint32_t purpose, std::int64_t cost) override {
        items[{h, purpose}].cost = cost;
    }
    void ChangeBudgetItemLocalFunding(cISCPropertyHolder* h, std::uint32_t purpose, float f) override {
        items[{h, purpose}].localFunding = f;
    }
    void NeededFundingChanged(cSC4DepartmentBudget*) override { ++neededChanged; }
    void FundingPercentageChanged(cSC4DepartmentBudget*) override { ++fundingChanged; }
    void PostBudgetMessage(std::uint32_t type, cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override { messages.push_back(type); }
    void NotifyLocalFundingChanged(cISCPropertyHolder*) override { ++notified; }
    std::string LoadLocalizedString(std::uint32_t g, std::uint32_t i) override { return "str" + std::to_string(g) + "_" + std::to_string(i); }
    std::string BuildingName(std::uint32_t t) override { return "building" + std::to_string(t); }
};
}  // namespace

int main() {
    Host host;
    SetBudgetHost(&host);
    auto LF = [&](cISCPropertyHolder* h) { return host.items[std::make_pair(h, 3u)].localFunding; };
    auto COST = [&](cISCPropertyHolder* h) { return host.items[std::make_pair(h, 3u)].cost; };
    auto* dept = new cSC4DepartmentBudget();
    dept->AddRef();
    dept->SetDepartmentID(7);
    CHECK(dept->Init());
    dept->SetDepartmentName(0x10, 0x20);
    cRZString name;
    dept->GetDepartmentName(name);
    CHECK(std::string(name.ToChar()) == "str16_32");
    CHECK(dept->GetMaxAllowedFundingPercentage() == 1.0f);

    cSC4LineItem* fixed = dept->CreateLineItem(10, false);
    cSC4LineItem* local = dept->CreateLineItemForBuildingType(20, true);
    cRZString itemName;
    local->GetName(itemName);
    CHECK(std::string(itemName.ToChar()) == "building20");
    CHECK(fixed->GetDisplayFlags() == 5 && fixed->GetType() == 1);
    fixed->SetDisplayFlag(2, true);
    CHECK(fixed->GetDisplayFlags() == 7);
    fixed->SetDisplayFlag(1, false);
    CHECK(fixed->GetDisplayFlags() == 6);

    dept->SetFundingPercentage(0.5f, 0);
    fixed->SetFullExpenses(1000);                     // current = funding * full
    CHECK(fixed->GetCurrentExpenses() == 500);
    fixed->AddToFullExpenses(1000);
    CHECK(fixed->GetFullExpenses() == 2000 && fixed->GetCurrentExpenses() == 1000);
    fixed->SetIncome(-5);
    CHECK(fixed->GetIncome() == 0);
    fixed->AddToIncome(30);
    CHECK(fixed->GetIncome() == 30 && dept->GetTotalIncome() == 30);
    CHECK(dept->GetIdealMonthlyFunding() == 2000 && dept->GetTotalExpenses() == 1000);
    CHECK(host.neededChanged > 0);

    // locally funded objects feed the local line item
    auto* pa = new cSCPropertyHolder();
    auto* pb = new cSCPropertyHolder();
    pa->AddRef();
    pb->AddRef();
    cSCPropertyHolder& a = *pa;
    cSCPropertyHolder& b = *pb;
    BudgetItem ia; ia.departmentId = 7; ia.lineItemId = 20; ia.cost = 400; ia.purpose = 3; ia.localFunding = 0.5f;
    BudgetItem ib = ia; ib.cost = 600;
    host.items[std::make_pair<cISCPropertyHolder*, std::uint32_t>(&a, 3)] = ia;
    host.items[std::make_pair<cISCPropertyHolder*, std::uint32_t>(&b, 3)] = ib;
    dept->AddLocallyFundedObject(&a, 3);
    dept->AddLocallyFundedObject(&b, 3);
    CHECK(dept->IsLocallyFundedObjectInDepartment(&a, 3) && !dept->IsLocallyFundedObjectInDepartment(&a, 4));
    CHECK(dept->HasLocallyFundedItemsByPurpose(3) && !dept->HasLocallyFundedItemsByPurpose(9));
    CHECK(local->GetFullExpenses() == 1000);
    CHECK(dept->GetLocalFullFunding(&a, 3) == 400);
    dept->SetFundingPercentage(0.8f, 0);              // redistributes: 200+300 -> 800
    // ceil(400 * 0.3f) is 121 in float arithmetic, so the shares are 321 and 479 (sum 800)
    CHECK(std::fabs(LF(&a) - 321.0f / 400.0f) < 1e-6f);
    CHECK(std::fabs(LF(&b) - 479.0f / 600.0f) < 1e-6f);
    CHECK(local->GetCurrentExpenses() == 800);
    // the maximum caps an object
    dept->SetMaxAllowedFundingPercentage(0.9f);
    dept->SetFundingPercentage(1.0f, 0);              // clamped to 0.9
    CHECK(std::fabs(dept->GetFundingPercentage() - 0.9f) < 1e-6f);
    // changing one object's cost
    dept->SetLocalFullFunding(&a, 800, 3);
    CHECK(COST(&a) == 800 && local->GetFullExpenses() == 1400);
    std::vector<cISCPropertyHolder*> funded;
    dept->GetLocallyFundedItemsByPurpose(3, funded);
    CHECK(funded.size() == 2);
    for (auto* f : funded) f->Release();
    dept->RemoveLocallyFundedObject(&a, 3);
    CHECK(!dept->IsLocallyFundedObjectInDepartment(&a, 3) && local->GetFullExpenses() == 600);

    // serialization of the plain fields
    PortMemoryOStream out;
    CHECK(fixed->Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* copy = new cSC4LineItem(nullptr, 0, false);
    CHECK(copy->Read(in));
    CHECK(copy->GetID() == 10 && copy->GetFullExpenses() == 2000 && copy->GetIncome() == 30 &&
          copy->GetDisplayFlags() == 6);
    copy->Release();

    // full recalculation: the Mac build forms localFunding * 100 first, so 900 at 13 % sums to exactly 11700 (117);
    // multiplying 900 * 0.13f first (the Windows order) gives 11699.999 and 116 (needs -ffp-contract=off to show)
    {
        auto* d2 = new cSC4DepartmentBudget();
        d2->AddRef();
        d2->SetDepartmentID(8);
        CHECK(d2->Init());
        cSC4LineItem* line = d2->CreateLineItem(30, true);
        auto* pc = new cSCPropertyHolder();
        pc->AddRef();
        BudgetItem ic; ic.departmentId = 8; ic.lineItemId = 30; ic.cost = 900; ic.purpose = 3; ic.localFunding = 1.0f;
        host.items[std::make_pair<cISCPropertyHolder*, std::uint32_t>(pc, 3)] = ic;
        d2->AddLocallyFundedObject(pc, 3);
        d2->SetFundingPercentage(0.13f, 0);
        host.items[std::make_pair<cISCPropertyHolder*, std::uint32_t>(pc, 3)].localFunding = 0.13f;
        line->SetType(0);                             // only local expense lines (type 0) are summed
        line->SetCurrentExpenses(5);                  // must be recomputed
        d2->RecalculateAllLineItemCurrentExpenses();
        CHECK(line->GetCurrentExpenses() == 117);
        d2->RemoveLocallyFundedObject(pc, 3);
        host.items.erase(std::make_pair<cISCPropertyHolder*, std::uint32_t>(pc, 3));
        pc->Release();
        d2->Shutdown();
        d2->Release();
    }

    PortMemoryOStream out2;
    CHECK(dept->Write(out2));
    CHECK(dept->GetLineItem(10) == fixed && dept->GetLineItem(99) == nullptr);
    dept->RemoveLineItem(10);
    CHECK(dept->GetLineItem(10) == nullptr);
    dept->Shutdown();
    dept->Release();
    pa->Release();
    pb->Release();
    std::printf(failures ? "FAILED\n" : "sc4_budget: all passed\n");
    return failures ? 1 : 0;
}
