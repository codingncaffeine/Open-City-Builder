// cSC4CrimeSimulator: the monthly cycle, census/economy maths, event creation, grids, hotspot, streams.
// Expected values are derived by hand from the decompiled formulas (0x1001fd15c..0x1001ffffa).
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZString.h"
#include "sc4/CrimeSimulator.h"
#include "tools/city_probe_plain.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECKF(a, b) do { if (std::fabs((double)(a) - (double)(b)) > 1e-4) { std::printf("FAIL line %d: %s=%g != %g\n", __LINE__, #a, (double)(a), (double)(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

class FakeCity : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 16; }
    std::int32_t CitySizeZInCells() const override { return 16; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct FakeBuilding : ICrimeBuilding {
    std::int32_t info[2] = {0, 0};
    const std::int32_t* ClassAndSubclass() override { return info; }
    std::uint32_t OccupantId() override { return 77; }
};

struct FakeLot : ICrimeLot {
    int x0 = 4, z0 = 4, x1 = 5, z1 = 5, cx = 4, cz = 4;
    std::uint32_t wealth = 0;
    FakeBuilding* building = nullptr;
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void GetBounds(int& a, int& b, int& c, int& d) override { a = x0; b = z0; c = x1; d = z1; }
    void GetCell(int& x, int& z) override { x = cx; z = cz; }
    std::uint32_t Wealth() override { return wealth; }
    ICrimeBuilding* Building() override { return building; }
    void GetPosition(float& x, float& z) override { x = float(x0) * 16.0f; z = float(z0) * 16.0f; }
};

struct FakeCensus : ICrimeCensusGrid {
    std::vector<std::vector<std::uint16_t>> pop;
    std::vector<std::vector<float>> value;
    int CountX() override { return int(pop.size()); }
    int CountZ() override { return int(pop[0].size()); }
    std::uint16_t Population(int x, int z) override { return pop[x][z]; }
    float Value(int x, int z) override { return value[x][z]; }
};

struct FakeOccupant : ICrimeOccupant {
    std::uint32_t type = 0;
    cSCPropertyHolder props;
    float pos[3] = {0, 0, 0};
    bool department = false, responder = false;
    std::uint32_t groups = 0;
    std::uint32_t OccupantType() override { return type; }
    cISCPropertyHolder* PropertyHolder() override { return &props; }
    void Position(float xyz[3]) override { for (int i = 0; i < 3; ++i) xyz[i] = pos[i]; }
    void* IdlePropOccupant() override { return department ? static_cast<void*>(this) : nullptr; }
    void* CrimeAutomaton() override { return responder ? static_cast<void*>(this) : nullptr; }
    bool IsOccupantGroup(std::uint32_t g) override { return groups == g; }
};

// a plain serializable that is not a requester: its adapter must not answer 0x89c498be
struct PlainNoRequester {
    bool Write(cIGZOStream&) const { return true; }
    bool Read(cIGZIStream&) { return true; }
    std::uint32_t GetGZCLSID() const { return 0x12345678; }
};

struct Host : ICrimeHost {
    int requesterRegistrations = 0;
    void RegisterPropRequester() override { ++requesterRegistrations; }
    FakeLot* lot = nullptr;
    std::map<std::pair<int, int>, int> zones;
    int jailPopulation = 0, unjailed = 0, statsCalls = 0, updated = 0;
    cSC4SimGrid<std::uint8_t>* police = nullptr;
    std::map<int, std::array<float, 3>> demand;
    float ordinance = 1.0f;
    FakeCensus pop[3], val[3];
    std::vector<cGZPersistResourceKey> typeKeys;
    std::map<std::uint32_t, cSCPropertyHolder*> exemplars;
    std::map<std::uint32_t, std::string> names;
    std::vector<std::pair<std::uint32_t, std::string>> events;
    int arsons = 0;
    double nextRoll = 1.0, nextRange = 0.0;
    std::vector<std::uint32_t> randomUint;
    std::map<std::uint32_t, int> propCrime;
    std::vector<ICrimeOccupant*> box;

    ICrimeLot* RandomLot() override { return lot; }
    ICrimeLot* LotAt(int, int) override { return lot; }
    int ZoneTypeAt(int x, int z) override { auto it = zones.find({x, z}); return it == zones.end() ? 0 : it->second; }
    int PropCrimeTypeId(std::uint32_t id) override { return propCrime[id]; }
    int PoliceJailPopulation() override { return jailPopulation; }
    int PoliceResetUnjailedCriminals() override { return unjailed; }
    void PoliceProcessMonthlyCrimeStats() override { ++statsCalls; }
    const cSC4SimGrid<std::uint8_t>* PoliceCoverageGrid() override { return police; }
    void DemandValues(int id, float& a, float& b, float& c) override { auto d = demand[id]; a = d[0]; b = d[1]; c = d[2]; }
    float OrdinanceFactor(std::uint32_t) override { return ordinance; }
    void ResidentialGrids(int w, ICrimeCensusGrid*& p, ICrimeCensusGrid*& v) override { p = &pop[w]; v = &val[w]; }
    std::vector<cGZPersistResourceKey> ListCrimeTypeExemplars() override { return typeKeys; }
    cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& k) override {
        auto it = exemplars.find(k.instance);
        return it == exemplars.end() ? nullptr : it->second;
    }
    void ReleaseExemplar(cISCPropertyHolder*) override {}
    bool LoadExemplarString(std::uint32_t, std::uint32_t instance, std::uint32_t, std::string& out) override {
        auto it = names.find(instance);
        if (it == names.end()) return false;
        out = it->second;
        return true;
    }
    void PostCrimeEvent(std::uint32_t id, ICrimeLot*, const std::string& text) override { events.push_back({id, text}); }
    void PostCrimesUpdated() override { ++updated; }
    void StartArson(ICrimeLot*, ICrimeBuilding*) override { ++arsons; }
    bool WriteLot(cIGZOStream& s, ICrimeLot* l) override { s.SetUint8(l ? 1 : 0); return true; }
    ICrimeLot* ReadLot(cIGZIStream& s) override { std::uint8_t b = 0; s.GetUint8(b); if (!b) return nullptr; lot->AddRef(); return lot; }
    void LotPositionToIncidentXZ(float x, float z, float& ox, float& oz) override { ox = x + 1; oz = z + 2; }
    std::vector<ICrimeOccupant*> OccupantsInBox(const float*, const float*) override { return box; }
    void ReleaseOccupants(std::vector<ICrimeOccupant*>&) override {}
    double RandomDouble() override { return nextRoll; }
    double RandomDoubleRange(double, double) override { return nextRange; }
    std::uint32_t RandomUint32(std::uint32_t bound) override {
        std::uint32_t v = randomUint.empty() ? 0 : randomUint.front();
        if (!randomUint.empty()) randomUint.erase(randomUint.begin());
        return v % bound;
    }
};

cSCPropertyHolder* MakeCrimeType(std::vector<std::uint8_t> zone, std::uint8_t severity, bool arson, float weight) {
    auto* p = new cSCPropertyHolder();
    cSCPropertyHelp::SetPropertyValue(p, kCrimeTypeZoneFilter, zone);
    cSCPropertyHelp::SetPropertyValue(p, kCrimeTypeSeverity, severity);
    cSCPropertyHelp::SetPropertyValue(p, kCrimeTypeArson, std::uint8_t(arson ? 1 : 0));
    cSCPropertyHelp::SetPropertyValue(p, kCrimeTypeWeight, weight);
    cSCPropertyHelp::SetPropertyValue(p, kCrimeTypeName, cRZString("Mugging"));
    return p;
}

void TestHelpers() {
    CHECK(ParseCrimeFilterData({}) == 0);                       // empty: 0xffffffff is overwritten by 0
    CHECK(ParseCrimeFilterData({0}) == 0xffffffff);              // a zero entry means "any"
    CHECK(ParseCrimeFilterData({1, 3}) == 0x5);
    CHECK(ParseCrimeFilterData({2, 0, 4}) == 0xffffffff);
    CHECK(ParseCrimeFilterData({32}) == 0x80000000);
    CHECK(TruncateFloatX86(2.9f) == 2 && TruncateFloatX86(-2.9f) == -2);
    CHECK(TruncateFloatX86(NAN) == INT_MIN && TruncateFloatX86(3e10f) == INT_MIN);
    for (float v : {0.0f, 0.5f, 1.5f, 2.5f, -0.5f, -1.5f, 7.3f, 1000.49f, -1234.5f})
        CHECK(RoundFloatMagic(v) == int(std::nearbyint(v)));
    // radius effect on a byte grid: peak at the centre, falling to zero at the radius, saturating at 255
    std::vector<std::vector<std::uint8_t>> g(8, std::vector<std::uint8_t>(8, 0));
    AddRadiusEffectU8(g, 8, 8, 3, 3, 4.0f, 100.0f, 0.0f);
    CHECK(g[3][3] == 100);
    CHECK(g[4][3] == 75 && g[3][2] == 75);
    CHECK(g[7][3] == 0);   // distance 4 is not < radius
    AddRadiusEffectU8(g, 8, 8, 3, 3, 4.0f, 200.0f, 0.0f);
    CHECK(g[3][3] == 255);
    AddRadiusEffectU8(g, 8, 8, 3, 3, 4.0f, -400.0f, 0.0f);
    CHECK(g[3][3] == 0);
}

}  // namespace

int main() {
    FakeCity city;
    SetSimGridCity(&city);
    TestHelpers();

    Host host;
    FakeLot lot;
    host.lot = &lot;
    FakeBuilding building;
    lot.building = &building;
    // crime types: A matches zone 1 (residential-low) lots, B matches zone 2 and burns buildings
    cSCPropertyHolder* typeA = MakeCrimeType({1}, 40, false, 10.0f);
    cSCPropertyHolder* typeB = MakeCrimeType({2}, 90, true, 30.0f);
    host.exemplars[0xA1] = typeA;
    host.exemplars[0xB2] = typeB;
    host.typeKeys = {{kCrimeTypeExemplarType, kCrimeTypeExemplarGroup, 0xA1}, {kCrimeTypeExemplarType, kCrimeTypeExemplarGroup, 0xB2}};
    host.names[0xA1] = "Pickpocket";

    CrimeTypeRegistry registry;
    cSC4CrimeSimulator sim(&host, &registry);
    CHECK(sim.GetGZCLSID() == 0x8990c09a && sim.GetSimulatorType() == 0x27567222);
    CHECK(sim.GetRequesterSatisfactionId() == 0x49a1e996 && sim.UsesCustomPropSelector());

    // tunables: defaults without an exemplar, then from properties
    sim.ReadSimulatorTunableValues(nullptr);
    CHECK(sim.Tunables().tractSize == 2);
    CHECKF(sim.Tunables().decay, 0.5f);
    CHECKF(sim.Tunables().crimeFactor, 1.0f);
    cSCPropertyHolder tunables;
    cSCPropertyHelp::SetPropertyValue(&tunables, 0x47e2c200, std::int32_t(4));
    cSCPropertyHelp::SetPropertyValue(&tunables, 0x8a0cbaac, 0.75f);
    cSCPropertyHelp::SetPropertyValue(&tunables, 0x8a0cbaad, 2.0f);
    sim.ReadSimulatorTunableValues(&tunables);
    CHECK(sim.Tunables().tractSize == 4);
    CHECKF(sim.Tunables().decay, 0.75f);
    CHECKF(sim.Tunables().crimeFactor, 2.0f);
    // default curves installed with an exemplar present (before any override)
    CHECKF(sim.ResidentCurve(0).GetOutputValue(0.0f), 0.1f);
    CHECKF(sim.ResidentCurve(1).GetOutputValue(255.0f), 0.0f);
    CHECKF(sim.FactorCurve(2).GetOutputValue(2.0f), 0.1f);
    CHECKF(sim.FactorCurve(2).GetOutputValue(5.0f), 0.5f);
    CHECKF(sim.CriminalCurve().GetOutputValue(500000.0f), 25000.0f);
    CHECKF(sim.CrimeChanceCurve().GetOutputValue(127.5f), 0.5f);

    // Init: tract size 2 for the rest of the test; crime types are loaded once
    CHECK(sim.Init(nullptr));
    CHECK(!sim.Init(nullptr));
    CHECK(host.requesterRegistrations == 1);   // a new city: the requester view joins the prop developer once
    CHECK(registry.Types().size() == 2);
    CHECK(registry.Types()[0]->id == 0xA1 && registry.Types()[0]->name == "Pickpocket");   // localized string wins
    CHECK(registry.Types()[1]->name == "Mugging" && registry.Types()[1]->arson);          // property fallback
    CHECK(registry.Types()[0]->filter[0] == 1 && registry.Types()[1]->filter[0] == 2);
    CHECK(registry.Types()[0]->filter[1] == 0xffffffff && registry.Types()[0]->severity == 40);
    CHECKF(registry.Types()[1]->weight, 30.0f);
    CHECK(sim.GetCrimeGrid()->GetTractSize() == 2 && sim.GetCrimeGrid()->GetTractCountX() == 8);
    CHECK(sim.GetCrimeGrid()->GetGridData()[0][0] == 0);
    CHECK(sim.CurrentStep() == cSC4CrimeSimulator::kIdle);

    // lot bits: zone 1/2 -> bits, wealth table, building class/subclass
    lot.cx = lot.cz = 4;
    host.zones[{4, 4}] = 5;   // 1-based zone type 5 -> bit 2
    lot.wealth = 4;           // wealth table[4] = 4
    building.info[0] = 0; building.info[1] = 3;
    std::uint32_t zone, cls, sub, wealth;
    sim.GetLotBits(&lot, zone, cls, sub, wealth);
    CHECK(zone == 2 && wealth == 4 && cls == 4 && sub == 0x100);
    building.info[0] = 2;
    sim.GetLotBits(&lot, zone, cls, sub, wealth);
    CHECK(sub == 2);
    lot.building = nullptr;
    lot.wealth = 9;
    host.zones[{4, 4}] = 12;
    sim.GetLotBits(&lot, zone, cls, sub, wealth);
    CHECK(zone == 0x80000000 && cls == 0x80000000 && sub == 0x100 && wealth == 0x80000000);
    lot.building = &building;
    lot.wealth = 0;

    // census: population bucketed by value / 50; bands past 3 are dropped
    for (int w = 0; w < 3; ++w) {
        host.pop[w].pop.assign(2, std::vector<std::uint16_t>(2, 0));
        host.val[w].value.assign(2, std::vector<float>(2, 0.0f));
    }
    host.pop[0].pop[0][0] = 10; host.val[0].value[0][0] = 10.0f;    // band 0
    host.pop[0].pop[1][0] = 5;  host.val[0].value[1][0] = 120.0f;   // band 2
    host.pop[0].pop[0][1] = 7;  host.val[0].value[0][1] = 300.0f;   // band 6: ignored here
    host.pop[1].pop[1][1] = 20; host.val[1].value[1][1] = 199.0f;   // band 3
    host.pop[2].pop[0][0] = 3;  host.val[2].value[0][0] = 55.0f;    // band 1
    sim.DoCensus();
    CHECK(sim.Census()[0][0] == 10 && sim.Census()[0][2] == 5 && sim.Census()[0][1] == 0 && sim.Census()[0][3] == 0);
    CHECK(sim.Census()[1][3] == 20 && sim.Census()[2][1] == 3);

    // economic factors: 2 * (sum a - sum c) / (sum b - sum c) - 1 over each wealth level's demand ids
    for (int id : {0x4100, 0x4200, 0x4300, 0x3110, 0x3120}) host.demand[id] = {2.0f, 6.0f, 1.0f};
    host.demand[0x4400] = {1, 1, 0}; host.demand[0x3320] = {0, 3, 0}; host.demand[0x3130] = {0, 0, 0};
    host.demand[0x3330] = {9.0f, 3.0f, 3.0f};   // (9-3)/(3-3) -> division by zero
    sim.CalculateEconomicFactors();
    CHECKF(sim.Factors()[0], 2.0f * ((10.0f - 5.0f) / (30.0f - 5.0f)) - 1.0f);   // 2*0.2 - 1 = -0.6
    CHECKF(sim.Factors()[1], 2.0f * ((1.0f - 0.0f) / (4.0f - 0.0f)) - 1.0f);     // -0.5
    CHECK(std::isinf(sim.Factors()[2]));

    // criminal base: install simple curves and check the per-level formula
    sim.ResidentCurve(0).SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {100.0f, 3.0f}});   // 1 + x/50
    sim.FactorCurve(0).SetResponseCurveData(std::vector<SC4Point>{{-1.0f, 0.0f}, {1.0f, 10.0f}});    // 5 + 5x
    sim.ResidentCurve(1).SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}});
    sim.FactorCurve(1).SetResponseCurveData(std::vector<SC4Point>{{-1.0f, 0.0f}, {1.0f, 2.0f}});      // 1 + x
    sim.ResidentCurve(2).SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}});
    sim.FactorCurve(2).SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}});
    // level 0: bands 0 and 2 hold 10 and 5 residents: 10*curve(0) + 5*curve(50) = 10 + 5*2 = 20, plus all 15 residents
    // times factorCurve(-0.6) = 5 + 5*(-0.6) = 2 -> 30, total 50
    // level 1: 20 * factor(-0.5)=20*0.5=10; level 2: 0
    CHECK(sim.ComputeCriminalBase() == 60);

    // idle cycle: Waiting -> census -> base -> crimes -> apply -> post -> idle
    sim.ForceStep(cSC4CrimeSimulator::kWaiting);
    host.jailPopulation = 40;
    host.unjailed = 25;
    sim.DoMessage(kCrimeMsgIdle);
    CHECK(sim.CurrentStep() == cSC4CrimeSimulator::kCensus);
    sim.DoMessage(kCrimeMsgIdle);
    CHECK(sim.CurrentStep() == cSC4CrimeSimulator::kCriminalBase);
    sim.DoMessage(kCrimeMsgIdleAlt);
    CHECK(sim.CurrentStep() == cSC4CrimeSimulator::kCrimesToCommit && sim.CriminalBase() == 60);
    // base 60 > jailed 40: crimes = (int)(25 * 1.0) + (int)curve(20) = 25 + 20*0.05 = 26; x multiplier 1.5 = 39
    sim.DoMessage(kCrimeMsgOrdinanceChanged);
    host.ordinance = 1.5f;
    sim.DoMessage(kCrimeMsgOrdinanceChanged);
    CHECKF(sim.CrimeMultiplier(), 1.5f);
    sim.DoMessage(kCrimeMsgIdle);
    CHECK(sim.CurrentStep() == cSC4CrimeSimulator::kApplyCrimes);
    CHECK(sim.CrimesToAdd() == 39);
    // jail covers the base: crimes = (int)(25 * 1.0) = 25 -> 37.5 rounds half to even = 38
    sim.ForceStep(cSC4CrimeSimulator::kCrimesToCommit);
    host.jailPopulation = 60;
    sim.DoIdle();
    CHECK(sim.CrimesToAdd() == 38);

    // AddCrimes: every roll above the chance -> an event; the lot (zone 1) matches only type A
    host.zones[{4, 4}] = 1;
    lot.building = &building;
    host.nextRoll = 1.0;
    host.nextRange = 4.0;
    host.police = new cSC4SimGrid<std::uint8_t>();
    host.police->AddRef();
    host.police->SetTractSize(2);
    host.police->SetTractValues(0);
    // chance curve: (0,0)->(255,1): police 0 - effect 0 = 0 -> chance 0, roll 1.0 -> crime happens
    int added = sim.AddCrimes(3);
    CHECK(added == 3);
    CHECK(host.events.size() == 3 && host.events[0].first == 0xA1 && host.events[0].second == "Pickpocket");
    CHECK(lot.refs == 3 && sim.Events().size() == 3);
    // each crime adds severity 40 to the tracts under the lot: cells 4..5 -> tract 2; 3 crimes = 120
    CHECK(sim.GetCrimeGrid()->GetGridData()[2][2] == 120);
    std::uint8_t hv = 0; int hx = -1, hz = -1;
    CHECK(sim.GetCrimeHotspot(&hv, &hx, &hz) && hv == 120 && hx == 4 && hz == 4);
    sim.AddCrimes(2);   // 200 saturates at 255 after the fourth/fifth
    CHECK(sim.GetCrimeGrid()->GetGridData()[2][2] == 200);
    sim.AddCrimes(2);
    CHECK(sim.GetCrimeGrid()->GetGridData()[2][2] == 255);
    // police coverage lowers the chance: coverage 255 -> chance 1.0, roll 1.0 -> still crime (<=); roll 0.5 -> none
    for (auto& column : host.police->GetGridData()) for (auto& v : column) v = 255;
    host.nextRoll = 0.5;
    CHECK(sim.AddCrimes(5) == 0);
    host.nextRoll = 1.0;
    CHECK(sim.AddCrimes(1) == 1);
    // a lot that no type accepts gives no event
    host.zones[{4, 4}] = 9;
    CHECK(sim.AddCrimes(4) == 0);

    // type B (arson, severity 90) on a zone-2 lot with a building; weighted pick over {A?} only B qualifies
    host.zones[{4, 4}] = 4;   // zone table[3] = 2
    sim.ClearCrimeEvents();
    CHECK(lot.refs == 0);
    host.events.clear();
    std::uint8_t severity = sim.AddCrimeEvent(&lot);
    CHECK(severity == 90 && host.arsons == 1 && host.events.size() == 1 && host.events[0].first == 0xB2);
    lot.building = nullptr;
    severity = sim.AddCrimeEvent(&lot);
    CHECK(severity == 90 && host.arsons == 1);   // no building, no fire
    lot.building = &building;

    // incident array and visual flags
    std::vector<CrimeIncident> incidents;
    CHECK(sim.GetCrimeIncidentArray(incidents) && incidents.size() == 2);
    CHECKF(incidents[0].x, 4 * 16.0f + 1.0f);
    CHECKF(incidents[0].z, 4 * 16.0f + 2.0f);
    CHECK(incidents[0].typeId == 0xB2 && incidents[0].visualsCreated == 0);
    CHECK(sim.SetVisualsCreated(1, true) && sim.GetVisualsCreated(1) && !sim.GetVisualsCreated(0));
    CHECK(!sim.SetVisualsCreated(2, true) && !sim.GetVisualsCreated(2));

    // decay keeps zero cells, multiplies others by the decay factor and refreshes the hotspot value
    sim.ReadSimulatorTunableValues(nullptr);   // decay back to 0.5
    sim.DecayCrimeGrid();
    CHECK(sim.GetCrimeGrid()->GetGridData()[2][2] == 127);
    CHECK(sim.GetCrimeHotspot(&hv, &hx, &hz) && hv == 127);
    sim.DecayCrimeGrid();
    CHECK(sim.GetCrimeGrid()->GetGridData()[2][2] == 63);

    // average crime over zoned tracts; GetAverageCrime truncates like the original (NaN -> 0)
    host.zones.clear();
    sim.ComputeAverageCrime();
    CHECK(sim.GetAverageCrime() == 0);   // nothing zoned: 0/0 = NaN, truncated like cvttss2si
    host.zones[{4, 4}] = 1;
    host.zones[{0, 0}] = 1;
    sim.ComputeAverageCrime();
    CHECK(sim.GetAverageCrime() == (63 + 0) / 2);

    // requested share: crime / 255 through the share curve
    sim.ShareCurve().SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {1.0f, 100.0f}});
    CHECK(sim.GetRequestedShare(4, 4, 6, 6) == RoundFloatMagic(100.0f * (63.0f / 255.0f)));
    CHECK(sim.GetRequestedShare(0, 0, 2, 2) == 0);

    // SelectProp prefers props whose crime type already happened on the lot
    sim.ClearCrimeEvents();
    host.zones[{4, 4}] = 1;
    host.nextRoll = 1.0;
    sim.AddCrimes(1);   // type A (0xA1) event on the lot
    host.propCrime[100] = 0;       // any crime
    host.propCrime[101] = 0xA1;    // matches the event
    host.propCrime[102] = 0xB2;    // does not
    host.randomUint = {0};
    CHECK(sim.SelectProp({100, 101, 102}, 4, 4) == 101);
    sim.ClearCrimeEvents();
    host.randomUint = {1};
    host.randomUint = {1};
    host.propCrime[103] = 0;
    CHECK(sim.SelectProp({100, 102, 103}, 4, 4) == 103);   // no matches: choose among the "any crime" props
    CHECK(sim.SelectProp({102}, 4, 4) == 0);

    // occupants: a building with {strength, radius} adds / removes a radius effect on the effect grid
    FakeOccupant station;
    cSCPropertyHelp::SetPropertyValue(&station.props, kCrimeBuildingEffectProperty, std::vector<std::uint8_t>{50, 8});
    station.pos[0] = 4 * 16.0f + 1.0f;  // tract width 32 m -> tract 2
    station.pos[2] = 4 * 16.0f + 1.0f;
    sim.DoMessage(kCrimeMsgOccupantAdded, 0, 0, &station);
    // radius 8 / tract size 2 = 4 tracts, strength 50: centre tract (2,2) gets 50, (3,2) gets round(50 - 1*12.5) = 38
    CHECK(sim.GetCrimeEffectGrid()->GetGridData()[2][2] == 50);
    CHECK(sim.GetCrimeEffectGrid()->GetGridData()[3][2] == 38);
    sim.DoMessage(kCrimeMsgOccupantRemoved, 0, 0, &station);
    CHECK(sim.GetCrimeEffectGrid()->GetGridData()[2][2] == 0);
    cSCPropertyHelp::SetPropertyValue(&station.props, kCrimeNonCrimeOccupantProperty, std::uint32_t(1));
    sim.DoMessage(kCrimeMsgOccupantAdded, 0, 0, &station);
    CHECK(sim.GetCrimeEffectGrid()->GetGridData()[2][2] == 0);   // flagged occupants are ignored

    // nearest prop / sim searches
    FakeOccupant near1, near2, far1;
    near1.type = kCrimePropOccupantType; near1.department = true; near1.pos[0] = 10; near1.pos[2] = 0;
    cSCPropertyHelp::SetPropertyValue(&near1.props, kCrimePropFilterProperty, std::uint32_t(1));
    near2.type = kCrimePropOccupantType; near2.department = true; near2.pos[0] = 4; near2.pos[2] = 3;
    cSCPropertyHelp::SetPropertyValue(&near2.props, kCrimePropFilterProperty, std::uint32_t(1));
    far1.type = kCrimePropOccupantType; far1.department = true; far1.pos[0] = 500; far1.pos[2] = 0;
    cSCPropertyHelp::SetPropertyValue(&far1.props, kCrimePropFilterProperty, std::uint32_t(1));
    host.box = {&near1, &near2, &far1};
    const float origin[3] = {0, 0, 0};
    CHECK(sim.GetNearestCrimeProp(origin, 100.0f) == static_cast<void*>(&near2));
    near2.department = false;   // not available any more
    CHECK(sim.GetNearestCrimeProp(origin, 100.0f) == static_cast<void*>(&near1));
    CHECK(sim.GetNearestCrimeProp(origin, 5.0f) == nullptr);
    FakeOccupant s1;
    s1.type = kCrimeSimOccupantType; s1.groups = 0x4102; s1.responder = true; s1.pos[0] = 20;
    FakeOccupant s2;
    s2.type = kCrimeSimOccupantType; s2.groups = 0x0102; s2.responder = true; s2.pos[0] = 1;   // wrong group
    host.box = {&s1, &s2};
    CHECK(sim.GetNearestCrimeSim(origin, 100.0f) == static_cast<void*>(&s1));
    CHECK(IsCrimeSimOccupant(&s1) && !IsCrimeSimOccupant(&s2) && !IsCrimePropOccupant(&s1));

    // streams: scalar state round-trips (a plain memory stream skips the nested grids, as for the other simulators)
    PortMemoryOStream os;
    sim.ClearCrimeEvents();
    host.zones[{4, 4}] = 1;
    sim.AddCrimes(2);
    CHECK(sim.Write(os));
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    cSC4CrimeSimulator other(&host, &registry);
    CHECK(other.Read(is));
    CHECK(other.GetMonthlyCrimeCount() == 0 && other.Events().size() == 2);
    CHECK(other.Events()[0]->typeId == 0xA1 && other.Events()[0]->description == "Pickpocket");
    CHECKF(other.CrimeMultiplier(), sim.CrimeMultiplier());
    CHECK(other.Init(nullptr) && host.requesterRegistrations == 1);   // a loaded city keeps the saved requester list
    std::uint8_t bad[] = {5, 0};
    PortMemoryIStream badStream(bad, sizeof bad);
    cSC4CrimeSimulator third(&host, &registry);
    CHECK(!third.Read(badStream));

    // the plain simulator's adapter answers the prop requester interface (+8, 0x89c498be) with a view that shares its
    // count and passes the rectangle through (the original averages x0..x1-1, z0..z1-1: width x1 - x0)
    {
        cSCPropertyHolder shareTunables;
        cSCPropertyHelp::SetPropertyValue(&shareTunables, 0x49a34301, std::vector<float>{0.0f, 0.0f, 1.0f, 100.0f});
        auto* plain = new cSC4CrimeSimulator(&host, &registry);
        CHECK(plain->Init(&shareTunables) && host.requesterRegistrations == 2);
        cIGZSerializable* adapter = WrapPlain(plain);
        void* raw = nullptr;
        CHECK(adapter->QueryInterface(kGZIID_cISC4PropRequester, &raw) && raw);
        auto* view = static_cast<cISC4PropRequester*>(raw);
        CHECK(view->GetRequesterSatisfactionId() == 0x49a1e996 && view->UsesCustomPropSelector());
        auto& cells = const_cast<cSC4SimGrid<std::uint8_t>*>(plain->GetCrimeGrid())->GetGridData();
        cells[1][1] = 255;   // tract size 2 (the tunables' default): cells 2..3
        const std::int32_t hit[4] = {2, 2, 4, 4}, miss[4] = {4, 4, 6, 6};
        CHECK(view->GetRequestedShare(hit) == plain->GetRequestedShare(2, 2, 4, 4) && view->GetRequestedShare(hit) > 0);
        CHECK(view->GetRequestedShare(miss) == 0);
        void* back = nullptr;
        CHECK(view->QueryInterface(kGZIID_cIGZSerializable, &back) && back == adapter);
        if (back) static_cast<cIGZSerializable*>(back)->Release();
        view->AddRef();       // the prop developer's reference (AddPropRequester) counts on the adapter
        adapter->Release();
        view->Release();
        view->Release();      // the last reference: deletes the adapter and the simulator
    }
    void* none = nullptr;
    {
        cIGZSerializable* plainOther = WrapPlain(new PlainNoRequester());
        CHECK(!plainOther->QueryInterface(kGZIID_cISC4PropRequester, &none) && !none);
        plainOther->Release();
    }

    // shutdown frees everything; events release their lots
    sim.Shutdown();
    CHECK(sim.Events().empty() && sim.GetCrimeGrid() == nullptr);

    host.police->Release();
    delete typeA;
    delete typeB;
    std::printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures != 0;
}
