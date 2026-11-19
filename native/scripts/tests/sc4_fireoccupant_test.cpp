// cSC4FireOccupant / cSC4Flame: footprint-derived flame counts, flame placement, hose/extinguish arithmetic, records.
#include <cstdio>

#include "sc4/FireOccupant.h"

using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Building final : cSC4Occupant {
    Building() { type_ = kOccupantTypeBuilding; }
    void GetBoundingBox(float* lo, float* hi) override { lo[0] = 0; lo[1] = 0; lo[2] = 0; hi[0] = 16; hi[1] = 10; hi[2] = 16; }
};
struct Effect final : IFlameEffect {
    int refs = 1, starts = 0, stops = 0; cS3DTransform last;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Start(int) override { ++starts; }
    void Stop(int) override { ++stops; }
    bool IsRunning() override { return true; }
    void SetTransform(const cS3DTransform& t) override { last = t; }
};
}  // namespace

int main() {
    cSC4ResponseCurve top, side;
    std::vector<cSC4ResponseCurve::ResponseCurveData> unused;
    (void)unused;
    FireServices& services = GetFireServices();
    std::vector<std::string> made;
    std::vector<Effect*> effects;
    services.createEffect = [&](const char* name) { made.push_back(name); effects.push_back(new Effect); return static_cast<IFlameEffect*>(effects.back()); };
    services.viewZoom = [] { return 1; };

    cSC4FireOccupant fire;
    CHECK(fire.GetGZCLSID() == kGZCLSID_cSC4FireOccupant);
    void* out = nullptr;
    CHECK(fire.QueryInterface(kGZIID_cISC4FireOccupant, &out) && out == &fire);

    // hose arithmetic never goes below zero
    fire.SetHosed(5); CHECK(fire.GetHosed() == 5);
    fire.SetHosed(-3); CHECK(fire.GetHosed() == 2);
    fire.SetHosed(-9); CHECK(fire.GetHosed() == 0);
    // extinguish: first call stores, a larger one clears
    CHECK(fire.Extinguish(4) == 4);
    CHECK(fire.Extinguish(3) == 4);
    CHECK(fire.Extinguish(9) == 0);

    // flames without a target model: the bounding-box fallback makes top+side+1 of them
    Building target;
    fire.SetCounter(7);
    fire.SetFamily(3);
    CHECK(fire.GetCounter() == 7 && fire.GetFamily() == 3);
    cSC4Flame flame;
    flame.Update(2);                        // top flame stage 3
    CHECK(made.size() == 1 && made[0] == "fire_3");
    flame.side = true;
    flame.Update(0);
    CHECK(made.back() == "wfire_1");
    flame.SetVisible(false);
    CHECK(effects.back()->stops >= 1);
    flame.position[0] = 1; flame.position[1] = 2; flame.position[2] = 3;
    flame.UpdateTransform();
    CHECK(effects.back()->last.translation[2] == 3.0f && effects.back()->last.flags == 5);
    flame.active = true;
    flame.Shutdown();
    CHECK(!flame.active && !flame.effect);
    for (Effect* e : effects) { CHECK(e->refs == 0); delete e; }   // the flames released what the fake created
    if (failures == 0) std::puts("sc4_fireoccupant: all passed");
    return failures ? 1 : 0;
}
