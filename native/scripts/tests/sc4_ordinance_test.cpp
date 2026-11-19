#include <cstdio>
#include <vector>
#include "sc4/Exemplar.h"
#include "sc4/Ordinance.h"
#include "sc4/SCPropertyHelp.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Host final : public IOrdinanceHost {
public:
    int year = 2000, base = 1000;
    std::vector<std::uint32_t> msgs;
    int enumerated = 0;
    std::vector<cGZPersistResourceKey> keys{cGZPersistResourceKey{}, cGZPersistResourceKey{}};   // two keys that load nothing
    cSCResExemplar* ordinanceExemplar = nullptr;   // answered for any key of group 0x4a5e8ef6 (exemplar type 14)
    std::vector<std::uint32_t> created;            // the class ids asked for
    int agents = 0;
    void ForEachOrdinanceKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override { ++enumerated; for (const auto& key : keys) fn(key); }
    cISCResExemplar* LoadExemplar(const cGZPersistResourceKey& key) override {
        if (key.group != 0x4a5e8ef6 || !ordinanceExemplar) return nullptr;
        ordinanceExemplar->AddRef();
        return ordinanceExemplar;
    }
    // the classes a GZCOM directory would hold: only 0x62bf1db9 (as a standard ordinance) exists
    cSC4OrdinanceSimple* CreateOrdinance(std::uint32_t classId) override {
        created.push_back(classId);
        if (classId != 0x62bf1db9) return nullptr;
        auto* o = new cSC4OrdinanceSimple();
        o->AddRef();
        return o;
    }
    void AddSimulatorAgent(cSC4OrdinanceSimulator*) override { ++agents; }
    void RemoveSimulatorAgent(cSC4OrdinanceSimulator*) override { --agents; }
    int CurrentYear() override { return year; }
    int ResidentialIncomeBase() override { return base; }
    std::string LoadString(const cGZPersistResourceKey&) override { return ""; }
    void PostMessage(std::uint32_t t, std::uint32_t, std::uint32_t) override { msgs.push_back(t); }
};
cSC4OrdinanceSimple* Make(std::uint32_t id, const char* name, std::int64_t e, std::int64_t r, std::int64_t c, float f, std::uint32_t y) {
    auto* o = new cSC4OrdinanceSimple();
    o->AddRef(); o->ForceID(id); o->ForceName(name); o->ForceIncomes(e, r, c, f, y); o->SetEnabled(true);
    return o;
}
}  // namespace
int main() {
    Host host;
    SetOrdinanceHost(&host);
    auto* sim = new cSC4OrdinanceSimulator();
    sim->AddRef();
    CHECK(sim->GetGZCLSID() == 0xa990c08e && sim->GetSimulatorType() == 0x41193c3a);
    auto* a = Make(1, "Tax", 100, 30, 50, 0.5f, 1990);
    auto* b = Make(2, "Future", 0, 0, 0, 0.0f, 2100);
    sim->AddOrdinance(a);
    sim->AddOrdinance(b);
    CHECK(sim->GetOrdinanceByID(1) == a && sim->GetOrdinanceByName("tAX") == a && sim->GetOrdinanceIDByName("Future") == 2);
    CHECK(a->IsAvailable() && !b->IsAvailable());                 // year gating on add
    CHECK(sim->GetOrdinanceAvailableCount() == 1 && sim->GetOrdinanceOnCount() == 0);
    CHECK(sim->IsOrdinanceAvailableButOff(1));
    CHECK(sim->SetOrdinanceOn(1, true) && a->IsOn());
    CHECK(a->GetMonthlyAdjustedIncome() == 100);                  // enactment income added
    a->Simulate();
    CHECK(a->GetMonthlyAdjustedIncome() == 1000 * 0.5f + 50);
    sim->DoMonthlySimulation();
    CHECK(sim->GetOrdinanceMonthlyIncome() == 100);                // pending enactment settled; not an income ordinance
    a->ForceIncomeOrdinance(true);
    sim->DoMonthlySimulation();
    CHECK(sim->GetOrdinanceMonthlyIncome() == a->GetMonthlyAdjustedIncome());
    host.year = 2200;
    sim->DoMonthlySimulation();
    CHECK(b->IsAvailable());
    std::uint32_t ids[4]; std::uint32_t n = 4;
    CHECK(sim->GetOrdinanceAvailableArray(ids, n) == 2 && n == 2);
    CHECK(sim->SetOrdinanceAvailable(1, false) && !a->IsOn());     // unavailable forces off
    CHECK(sim->RemoveOrdinance(b) && !sim->GetOrdinanceByID(2));
    CHECK(sim->DoMessage(0x66956810));
    sim->LoadAllStandardOrdinances();                      // walks the host's ordinance exemplars (here two keys that load nothing)
    CHECK(host.enumerated == 1);
    a->Release(); b->Release(); sim->Release();

    // ordinance.md 8.1: a city that was not loaded gets one ordinance per ordinance exemplar whose INSTANCE has a class (an
    // instance without a class makes none), not yet available; Init registers the month agent and Shutdown removes it
    {
        cSCResExemplar exemplar;
        exemplar.AddRef();
        cSCPropertyHelp::SetPropertyValue(exemplar.AsISCPropertyHolder(), 0x10, static_cast<std::uint32_t>(14));
        host.ordinanceExemplar = &exemplar;
        host.keys = {cGZPersistResourceKey{0x6534284a, 0x4a5e8ef6, 0x62bf1db9}, cGZPersistResourceKey{0x6534284a, 0x4a5e8ef6, 0x12345678}};
        host.enumerated = 0;
        auto* fresh = new cSC4OrdinanceSimulator();
        fresh->AddRef();
        CHECK(fresh->Init());
        CHECK(host.agents == 1 && host.enumerated == 1);
        CHECK(host.created.size() == 2 && host.created[0] == 0x62bf1db9 && host.created[1] == 0x12345678);
        std::uint32_t freshIds[4]; std::uint32_t freshCount = 4;
        CHECK(fresh->GetOrdinanceIDArray(freshIds, freshCount) == 1 && freshCount == 1);
        CHECK(fresh->GetOrdinanceAvailableCount() == 0);
        CHECK(fresh->Shutdown() && host.agents == 0);
        fresh->Release();
        host.ordinanceExemplar = nullptr;
    }
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}
