// The concrete advisor classes (cSC4AdvisorBase and its six flavours): object layout and class ids, the base's
// message subscription / post_self_update / pending-update simulation, each flavour's own subscriptions and DoMessage
// routing, Finance's budget record, HQ's health/education record, Utility's power-strike handling, Traffic's car-zot
// set, and cSC4AdvisorCityPlanning's landmark map, QueryInterface and Write/Read round trip.
//
// The advisor system, simulators, message bus and property holder are fakes (IAdvisorConcreteHost); the script tables
// are the shared FakeLua driven through IAdvisorHost. The large simulator-statistics walks (occupant lists, traffic
// counters, ...) are still unrecovered, so they are routed through IAdvisorConcreteHost::UpdateSimulatorStatistics and
// only their dispatch is asserted here. Advisors that go through Init/QueryInterface are heap-allocated: the original
// notifies through QueryInterface/Release, which deletes a COM-style object whose refcount reaches zero.
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "fake_resourcemanager.h"
#include "fake_scriptlua.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZSysServPtr.h"
#include "sc4/AdvisorConcrete.h"
#include "sc4/TablePropertyReader.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
using namespace fakelua;

namespace {

// ---- string resources (cSC4Advisor::Init loads the caption / name) -------------------------------------
struct Manager final : FakeResourceManager {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> strings;
    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t, cIGZUnknown*) override {
        if (iid != kGZIID_cIGZString || key.type != kResourceTypeSLString) return false;
        auto it = strings.find({key.group, key.instance});
        if (it == strings.end()) return false;
        *out = new cRZString(it->second.c_str());
        static_cast<cIGZString*>(*out)->AddRef();
        return true;
    }
};
Manager g_manager;
bool Resolve(std::uint32_t service, std::uint32_t iid, void** out) {
    if (service == 0x056b906e && iid == 0x656b8efc) { *out = static_cast<cIGZPersistResourceManager*>(&g_manager); return true; }
    return false;
}
struct SlHost final : ISLStringHost {
    int LanguageOffset() override { return 0; }
    cSCStringDetokenizer* Detokenizer() override { return nullptr; }
};

// ---- the message bus ------------------------------------------------------------------------------------
struct SentMessage { std::uint32_t type = 0; std::uint64_t data1 = 0; std::uint64_t data2 = 0; bool post = false; };
struct FakeMessages final : cIGZMessageServer2 {
    int refs = 1;
    std::vector<SentMessage> sent;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this); AddRef(); return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2* message) override {
        auto* s = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back(SentMessage{message->GetType(), s->GetData1(), s->GetData2(), false}); return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        auto* s = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back(SentMessage{message->GetType(), s->GetData1(), s->GetData2(), true}); return true;
    }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { added.emplace_back(target, type); return true; }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { removed.emplace_back(target, type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2* message, cIGZMessageTarget2*) override {
        auto* s = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back(SentMessage{message->GetType(), s->GetData1(), s->GetData2(), true}); return true;
    }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return static_cast<std::uint32_t>(sent.size()); }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// ---- a scripted subject (CreateAdviceSubject) -----------------------------------------------------------
struct FakeSubject final : cISC4AdviceSubject {
    int refs = 1;
    bool hasLocation = false; float x = 0, z = 0; cIGZUnknown* object = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this); AddRef(); return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    void SetLocation(float px, float pz) override { hasLocation = true; x = px; z = pz; }
    float GetLocationX() override { return x; }
    float GetLocationZ() override { return z; }
    bool HasLocation() override { return hasLocation; }
    bool SetObject(cIGZUnknown* value) override { object = value; return true; }
    cIGZUnknown* GetObjectA() override { return object; }
    std::uint32_t GetData1() const override { return 0; }
    void SetData1(std::uint32_t) override {}
    std::uint32_t GetData2() const override { return 0; }
    void SetData2(std::uint32_t) override {}
    bool Clone(cISC4AdviceSubject**) override { return false; }
};

// ---- the advisor host (cSC4Advisor::Init) ---------------------------------------------------------------
struct Host final : IAdvisorHost {
    FakeLua lua;
    FakeMessages* messages = nullptr;
    int dayNumber = 7;
    std::vector<std::pair<int, cISC4Advisor*>> registered, unregistered;
    IScriptLua* Lua() override { return &lua; }
    void RegisterAdvice(int eventId, cISC4Advisor* advisor) override { registered.emplace_back(eventId, advisor); }
    void UnregisterAdvice(int eventId, cISC4Advisor* advisor) override { unregistered.emplace_back(eventId, advisor); }
    cIGZMessageServer2* Messages() override { return messages; }
    bool CreateInstance(std::uint32_t, std::uint32_t, void** out) override { if (out) *out = nullptr; return false; }
    int SimDayNumber() override { return dayNumber; }
    void PostMessage2(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, cIGZUnknown*) override {}
};

// ---- the concrete host (IAdvisorConcreteHost) -----------------------------------------------------------
struct ConcreteHost final : IAdvisorConcreteHost {
    FakeMessages* messages = nullptr;
    std::map<std::string, double> globals;
    std::vector<std::tuple<std::string, double, cISC4AdviceSubject*>> locals;
    std::vector<std::pair<int, cISC4AdviceSubject*>> pushedEvents;
    std::vector<std::uint32_t> statisticsUpdates;
    std::map<std::uint32_t, std::int64_t> budgetInts;
    std::vector<float> taxRates;
    std::map<std::uint32_t, float> lineItems;
    std::map<std::uint32_t, std::int64_t> neighborsInts;
    std::map<std::uint32_t, std::int64_t> residentialInts;
    std::map<std::uint32_t, float> residentialFloats;
    bool void1IsHolder = false;
    std::vector<std::uint32_t> holderProperties;
    bool developmentLookup = false;
    std::uint32_t developmentType = 0;
    bool civicAllowsLinks = true;
    float tractBest = 0, tractWorst = 0; int tractBestX = 0, tractBestZ = 0, tractWorstX = 0, tractWorstZ = 0;

    cIGZMessageServer2* Messages() override { return messages; }
    void SetGlobal(const char* name, double value) override { globals[name] = value; }
    void SetLocal(const char* name, double value, cISC4AdviceSubject* subject) override { locals.emplace_back(name, value, subject); }
    bool PushEvent(int eventId, cISC4AdviceSubject* subject) override { pushedEvents.emplace_back(eventId, subject); return true; }
    std::vector<FakeSubject*> made;   // every subject handed out; the game must have released its reference by the end
    bool CreateAdviceSubject(cISC4AdviceSubject** out) override { auto* s = new FakeSubject(); made.push_back(s); *out = s; return true; }
    std::int64_t BudgetInt(std::uint32_t vslot) override { auto it = budgetInts.find(vslot); return it == budgetInts.end() ? 0 : it->second; }
    float BudgetTaxRate(int index) override { return index >= 0 && static_cast<std::size_t>(index) < taxRates.size() ? taxRates[index] : 0.0f; }
    float BudgetLineItemFunding(std::uint32_t id) override { auto it = lineItems.find(id); return it == lineItems.end() ? 0.0f : it->second; }
    std::int64_t NeighborsInt(std::uint32_t vslot) override { auto it = neighborsInts.find(vslot); return it == neighborsInts.end() ? 0 : it->second; }
    std::int64_t ResidentialInt(std::uint32_t vslot) override { auto it = residentialInts.find(vslot); return it == residentialInts.end() ? 0 : it->second; }
    float ResidentialFloat(std::uint32_t vslot) override { auto it = residentialFloats.find(vslot); return it == residentialFloats.end() ? 0.0f : it->second; }
    void ResidentialTracts(std::uint32_t, float& best, int& bx, int& bz, float& worst, int& wx, int& wz) override {
        best = tractBest; bx = tractBestX; bz = tractBestZ; worst = tractWorst; wx = tractWorstX; wz = tractWorstZ;
    }
    bool MessageVoid1IsPropertyHolder(cIGZMessage2*) override { return void1IsHolder; }
    bool MessageVoid1HasProperty(cIGZMessage2*, std::uint32_t propertyId) override {
        for (std::uint32_t p : holderProperties) if (p == propertyId) return true;
        return false;
    }
    bool BuildingDevelopmentLookup(cIGZMessage2*, std::uint32_t* typeOut) override {
        if (!developmentLookup) return false;
        if (typeOut) *typeOut = developmentType; return true;
    }
    bool CivicBuildingAllowsLinks(std::uint32_t) override { return civicAllowsLinks; }
    void UpdateSimulatorStatistics(std::uint32_t advisorClassId) override { statisticsUpdates.push_back(advisorClassId); }
};

ValuePtr Row(std::map<std::string, ValuePtr> fields) {
    ValuePtr t = Tab();
    t->table->byName = std::move(fields);
    return t;
}
std::uint32_t AddedCount(FakeMessages& messages, std::uint32_t type) {
    std::uint32_t n = 0;
    for (auto& a : messages.added) if (a.second == type) ++n;
    return n;
}

// A base subclass that counts update_variables calls.
struct CountingAdvisor final : cSC4AdvisorBase {
    int updates = 0;
    void update_variables() override { ++updates; }
};

}  // namespace

int main() {
    SlHost slHost;
    SetSLStringHost(&slHost);
    SetRZServiceResolver(&Resolve);

    FakeMessages messages;
    Host host;
    host.messages = &messages;
    SetAdvisorHost(&host);

    ConcreteHost concrete;
    concrete.messages = &messages;
    SetAdvisorConcreteHost(&concrete);

    // One "advisors" row; no "advices" table so the advice list stays empty.
    ValuePtr advisors = Tab();
    advisors->table->length = 1;
    advisors->table->byIndex[1] = Row({{"id", Num(21)}, {"caption", Str("")}, {"name", Str("")}, {"advice_type", Num(4)}});
    host.lua.globals["advisors"] = advisors;

    // ---- layout and class ids ---------------------------------------------------------------------------
    CHECK(sizeof(cSC4AdvisorBase) == 0x148);
    CHECK(sizeof(cSC4AdvisorHQ) == 0x150);
    CHECK(sizeof(cSC4AdvisorFinance) == 0x150);
    CHECK(sizeof(cSC4AdvisorUtility) == 0x150);
    CHECK(sizeof(cSC4AdvisorSafety) == 0x150);
    CHECK(sizeof(cSC4AdvisorTraffic) == 0x168);
    CHECK(sizeof(cSC4AdvisorCityPlanning) == 0x170);
    {
        cSC4AdvisorBase* b = new cSC4AdvisorBase();
        const auto base = reinterpret_cast<const char*>(b);
        CHECK(reinterpret_cast<const char*>(static_cast<cISC4Advisor*>(b)) - base == 0x10);
        CHECK(reinterpret_cast<const char*>(static_cast<cIGZSerializable*>(b)) - base == 0x18);
        CHECK(reinterpret_cast<const char*>(static_cast<cIGZMessageTarget2*>(b)) - base == 0x20);
        CHECK(b->UpdatePending() && !b->SubscribedToMessages() && b->DirtyMask() == 0);
        delete b;
    }
    {
        cSC4AdvisorHQ a; CHECK(a.GetGZCLSID() == 0x6a5f8755);
        cSC4AdvisorFinance f; CHECK(f.GetGZCLSID() == 0xaa5292d7);
        cSC4AdvisorUtility u; CHECK(u.GetGZCLSID() == 0xca2c2a2f);
        cSC4AdvisorSafety s; CHECK(s.GetGZCLSID() == 0xea19e825);
        cSC4AdvisorTraffic t; CHECK(t.GetGZCLSID() == 0x4a3ad3e1);
        cSC4AdvisorCityPlanning c; CHECK(c.GetGZCLSID() == 0x2a5f877d);
    }

    // ---- the base: Init/Shutdown subscription, post_self_update, pending-update simulation --------------
    {
        cSC4AdvisorBase* a = new cSC4AdvisorBase();
        a->AddRef();
        CHECK(a->Init(1));
        CHECK(a->GetID() == 21 && a->UpdatePending() && a->SubscribedToMessages());
        CHECK(AddedCount(messages, 0x99ef1142) == 1 && AddedCount(messages, 0x99ef1143) == 1);
        CHECK(AddedCount(messages, 0x26d31ec1) == 1 && AddedCount(messages, 0xea8ae29a) == 1 && AddedCount(messages, 0xc2c09554) == 1);
        CHECK(a->Shutdown() && !a->SubscribedToMessages());
        CHECK(messages.removed.size() == 9);   // 4 advisor messages + 5 base messages
        CHECK(a->Shutdown() && messages.removed.size() == 9);   // idempotent
        a->Release();
    }
    {
        // post_self_update posts a 0xca8f3db6 message straight back to the advisor.
        cSC4AdvisorBase* a = new cSC4AdvisorBase();
        a->AddRef();
        const std::size_t before = messages.sent.size();
        a->post_self_update(1, 0x5151);
        CHECK(messages.sent.size() == before + 1);
        CHECK(messages.sent.back().type == 0xca8f3db6 && messages.sent.back().data1 == 1 && messages.sent.back().data2 == 0x5151);
        a->Release();
    }
    {
        // simulate_day runs update_variables once when the update is pending; simulate_month runs it every time.
        CountingAdvisor a;
        a.simulate_day();
        CHECK(a.updates == 1 && !a.UpdatePending());
        a.simulate_day();
        CHECK(a.updates == 1);   // no longer pending
        a.simulate_month();
        CHECK(a.updates == 2);
        a.simulate_month();
        CHECK(a.updates == 3);
    }
    {
        // DoMessage: the city-started message runs update_variables; 0xc2c09554 posts a deferred self-update.
        CountingAdvisor a;
        cRZMessage2Standard started;
        started.SetType(0x26d31ec1);
        CHECK(a.DoMessage(&started) && a.updates == 1);
        cRZMessage2Standard post2;
        post2.SetType(0xc2c09554);
        CHECK(a.DoMessage(&post2));
        CHECK(messages.sent.back().type == 0xca8f3db6 && messages.sent.back().data1 == 2 && messages.sent.back().data2 == 0);
        // the reward-advice and self-update data1==1/2 walks are no-ops with an empty advice list.
        cRZMessage2Standard reward;
        reward.SetType(0xea8ae29a);
        CHECK(a.DoMessage(&reward));
        cRZMessage2Standard self1;
        self1.SetType(0xca8f3db6);
        self1.SetData1(1);
        self1.SetData2(0x1234);
        CHECK(a.DoMessage(&self1));
        cRZMessage2Standard self2;
        self2.SetType(0xca8f3db6);
        self2.SetData1(2);
        CHECK(a.DoMessage(&self2));
    }

    // ---- Finance: DoMessage deferred flag and the budget record -----------------------------------------
    {
        cSC4AdvisorFinance* a = new cSC4AdvisorFinance();
        a->AddRef();
        concrete.budgetInts[0x30] = 1000000; concrete.budgetInts[0x80] = 50000; concrete.budgetInts[0x70] = 30000;
        concrete.budgetInts[0x90] = 4000; concrete.budgetInts[0x148] = 500000; concrete.budgetInts[0x150] = 1000000;
        concrete.budgetInts[0x168] = 1200;
        concrete.taxRates = {0.09f, 0.10f, 0.11f, 0.07f, 0.08f, 0.09f, 0.06f, 0.07f, 0.05f, 0.08f, 0.10f, 0.12f};
        concrete.neighborsInts[0x100] = 2500; concrete.neighborsInts[0x108] = 700;
        a->update_variables();
        CHECK(concrete.globals["g_funds"] == 1000000.0 && concrete.globals["g_income_monthly"] == 50000.0);
        CHECK(concrete.globals["g_expense_monthly"] == 30000.0);
        CHECK(concrete.globals["g_tax_rate_r_low"] == static_cast<double>(0.09f));
        CHECK(concrete.globals["g_tax_rate_i_hightech"] == static_cast<double>(0.12f));
        CHECK(concrete.globals["g_tax_income"] == 4000.0);
        CHECK(concrete.globals["g_borrowed"] == 500000.0 && concrete.globals["g_borrowing_limit"] == 1000000.0);
        CHECK(concrete.globals["g_bond_payments_monthly"] == 1200.0);
        CHECK(concrete.globals["g_nd_income"] == 2500.0 && concrete.globals["g_nd_expense"] == 700.0);
        // DoMessage: a building change sets the deferred flag and posts a self-update.
        concrete.void1IsHolder = true;
        const std::size_t before = messages.sent.size();
        cRZMessage2Standard built;
        built.SetType(0x99ef1142);
        CHECK(a->DoMessage(&built) && a->DirtyMask() == 1);
        CHECK(messages.sent.size() == before + 1 && messages.sent.back().type == 0xca8f3db6 && messages.sent.back().data1 == 0);
        // the deferred self-update clears the flag.
        cRZMessage2Standard update;
        update.SetType(0xca8f3db6);
        CHECK(a->DoMessage(&update) && a->DirtyMask() == 0);
        a->Release();
    }

    // ---- HQ: DoMessage bit 1 runs update_occupant_variables --------------------------------------------
    {
        cSC4AdvisorHQ* a = new cSC4AdvisorHQ();
        a->AddRef();
        CHECK(a->Init(1) && a->GroupVectorsInitialized());
        concrete.void1IsHolder = true;
        cRZMessage2Standard built;
        built.SetType(0x99ef1142);
        CHECK(a->DoMessage(&built) && a->DirtyMask() == 1);
        concrete.statisticsUpdates.clear();
        cRZMessage2Standard update;
        update.SetType(0xca8f3db6);
        CHECK(a->DoMessage(&update) && a->DirtyMask() == 0);
        CHECK(concrete.statisticsUpdates.size() == 1 && concrete.statisticsUpdates[0] == 0x6a5f8755);
        CHECK(a->Shutdown() && !a->GroupVectorsInitialized());
        // the health/education record sets the funding globals and the tract locals.
        concrete.lineItems[0x9188f42] = 0.42f;
        concrete.lineItems[0x9188f4c] = 0.55f;
        concrete.tractBest = 0.8f; concrete.tractBestX = 10; concrete.tractBestZ = 11;
        concrete.tractWorst = 0.2f; concrete.tractWorstX = 12; concrete.tractWorstZ = 13;
        a->update_variables();
        CHECK(concrete.globals["g_health_funding_p"] > 41.9 && concrete.globals["g_health_funding_p"] < 42.1);
        CHECK(concrete.globals["g_education_funding_p"] > 54.9 && concrete.globals["g_education_funding_p"] < 55.1);
        CHECK(concrete.globals["g_num_treated_patients"] == 0.0 && concrete.globals["g_health_capacity"] == 0.0);
        a->Release();
    }

    // ---- Utility: power-strike messages and the deferred flags ------------------------------------------
    {
        cSC4AdvisorUtility* a = new cSC4AdvisorUtility();
        a->AddRef();
        CHECK(a->Init(1) && a->SubscribedToMessages());
        CHECK(AddedCount(messages, 0xea418d8f) == 1 && AddedCount(messages, 0xca358f85) == 1 && AddedCount(messages, 0xca358f9c) == 1);
        cRZMessage2Standard strikeOn;
        strikeOn.SetType(0xca358f85);
        CHECK(a->DoMessage(&strikeOn));
        CHECK(concrete.globals["g_power_strike"] == 1.0);
        CHECK(concrete.pushedEvents.size() == 1 && concrete.pushedEvents.back().first == static_cast<int>(0xea31a30a));
        cRZMessage2Standard strikeOff;
        strikeOff.SetType(0xca358f9c);
        CHECK(a->DoMessage(&strikeOff));
        CHECK(concrete.globals["g_power_strike"] == 0.0);
        CHECK(concrete.pushedEvents.back().first == static_cast<int>(0x2a31a30f));
        // the building message (data word 3) pushes a location event with the message's unknown as subject.
        cRZMessage2Standard building;
        building.SetType(0xea418d8f);
        building.SetData1(3);
        const std::size_t events = concrete.pushedEvents.size();
        CHECK(a->DoMessage(&building) && a->DirtyMask() == 4);
        CHECK(concrete.pushedEvents.size() == events + 1);
        CHECK(concrete.pushedEvents.back().first == static_cast<int>(0x3a383059));
        CHECK(a->Shutdown() && !a->SubscribedToMessages());
        a->Release();
    }

    // ---- Safety: sixteen subscriptions and the deferred occupant update ---------------------------------
    {
        cSC4AdvisorSafety* a = new cSC4AdvisorSafety();
        a->AddRef();
        CHECK(a->Init(1) && a->SubscribedToMessages());
        CHECK(AddedCount(messages, 0xea1f0dcd) == 1 && AddedCount(messages, 0xaa552f77) == 1);
        CHECK(AddedCount(messages, 0x69247dc4) == 1 && AddedCount(messages, 0x69247dc5) == 1);
        concrete.void1IsHolder = true;
        cRZMessage2Standard built;
        built.SetType(0x99ef1142);
        CHECK(a->DoMessage(&built) && a->DirtyMask() == 1);
        concrete.statisticsUpdates.clear();
        cRZMessage2Standard update;
        update.SetType(0xca8f3db6);
        CHECK(a->DoMessage(&update) && a->DirtyMask() == 0);
        CHECK(concrete.statisticsUpdates.size() == 1 && concrete.statisticsUpdates[0] == 0xea19e825);
        CHECK(a->Shutdown() && !a->SubscribedToMessages());
        a->Release();
    }

    // ---- Traffic: car-zot set and the 0x4c04de69 subscription -------------------------------------------
    {
        cSC4AdvisorTraffic* a = new cSC4AdvisorTraffic();
        a->AddRef();
        CHECK(a->Init(1) && a->SubscribedToMessages());
        CHECK(AddedCount(messages, 0x4c04de69) == 1);
        cRZMessage2Standard add;
        add.SetType(0x4c04de69);
        add.SetData1(1);
        CHECK(a->DoMessage(&add) && a->CarZotCount() == 1);
        CHECK(concrete.globals["g_car_zot_count"] == 1.0);
        cRZMessage2Standard remove;
        remove.SetType(0x4c04de69);
        remove.SetData1(2);
        CHECK(a->DoMessage(&remove) && a->CarZotCount() == 0);
        CHECK(concrete.globals["g_car_zot_count"] == 0.0);
        CHECK(a->Shutdown() && !a->SubscribedToMessages());
        a->Release();
    }

    // ---- CityPlanning: QueryInterface, landmarks, subscriptions -----------------------------------------
    {
        cSC4AdvisorCityPlanning* a = new cSC4AdvisorCityPlanning();
        a->AddRef();
        // QueryInterface answers its own class id as the primary interface id.
        void* iface = nullptr;
        CHECK(a->QueryInterface(0x2a5f877d, &iface) && iface == a);
        a->Release();   // the QueryInterface reference
        a->AddRef();
        CHECK(a->Init(1) && a->SubscribedToMessages());
        CHECK(AddedCount(messages, 0xea394b74) == 1 && AddedCount(messages, 0x4a490d33) == 1);
        CHECK(a->BuildingCount() == 0 && a->ParkCount() == 0 && a->RecreationCount() == 0);
        CHECK(a->GetLandmarkCount(0x55) == 0 && !a->HasLandmark(0x55));
        // a plop (0x99ef1142) with park + landmark properties.
        concrete.void1IsHolder = true;
        concrete.holderProperties = {0x1006, 0x150a};
        concrete.developmentLookup = true;
        concrete.developmentType = 0x55;
        cRZMessage2Standard plop;
        plop.SetType(0x99ef1142);
        CHECK(a->DoMessage(&plop));
        CHECK(a->BuildingCount() == 1 && a->ParkCount() == 1 && a->RecreationCount() == 1);
        CHECK(a->GetLandmarkCount(0x55) == 1 && a->HasLandmark(0x55));
        // a bulldoze (0x99ef1143) decrements (the landmark entry is never erased).
        cRZMessage2Standard bulldoze;
        bulldoze.SetType(0x99ef1143);
        CHECK(a->DoMessage(&bulldoze));
        CHECK(a->BuildingCount() == 0 && a->ParkCount() == 0 && a->RecreationCount() == 0);
        CHECK(a->GetLandmarkCount(0x55) == 0 && a->HasLandmark(0x55));   // still present
        a->Shutdown();
        a->Release();
        a->Release();   // the construction reference (the pair above covered the QueryInterface and the re-acquire)
    }
    // CityPlanning Write/Read (fresh object, built through DoMessage).
    {
        cSC4AdvisorCityPlanning* a = new cSC4AdvisorCityPlanning();
        a->AddRef();
        a->Init(1);
        concrete.void1IsHolder = true;
        concrete.holderProperties = {0x1006, 0x150a};
        concrete.developmentLookup = true;
        concrete.developmentType = 0x55;
        cRZMessage2Standard plop;
        plop.SetType(0x99ef1142);
        a->DoMessage(&plop);
        CHECK(a->GetLandmarkCount(0x55) == 1);
        PortMemoryOStream os;
        CHECK(a->Write(os));
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4AdvisorCityPlanning r;
        CHECK(r.Read(is));
        CHECK(r.BuildingCount() == 1 && r.ParkCount() == 1 && r.RecreationCount() == 1);
        CHECK(r.GetLandmarkCount(0x55) == 1 && r.HasLandmark(0x55));
        a->Shutdown();
        a->Release();
    }

    for (FakeSubject* s : concrete.made) { CHECK(s->refs == 0); delete s; }

    std::printf(failures ? "FAILED (%d)\n" : "sc4_advisorconcrete OK\n", failures);
    return failures ? 1 : 0;
}
