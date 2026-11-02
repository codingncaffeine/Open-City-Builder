#include "sc4/CivicBuildingSimulator.h"

#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "rz/cRZVariant.h"
#include "sc4/DBSegment.h"
#include "sc4/GameDataRegistryHelpers.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

namespace {
ICivicHost* g_host = nullptr;

constexpr std::uint32_t kBuildingOccupantIid = kGZIID_cISC4BuildingOccupant;
constexpr std::uint32_t kDepartmentIid = kGZIID_cISC4DepartmentBudget;
constexpr std::uint32_t kOccupantIid = kGZIID_cISC4Occupant;

// The monthly ape escape compares the uniform double against (double)0.01f — the original holds the float at
// 0x100a182c8 and widens it, so 0x3f847ae1/0x3f847ae147ae147b instead of 0.01.
constexpr double kApeEscapeChance = static_cast<double>(0.01f);
// 0.5f at 0x100a158bc: funding at or above it counts as funded.
constexpr float kCivicFundedThreshold = 0.5f;
// kSC4PercentageOneHundred (0x100a182d0).
constexpr float kCivicPercentageOneHundred = 1.0f;

cIGZMessage2Standard& StandardMessage(cIGZMessage2* message) {
    return *static_cast<cIGZMessage2Standard*>(message);
}

// 0x100222758: the original's GetBudgetItemForPurpose asks for the whole item list (vslot 0x190) and returns the
// item whose "reserved" word (property 0xea54d285, BudgetItem::reserved8) equals the purpose id, as
// cSC4BudgetSimulator::GetBudgetItemForPurpose does.
bool FindBudgetItemByPurpose(cSC4BudgetSimulator* budget, cISCPropertyHolder* holder, std::uint32_t purpose,
                             BudgetItem& item) {
    if (!budget || !holder) return false;
    std::vector<BudgetItem> items;
    if (!budget->GetBudgetItemInfo(holder, items)) return false;
    for (const BudgetItem& candidate : items) {
        if (static_cast<std::uint32_t>(candidate.reserved8) == purpose) {
            item = candidate;
            return true;
        }
    }
    return false;
}

// The original's generic list operators (0x1000cfbd7 write / 0x1000cfb37 read): a u32 count, then each element
// through its serializable view with the cISC4Occupant iid.
void WriteOccupantList(cIGZOStream& stream, const std::list<cISC4Occupant*>& list) {
    stream.SetUint32(static_cast<std::uint32_t>(list.size()));
    for (cISC4Occupant* occupant : list) SC4WriteGZSerializable(stream, occupant);
}

void ReadOccupantList(cIGZIStream& stream, std::list<cISC4Occupant*>& list) {
    // The original's std::list clear drops the pointers without releasing them; the port drops the references it
    // holds so the counts stay honest (documented in the header).
    while (!list.empty()) {
        list.front()->Release();
        list.pop_front();
    }
    std::uint32_t count = 0;
    stream.GetUint32(count);
    if (count > kCivicListCountLimit) return;
    for (; count != 0; --count) {
        cISC4Occupant* occupant = nullptr;
        // The original pushes whatever the read produced; the port stops so a truncated stream stores no null.
        if (!SC4ReadGZSerializable(stream, kOccupantIid, reinterpret_cast<void**>(&occupant), false)) break;
        list.push_back(occupant);
    }
}

}  // namespace

ICivicHost* CivicBuildingHost() { return g_host; }
void SetCivicBuildingHost(ICivicHost* host) { g_host = host; }

// @ 0x1000e8dec. Resets the three views, drops both occupant lists (releasing each occupant) and the department.
cSC4CivicBuildingSimulator::~cSC4CivicBuildingSimulator() {
    ReleaseOccupantList(zoos_);
    ReleaseOccupantList(landmarks_);
    if (department_) {
        department_->Release();
        department_ = nullptr;
    }
}

void cSC4CivicBuildingSimulator::ReleaseOccupantList(std::list<cISC4Occupant*>& list) {
    while (!list.empty()) {
        list.front()->Release();
        list.pop_front();
    }
}

bool cSC4CivicBuildingSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cISC4CivicBuildingSimulator) {
        *out = static_cast<cISC4CivicBuildingSimulator*>(this);
    } else if (iid == 0x452294aaU) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == 0xe4fda3d4U) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4CivicBuildingSimulator::AddRef() { return ++refCount_; }

std::uint32_t cSC4CivicBuildingSimulator::Release() {
    // 0x1000e8f52: "1 < count" decrements, anything else destroys through the deleting destructor and answers 0.
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

cSC4BudgetSimulator* cSC4CivicBuildingSimulator::Budget() const {
    return g_host ? g_host->BudgetSimulator() : nullptr;
}

// @ 0x1000e8fb0. Wires the civic department (created on first use, 100% funding cap), the two further civic
// departments, the three message subscriptions, the monthly simulator agent and the game-data node/value.
bool cSC4CivicBuildingSimulator::Init() {
    if (initialised_) return true;
    initialised_ = true;

    cSC4BudgetSimulator* budget = Budget();
    if (!department_ && budget) {
        cSC4DepartmentBudget* created = budget->CreateDepartmentBudget(kCivicDepartment, kCivicDepartmentGroup);
        if (created != department_) {
            if (created) created->AddRef();
            cSC4DepartmentBudget* previous = department_;
            department_ = created;
            if (previous) previous->Release();
        }
        // The original dereferences the created department without a null test (0x1000e902a); guarded here.
        if (department_) {
            department_->SetDepartmentName(kBudgetGroupNameTable, kCivicDepartmentName);
            department_->SetMaxAllowedFundingPercentage(kCivicPercentageOneHundred);
        }
    }
    if (budget) {
        if (!budget->GetDepartmentBudget(kCivicDepartment2)) {
            if (cSC4DepartmentBudget* second = budget->CreateDepartmentBudget(kCivicDepartment2, kCivicDepartment2Group)) {
                second->SetDepartmentName(kBudgetGroupNameTable, kCivicDepartment2Name);
                second->SetMaxAllowedFundingPercentage(kCivicPercentageOneHundred);
            }
        }
        if (!budget->GetDepartmentBudget(kCivicDepartment3)) {
            if (cSC4DepartmentBudget* third = budget->CreateDepartmentBudget(kCivicDepartment3, kCivicDepartment3Group)) {
                third->SetDepartmentName(kBudgetGroupNameTable, kCivicDepartment3Name);
                third->SetMaxAllowedFundingPercentage(kCivicPercentageOneHundred);
            }
        }
    }

    if (SL::spMS2) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        SL::spMS2->AddNotification(target, kCivicMessageLotStateChanged);
        SL::spMS2->AddNotification(target, kCivicMessageOccupantInserted);
        SL::spMS2->AddNotification(target, kCivicMessageOccupantRemoved);
    }
    if (g_host) {
        // The original builds the name in a 32-byte heap buffer (0x1000e9151) and passes it as an empty-then-filled
        // cRZString; the port passes a cRZString literal with the same text.
        g_host->AddSimulatorAgent(static_cast<cIGZMessageTarget2*>(this), kCivicAgentInterval, kCivicSimulatorName,
                                  kCivicAgentFlags);
    }
    if (g_host) {
        if (cISC4GameDataRegistry* registry = g_host->GameDataRegistry()) {
            nsISC4GameDataRegistry::AddNode<cSC4CivicBuildingSimulator, cIGZUnknown>(this, "civic", nullptr, 0, nullptr,
                                                                                    registry);
            // The original registers the function pointer itself as the entry handle and `this` as the context; the
            // handler resolves the context from the handle it is called with (0x1000e94cc).
            void* handle = reinterpret_cast<void*>(&cSC4CivicBuildingSimulator::sGetLandmarkCount);
            nsISC4GameDataRegistry::AddValue(handle, "get_landmark_count", &cSC4CivicBuildingSimulator::sGetLandmarkCount,
                                             this, 0, this, registry);
        }
    }
    return true;
}

// @ 0x1000e94cc. Registry command "get_landmark_count": the entry's context is the simulator, and its answer is
// written to result parameter 1 as a uint32 variant.
bool cSC4CivicBuildingSimulator::sGetLandmarkCount(void* handle, cIGZCommandParameterSet* /*in*/,
                                                   cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry) {
    if (!registry) return false;
    void* object = nullptr;
    const EntryId entry = registry->GetEntryID(handle);          // registry vslot 0x48
    if (entry) object = registry->GetEntryContext(entry);        // registry vslot 0x90
    auto* simulator = static_cast<cSC4CivicBuildingSimulator*>(object);
    const std::uint32_t count = simulator ? simulator->GetLandmarkCount() : 0;
    if (out) {
        cRZVariant value(count);
        out->SetParameter(1, value);                             // parameter set vslot 0x38
    }
    return true;
}

// @ 0x1000e9564. Unregisters the game-data node, drops both lists, takes the monthly agent out, releases the
// department and unsubscribes the three messages.
bool cSC4CivicBuildingSimulator::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;

    if (g_host) {
        if (cISC4GameDataRegistry* registry = g_host->GameDataRegistry()) {
            cIGZUnknown* node = nullptr;
            QueryInterface(1, reinterpret_cast<void**>(&node));
            registry->RemoveChildEntry(node);                    // registry vslot 0x30
            if (node) node->Release();
        }
    }
    ReleaseOccupantList(zoos_);
    ReleaseOccupantList(landmarks_);
    if (g_host) g_host->RemoveSimulatorAgent(static_cast<cIGZMessageTarget2*>(this));
    if (department_) {
        department_->Release();
        department_ = nullptr;
    }
    if (SL::spMS2) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        SL::spMS2->RemoveNotification(target, kCivicMessageLotStateChanged);
        SL::spMS2->RemoveNotification(target, kCivicMessageOccupantInserted);
        SL::spMS2->RemoveNotification(target, kCivicMessageOccupantRemoved);
    }
    return true;
}

bool cSC4CivicBuildingSimulator::IsBuildingExcluded(std::uint32_t buildingId) {
    return excludedBuildings_.find(buildingId) != excludedBuildings_.end();
}

cISC4CivicBuildingSimulator::ConditionalBuildingStatus* cSC4CivicBuildingSimulator::GetConditionalBuildingStatus(
    std::uint32_t id) {
    auto it = statuses_.find(id);
    return it == statuses_.end() ? nullptr : &it->second;
}

void cSC4CivicBuildingSimulator::UpdateConditionalBuildingStatus(std::uint32_t id,
                                                                const ConditionalBuildingStatus* status) {
    if (!status) return;
    ConditionalBuildingStatus& entry = statuses_[id];
    // 0x1000ea743 copies the 32-bit word at +0 and then the cRZString's inner std::string; the padding at +0x04 and
    // the text's reference count are not touched.
    entry.state = status->state;
    entry.text = status->text;
}

std::uint32_t cSC4CivicBuildingSimulator::GetLandmarkCount() const {
    return static_cast<std::uint32_t>(landmarks_.size());
}

std::uint32_t cSC4CivicBuildingSimulator::GetGZCLSID() const { return kGZCLSID_cSC4CivicBuildingSimulator; }

// @ 0x1000e9712. Version 6: the excluded-building ids, then the conditional building statuses, then the civic
// department and the two occupant lists.
bool cSC4CivicBuildingSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(kCivicSaveVersion);
    stream.SetUint32(static_cast<std::uint32_t>(excludedBuildings_.size()));
    for (std::uint32_t id : excludedBuildings_) stream.SetUint32(id);
    stream.SetUint32(static_cast<std::uint32_t>(statuses_.size()));
    for (const auto& entry : statuses_) {
        stream.SetUint32(entry.first);
        stream.SetUint8(static_cast<std::uint8_t>(entry.second.state & 0xffu));
        stream.SetUint16(static_cast<std::uint16_t>(entry.second.state >> 16));
        stream.SetGZStr(entry.second.text);
    }
    SC4WriteGZSerializable(stream, static_cast<const cIGZUnknown*>(static_cast<const cIGZSerializable*>(department_)));
    WriteOccupantList(stream, zoos_);
    WriteOccupantList(stream, landmarks_);
    return !stream.GetError();
}

// @ 0x1000e97f6. Only version 6 is understood; the two tables are merged into (not cleared first), exactly like the
// original inserts into its live containers.
bool cSC4CivicBuildingSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kCivicSaveVersion) return false;

    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (; count != 0; --count) {
        std::uint32_t id = 0;
        stream.GetUint32(id);
        excludedBuildings_.insert(id);
    }

    std::uint32_t statusCount = 0;
    stream.GetUint32(statusCount);
    for (; statusCount != 0; --statusCount) {
        std::uint32_t id = 0;
        std::uint8_t state = 0;
        std::uint16_t buildingCount = 0;
        cRZString text;
        stream.GetUint32(id);
        stream.GetUint8(state);
        stream.GetUint16(buildingCount);
        stream.GetGZStr(text);
        ConditionalBuildingStatus& entry = statuses_[id];
        // Byte +1 of the state word is not serialized, so it always comes back 0.
        entry.state = static_cast<std::uint32_t>(state) | (static_cast<std::uint32_t>(buildingCount) << 16);
        entry.text = text;
    }

    if (department_) {
        department_->Release();
        department_ = nullptr;
    }
    SC4ReadGZSerializable(stream, kDepartmentIid, reinterpret_cast<void**>(&department_), false);
    ReadOccupantList(stream, zoos_);
    ReadOccupantList(stream, landmarks_);
    return !stream.GetError();
}

// @ 0x1000e9c7a. Occupant insert/remove keeps the conditional building count and the excluded-building set in step;
// the lot-state message (new state 3 = livable) registers a civic building; the month message runs the pass.
bool cSC4CivicBuildingSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type == kCivicMessageOccupantInserted || type == kCivicMessageOccupantRemoved) {
        cIGZMessage2Standard& payload = StandardMessage(message);
        auto* occupant = static_cast<cISC4Occupant*>(payload.GetVoid1());      // slot 0x50
        if (!occupant) return true;
        if (occupant->GetType() != kOccupantTypeBuilding) return true;        // slot 0x38
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();            // slot 0x30
        if (!holder) return true;

        bool conditionalBuilding = false;
        cSCPropertyHelp::GetPropertyValue(holder, kCivicPropertyConditionalBuilding, conditionalBuilding);
        if (conditionalBuilding) {
            cISC4BuildingOccupant* building = nullptr;
            if (occupant->QueryInterface(kBuildingOccupantIid, reinterpret_cast<void**>(&building)) && building) {
                cGZPersistResourceKey key{};
                if (g_host && g_host->GetBuildingKeyFromType(building->GetBuildingType(), key)) {
                    if (ConditionalBuildingStatus* status = GetConditionalBuildingStatus(key.instance)) {
                        status->AddToBuildingCount(type == kCivicMessageOccupantInserted ? 1 : -1);
                    }
                }
                building->Release();
            }
        }

        std::uint32_t exclusionId = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kCivicPropertyExclusionId, exclusionId)) {
            if (type == kCivicMessageOccupantInserted) excludedBuildings_.insert(exclusionId);
            else excludedBuildings_.erase(exclusionId);
        }
        if (type != kCivicMessageOccupantRemoved) return true;
        cISC4BuildingOccupant* building = nullptr;
        if (!occupant->QueryInterface(kBuildingOccupantIid, reinterpret_cast<void**>(&building)) || !building) return true;
        DoBuildingRemoved(building);
        building->Release();
        return true;
    }
    if (type == kCivicMessageMonth) {
        DoMonthlySimulation();
        return true;
    }
    if (type == kCivicMessageLotStateChanged) {
        ICivicLot* lot = g_host ? g_host->MessageLot(message) : nullptr;
        if (!lot) return true;
        if (StandardMessage(message).GetData2() == 3) DoLotActivated(lot);
        return true;
    }
    return true;
}

// The list search the original performs (0x1000ea285 / 0x1000ea35a): walk from the last node backwards and stop at
// end() when it was not found.
namespace {
template <class List>
typename List::iterator FindBackwards(List& list, const typename List::value_type& value) {
    for (auto it = list.end(); it != list.begin();) {
        --it;
        if (*it == value) return it;
    }
    return list.end();
}
}  // namespace

// @ 0x1000ea076. Registers a civic building on its lot: the landmark item's cost goes into the department's line
// item for the building type, the second civic item's cost into that line item's income, the building joins the
// landmark list and (occupant group 0x1917) the zoo list.
void cSC4CivicBuildingSimulator::DoLotActivated(ICivicLot* lot) {
    if (!lot) return;
    cISC4BuildingOccupant* building = lot->GetBuilding();       // lot vslot 0x180, borrowed
    if (!building) return;
    cISC4Occupant* occupant = building->AsOccupant();           // building occupant vslot 0x18
    if (!occupant) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();  // occupant vslot 0x30
    cSC4BudgetSimulator* budget = Budget();

    if (budget && holder) {
        BudgetItem item;
        item.localFunding = 0.0f;
        if (FindBudgetItemByPurpose(budget, holder, kCivicBudgetPurposeLandmark, item)) {
            if (item.lineItemId == 0) {
                item.lineItemId = static_cast<std::int32_t>(building->GetBuildingType());   // building occupant 0x20
                budget->ChangeBudgetItemLine(holder, static_cast<int>(item.purpose),
                                             static_cast<std::uint32_t>(item.lineItemId));
            }
            if (cSC4DepartmentBudget* department =
                    budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId))) {
                cSC4LineItem* line = department->GetLineItem(static_cast<std::uint32_t>(item.lineItemId));
                if (!line) line = department->CreateLineItemForBuildingType(
                    static_cast<std::uint32_t>(item.lineItemId), false);
                if (line) line->AddToFullExpenses(item.cost);
            }
            // The original pushes unconditionally (0x1000ea195): a second activation of the same lot would add a
            // second reference and a second list entry.
            building->AddRef();                        // 0x1000ea166
            landmarks_.push_back(occupant);
        }
        BudgetItem incomeItem;
        incomeItem.localFunding = 0.0f;
        if (FindBudgetItemByPurpose(budget, holder, kCivicBudgetPurposeIncome, incomeItem)) {
            if (cSC4DepartmentBudget* department =
                    budget->GetDepartmentBudget(static_cast<std::uint32_t>(incomeItem.departmentId))) {
                const std::uint32_t buildingType = building->GetBuildingType();
                cSC4LineItem* line = department->GetLineItem(buildingType);
                if (!line) {
                    line = department->CreateLineItemForBuildingType(buildingType, false);
                    // The original sets the type before its null test (0x1000ea22b); guarded here.
                    if (line) line->SetType(1);
                }
                if (line) line->AddToIncome(incomeItem.cost);
            }
        }
    }
    if (occupant->IsOccupantGroup(kCivicOccupantGroupZoo)) {    // occupant vslot 0xb0
        if (FindBackwards(zoos_, occupant) != zoos_.end()) return;
        zoos_.push_back(occupant);
        building->AddRef();
    }
}

// @ 0x1000ea2f0. The monthly pass: a 1% chance that a random zoo lets its animals out, then the landmark lot
// conditions follow the landmark department's funding (>= 50% clears a warning, < 50% raises one).
void cSC4CivicBuildingSimulator::DoMonthlySimulation() {
    if (!zoos_.empty()) {
        cRZRandom random(0xffffffffu);                          // seeded from the clock, as the original does
        const double roll = random.RandomDoubleUniform();
        if (roll <= kApeEscapeChance) {
            const std::uint32_t index =
                random.RandomUint32Uniform(static_cast<std::uint32_t>(zoos_.size()));   // 0x1000ea34a
            // The original walks backwards from the last node (0x1000ea353), so the pick counts from the end.
            auto it = zoos_.end();
            --it;
            for (std::uint32_t step = 0; step < index; ++step) --it;
            cISC4Occupant* zoo = *it;
            if (g_host && zoo) {
                cRZString group(kCivicAutomataGroupApeEscape);
                g_host->AddControllersFromOccupantGroup(group, zoo);
            }
        }
    }
    if (!landmarks_.empty()) {
        cSC4BudgetSimulator* budget = Budget();
        // The original walks the list from its last node backwards (0x1000ea4fe/0x1000ea442).
        for (auto it = landmarks_.rbegin(); it != landmarks_.rend(); ++it) {
            cISC4Occupant* occupant = *it;
            if (!occupant || !budget) continue;
            cISCPropertyHolder* holder = occupant->AsPropertyHolder();
            BudgetItem item;
            if (!holder || !FindBudgetItemByPurpose(budget, holder, kCivicBudgetPurposeLandmark, item)) continue;
            cSC4DepartmentBudget* department =
                budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
            if (!department) continue;
            ICivicLot* lot = g_host ? g_host->LotForOccupant(occupant) : nullptr;
            if (!lot) continue;
            const float funding = department->GetFundingPercentage();     // department vslot 0x50
            const std::uint32_t condition = lot->GetCondition();          // lot vslot 0x118
            if (funding >= kCivicFundedThreshold) {
                if (condition == kCivicLotConditionWarning) lot->SetCondition(kCivicLotConditionNormal);
            } else if (condition == kCivicLotConditionNormal) {
                lot->SetCondition(kCivicLotConditionWarning);
            }
        }
    }
}

// @ 0x1000ea536. Takes the building out of the civic bookkeeping: the landmark item leaves the budget with the
// occupant off the landmark list, the income item's cost is subtracted from the line item (which goes away when its
// income drops below 1), and a zoo leaves the zoo list.
void cSC4CivicBuildingSimulator::DoBuildingRemoved(cISC4BuildingOccupant* building) {
    if (!building) return;
    cISC4Occupant* occupant = building->AsOccupant();
    if (!occupant) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    cSC4BudgetSimulator* budget = Budget();

    if (budget && holder) {
        BudgetItem item;
        item.localFunding = 0.0f;
        if (FindBudgetItemByPurpose(budget, holder, kCivicBudgetPurposeLandmark, item)) {
            budget->RemoveBudgetItemFromBudget(holder, static_cast<int>(item.purpose));
            cISC4Occupant* listed = building->AsOccupant();
            auto it = FindBackwards(landmarks_, listed);
            if (it != landmarks_.end()) {
                listed->Release();                    // 0x1000ea5d5: release the listed occupant, then erase
                landmarks_.erase(it);
            }
        }
        BudgetItem incomeItem;
        incomeItem.localFunding = 0.0f;
        if (FindBudgetItemByPurpose(budget, holder, kCivicBudgetPurposeIncome, incomeItem)) {
            if (cSC4DepartmentBudget* department =
                    budget->GetDepartmentBudget(static_cast<std::uint32_t>(incomeItem.departmentId))) {
                const std::uint32_t buildingType = building->GetBuildingType();
                cSC4LineItem* line = department->GetLineItem(buildingType);
                if (line) {
                    line->SetIncome(line->GetIncome() - incomeItem.cost);
                    if (line->GetIncome() < 1) department->RemoveLineItem(buildingType);
                }
            }
        }
    }
    if (occupant->IsOccupantGroup(kCivicOccupantGroupZoo)) {
        auto it = FindBackwards(zoos_, occupant);
        if (it != zoos_.end()) {
            zoos_.erase(it);
            building->Release();
        }
    }
}

}  // namespace sc4
