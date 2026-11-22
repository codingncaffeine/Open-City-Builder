// cSC4PollutionSimulator::UpdatePollutionVisualEffect: the 8x8-cell block walk (one rand() call), the worst tracts, the per-block
// effect slots (create / update / stop) and the call sites (radiation, PostCityInit, view level, month, Shutdown).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/cSC4SimGrid.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
class City final : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 64; }
    std::int32_t CitySizeZInCells() const override { return 48; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct Effect final : IPollutionVisualEffect {
    std::uint32_t group = 0, type = 0;
    bool started = false, released = false;
    int starts = 0, stops = 0, lastStop = -1, params = 0;
    std::uint32_t paramType = 0;
    float strength = 0;
    cS3DTransform transform;
    void Start(int) override { started = true; ++starts; }
    void Stop(int t) override { started = false; ++stops; lastStop = t; }
    bool IsStarted() override { return started; }
    void SetSourceTransform(const cS3DTransform& t) override { transform = t; }
    void SetFloatParams(std::uint32_t t, const float* v, int count) override { paramType = t; strength = count == 1 ? v[0] : -1; ++params; }
    void Release() override { released = true; }   // the test keeps the objects to look at them afterwards
};

class Host final : public IPollutionHost {
public:
    std::vector<Effect*> made;
    bool bound = true;
    const cISCPropertyHolder* exemplar = nullptr;
    IPollutionVisualEffect* CreateVisualEffect(std::uint32_t group, std::uint32_t type) override {
        if (!bound) return nullptr;
        auto* e = new Effect();
        e->group = group;
        e->type = type;
        made.push_back(e);
        return e;
    }
    float TerrainAltitude(float x, float z) override { return 100.0f + x * 0.001f + z * 0.0001f; }
    int Live(std::uint32_t type) const {
        int n = 0;
        for (const Effect* e : made) n += (e->type == type && !e->released) ? 1 : 0;
        return n;
    }
    Effect* LiveOf(std::uint32_t type) const {
        for (Effect* e : made) if (e->type == type && !e->released) return e;
        return nullptr;
    }
    void AddAgent(cIGZMessageTarget2*, int, const char*, std::uint32_t) override {}
    void RemoveAgent(cIGZMessageTarget2*) override {}
    void AddNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    const cISCPropertyHolder* SimulatorExemplar() override { return exemplar; }
    void CreateHistory(std::uint32_t, std::uint32_t) override {}
    float OrdinanceFactor(std::uint32_t, std::uint32_t) override { return 1.0f; }
    int ZoneTypeAt(int, int) override { return 0; }
    std::uint8_t TrafficAirAt(int, int) override { return 0; }
    void GarbageDeals(std::uint32_t, std::vector<IPollutionDeal*>& out) override { out.clear(); }
    void SetSanitationLineItemCost(int, std::int64_t) override {}
    bool OccupantFootprint(void*, PollutionOccupantFootprint&) override { return false; }
    bool OccupantPowered(IPollutionOccupant*) override { return true; }
    float SanitationFunding() override { return 1.0f; }
    void LandfillRange(int& l, int& h) override { l = 0; h = 0; }
    int LandfillTransportLoad() override { return 0; }
    void PostMessage(std::uint32_t, std::uint32_t) override {}
    void AddHistory(std::uint32_t, std::int64_t) override {}
    float RecyclingCapacitySlots() override { return 0.0f; }
};

// every cell of block (bx, bz) through a setter
template <typename F> void FillBlock(int bx, int bz, F set) {
    for (int x = bx * 8; x < bx * 8 + 8; ++x)
        for (int z = bz * 8; z < bz * 8 + 8; ++z) set(unsigned(x), unsigned(z));
}

void SendViewLevel(cSC4PollutionSimulator& sim, std::uint32_t level) {
    std::uint32_t old = 1;
    auto* m = new cRZMessage2Standard();
    m->AddRef();
    m->SetType(0xc6cdbf1a);
    m->SetVoid1(&old);
    m->SetVoid2(&level);
    sim.DoMessage(m);
    m->Release();
}
}  // namespace

int main() {
    City city;
    SetSimGridCity(&city);
    // the shipped strength curves (exemplar 0xc911e35b properties 0x0a10c772..5)
    cSCPropertyHolder exemplar;
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x0a10c772, std::vector<float>{0, 0, 0.002f, 0.05f, 0.005f, 0.15f, 0.01f, 0.4f, 0.02f, 1});
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x0a10c773, std::vector<float>{0, 0, 0.005f, 0.05f, 0.01f, 0.25f, 0.02f, 0.5f, 0.03f, 0.6f});
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x0a10c774, std::vector<float>{0, 0, 0.001f, 1, 0.01f, 1});
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x0a10c775, std::vector<float>{0, 0, 0.02f, 0.02f, 0.03f, 0.06f, 0.04f, 0.2f, 0.05f, 0.4f,
                                                                            0.06f, 0.53f, 0.085f, 0.75f, 0.13f, 1});
    Host host;
    host.exemplar = &exemplar;
    auto* sim = new cSC4PollutionSimulator(&host);
    sim->AddRef();
    CHECK(sim->Init());
    // Init: one empty slot per 8x8 block and kind, (64/8) x (48/8)
    CHECK(sim->EffectBlocksX() == 8 && sim->EffectSlots(cSC4PollutionSimulator::kEffectAir).size() == 48);
    CHECK(sim->EffectsVisible());

    // a clean city: no effects, worst values 0; the pass makes exactly one rand() call (the block iterator's reset)
    std::srand(77);
    sim->UpdatePollutionVisualEffect();
    const int afterPass = std::rand();
    std::srand(77);
    std::rand();
    CHECK(afterPass == std::rand());
    CHECK(host.made.empty());
    int x = -1, z = -1, v = -1;
    sim->GetWorstAirTract(x, z, v);
    CHECK(v == 0);

    // conformance example: average water 655 -> strength 0.9993775 -> a water effect (kind 1, parameter 3) at height 0 near the
    // block corner; average 33 elsewhere -> 0.0251778, no effect
    FillBlock(2, 3, [&](unsigned cx, unsigned cz) { sim->SetWaterValue(cx, cz, 655); });
    FillBlock(6, 0, [&](unsigned cx, unsigned cz) { sim->SetWaterValue(cx, cz, 33); });
    // air 655 in block (5, 1): strength 0.4997 -> kind 2, parameter 2, block centre on the terrain
    FillBlock(5, 1, [&](unsigned cx, unsigned cz) { sim->SetAirValue(cx, cz, 655); });
    FillBlock(1, 1, [&](unsigned cx, unsigned cz) { sim->SetAirValue(cx, cz, 100); });
    // garbage 3000 in block (0, 5) (bit 15 is the landfill flag and is not counted)
    FillBlock(0, 5, [&](unsigned cx, unsigned cz) { sim->SetGarbageValue(cx, cz, 3000); });
    sim->UpdatePollutionVisualEffect();
    CHECK(host.made.size() == 3);
    Effect* water = host.LiveOf(1);
    Effect* air = host.LiveOf(2);
    Effect* garbage = host.LiveOf(4);
    CHECK(water && air && garbage);
    if (!water || !air || !garbage) { std::printf("%d failures\n", failures + 1); return 1; }
    CHECK(water->group == 0x0a78747a && water->started && water->starts == 1);
    CHECK(water->paramType == 3 && std::fabs(water->strength - 0.9993775f) < 2e-6f);
    CHECK(water->transform.translation[1] == 0.0f);
    CHECK(water->transform.translation[0] >= 256.0f - 32.0f && water->transform.translation[0] <= 256.0f + 32.0f);
    CHECK(water->transform.translation[2] >= 384.0f - 32.0f && water->transform.translation[2] <= 384.0f + 32.0f);
    CHECK(water->transform.changeCounter == 1 && (water->transform.flags & 1) && water->transform.scale == 1.0f);
    CHECK(water->transform.rotation[0][0] == 1.0f && water->transform.rotation[1][1] == 1.0f && water->transform.rotation[0][1] == 0.0f);
    CHECK(air->paramType == 2 && std::fabs(air->strength - 0.49975f) < 1e-3f);
    CHECK(air->transform.translation[0] == 5 * 128 + 64 && air->transform.translation[2] == 1 * 128 + 64);
    CHECK(air->transform.translation[1] == host.TerrainAltitude(704, 192));
    CHECK(garbage->paramType == 2 && garbage->strength > 0.75f && garbage->transform.translation[0] == 64.0f &&
          garbage->transform.translation[2] == 5 * 128 + 64);
    // the effect slot of the block: index bz * blocksX + bx
    CHECK(sim->EffectSlots(cSC4PollutionSimulator::kEffectWater)[3 * 8 + 2] == water);
    CHECK(sim->EffectSlots(cSC4PollutionSimulator::kEffectAir)[1 * 8 + 5] == air);
    // worst tracts: the centre cell of the worst block and its average
    sim->GetWorstWaterTract(x, z, v);   CHECK(x == 20 && z == 28 && v == 655);
    sim->GetWorstAirTract(x, z, v);     CHECK(x == 44 && z == 12 && v == 655);
    sim->GetWorstGarbageTract(x, z, v); CHECK(x == 4 && z == 44 && v == 3000);

    // a shown effect that is already there only takes the new strength
    FillBlock(5, 1, [&](unsigned cx, unsigned cz) { sim->SetAirValue(cx, cz, 327); });   // 327 / 32767 = 0.00998 -> 0.2490
    sim->UpdatePollutionVisualEffect();
    CHECK(host.made.size() == 3 && air->starts == 1 && air->params == 2 && std::fabs(air->strength - 0.249f) < 1e-3f);
    sim->GetWorstAirTract(x, z, v);     CHECK(x == 44 && z == 12 && v == 327);   // refreshed from 0 every pass

    // below 0.05 a present effect is stopped (it was started) and released, its slot emptied
    FillBlock(2, 3, [&](unsigned cx, unsigned cz) { sim->SetWaterValue(cx, cz, 0); });
    sim->UpdatePollutionVisualEffect();
    CHECK(water->released && water->stops == 1 && water->lastStop == 0);
    CHECK(sim->EffectSlots(cSC4PollutionSimulator::kEffectWater)[3 * 8 + 2] == nullptr);
    sim->GetWorstWaterTract(x, z, v);   CHECK(v == 33 && x == 52 && z == 4);

    // radiation from a disaster: the pass runs right after it (kind 3, 3 m above the terrain)
    const float where[3] = {3 * 128 + 64, 0, 4 * 128 + 64};
    sim->AddRadiationToCity(where, 500.0f, 3.0f);
    Effect* radiation = host.LiveOf(3);
    CHECK(radiation && radiation->started && radiation->paramType == 2 && radiation->strength > 0.05f);
    if (radiation) CHECK(radiation->transform.translation[1] == host.TerrainAltitude(radiation->transform.translation[0], radiation->transform.translation[2]) + 3.0f);

    // leaving the surface view (level 3, water pipes) removes air, radiation and garbage effects; water effects stay
    FillBlock(2, 3, [&](unsigned cx, unsigned cz) { sim->SetWaterValue(cx, cz, 655); });
    sim->UpdatePollutionVisualEffect();
    const std::size_t madeBefore = host.made.size();
    SendViewLevel(*sim, 3);
    CHECK(!sim->EffectsVisible());
    CHECK(host.Live(2) == 0 && host.Live(3) == 0 && host.Live(4) == 0 && host.Live(1) == 1);
    CHECK(air->released && air->lastStop == 0);
    SendViewLevel(*sim, 3);                                  // no change: no pass
    CHECK(host.made.size() == madeBefore);
    SendViewLevel(*sim, 1);                                  // back in the surface view: created again
    CHECK(host.Live(2) == 1 && host.Live(3) >= 1 && host.Live(4) == 1);

    // PostCityInit and the month run the pass too
    FillBlock(7, 5, [&](unsigned cx, unsigned cz) { sim->SetAirValue(cx, cz, 1000); });
    auto* started = new cRZMessage2Standard();
    started->AddRef();
    started->SetType(0x26d31ec1);
    sim->DoMessage(started);
    started->Release();
    CHECK(host.Live(2) == 2);
    FillBlock(7, 4, [&](unsigned cx, unsigned cz) { sim->SetAirValue(cx, cz, 1000); });
    sim->DoMonthlySimulation();
    CHECK(host.Live(2) == 3);

    // an unbound effect key leaves the slot empty and is asked for again on the next pass
    host.bound = false;
    FillBlock(0, 0, [&](unsigned cx, unsigned cz) { sim->SetGarbageValue(cx, cz, 3000); });
    sim->UpdatePollutionVisualEffect();
    CHECK(sim->EffectSlots(cSC4PollutionSimulator::kEffectGarbage)[0] == nullptr);
    host.bound = true;

    // Shutdown stops every started effect at once (transition 1) and releases all of them
    Effect* liveRadiation = host.LiveOf(3);
    sim->Shutdown();
    int live = 0;
    for (Effect* e : host.made) live += e->released ? 0 : 1;
    CHECK(live == 0);
    CHECK(liveRadiation && liveRadiation->lastStop == 1);
    for (std::size_t k = 0; k < 4; ++k)
        for (IPollutionVisualEffect* slot : sim->EffectSlots(static_cast<cSC4PollutionSimulator::EffectKind>(k))) CHECK(slot == nullptr);

    sim->Release();
    for (Effect* e : host.made) delete e;
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
