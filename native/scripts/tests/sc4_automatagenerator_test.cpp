// cSC4AutomataGenerator: time maths, expiry, template setup, ring cell search, spawning, ticks, streams.
// Expected values are derived by hand from the decompiled formulas (0x1000fa3e0..0x1000fbbe0).
#include <cmath>
#include <cstdio>
#include <list>
#include <set>

#include "rz/PortMemoryStreams.h"
#include "sc4/AutomataGenerator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECKF(a, b) do { if (std::fabs((double)(a) - (double)(b)) > 1e-4) { std::printf("FAIL line %d: %s=%g != %g\n", __LINE__, #a, (double)(a), (double)(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

struct FakeAutomaton : IAutomaton {
    int refs = 1, detached = 0, released = 0, startBehavior = 0, startDefault = 0;
    float pos[3] = {0, 0, 0};
    bool callbackSet = false;
    void AddRef() override { ++refs; }
    void Release() override { --refs; ++released; }
    void DetachController(void*) override { ++detached; }
    void AddCallback(const void* target) override { callbackSet = target != nullptr; }
    void StartBehavior(const void*, cSC4AutomataGenerator*) override { ++startBehavior; }
    void StartDefault(float speed, cSC4AutomataGenerator*) override { ++startDefault; CHECKF(speed, -1.0f); }
};

struct FakeManager : IAutomatonManager {
    std::vector<FakeAutomaton*> made;
    std::vector<std::uint32_t> groups;
    std::uint8_t lastArg = 0;
    IAutomaton* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags, const float zero[3], std::uint32_t,
                                std::uint8_t arg) override {
        CHECK(flags == 0x2000002 && zero[0] == 0 && zero[1] == 0 && zero[2] == 0);
        auto* a = new FakeAutomaton();
        for (int i = 0; i < 3; ++i) a->pos[i] = position[i];
        made.push_back(a);
        groups.push_back(group);
        lastArg = arg;
        return a;
    }
};

struct Host : IAutomataGeneratorHost {
    std::set<std::uint32_t> flags;
    float position[3] = {100, 5, 200}, facing[3] = {1, 2, 3};
    float bmin[3] = {32, 0, 32}, bmax[3] = {48, 0, 48};
    bool intersects = true;
    float hit[2] = {100, 200};
    int baseInit = 0, baseShutdown = 0, baseActivate = 0, baseTicks = 0;
    bool hasOccupant = true;
    std::uint32_t occType = kBuildingOccupantType;
    std::set<std::uint32_t> occFlags;
    int facingDir = -1;
    int clockObj = 0;
    float now = 12.0f;
    std::int32_t day = 3;
    bool layerVisible = true;
    int cityCount = 10;
    float measureA = 4, measureB = 6, lotMeasure = 7;
    bool lotPresent = true;
    std::uint32_t lastMask = 0xdead;
    bool networkOk = true;
    float height = 9.0f;
    bool terrain = false;
    float terrainHeight = 33.0f;
    const void* behavior = nullptr;
    FakeManager manager;
    bool managerPresent = true;
    int getNetworksCalls = 0;
    std::vector<std::pair<int, int>> networkQueries;

    bool BaseInit() override { ++baseInit; return true; }
    bool BaseShutdown() override { ++baseShutdown; return true; }
    void BaseActivate(bool) override { ++baseActivate; }
    void BaseDoAnimationTick(float, std::uint32_t, const ControllerTime&) override { ++baseTicks; }
    bool BaseWrite(cIGZOStream& s) override { s.SetUint32(0x77); return true; }
    bool BaseRead(cIGZIStream& s) override { std::uint32_t v = 0; s.GetUint32(v); CHECK(v == 0x77); return true; }
    // the base controller's own state (one list, one count, the age and lifetime, the template's part of SetTemplate)
    std::list<IAutomaton*> automata;
    std::uint32_t liveCount = 0;
    float age = 0.0f, lifetime = 0.0f;
    int baseSetTemplate = 0;
    const GeneratorTemplate* pendingTemplate = nullptr;
    void BaseSetTemplate() override { ++baseSetTemplate; if (pendingTemplate) lifetime = pendingTemplate->lifetime; }
    std::uint32_t BaseLiveCount() override { return liveCount; }
    void BaseAddAutomaton(IAutomaton* a) override { automata.push_back(a); a->AddRef(); ++liveCount; }
    void BaseReleaseAutomata() override {
        for (IAutomaton* a : automata) { a->DetachController(nullptr); a->Release(); }
        automata.clear();
    }
    void BaseSyncLiveCount() override { liveCount = static_cast<std::uint32_t>(automata.size()); }
    float BaseAge() override { return age; }
    float BaseLifetime() override { return lifetime; }
    bool CheckFlag(std::uint32_t f) override { return flags.count(f) != 0; }
    void GetPosition(float out[3]) override { for (int i = 0; i < 3; ++i) out[i] = position[i]; }
    void GetLotFacingPoint(float out[3]) override { for (int i = 0; i < 3; ++i) out[i] = facing[i]; }
    void GetOccupantBoundingBox(float a[3], float b[3]) override { for (int i = 0; i < 3; ++i) { a[i] = bmin[i]; b[i] = bmax[i]; } }
    bool GetBoundingBoxIntersection(const float[2], const float[2], float out[2]) override {
        out[0] = hit[0]; out[1] = hit[1];
        return intersects;
    }
    const void* GetBehaviorTemplate(float, const float[3]) override { return behavior; }
    bool HasOccupant() override { return hasOccupant; }
    std::uint32_t OccupantType() override { return occType; }
    bool OccupantHasFlag(std::uint32_t f) override { return occFlags.count(f) != 0; }
    void OccupantLotCell(int& x, int& z) override { x = 1; z = 2; }
    int OccupantFacing() override { return facingDir; }
    const void* Clock() override { return &clockObj; }
    void ClockNow(float& t, std::int32_t& d) override { t = now; d = day; }
    bool IsAutomataLayerVisible() override { return layerVisible; }
    int CityCount() override { return cityCount; }
    float OccupantMeasureA() override { return measureA; }
    float OccupantMeasureB() override { return measureB; }
    bool LotMeasure(int x, int z, float& out) override { CHECK(x == 1 && z == 2); out = lotMeasure; return lotPresent; }
    bool NetworkCellHasType(int x, int z, std::uint32_t mask) override { lastMask = mask; networkQueries.push_back({x, z}); return true; }
    bool NetworkPositionAccepts(const float[3], std::uint32_t) override { return networkOk; }
    float NetworkHeight(float, float) override { return height; }
    bool TerrainHeightEnabled(float, float) override { return terrain; }
    float TerrainHeight(float, float) override { return terrainHeight; }
    IAutomatonManager* ManagerForGroup(std::uint32_t) override { return managerPresent ? &manager : nullptr; }
};

GeneratorTemplate MakeTemplate() {
    GeneratorTemplate t;
    t.minRadius = 10.0f;
    t.radius = 30.0f;
    const float curve[4] = {0, 5, 24, 5};
    t.spawnCurve.SetResponseCurveData(curve, 4);
    t.groups = {5, 6};
    t.countScale = 1.0f;
    t.countMode = 0;
    t.groupSizeMin = 3;
    t.groupSizeMax = 4;   // RandomSint32RangeUniform(3, 4) == 3
    t.rate = 2.0f;
    t.periodUnit = 1;
    t.probability = 1.0f;
    t.maxAutomata = 0;
    t.spawnArgument = 9;
    t.lifetime = 8.0f;
    t.field_bc = 0x1234;
    t.lifetimeAutomataLimit = 0;
    return t;
}

void TestTime() {
    ControllerTime t;
    t.time = 1.0f;
    t += 25.0f;   // 25 -> 1 with one more day, then added to 1
    CHECKF(t.time, 2.0f);
    CHECK(t.days == 1);
    t += -1.0f;   // -1 -> 23, one day back
    CHECKF(t.time, 25.0f);   // no re-normalisation of the sum (kept)
    CHECK(t.days == 0);
    ControllerTime u;
    u += 72.0f;
    CHECKF(u.time, 0.0f);
    CHECK(u.days == 3);
    CHECK(AutomataFloorIdiom(2.5f) == 2);
    CHECK(AutomataFloorIdiom(-0.5f) == -1);
    CHECK(AutomataFloorIdiom(3.0f) == 3);
    CHECK(AutomataFloorIdiom(-3.0f) == -3);
    CHECK(AutomataFloorIdiom(0.0f) == 0);
    CHECK(AutomataFloorIdiom(0.9999f) == 0);
    CHECK(AutomataRoundMagic(2.5f) == 2 && AutomataRoundMagic(3.5f) == 4 && AutomataRoundMagic(-2.5f) == -2);
}

void TestBasics() {
    Host host;
    auto* g = new cSC4AutomataGenerator(&host);
    CHECK(g->GetGZCLSID() == 0x4a1628c5 && g->GetControllerType() == 0x4a1628c5);
    CHECK(g->ReadyToExpire());
    CHECKF(g->GetRadius(), 0.0f);
    CHECK(g->Init() && host.baseInit == 1);   // not initialised: forwards to the base Init
    g->initialized = true;
    CHECK(!g->Init() && host.baseInit == 1);
    CHECK(g->Shutdown() && host.baseShutdown == 1);

    GeneratorTemplate t = MakeTemplate();
    host.pendingTemplate = &t;
    g->SetTemplate(&t);
    CHECK(host.baseSetTemplate == 1);       // the base's part (name, lifetime, flags) is the base's
    CHECKF(host.lifetime, 8.0f);
    CHECK(g->GroupLimit() == 3 && g->LifetimeLimit() == 0);
    CHECKF(g->GetRadius(), 30.0f);
    t.lifetimeAutomataLimit = 2;
    g->SetTemplate(&t);
    CHECK(g->LifetimeLimit() == 2);
    g->SetTemplate(nullptr);
    CHECK(g->Template() == nullptr && host.baseSetTemplate == 2);   // a null template never reaches the base's part
    g->SetTemplate(&t);
    g->SetLifetimeAutomataCount(0);

    // IsExpired
    host.age = 1.0f;
    CHECK(!g->IsExpired());                 // elapsed < lifetime, no limit
    host.age = 8.0f;
    CHECK(g->IsExpired());                  // lifetime reached
    g->SetLifetimeAutomataCount(0);
    host.age = 0.0f;
    g->SetLifetimeAutomataCount(1);
    CHECK(!g->IsExpired());                 // limit 1, none spawned
    g->SetAutomataGroups({1, 2, 3});
    CHECK(t.groups.size() == 3 && t.groups[2] == 3);   // writes into the template

    // IsTickable forwards flag 1
    CHECK(!g->IsTickable());
    host.flags.insert(1);
    CHECK(g->IsTickable());

    // CheckTimeOfDay adds the delta
    ControllerTime d;
    d.time = 3.0f;
    d.days = 2;
    g->CheckTimeOfDay(ControllerTime(), d);
    CHECKF(g->Elapsed().time, 3.0f);
    CHECK(g->Elapsed().days == 2);
    g->Release();   // count 0 -> final release: Shutdown + delete
    CHECK(host.baseShutdown == 2);

    auto* h = new cSC4AutomataGenerator(&host);
    h->AddRef();
    h->AddRef();
    CHECK(h->Release() == 1);
    CHECK(h->Release() == 0);   // final
}

void TestNetworks() {
    Host host;
    cSC4AutomataGenerator g(&host);
    GeneratorTemplate t = MakeTemplate();
    t.minRadius = 16.0f;
    t.radius = 32.0f;
    g.SetTemplate(&t);
    std::vector<CellPoint> cells;
    g.GetNetworksInRadius(cells, 0xc49);
    // outer x,z: floor((32-32)/16)=0 .. floor((48+32)/16)=5; inner: floor(16/16)=1 .. floor(64/16)=4; excluded: x,z in {2,3}
    CHECK(cells.size() == 32);
    CHECK(host.lastMask == 0xc49);
    CHECK(cells.front().x == 0 && cells.front().z == 0);
    for (auto& c : cells) CHECK(!((c.x == 2 || c.x == 3) && (c.z == 2 || c.z == 3)));

    // facing 0 keeps only z <= innerMinZ (1): z in 0..1, six cells each
    host.flags.insert(kAutoFlagLotFacing);
    host.facingDir = 0;
    cells.clear();
    g.GetNetworksInRadius(cells, 2);
    CHECK(cells.size() == 12);
    for (auto& c : cells) CHECK(c.z <= 1);
    // facing 1: outerMinX = innerMaxX (4): x in 4..5
    host.facingDir = 1;
    cells.clear();
    g.GetNetworksInRadius(cells, 2);
    CHECK(cells.size() == 12);
    for (auto& c : cells) CHECK(c.x >= 4);
    // facing 2: z in 4..5
    host.facingDir = 2;
    cells.clear();
    g.GetNetworksInRadius(cells, 2);
    CHECK(cells.size() == 12);
    for (auto& c : cells) CHECK(c.z >= 4);
    // facing 3: x in 0..1
    host.facingDir = 3;
    cells.clear();
    g.GetNetworksInRadius(cells, 2);
    CHECK(cells.size() == 12);
    for (auto& c : cells) CHECK(c.x <= 1);
    // equal radii: the inner margin is zero -> inner box is the bounding box cells only
    host.flags.clear();
    t.minRadius = t.radius = 16.0f;
    cells.clear();
    g.GetNetworksInRadius(cells, 2);
    // outer 1..4 square (16 cells); inner 2..3: excluded where 3 > z > 2 never -> all 16 (z in {2,3}: 3<=z or z<=2)
    CHECK(cells.size() == 16);
}

void TestPolarSpawn() {
    Host host;
    auto* g = new cSC4AutomataGenerator(&host);
    g->Random().Seed(1234);
    GeneratorTemplate t = MakeTemplate();
    g->SetTemplate(&t);
    // mode 0: round(5 * 1.0) = 5 attempts; group limit 3 -> maxLive 3: spawn while live <= 3, i.e. four automata (quirk)
    g->TriggerGenerator(5.0f);
    CHECK(host.manager.made.size() == 4);
    CHECK(g->LiveCount() == 4 && host.automata.size() == 4);
    CHECK(host.manager.lastArg == 9);
    CHECK(g->Group() == 5 || g->Group() == 6);
    for (std::size_t i = 0; i < host.manager.made.size(); ++i) {
        FakeAutomaton* a = host.manager.made[i];
        CHECK(a->refs == 2 && a->callbackSet && a->startDefault == 1 && a->startBehavior == 0);
        // the hit point is (100, 200); the spawn point lies minRadius..radius away from it in the xz plane, y = origin y
        const float dx = a->pos[0] - 100.0f, dz = a->pos[2] - 200.0f;
        const float dist = std::sqrt(dx * dx + dz * dz);
        CHECK(dist >= 10.0f - 1e-2f && dist <= 30.0f + 1e-2f);
        CHECKF(a->pos[1], 5.0f);
    }

    // terrain height replaces y, and a behaviour template switches the start call
    Host h2;
    h2.terrain = true;
    h2.behavior = &h2;
    auto* g2 = new cSC4AutomataGenerator(&h2);
    g2->Random().Seed(5);
    GeneratorTemplate t2 = MakeTemplate();
    t2.groupSizeMin = 1; t2.groupSizeMax = 2;   // limit 1 -> maxLive from maxAutomata (0 -> unlimited)
    g2->SetTemplate(&t2);
    g2->TriggerGenerator(2.0f);
    CHECK(h2.manager.made.size() == 2);
    for (auto* a : h2.manager.made) { CHECKF(a->pos[1], 33.0f); CHECK(a->startBehavior == 1 && a->startDefault == 0); }
    // no intersection: the base point is the origin position
    h2.intersects = false;
    h2.manager.made.clear();
    g2->TriggerGenerator(1.0f);
    CHECK(h2.manager.made.size() == 1);
    {
        FakeAutomaton* a = h2.manager.made[0];
        const float dx = a->pos[0] - 100.0f, dz = a->pos[2] - 200.0f;
        const float dist = std::sqrt(dx * dx + dz * dz);
        CHECK(dist >= 10.0f - 1e-2f && dist <= 30.0f + 1e-2f);
    }
    // missing manager stops the loop; missing clock/template do nothing
    h2.managerPresent = false;
    h2.manager.made.clear();
    g2->TriggerGenerator(3.0f);
    CHECK(h2.manager.made.empty());

    // lifetime limit: stops after two spawned and reports expiry
    Host h3;
    auto* g3 = new cSC4AutomataGenerator(&h3);
    g3->Random().Seed(9);
    GeneratorTemplate t3 = MakeTemplate();
    t3.lifetimeAutomataLimit = 2;
    t3.groupSizeMin = 1; t3.groupSizeMax = 2;
    g3->SetTemplate(&t3);
    g3->TriggerGenerator(10.0f);
    CHECK(h3.manager.made.size() == 2 && g3->SpawnedCount() == 2 && g3->IsExpired());

    // flags 0x80 / 0x800 demand network cells; none means no spawn
    Host h4;
    h4.flags.insert(kAutoFlagLocalRoads);
    auto* g4 = new cSC4AutomataGenerator(&h4);
    GeneratorTemplate t4 = MakeTemplate();
    g4->SetTemplate(&t4);
    h4.bmin[0] = 1e9f; h4.bmax[0] = 1e9f + 16;   // ring cells exist but report no network? use a false query via empty ring
    g4->TriggerGenerator(1.0f);
    CHECK(h4.lastMask == 2);

    // delete (releases nothing: fake automata are heap-owned by the test)
    for (auto* a : host.manager.made) delete a;
    for (auto* a : h2.manager.made) delete a;
    for (auto* a : h3.manager.made) delete a;
    for (auto* a : h4.manager.made) delete a;
    delete g; delete g2; delete g3; delete g4;
}

void TestModes() {
    // mode 2: round-even(10 * 0.25 = 2.5) = 2
    Host host;
    auto* g = new cSC4AutomataGenerator(&host);
    g->Random().Seed(3);
    GeneratorTemplate t = MakeTemplate();
    t.countMode = 2;
    t.countScale = 0.25f;
    t.groupSizeMin = 1; t.groupSizeMax = 2;
    g->SetTemplate(&t);
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.size() == 2);
    // mode 1: not a building -> nothing
    host.manager.made.clear();
    t.countMode = 1;
    t.countScale = 0.3f;
    host.occType = 1;
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.empty());
    // building with lot measure 7 * 0.3 = 2.1 -> 2
    host.occType = kBuildingOccupantType;
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.size() == 2);
    // flag 0x1503 uses measure A (4 * 0.3 = 1.2 -> 1)
    host.manager.made.clear();
    host.occFlags.insert(0x1503);
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.size() == 1);
    // flag 0x1507 uses measure B (6 * 0.3 = 1.8 -> 2) once 0x1503/0x1504 are absent
    host.manager.made.clear();
    host.occFlags.clear();
    host.occFlags.insert(0x1507);
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.size() == 2);
    // no lot: nothing
    host.manager.made.clear();
    host.occFlags.clear();
    host.lotPresent = false;
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.empty());
    // unknown mode: nothing
    t.countMode = 7;
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.empty());
    // mode 0 with a zero product still spawns one
    t.countMode = 0;
    host.lotPresent = true;
    g->TriggerGenerator(0.0f);
    CHECK(host.manager.made.size() == 1);
    for (auto* a : host.manager.made) delete a;
    delete g;
}

void TestRingSpawn() {
    // network-cell spawn: cells exist, the position is accepted and the height comes from the network manager
    Host host;
    host.flags.insert(kAutoFlagNoNetworks);
    auto* g = new cSC4AutomataGenerator(&host);
    g->Random().Seed(77);
    GeneratorTemplate t = MakeTemplate();
    t.minRadius = 16.0f;
    t.radius = 32.0f;
    t.groupSizeMin = 1; t.groupSizeMax = 2;
    g->SetTemplate(&t);
    host.intersects = false;
    g->TriggerGenerator(3.0f);
    CHECK(host.lastMask == 0xc49);
    CHECK(host.manager.made.size() == 3);
    for (auto* a : host.manager.made) {
        CHECKF(a->pos[1], 9.0f);
        // inside one of the ring cells' 16x16 area (cells 0..5), allowing the clamp to push it out
        CHECK(a->pos[0] > -100 && a->pos[0] < 200 && a->pos[2] > -100 && a->pos[2] < 200);
    }
    // a rejected position never spawns
    host.manager.made.clear();
    host.networkOk = false;
    g->TriggerGenerator(3.0f);
    CHECK(host.manager.made.empty());
    for (auto* a : host.manager.made) delete a;
    delete g;
}

void TestTick() {
    Host host;
    host.flags.insert(kAutoFlagVisible);
    host.manager.groups.clear();
    auto* g = new cSC4AutomataGenerator(&host);
    g->Random().Seed(21);
    GeneratorTemplate t = MakeTemplate();
    t.groupSizeMin = 1; t.groupSizeMax = 2;
    g->SetTemplate(&t);
    ControllerTime dt;
    dt.time = 0.25f;
    g->DoAnimationTick(1.0f, 0, dt);
    CHECK(host.baseTicks == 1);
    // period 0 <= elapsed 0.25: fires (curve 5 -> 5 attempts, limit 1) and sets the period 1 / 2 = 0.5 hours
    CHECK(host.manager.made.size() == 5);
    CHECKF(g->Period().time, 0.5f);
    CHECKF(g->Elapsed().time, 0.0f);
    // second tick: elapsed 0.25 < 0.5 -> nothing
    g->DoAnimationTick(1.0f, 0, dt);
    CHECK(host.manager.made.size() == 5);
    CHECKF(g->Elapsed().time, 0.25f);
    // third tick reaches the period exactly and fires again
    g->DoAnimationTick(1.0f, 0, dt);
    CHECK(host.manager.made.size() == 10);
    // day mask excluding day 3 skips triggering but still re-arms the period
    t.dayMask = 0xfffffff7;
    g->DoAnimationTick(1.0f, 0, dt);
    g->DoAnimationTick(1.0f, 0, dt);
    CHECK(host.manager.made.size() == 10);
    CHECKF(g->Elapsed().time, 0.0f);
    // invisible layer / suspended flag / missing visible flag: no accumulation
    host.layerVisible = false;
    g->DoAnimationTick(1.0f, 0, dt);
    CHECKF(g->Elapsed().time, 0.0f);
    host.layerVisible = true;
    host.flags.insert(kAutoFlagSuspended);
    g->DoAnimationTick(1.0f, 0, dt);
    CHECKF(g->Elapsed().time, 0.0f);
    host.flags.erase(kAutoFlagSuspended);
    host.flags.erase(kAutoFlagVisible);
    g->DoAnimationTick(1.0f, 0, dt);
    CHECKF(g->Elapsed().time, 0.0f);
    host.flags.insert(kAutoFlagVisible);
    // expired generators do nothing past the base tick
    host.lifetime = 8.0f;
    host.age = 100.0f;
    const int ticks = host.baseTicks;
    g->DoAnimationTick(1.0f, 0, dt);
    CHECK(host.baseTicks == ticks + 1 && g->Elapsed().time == 0.0f);
    // period unit out of range gives a zero period
    host.age = 0.0f;
    t.periodUnit = 9;
    t.dayMask = 0xffffffff;
    for (int i = 0; i < 2; ++i) g->DoAnimationTick(1.0f, 0, dt);
    CHECKF(g->Period().time, 0.0f);
    // curve of zero never triggers
    const float zero[4] = {0, 0, 24, 0};
    t.spawnCurve.SetResponseCurveData(zero, 4);
    const std::size_t before = host.manager.made.size();
    g->DoAnimationTick(1.0f, 0, dt);
    CHECK(host.manager.made.size() == before);
    for (auto* a : host.manager.made) delete a;
    delete g;
}

void TestActivateAndStreams() {
    Host host;
    auto* g = new cSC4AutomataGenerator(&host);
    GeneratorTemplate t = MakeTemplate();
    g->SetTemplate(&t);
    g->initialized = true;
    g->Random().Seed(2);
    g->TriggerGenerator(2.0f);
    CHECK(g->LiveCount() == 2);
    // activate(true) just forwards
    g->Activate(true);
    CHECK(g->LiveCount() == 2 && host.baseActivate == 1);
    // deactivate without flag 0x100: live count mirrors the list size, automata kept
    g->Activate(false);
    CHECK(g->LiveCount() == 2 && host.automata.size() == 2);
    // an automaton the base took off its list (it was removed from the city) frees a place: the count is the base's
    host.automata.pop_back();
    host.liveCount = 1;
    CHECK(g->LiveCount() == 1);
    host.automata.push_back(host.manager.made.back());
    host.liveCount = 2;
    // with flag 0x100 the automata are detached and released
    host.flags.insert(kAutoFlagReleaseOnDeactivate);
    auto made = host.manager.made;
    g->Activate(false);
    CHECK(g->LiveCount() == 0 && host.automata.empty());
    for (auto* a : made) CHECK(a->detached == 1 && a->released == 1);

    // streams
    g->MutableElapsed().time = 1.5f; g->MutableElapsed().days = -2;
    g->MutablePeriod().time = 0.75f; g->MutablePeriod().days = 4;
    PortMemoryOStream os;
    CHECK(g->Write(os));
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    Host h2;
    cSC4AutomataGenerator other(&h2);
    CHECK(other.Read(is));
    CHECKF(other.Elapsed().time, 1.5f);
    CHECK(other.Elapsed().days == -2 && other.Period().days == 4);
    CHECKF(other.Period().time, 0.75f);
    CHECK(other.Group() == g->Group() && other.GroupSpawned() == g->GroupSpawned());
    // version 2 bytes: 0x0004 little endian first, then base marker
    CHECK(os.bytes()[0] == 4 && os.bytes()[1] == 0 && os.bytes()[2] == 0x77);
    std::uint8_t bad[] = {5, 0};
    PortMemoryIStream badStream(bad, sizeof bad);
    CHECK(!other.Read(badStream));
    for (auto* a : made) delete a;
    delete g;
}

}  // namespace

int main() {
    TestTime();
    TestBasics();
    TestNetworks();
    TestPolarSpawn();
    TestModes();
    TestRingSpawn();
    TestTick();
    TestActivateAndStreams();
    std::printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures != 0;
}
