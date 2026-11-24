// cSC4CoverageRadiusEffectHelper (i386 0x00511918..0x00512056): the view's coverage rings of the existing police, fire,
// health and education buildings. Fake city: buildings per category with a radius, lot centres, effects that record what
// they were told.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "rz/cRZVariant.h"
#include "sc4/CoverageRadiusEffectHelper.h"
#include "sc4/Occupant.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

struct TestBuilding final : sc4::cISC4Occupant {
    int refs = 1;
    std::uint32_t type = 0x278128a0;
    float position[3] = {0, 0, 0};
    cSCPropertyHolder holder;
    TestBuilding(float x, float z) { position[0] = x; position[2] = z; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4Occupant) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof(position)); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }

    void SetUint(std::uint32_t id, std::uint32_t value) { cRZVariant v(value); holder.AddProperty(id, &v, false); }
    std::uint32_t Uint(std::uint32_t id) { std::uint32_t v = 0; cSCPropertyHelp::GetPropertyValue(&holder, id, v); return v; }
};

struct Effect final : ICoverageRingEffect {
    std::string name;
    bool started = false;
    int starts = 0, stops = 0, transforms = 0;
    cS3DTransform last;
    static int alive;
    explicit Effect(const char* n) : name(n) { ++alive; }
    ~Effect() override { --alive; }
    void Start(int) override { started = true; ++starts; }
    void Stop(int) override { started = false; ++stops; }
    bool IsStarted() override { return started; }
    void SetEffectTransform(const cS3DTransform& t) override { last = t; ++transforms; }
};
int Effect::alive = 0;

struct City final : ICoverageRadiusHost {
    std::map<int, std::vector<TestBuilding>> buildings;   // category -> list (with hasRadius/radius below)
    std::set<sc4::cISC4Occupant*> police, fire, noLot;
    std::map<sc4::cISC4Occupant*, std::pair<bool, float>> radius;
    std::vector<float> shares[4];
    std::vector<Effect*> made;
    bool refuseEffects = false;
    bool IsPoliceStation(sc4::cISC4Occupant* o) override { return police.count(o) != 0; }
    bool IsFireStation(sc4::cISC4Occupant* o) override { return fire.count(o) != 0; }
    bool Buildings(int category, float share, std::vector<ICoverageRadiusHost::Building>& out) override {
        shares[category].push_back(share);
        out.clear();
        for (TestBuilding& b : buildings[category]) {
            ICoverageRadiusHost::Building entry;
            entry.occupant = &b;
            b.AddRef();
            entry.hasRadius = radius[&b].first;
            entry.radius = radius[&b].second;
            out.push_back(entry);
        }
        return true;
    }
    bool LotCenter(sc4::cISC4Occupant* o, float c[3]) override {
        if (noLot.count(o)) return false;
        o->GetPosition(c);
        return true;
    }
    ICoverageRingEffect* CreateEffect(const char* name) override {
        if (refuseEffects) return nullptr;
        made.push_back(new Effect(name));
        return made.back();
    }
};
}  // namespace

int main() {
    City city;
    SetCoverageRadiusHost(&city);
    auto& police = city.buildings[0];
    police.emplace_back(100.0f, 200.0f);
    police.emplace_back(300.0f, 400.0f);
    auto& schools = city.buildings[3];
    schools.emplace_back(50.0f, 60.0f);
    city.radius[&police[0]] = {true, 40.0f};
    city.radius[&police[1]] = {false, 99.0f};   // refused: the default 32 m
    city.radius[&schools[0]] = {true, 0.5f};    // scale 1: translation only

    // the categories of one building: police / fire by their simulators, health / education by property
    {
        TestBuilding b(0, 0);
        city.police.insert(&b);
        CHECK(cSC4CoverageRadiusEffectHelper::GetCategoriesForOccupant(&b) == 1);
        city.fire.insert(&b);
        b.SetUint(0x091b42a2, 7);
        CHECK(cSC4CoverageRadiusEffectHelper::GetCategoriesForOccupant(&b) == 7);
        b.SetUint(0x691b42b3, 0);   // the property's presence counts, not its value
        CHECK(cSC4CoverageRadiusEffectHelper::GetCategoriesForOccupant(&b) == 15);
        TestBuilding plain(0, 0);       // control
        CHECK(cSC4CoverageRadiusEffectHelper::GetCategoriesForOccupant(&plain) == 0);
    }

    cSC4CoverageRadiusEffectHelper rings;
    // no categories: Init draws nothing and asks no simulator
    rings.Init();
    CHECK(rings.RingCount() == 0 && city.made.empty() && city.shares[0].empty());

    // police: one ring per station, at the lot centre, twice the radius, started; police asked at half capacity
    rings.SetCategories(1);
    rings.RecalcCoverage();
    CHECK(rings.RingCount() == 2 && city.made.size() == 2);
    CHECK(city.shares[0].size() == 1 && city.shares[0][0] == 0.5f && city.shares[3].empty());
    Effect* first = city.made[0];
    Effect* second = city.made[1];
    CHECK(first->name == "PlopMode_Police_Existing" && first->started && first->starts == 1);
    CHECK(first->last.translation[0] == 100.0f && first->last.translation[2] == 200.0f);
    CHECK(first->last.scale == 80.0f && first->last.flags == 3 && first->last.changeCounter == 2);
    CHECK(second->last.scale == 64.0f);   // the refused radius
    // every building reference handed out came back
    CHECK(police[0].refs == 1 && police[1].refs == 1);

    // again: the rings stay (no new effect); a changed radius moves the ring
    city.radius[&police[0]] = {true, 50.0f};
    rings.RecalcCoverage();
    CHECK(city.made.size() == 2 && first->transforms == 2 && first->last.scale == 100.0f && second->transforms == 1);

    // education instead: SetCategories stops and forgets the police rings; schools at 70 %, scale 1 = translation only
    rings.SetCategories(8);
    CHECK(rings.RingCount() == 0 && Effect::alive == 0);
    rings.RecalcCoverage();
    CHECK(rings.RingCount() == 1 && city.made.size() == 3 && city.made[2]->name == "PlopMode_Education_Existing");
    CHECK(city.shares[3].size() == 1 && city.shares[3][0] == 0.7f);
    CHECK(city.made[2]->last.scale == 1.0f && city.made[2]->last.flags == 1);

    // a school that went away: its ring stops and loses its effect, the entry stays
    Effect* school = city.made[2];
    (void)school;
    schools.clear();
    rings.RecalcCoverage();
    CHECK(rings.RingCount() == 1 && Effect::alive == 0);

    // a building without a lot gets no ring; a refused effect makes no entry
    rings.SetCategories(1);
    city.noLot.insert(&police[1]);
    city.refuseEffects = false;
    rings.RecalcCoverage();
    CHECK(rings.RingCount() == 1);
    rings.SetCategories(1);
    city.refuseEffects = true;
    rings.RecalcCoverage();
    CHECK(rings.RingCount() == 0);

    // Shutdown stops every ring
    city.refuseEffects = false;
    city.noLot.clear();
    rings.RecalcCoverage();
    CHECK(rings.RingCount() == 2 && Effect::alive == 2);
    Effect* a = city.made[city.made.size() - 2];
    rings.Shutdown();
    CHECK(rings.RingCount() == 0 && Effect::alive == 0);
    (void)a;
    CHECK(rings.GetCategories() == 1);   // Shutdown keeps the categories

    SetCoverageRadiusHost(nullptr);
    std::printf(failures ? "FAILED (%d)\n" : "sc4_coverageradius ok\n", failures);
    return failures ? 1 : 0;
}
