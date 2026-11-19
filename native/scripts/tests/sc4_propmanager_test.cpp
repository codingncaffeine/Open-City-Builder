#include <cstdio>
#include "rz/cRZString.h"
#include "sc4/PropManager.h"
#include "sc4/SCPropertyHelp.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
// A tiny occupant manager that just keeps inserted occupants in a list.
class MiniManager final : public cISC4OccupantManager {
public:
    OccupantList items;
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool SetCitySize(float, float) override { return true; }
    void GetOccupantManagerCellSizes(float&, float&) const override {}
    void GetWorldCellCount(int&, int&) const override {}
    void GetOccupantManagerCellCount(int&, int&) const override {}
    void GetOccupantManagerCellBounds(int&, int&, int&, int&) const override {}
    void WorldCellToOccupantManagerCell(int, int, int&, int&) const override {}
    void OccupantManagerCellToWorldCell(int, int, int&, int&) const override {}
    void WorldCellToStandardCityCell(int, int, int&, int&) const override {}
    void StandardCityCellToWorldCell(int, int, int&, int&) const override {}
    void WorldCellToPosition(int, int, float&, float&) const override {}
    void PositionToWorldCell(float, float, int&, int&) const override {}
    void OccupantManagerCellToPosition(int, int, float&, float&) const override {}
    void PositionToOccupantManagerCell(float, float, int&, int&) const override {}
    void StandardCityCellToPosition(int, int, float&, float&) const override {}
    void PositionToStandardCityCell(float, float, int&, int&) const override {}
    bool InsertOccupant(cISC4Occupant* o, std::uint32_t) override { o->AddRef(); items.push_back(o); o->SetFlag(1); return true; }
    bool RemoveOccupant(cISC4Occupant* o, bool, std::uint32_t) override {
        for (auto it = items.begin(); it != items.end(); ++it)
            if (*it == o) { items.erase(it); o->ClearFlag(1); o->Release(); return true; }
        return false;
    }
    bool RemoveOccupants(int, bool, std::uint32_t) override { return false; }
    bool RemoveOccupants(cISC4OccupantFilter*, bool, std::uint32_t) override { return false; }
    bool MoveOccupant(cISC4Occupant*, bool) override { return false; }
    bool IsCellEmpty(int, int) const override { return true; }
    int GetBoundingCells(int, int, int (*)[2], int&) override { return 0; }
    bool FindOccupant(cISC4Occupant*, bool, int*, int*) override { return false; }
    bool GetFirstOccupantByPosition(cISC4Occupant*&, float, float, std::uint32_t) override { return false; }
    bool GetFirstOccupantByPosition(cISC4Occupant*&, float, float, cISC4OccupantFilter*) override { return false; }
    bool GetFirstOccupant(cISC4Occupant*&, int, int, std::uint32_t) override { return false; }
    bool GetFirstOccupant(cISC4Occupant*&, int, int, cISC4OccupantFilter*) override { return false; }
    bool GetFirstOccupantByStandardCityCell(cISC4Occupant*&, int, int, std::uint32_t) override { return false; }
    bool GetFirstOccupantByStandardCityCell(cISC4Occupant*&, int, int, cISC4OccupantFilter*) override { return false; }
    bool GetFirstOccupantByStandardCityCells(cISC4Occupant*&, const int*, const int*, std::uint32_t) override { return false; }
    bool GetFirstOccupantByStandardCityCells(cISC4Occupant*&, const int*, const int*, cISC4OccupantFilter*) override { return false; }
    bool GetOccupantsByBBox(OccupantList& out, const float* lo, const float* hi, std::uint32_t, std::uint32_t) override { return Box(out, lo, hi, nullptr); }
    bool GetOccupantsByBBox(OccupantList& out, const float* lo, const float* hi, cISC4OccupantFilter* f, std::uint32_t) override { return Box(out, lo, hi, f); }
    bool GetOccupantsByOccupantManagerCells(OccupantList&, const int*, const int*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByOccupantManagerCells(OccupantList&, const int*, const int*, cISC4OccupantFilter*, std::uint32_t) override { return false; }
    bool GetOccupantsByStandardCityCells(OccupantList& out, const int*, const int*, std::uint32_t, std::uint32_t) override { return All(out, nullptr); }
    bool GetOccupantsByStandardCityCells(OccupantList& out, const int*, const int*, cISC4OccupantFilter* f, std::uint32_t) override { return All(out, f); }
    bool IterateOccupantsByBBox(OccupantCallback, void*, const float*, const float*, std::uint32_t) override { return false; }
    bool IterateOccupantsByBBox(OccupantCallback, void*, const float*, const float*, cISC4OccupantFilter*) override { return false; }
    bool IterateOccupants(OccupantCallback, void*, const int*, const int*, std::uint32_t) override { return false; }
    bool IterateOccupants(OccupantCallback, void*, const int*, const int*, cISC4OccupantFilter*) override { return false; }
    bool IterateOccupantsByStandardCityCell(OccupantCallback, void*, const int*, const int*, cISC4OccupantFilter*) override { return false; }
    bool IterateOccupantsByStandardCityCell(OccupantCallback, void*, const int*, const int*, std::uint32_t) override { return false; }
    void ReleaseOccupantList(OccupantList& list) override { for (auto* o : list) o->Release(); list.clear(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
private:
    bool All(OccupantList& out, cISC4OccupantFilter* f) {
        for (auto* o : items)
            if (!f || f->IsOccupantIncluded(o)) { o->AddRef(); out.push_back(o); }
        return !out.empty();
    }
    bool Box(OccupantList& out, const float* lo, const float* hi, cISC4OccupantFilter* f) {
        for (auto* o : items) {
            float p[3];
            o->GetPosition(p);
            if (p[0] < lo[0] || p[0] > hi[0] || p[2] < lo[2] || p[2] > hi[2]) continue;
            if (!f || f->IsOccupantIncluded(o)) { o->AddRef(); out.push_back(o); }
        }
        return !out.empty();
    }
};
class Host final : public IPropHost {
public:
    MiniManager manager;
    int ClockHour() override { return 12; }
    int ClockMinute() override { return 0; }
    void SimulatorDate(cGZDate& out) override { out.Set(1, 1, 2005); }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4OccupantManager* OccupantManager() override { return &manager; }
};
class PMHost final : public IPropManagerHost {
public:
    cSCPropertyHolder exemplar;
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        cGZPersistResourceKey key;
        key.instance = 0x1001;
        fn(key);
    }
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey&) override { return &exemplar; }
    void ReleaseExemplar(cISCPropertyHolder*) override {}
    cSC4PropOccupant* CreateRegisteredProp(std::uint32_t) override { return nullptr; }
    cSC4PropOccupant* CreateTerrainDecalProp() override { return nullptr; }
    cSC4PropOccupant* CreateVisualEffectProp() override { return nullptr; }
    void PostPropExemplarDataUpdated() override { ++posted; }
    void PositionToCityCell(float x, float z, int& cx, int& cz) override { cx = static_cast<int>(x / 16); cz = static_cast<int>(z / 16); }
    void CityCellToPosition(int cx, int cz, float& x, float& z) override { x = cx * 16.0f; z = cz * 16.0f; }
    cSC4Lot* LotAtCell(int, int) override { return nullptr; }
    void CreateTextDecal(const std::vector<std::uint32_t>&) override {}
    int posted = 0;
};
}
int main() {
    Host host;
    SetPropHost(&host);
    PMHost pmh;
    pmh.exemplar.AddRef();
    cSCPropertyHelp::SetPropertyValue(&pmh.exemplar, 0x10, static_cast<std::uint32_t>(kPropExemplarType));
    cSCPropertyHelp::SetPropertyValue(&pmh.exemplar, kPropPropertySize, std::vector<float>{2, 3, 4});
    cSCPropertyHelp::SetPropertyValue(&pmh.exemplar, kPropPropertyFamilies, std::vector<std::uint32_t>{0x500, 0x501});
    SetPropManagerHost(&pmh);

    auto* mgr = new cSC4PropManager();
    mgr->AddRef();
    CHECK(mgr->Init() && pmh.posted == 1);
    CHECK(mgr->IsPropTypeValid(0x1001) && !mgr->IsPropTypeValid(0x1002));
    CHECK(mgr->IsPropFamilyTypeValid(0x500) && mgr->IsPropInFamily(0x1001, 0x501) && mgr->IsPropInFamily(7, 7));
    std::vector<std::uint32_t> members;
    CHECK(mgr->GetPropTypesByFamily(members, 0x500) == 1 && members[0] == 0x1001);
    const PropProfile* profile = mgr->GetPropProfilePtr(0x1001);
    CHECK(profile && profile->size[0] == 2 && profile->size[2] == 4 && profile->propClass == kPropClassDefault);

    cSC4PropOccupant* prop = nullptr;
    CHECK(mgr->CreateProp(0x1001, prop) && prop && prop->GetPropType() == 0x1001);
    float lo[3], hi[3];
    prop->GetBoundingBox(lo, hi);
    CHECK(hi[0] - lo[0] == 2 && hi[2] - lo[2] == 4);
    prop->Release();

    const float pos[3] = {40, 0, 56};
    CHECK(mgr->AddCityProp(0x1001, pos, 1));
    CHECK(mgr->AddStreetProp(0x1001, pos, 0));
    auto* lotProp = mgr->AddLotProp(0x1001, pos, 2);
    CHECK(lotProp && lotProp->GetPropUsage() == 2);
    lotProp->Release();
    CHECK(host.manager.items.size() == 3);

    OccupantList list;
    const int cells[4] = {0, 0, 10, 10};
    CHECK(mgr->GetCityProps(list, cells) && list.size() == 1);
    host.manager.ReleaseOccupantList(list);
    CHECK(mgr->GetStreetProps(list, 2, 3) == false && list.size() == 1);   // position 40,56 -> cell (2,3)
    host.manager.ReleaseOccupantList(list);
    mgr->GetStreetProps(list, 0, 0);
    CHECK(list.empty());
    mgr->RemoveStreetPropsFromTile(2, 3);
    CHECK(host.manager.items.size() == 2);

    // building props
    cSC4PropOccupant* building = new cSC4PropOccupant();
    building->AddRef();
    cSC4PropOccupant* extra = nullptr;
    CHECK(mgr->CreateProp(0x1001, extra));
    CHECK(mgr->AddBuildingProp(building, extra));
    OccupantList bprops;
    CHECK(mgr->GetBuildingProps(bprops, building) && bprops.size() == 1 && extra->GetPropUsage() == 3);
    host.manager.ReleaseOccupantList(bprops);
    CHECK(mgr->RemoveBuildingProp(building, extra));
    CHECK(!mgr->GetBuildingProps(bprops, building));
    extra->Release();
    building->Release();

    // remove the rest through the city rectangle
    const float all[4] = {0, 0, 1000, 1000};
    CHECK(mgr->RemoveCityProps(all));
    mgr->RemoveLotProps(nullptr);
    {   // reloading all prop exemplars announces the update; a new view filter replaces the old one with the counts kept
        const int before = pmh.posted;
        mgr->LoadAllStandardPropExemplarData();
        CHECK(pmh.posted == before + 1);
        struct Filter final : IPropViewFilter {
            int refs = 0;
            std::uint32_t AddRef() override { return ++refs; }
            std::uint32_t Release() override { return --refs; }
        } first, second;
        PropViewFilterInformation none, a, b;
        a.filter = &first; b.filter = &second;
        mgr->DoPostViewFilterChange(&none, &a);
        CHECK(first.refs == 1);
        mgr->DoPostViewFilterChange(&a, &a);                                      // same filter: nothing changes
        CHECK(first.refs == 1);
        mgr->DoPostViewFilterChange(&a, &b);
        CHECK(first.refs == 0 && second.refs == 1);
        mgr->DoPostViewFilterChange(&b, &none);                                   // a null filter drops the held one
        CHECK(second.refs == 0);
    }
    mgr->Release();
    host.manager.ReleaseOccupantList(host.manager.items);   // the fake manager holds one reference per inserted prop
    cSC4PropManager::StaticShutdown();
    std::puts(failures ? "sc4_propmanager: FAILED" : "sc4_propmanager ok");
    return failures != 0;
}
