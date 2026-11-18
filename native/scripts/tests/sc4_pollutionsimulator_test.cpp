#include <cmath>
#include <cstdio>

#include "sc4/PollutionSimulator.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class City final : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 64; }
    std::int32_t CitySizeZInCells() const override { return 64; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};
class Occupant final : public IPollutionOccupant {
public:
    cSCPropertyHolder holder;
    bool powered = true;
    cISCPropertyHolder* PropertyHolder() override { return &holder; }
    void Position(float p[3]) override { p[0] = p[1] = p[2] = 0; }
};
class Host final : public IPollutionHost {
public:
    bool power = true, poweredOccupants = true;
    float funding = 0.5f, slots = 2.0f;
    int low = 0, high = 100, load = 5;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> messages;
    std::vector<std::pair<std::uint32_t, std::int64_t>> history;
    bool PowerAvailable() override { return power; }
    bool OccupantPowered(IPollutionOccupant*) override { return poweredOccupants; }
    float SanitationFunding() override { return funding; }
    float RecyclingCapacitySlots() override { return slots; }
    void LandfillRange(int& l, int& h) override { l = low; h = high; }
    int LandfillTransportLoad() override { return load; }
    void PostMessage(std::uint32_t m, std::uint32_t a) override { messages.push_back({m, a}); }
    void AddHistory(std::uint32_t id, std::int64_t v) override { history.push_back({id, v}); }
    void AddAgent(cIGZMessageTarget2*, int, const char*, std::uint32_t) override {}
    void RemoveAgent(cIGZMessageTarget2*) override {}
    void AddNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    const cISCPropertyHolder* exemplar = nullptr;
    std::vector<cISC4PropRequester*> requesters;
    const cISCPropertyHolder* SimulatorExemplar() override { return exemplar; }
    void AddPropRequester(cISC4PropRequester* requester) override { requesters.push_back(requester); }
    void CreateHistory(std::uint32_t, std::uint32_t) override {}
    float OrdinanceFactor(std::uint32_t, std::uint32_t) override { return 0.0f; }
    int ZoneTypeAt(int, int) override { return 0; }
    std::uint8_t TrafficAirAt(int, int) override { return 0; }
    void GarbageDeals(std::uint32_t, std::vector<IPollutionDeal*>&) override {}
    void SetSanitationLineItemCost(int, std::int64_t) override {}
    bool OccupantFootprint(void*, PollutionOccupantFootprint&) override { return false; }
};
}  // namespace

int main() {
    City city;
    SetSimGridCity(&city);
    Host host;
    cSC4PollutionSimulator sim(&host);
    sim.CreateGrids();
    sim.ReadSimulatorTunableValues(nullptr);
    CHECK(sim.Tunables().airThreshold == 16000 && sim.Tunables().landfillTileCapacity == 0x7fff);
    CHECK(sim.GetGZCLSID() == 0x8990c065 && sim.GetRequesterSatisfactionId() == 0xe9a4300b);

    // grids
    sim.SetAirValue(10, 12, 20000);
    int v = 0;
    sim.GetAirValue(10, 12, v);
    CHECK(v == 20000 && sim.IsAirPolluted(10, 12) && !sim.IsAirPolluted(40, 40));
    sim.SetWaterValue(4, 4, 100);
    CHECK(!sim.IsWaterPolluted(4, 4));
    sim.SetGarbageValue(6, 6, 17000);
    CHECK(sim.IsGarbageAccumulated(6, 6));
    sim.SetIsActiveLandfill(6, 6, true, 0xe);                      // quirk: writes tract (6,6) directly
    CHECK(sim.activeLandfillCount_ == 1);
    sim.SetIsActiveLandfill(6, 6, true, 0x3);                      // not a landfill zone: ignored
    CHECK(sim.activeLandfillCount_ == 1);
    sim.SetIsRadioactive(2, 2, true);
    CHECK(sim.IsRadioactive(2, 2) && !sim.IsRadioactive(30, 30));
    CHECK(sim.dirtyAir && sim.dirtyRadiation);

    // garbage supply never goes negative
    sim.AddToGarbageSupply(50);
    sim.AddToGarbageSupply(-80);
    CHECK(sim.GetGarbageSupply() == 0);
    sim.AddToGarbageSupply(30);
    CHECK(sim.GetGarbageSupply() == 30);

    // occupant helpers
    Occupant incinerator;
    cSCPropertyHelp::SetPropertyValue(&incinerator.holder, kPollutionOccIncinerator, std::uint32_t{1000});
    cSCPropertyHelp::SetPropertyValue(&incinerator.holder, kPollutionOccIncineratorUsed, std::uint32_t{250});
    sim.SetEfficiency(0.5f);
    std::uint32_t out = 0;
    sim.GetIncineratorCurrentCapacity(&incinerator, out);
    CHECK(out == 500);
    sim.GetIncineratorUsedCapacity(&incinerator, out);
    CHECK(out == 250);
    CHECK(sim.GetUnusedCapacity(kPollutionOccIncinerator, kPollutionOccIncineratorUsed, &incinerator, out) && out == 250);
    host.poweredOccupants = false;
    sim.GetIncineratorCurrentCapacity(&incinerator, out);
    CHECK(out == 0);
    host.poweredOccupants = true;
    // age curve scales current capacity
    cSCPropertyHelp::SetPropertyValue(&incinerator.holder, kPollutionOccAgeCurve, std::vector<float>{0, 1.0f, 10, 0.0f});
    cSCPropertyHelp::SetPropertyValue(&incinerator.holder, kPollutionOccAge, 5.0f);
    sim.GetIncineratorCurrentCapacity(&incinerator, out);
    CHECK(out == 250);
    float cond = 0;
    CHECK(sim.GetCurrentOccupantCondition(&incinerator, cond) && std::fabs(cond - 0.5f) < 1e-6f);
    float eff = 0;
    CHECK(sim.GetCurrentOccupantEfficiency(&incinerator, eff) && std::fabs(eff - 0.25f) < 1e-6f);
    host.power = false;
    CHECK(!sim.GetCurrentOccupantEfficiency(&incinerator, eff) && eff == 0.0f);
    host.power = true;
    CHECK(std::list<IPollutionOccupant*>().empty());
    CHECK(sim.AddOrRemoveOccupant(sim.IncineratorList(), &incinerator, true) && sim.IncineratorList().size() == 1);
    CHECK(sim.AddOrRemoveOccupant(sim.IncineratorList(), &incinerator, false) && sim.IncineratorList().empty());
    CHECK(!sim.AddOrRemoveOccupant(sim.IncineratorList(), &incinerator, false));

    // eatGarbage priority chain
    sim.SetEfficiency(1.0f);
    sim.recyclingAllowance_ = 10;
    std::uint32_t wte = 30, incin = 20;
    float eaten = sim.eatGarbage(8.0f, wte, incin);
    CHECK(eaten == 8.0f && sim.recycledThisMonth_ == 8 && wte == 30 && incin == 20);
    eaten = sim.eatGarbage(10.0f, wte, incin);                    // 2 recycled, 8 incinerated
    CHECK(eaten == 10.0f && sim.recycledThisMonth_ == 10 && incin == 12 && sim.incineratedThisMonth_ == 8);
    eaten = sim.eatGarbage(100.0f, wte, incin);                   // 12>=? budget 12 <= 100: emptied, quirk
    CHECK(incin == 0 && sim.incineratedThisMonth_ == 8);
    CHECK(wte == 0);                                               // 30 <= 100: emptied by the same quirk
    // remaining goes to landfill transport (curve empty -> 0), then abduction
    CHECK(eaten == 100.0f);                                        // whole request goes to abduction (allowance 5000)
    CHECK(sim.abductedThisMonth_ == 100);
    CHECK(sim.eatGarbage(0.0f, wte, incin) == 0.0f);

    // recycling effect
    Occupant r1, r2;
    cSCPropertyHelp::SetPropertyValue(&r1.holder, kPollutionOccRecyclingFactor, 1.0f);
    cSCPropertyHelp::SetPropertyValue(&r2.holder, kPollutionOccRecyclingFactor, 1.0f);
    cSCPropertyHelp::SetPropertyValue(&r2.holder, kPollutionOccAgeCurve, std::vector<float>{0, 0.5f, 10, 0.5f});
    sim.RecyclingList().push_back(&r2);
    sim.RecyclingList().push_back(&r1);
    sim.recyclingCenterCountSnapshot = 2;
    sim.SetEfficiency(1.0f);
    sim.CalculateRecyclingCenterEffect();
    CHECK(sim.RecyclingList().size() == 1 && sim.RecyclingList().front() == &r2);  // r1 has no efficiency, dropped
    // r1 has no age curve -> condition fails -> efficiency 0; r2 0.5: r2 sorts first
    // end of cycle
    sim.airSum = 400; sim.waterSum = 200; sim.cellCount = 4; sim.radiationCount = 1;
    sim.SetTotalGarbageUncollected(40);
    sim.FundingEfficiencyCurve().SetResponseCurveData(std::vector<float>{0, 0, 100, 100}.data(), 4);
    host.messages.clear();
    host.history.clear();
    sim.EndOfCycle();
    CHECK(sim.GetAverageAirValue() == 100 && sim.GetAverageWaterValue() == 50 && sim.GetAverageGarbageValue() == 10);
    CHECK(sim.IsGarbageUncollected());
    CHECK(host.history.size() == 5 && host.history[4].second == 25 + 100);
    CHECK(std::fabs(sim.GetEfficiency() - 0.5f) < 1e-5f);
    CHECK(!sim.dirtyAir && host.messages.size() >= 3);
    {   // small accessors
        sim.worstAir = {3, 4, 500}; sim.worstWater = {5, 6, -7}; sim.worstGarbage = {65535, 1, 9};
        int x = 0, z = 0, value = 0;
        sim.GetWorstAirTract(x, z, value);       CHECK(x == 3 && z == 4 && value == 500);
        sim.GetWorstWaterTract(x, z, value);     CHECK(x == 5 && z == 6 && value == -7);
        sim.GetWorstGarbageTract(x, z, value);   CHECK(x == 65535 && z == 1 && value == 9);
        CHECK(sim.GetLandfillTileCapacity() == 0x7fff);
        sim.IncineratorList().clear(); sim.WasteToEnergyList().clear(); sim.RecyclingList().clear();
        IPollutionOccupant* a = &incinerator;
        sim.IncineratorList().push_back(a);
        sim.WasteToEnergyList().push_back(a);
        sim.RecyclingList().push_back(a);
        sim.RecyclingList().push_back(a);
        std::list<IPollutionOccupant*> out;
        sim.GetGarbageBuildings(out);
        CHECK(out.size() == 4);
        sim.IncineratorList().clear(); sim.WasteToEnergyList().clear(); sim.RecyclingList().clear();
    }
    {   // occupant passes
        struct Lot final : IPollutionLot { std::uint32_t state = 0; void SetState(std::uint32_t v) override { state = v; } };
        struct Department final : IPollutionDepartment { std::int64_t added = 0; void AddCost(std::int64_t d) override { added += d; } };
        struct Services final : IPollutionServices {
            Lot lot; Department department; std::int64_t booked = 100; std::int64_t setCost = -1; std::int32_t setId = -1; bool found = true;
            IPollutionLot* LotAtPosition(const float*) override { return &lot; }
            bool FindBudgetLineItem(cISCPropertyHolder*, std::uint32_t id, PollutionBudgetRecord& r) override {
                if (id != 0xca58edc7 || !found) return false;
                r.cost = booked; r.id = 77; return true;
            }
            IPollutionDepartment* DepartmentFor(const PollutionBudgetRecord&) override { return &department; }
            void SetBudgetLineItem(cISCPropertyHolder*, std::int32_t id, std::int64_t c) override { setId = id; setCost = c; }
        } services;
        sim.SetServices(&services);
        sim.SetEfficiency(1.0f);

        // used capacity is spread over the list; an occupant with little room passes the rest on
        Occupant a, b;
        for (Occupant* o : {&a, &b}) {
            cSCPropertyHelp::SetPropertyValue(&o->holder, kPollutionOccIncinerator, std::uint32_t{100});
            cSCPropertyHelp::SetPropertyValue(&o->holder, kPollutionOccIncineratorUsed, std::uint32_t{0});
        }
        cSCPropertyHelp::SetPropertyValue(&a.holder, kPollutionOccIncineratorUsed, std::uint32_t{90});     // 10 free
        std::list<IPollutionOccupant*> list{&a, &b};
        std::uint32_t remaining = 60;
        sim.SetOccupantListUsedCapacity(list, kPollutionOccIncinerator, kPollutionOccIncineratorUsed, 1000, remaining, 2);
        std::uint32_t usedA = 0, usedB = 0;
        cSCPropertyHelp::GetPropertyValue(&a.holder, kPollutionOccIncineratorUsed, usedA);
        cSCPropertyHelp::GetPropertyValue(&b.holder, kPollutionOccIncineratorUsed, usedB);
        // a takes its 10 free; the re-spread uses the remaining count *before* a's share is subtracted (60, not 50), so b takes 60
        // and the unsigned remainder wraps: a quirk of the original that is kept
        CHECK(usedA == 100 && usedB == 60 && remaining == 0xfffffff6u);
        std::uint32_t none = 0;
        sim.SetOccupantListUsedCapacity(list, kPollutionOccIncinerator, kPollutionOccIncineratorUsed, 0, none, 2);
        CHECK(none == 0);

        // maintenance: only a changed cost reaches the department
        cSCPropertyHelp::SetPropertyValue(&a.holder, kPollutionOccMaintenanceBase, std::int64_t{250});
        cSCPropertyHelp::SetPropertyValue(&a.holder, kPollutionOccAge, 0.0f);
        cSCPropertyHelp::SetPropertyValue(&a.holder, kPollutionOccMaintenanceCurve, std::vector<float>{0, 1, 100, 1});
        std::list<IPollutionOccupant*> one{&a};
        sim.CalcMaintenanceCosts(one);
        CHECK(services.department.added == 150 && services.setId == 77 && services.setCost == 250);
        services.booked = 250; services.department.added = 0; services.setCost = -1;
        sim.CalcMaintenanceCosts(one);
        CHECK(services.department.added == 0 && services.setCost == -1);

        // ageing: wear x funding curve x load curve moves the age, thresholds change the lot state
        Occupant old;
        cSCPropertyHelp::SetPropertyValue(&old.holder, kPollutionOccAge, 10.0f);
        cSCPropertyHelp::SetPropertyValue(&old.holder, 0xea1cf220, 5.0f);
        cSCPropertyHelp::SetPropertyValue(&old.holder, 0xea1cf221, 12.0f);
        cSCPropertyHelp::SetPropertyValue(&old.holder, 0xea1cf222, 40.0f);
        sim.AgeCurve().SetResponseCurveData(std::vector<float>{0, 1, 100, 1}.data(), 4);
        sim.EfficiencyCurve().SetResponseCurveData(std::vector<float>{0, 1, 100, 1}.data(), 4);
        std::list<IPollutionOccupant*> aging{&old};
        sim.AgeOccupantList(aging, 0, 0);
        float age = 0;
        cSCPropertyHelp::GetPropertyValue(&old.holder, kPollutionOccAge, age);
        CHECK(std::fabs(age - 15.0f) < 1e-5f && services.lot.state == 2);        // past the warning age, below the abandon age
        cSCPropertyHelp::SetPropertyValue(&old.holder, 0xea1cf222, 14.0f);
        sim.AgeOccupantList(aging, 0, 0);
        CHECK(services.lot.state == 4);

        // radiation: adds an area effect to the radiation grid, announces it and runs the visual update hook
        int visual = 0;
        sim.SetVisualEffectUpdate([&] { ++visual; });
        host.messages.clear();
        const float where[3] = {64, 0, 64};
        sim.AddRadiationToCity(where, 0.0f, 20.0f);
        CHECK(visual == 0 && host.messages.empty());                             // a zero amount does nothing
        sim.AddRadiationToCity(where, 100.0f, 0.0f);
        CHECK(visual == 0);
        sim.AddRadiationToCity(where, 100.0f, 20.0f);
        CHECK(visual == 1 && host.messages.size() == 1 && host.messages[0].first == 0xda6dd35d);
        CHECK(sim.IsRadioactive(8, 8));
        sim.SetServices(nullptr);
    }
    // the prop requester (+8, iid 0x89c498be): a new city joins the prop developer once; the interface reads the
    // garbage grid over the rectangle it is given (inclusive), through the prop request curve
    {
        cSCPropertyHolder exemplar;
        cSCPropertyHelp::SetPropertyValue(&exemplar, 0x69a437f6, std::vector<float>{0.0f, 0.0f, 1.0f, 100.0f});
        Host h2;
        h2.exemplar = &exemplar;
        auto* fresh = new cSC4PollutionSimulator(&h2);
        fresh->AddRef();
        CHECK(fresh->Init());
        CHECK(h2.requesters.size() == 1 && h2.requesters[0] == static_cast<cISC4PropRequester*>(fresh));
        CHECK(fresh->Init() && h2.requesters.size() == 1);
        void* raw = nullptr;
        CHECK(fresh->QueryInterface(kGZIID_cISC4PropRequester, &raw) && raw == static_cast<cISC4PropRequester*>(fresh));
        auto* view = static_cast<cISC4PropRequester*>(raw);
        fresh->SetGarbageValue(9, 9, 30000);
        const std::int32_t hit[4] = {8, 8, 9, 9}, miss[4] = {0, 0, 7, 7}, edge[4] = {6, 6, 9, 9};
        CHECK(view->GetRequestedShare(hit) == fresh->GetRequestedShare(8, 8, 9, 9) && view->GetRequestedShare(hit) > 0);
        CHECK(view->GetRequestedShare(miss) == 0);
        CHECK(view->GetRequestedShare(edge) == fresh->GetRequestedShare(6, 6, 9, 9) && view->GetRequestedShare(edge) < view->GetRequestedShare(hit));
        CHECK(!view->UsesCustomPropSelector() && view->SelectProp({1, 2}, 0, 0) == 0 && view->GetRequesterSatisfactionId() == 0xe9a4300b);
        view->Release();
        fresh->Release();
    }
    std::printf(failures ? "FAILED\n" : "sc4_pollutionsimulator: all passed\n");
    return failures ? 1 : 0;
}
