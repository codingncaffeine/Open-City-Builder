#include <cstdio>
#include "rz/PortMemoryStreams.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4SimGrid.h"
#include "rz/cRZMessage2Standard.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Resources final : public IConstructionResources {
public:
    float total = 100, used = 0;
    float Total() const override { return total; }
    float Used() const override { return used; }
    void Add(float a) override { used += a; }
    void Update() override {}
    int inits = 0;
    void Init() override { ++inits; }
    void SetTotal(float t) override { total = t; }
    void SetUsed(float u) override { used = u; }
};
class Host final : public IBuildingDevelopmentSimHost {
public:
    std::map<std::uint32_t, cSCPropertyHolder*> exemplars;
    int posted = 0;
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        for (auto& kv : exemplars) {
            cGZPersistResourceKey key;
            key.instance = kv.first;
            fn(key);
        }
    }
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) override {
        auto it = exemplars.find(key.instance);
        return it == exemplars.end() ? nullptr : it->second;
    }
    void ReleaseExemplar(cISCPropertyHolder*) override {}
    std::int64_t MaintenanceCost(cISCPropertyHolder*) override { return 25; }
    bool LoadLocalizedName(cISCPropertyHolder*, std::uint32_t, cRZString&) override { return false; }
    void CityCellSize(float& x, float& z) override { x = z = 16.0f; }
    void PostBuildingTypesUpdated() override { ++posted; }
    std::int32_t SimulatorDay() override { return 7; }
    int registered = 0, unregistered = 0;
    std::map<std::uint32_t, float> channels;
    void RegisterMessageTarget(cSC4BuildingDevelopmentSimulator*) override { ++registered; }
    void UnregisterMessageTarget(cSC4BuildingDevelopmentSimulator*) override { ++unregistered; }
    float DemandChannelSupply(std::uint32_t type) override { return channels.count(type) ? channels[type] : 0.0f; }
    ITractDeveloperRef* tract = nullptr;
    ITractDeveloperRef* TractDeveloper() override { return tract; }
    std::uint32_t RandomBelow(std::uint32_t) override { return 0; }
    std::uint64_t BeginGrowSlice() override { return 0; }
    bool GrowSliceExpired(std::uint64_t) override { return false; }
    cSC4SimGrid<std::uint8_t>* CreateByteGrid() override {
        auto* grid = new cSC4SimGrid<std::uint8_t>();
        grid->AddRef();
        grid->Init();
        return grid;
    }
    ChangedLot lot;
    bool lotKnown = true;
    int lotAsks = 0;
    bool MessageLot(cIGZUnknown*, ChangedLot& out) override {
        ++lotAsks;
        out = lot;
        return lotKnown;
    }
    std::vector<std::pair<int, std::uint32_t>> events;
    void PushAdvisorEvent(int advisor, std::uint32_t event) override { events.emplace_back(advisor, event); }
    void LotConfigurations(std::vector<LotConfigurationInfo>& out) override {
        LotConfigurationInfo c;
        c.sizeX = 2; c.sizeZ = 2;
        c.buildingIds = {0x500};              // a family
        c.objectX = 16; c.objectZ = 16;
        c.zoneCompatibility.assign(16, false);
        c.zoneCompatibility[1] = c.zoneCompatibility[2] = true;
        out.push_back(c);
    }
};
cSCPropertyHolder* MakeBuilding(std::uint8_t purpose, std::uint8_t wealth, std::uint16_t capacity, std::uint32_t family,
                                std::vector<float> size) {
    auto* h = new cSCPropertyHolder();
    h->AddRef();
    cSCPropertyHelp::SetPropertyValue(h, 0x10, static_cast<std::uint32_t>(kExemplarTypeBuilding));
    cRZString name;
    name.AssignFromCString("Test building");
    cSCPropertyHelp::SetPropertyValue(h, 0x20, name);
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropWealth, wealth);
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropPurpose, purpose);
    const std::uint32_t dev = cSC4BuildingDevelopmentSimulator::GetDeveloperType_Static(purpose, wealth);
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropCapacities, std::vector<std::uint32_t>{dev, capacity});
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropFamilies, std::vector<std::uint32_t>{family});
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropSize, size);
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropGroups, std::vector<std::uint32_t>{0x1000, 0x1301, 0x9999});
    cSCPropertyHelp::SetPropertyValue(h, kBuildingPropCost, static_cast<std::int64_t>(5000));
    return h;
}
}
namespace {
struct FakeBuilding final : IPlacedBuilding {
    cISCPropertyHolder* PropertyHolder() override { return holder; }
    cISCPropertyHolder* holder = nullptr;
    std::uint32_t type = 0; float mn[3] = {-8, 0, -4}, mx[3] = {8, 5, 4}, pos[3] = {}; int rotation = -1; bool finished = false;
    float setMin[3] = {}, setMax[3] = {};
    void SetBuildingType(std::uint32_t t) override { type = t; }
    void SetBounds(const float a[3], const float b[3]) override { for (int i = 0; i < 3; ++i) { setMin[i] = a[i]; setMax[i] = b[i]; } }
    void SetRotation(int r) override { rotation = r; }
    void FinishPlacement() override { finished = true; }
    void SetPosition(const float p[3]) override { for (int i = 0; i < 3; ++i) pos[i] = p[i]; }
    void GetBounds(float a[3], float b[3]) override { for (int i = 0; i < 3; ++i) { a[i] = mn[i]; b[i] = mx[i]; } }
};
struct FakeFoundation final : IPlacedFoundation {
    cISCPropertyHolder* holder = nullptr; cISCPropertyHolder* exemplar = nullptr; std::uint32_t type = 0;
    float height = -1, size[3] = {}, pos[3] = {}; int style = -1, rotation = -1; bool finished = false;
    cISCPropertyHolder* PropertyHolder() override { return holder; }
    std::uint32_t FoundationType() override { return type; }
    void SetFoundationType(std::uint32_t t) override { type = t; }
    void SetHeight(float h) override { height = h; }
    void SetSize(const float s[3]) override { for (int i = 0; i < 3; ++i) size[i] = s[i]; }
    void SetStyle(int s) override { style = s; }
    void SetRotation(int r) override { rotation = r; }
    void SetPosition(const float p[3]) override { for (int i = 0; i < 3; ++i) pos[i] = p[i]; }
    void ApplyExemplar(cISCPropertyHolder* e) override { exemplar = e; }
    void FinishPlacement() override { finished = true; }
};
struct FakeDeveloper final : IGrowthDeveloperRef {
    int inits = 0, mapUpdates = 0;
    bool finished = true;
    void Init(cISCPropertyHolder*) override { ++inits; }
    void StartMapUpdate() override { ++mapUpdates; }
    bool IsMapUpdateFinished() const override { return finished; }
};
struct FakeServices final : IBuildingPlacementServices {
    FakeBuilding building; bool occupied = false; int added = 0, removed = 0, released = 0; float surface = 33;
    FakeDeveloper developer; int created = 0; float lastRect[4] = {};
    IPlacedBuilding* CreateBuilding() override { return &building; }
    void ReleaseBuilding(IPlacedBuilding*) override { ++released; }
    FakeFoundation foundation; int foundationReleased = 0; float heights[2] = {20, 5}; int lastWhich = -1;
    IPlacedFoundation* CreateFoundation() override { return &foundation; }
    void ReleaseFoundation(IPlacedFoundation*) override { ++foundationReleased; }
    float FoundationSurfaceHeight(const int c[4], int which) override { lastWhich = which; (void)c; return heights[which]; }
    void LoadBuildingProperties(IPlacedBuilding&, std::uint32_t) override {}
    IGrowthDeveloperRef* CreateGrowthDeveloper() override { ++created; return &developer; }
    float SurfaceHeightForFootprint(const float r[4]) override { for (int i = 0; i < 4; ++i) lastRect[i] = r[i]; return surface; }
    void CellToPosition(int x, int z, float& px, float& pz) override { px = x * 16.0f + 8; pz = z * 16.0f + 8; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = int(x / 16); cz = int(z / 16); }
    float TerrainHeight(float, float) override { return 12; }
    bool OccupantAtCell(int, int, std::uint32_t) override { return occupied; }
    bool AddOccupant(IPlacedOccupant*, bool) override { ++added; return true; }
    bool RemoveOccupant(IPlacedOccupant*, bool, bool) override { ++removed; return true; }
};
struct Source final : IDeclineSource { float value; explicit Source(float v) : value(v) {} float Condition() override { return value; } };
struct Tract final : ITractDeveloperRef {
    std::vector<int> results; int calls = 0; int ExecuteOutcome(const TractOutcome&) override { return results[calls++]; }
    std::vector<std::pair<int, int>> grown; int growResult = 1;
    int Grow(IGrowthDeveloperRef*, int x, int z, std::uint8_t) override { grown.push_back({x, z}); return growResult; }
};
struct GrowingDeveloper final : IGrowthDeveloperRef {
    float demand = 50; std::uint32_t type = 0x1010;
    void Init(cISCPropertyHolder*) override {}
    std::uint32_t SubType() const override { return type; }
    bool ListsValid() const override { return true; }
    std::vector<GrowthTract> GrowthList() const override { return {{3, 4, 1}, {7, 8, 2}}; }
    float ActiveDemand() const override { return demand; }
};
struct Filter final : IBuildingTypeFilter { bool Accepts(std::uint32_t t) override { return t == 0x1002; } };
}
int StageReached(const cSC4BuildingDevelopmentSimulator& sim, std::uint32_t type) {
    auto it = sim.StagesReached().find(type);
    return it == sim.StagesReached().end() ? -1 : it->second;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash keeps the failures printed before it
    // developer type / wealth tables
    using S = cSC4BuildingDevelopmentSimulator;
    CHECK(S::GetDeveloperType_Static(1, 1) == 0x1010 && S::GetDeveloperType_Static(1, 3) == 0x1030);
    CHECK(S::GetDeveloperType_Static(2, 2) == 0x3120 && S::GetDeveloperType_Static(3, 2) == 0x3320);
    CHECK(S::GetDeveloperType_Static(3, 1) == 0 && S::GetDeveloperType_Static(5, 0) == 0x4100);
    CHECK(S::GetDeveloperType_Static(8, 3) == 0x4400 && S::GetDeveloperType_Static(4, 1) == 0);
    CHECK(S::GetWealth_Static(0x1010) == 1 && S::GetWealth_Static(0x3320) == 2 && S::GetWealth_Static(0x4400) == 3 &&
          S::GetWealth_Static(0x1234) == 0);

    struct GridCity final : ISimGridCity {
        std::int32_t CitySizeXInCells() const override { return 8; }
        std::int32_t CitySizeZInCells() const override { return 8; }
        float CellSizeXMeters() const override { return 16.0f; }
        float CellSizeZMeters() const override { return 16.0f; }
    } gridCity;
    SetSimGridCity(&gridCity);
    Host host;
    host.exemplars[0x1001] = MakeBuilding(1, 2, 40, 0x500, {16, 10, 16});     // medium-wealth residential, 40 people
    host.exemplars[0x1002] = MakeBuilding(2, 1, 12, 0x500, {80, 10, 80});     // too big for the 2x2 lot
    SetBuildingDevelopmentSimHost(&host);

    auto* sim = new cSC4BuildingDevelopmentSimulator();
    sim->AddRef();
    CHECK(sim->Init() && host.posted == 1);
    CHECK(sim->IsBuildingTypeValid(0x1001) && !sim->IsBuildingTypeValid(0x7777));
    CHECK(sim->IsBuildingFamilyTypeValid(0x500) && sim->IsBuildingInFamily(0x1002, 0x500));
    std::vector<std::uint32_t> members;
    CHECK(sim->GetBuildingTypesByFamily(members, 0x500) == 2);
    const BuildingProfile* profile = sim->GetBuildingProfilePtr(0x1001);
    CHECK(profile && profile->purpose == 1 && profile->wealth == 2 && profile->capacity == 40 &&
          profile->capacities.size() == 1 && profile->capacities[0].at(0x1020) == 40);
    CHECK(profile->growthStage == 30 && profile->size[0] == 16 && profile->size[2] == 16 && profile->cost == 5000 &&
          profile->maintenance == 25);
    CHECK((profile->categoryFlags & 1) && (profile->categoryFlags & (1u << 8)) && !(profile->categoryFlags & (1u << 7)));
    CHECK(std::string(sim->GetExemplarName(0x1001).ToChar()) == "Test building");
    CHECK(std::string(sim->GetBuildingName(0x1001).ToChar()) == "Building");          // no localized / custom name
    // zone capacity: the 40-capacity building on a 2x2 lot -> 40/4 + 0.5 = 10.5 -> 10; the big one does not fit
    CHECK(sim->GetMaxCapacity(1, 1, 2) == 10 && sim->GetMaxCapacity(2, 1, 2) == 10);
    CHECK(sim->GetMaxCapacity(1, 2, 1) == 0 && sim->GetMaxCapacity(3, 1, 2) == 0);

    // The building map (i386 0x000fafaa: tract size 1, instance 0x49d5b678) and DoBuildingChanged (0x000f99ca).
    {
        cSC4SimGrid<std::uint8_t>* map = sim->GetBuildingMap();
        CHECK(map && map->GetTractSize() == 1 && map->GetInstanceID() == 0x49d5b678 && map->GetTractCountX() == 8);
        IBuildingDevelopmentSimHost::ChangedLot lot;
        lot.buildingType = 0x1001;   // purpose 1, wealth 2
        lot.rect[0] = 1; lot.rect[1] = 2; lot.rect[2] = 2; lot.rect[3] = 4;
        lot.growthStage = 3;
        lot.hasConfiguration = true;
        sim->DoBuildingChanged(lot, 2, true);
        CHECK(map->GetTractValue(1, 2) == 7 && map->GetTractValue(2, 4) == 7 && map->GetTractValue(3, 2) == 0 &&
              map->GetTractValue(1, 1) == 0 && map->GetTractValue(0, 2) == 0 && map->GetTractValue(1, 5) == 0);
        CHECK(StageReached(*sim, 0x1020) == 3);
        CHECK(host.events.size() == 1 && host.events[0].first == 23 && host.events[0].second == 0x4a64a9cc);
        sim->DoBuildingChanged(lot, 2, true);                      // the same stage again: no event
        lot.growthStage = 2;
        sim->DoBuildingChanged(lot, 2, true);                      // a lower stage: none either
        CHECK(host.events.size() == 1 && StageReached(*sim, 0x1020) == 3);
        lot.growthStage = 5;
        sim->DoBuildingChanged(lot, 1, true);                      // the message's wealth differs from the profile's: the key
        CHECK(map->GetTractValue(1, 2) == 6 && host.events.size() == 1 && StageReached(*sim, 0x1010) == -1);   // follows the message
        lot.hasConfiguration = false;
        sim->DoBuildingChanged(lot, 2, true);                      // no configuration (the original stops at the null lot pointers)
        CHECK(host.events.size() == 1);
        lot.hasConfiguration = true;
        sim->DoBuildingChanged(lot, 2, true);                      // stage 5 > 3: the next event
        CHECK(host.events.size() == 2 && StageReached(*sim, 0x1020) == 5);
        sim->DoBuildingChanged(lot, 2, false);                     // the building went: its cells are 0
        CHECK(map->GetTractValue(1, 2) == 0 && map->GetTractValue(2, 4) == 0 && host.events.size() == 2);
        lot.rect[2] = 40;                                          // a rectangle past the map is cut to it
        sim->DoBuildingChanged(lot, 2, true);
        CHECK(map->GetTractValue(7, 3) == 7);
        sim->DoBuildingChanged(lot, 2, false);

        // The lot-state message: a change into or out of the built states (2, 3) only; data4 is the wealth.
        host.lot = lot;
        host.lot.rect[2] = 2;
        cRZMessage2Standard state;
        state.SetType(0xe8bda2e7);
        const auto send = [&](std::uint64_t from, std::uint64_t to, std::uint64_t wealth) {
            state.SetData1(from);
            state.SetData2(to);
            state.SetData4(wealth);
            sim->DoMessage(&state);
        };
        send(1, 2, 2);
        CHECK(map->GetTractValue(1, 2) == 7 && host.lotAsks == 1);
        send(2, 3, 2);                                             // built to built: nothing changes
        send(3, 2, 1);
        CHECK(map->GetTractValue(1, 2) == 7);
        send(3, 4, 2);                                             // out of the built states: the cells clear
        CHECK(map->GetTractValue(1, 2) == 0);
        send(4, 1, 2);                                             // neither built: nothing
        CHECK(map->GetTractValue(1, 2) == 0);
        host.lot.buildingType = 0;                                 // a lot without a building type is ignored
        send(1, 2, 2);
        CHECK(map->GetTractValue(1, 2) == 0);
        host.lot.buildingType = 0x1001;
        host.lotKnown = false;                                     // not a lot
        send(1, 2, 2);
        CHECK(map->GetTractValue(1, 2) == 0);
        host.lotKnown = true;
    }

    // construction resources
    Resources resources;
    sim->SetConstructionResources(&resources);
    CHECK(sim->GetTotalConstructionResources() == 100 && sim->GetFreeConstructionResources() == 100);
    CHECK(sim->ClaimConstructionResources(60) && sim->GetFreeConstructionResources() == 40);
    CHECK(!sim->ClaimConstructionResources(41));
    sim->ReleaseConstructionResources(10);
    CHECK(sim->GetFreeConstructionResources() == 50);

    // growth marks and persisted state
    sim->MarkTract(0x1010, 4, 5);
    CHECK(!sim->IsTractUnmarked(0x1010, 4, 5) && sim->IsTractUnmarked(0x1010, 4, 6));
    sim->NoteSuccessfulBuild(0x1010, 33);
    sim->SetCheats(true, false, true);
    PortMemoryOStream os;
    CHECK(sim->Write(os));
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    auto* back = new cSC4BuildingDevelopmentSimulator();
    back->AddRef();
    CHECK(back->Read(is));
    CHECK(!back->IsTractUnmarked(0x1010, 4, 5) && back->GetDayOfLastSuccessfulBuild(0x1010) == 33 &&
          back->CheatIgnorePlumbing() && !back->CheatIgnorePower() && back->CheatIgnoreCaps());
    back->Release();

    // reloading a changed exemplar replaces its profile and family membership
    host.exemplars[0x1001]->Release();   // the host map owned the replaced exemplar
    host.exemplars[0x1001] = MakeBuilding(5, 0, 7, 0x600, {1, 1, 1});
    cISCPropertyHolder* changed = host.exemplars[0x1001];
    S::LoadBuildingProperties(*changed, 0x1001, false);
    CHECK(sim->GetBuildingProfilePtr(0x1001)->purpose == 5 && sim->IsBuildingInFamily(0x1001, 0x600) &&
          !sim->IsBuildingInFamily(0x1001, 0x500));

    // growth predicates (0x100150a4c / 0x100150a86), against the original's truth tables
    for (int zone = 0; zone < 12; ++zone) {
        for (int stage = 0; stage < 9; ++stage) {
            bool want = false;
            if (zone == 3 || (zone >= 6 && zone <= 9)) want = true;
            else if (zone == 1 || zone == 4) want = stage < 4;
            else if (zone == 2 || zone == 5) want = stage < 7;
            CHECK(S::GrowthStageFitsInZone(stage, static_cast<std::uint32_t>(zone)) == want);
        }
    }
    CHECK(!S::GrowthStageFitsInZone(0, 10) && !S::GrowthStageFitsInZone(-5, 0xffffffffu));
    for (std::uint32_t zone = 0; zone < 16; ++zone) {
        for (std::uint32_t purpose = 0; purpose < 16; ++purpose) {
            bool want = purpose == zone;
            if (zone == 2) want = want || purpose == 1;
            if (zone == 3) want = want || purpose == 1 || purpose == 2;
            if (zone == 5) want = want || purpose == 4;
            if (zone == 6) want = want || purpose == 4 || purpose == 5;
            if (zone == 9) want = want || purpose == 8;
            if (zone == 0xb || zone == 0xc || zone == 0xf) want = want || purpose == 0;
            CHECK(S::BuildingFitsInZone(purpose, zone) == want);
        }
    }

    // IsSuitableForConstruction (0x100150b0c): one failure code per branch
    struct Lot final : IConstructionLot { std::int32_t state = 1; std::int32_t ConstructionState() override { return state; } };
    struct City final : IConstructionSuitabilityHost {
        std::int8_t zone = 1; Lot* lot = nullptr; std::int32_t valueClass = 3; std::uint8_t value = 50; bool power = true;
        std::int8_t ZoneTypeAt(int, int) override { return zone; }
        IConstructionLot* LotAt(int, int) override { return lot; }
        std::int32_t LandValueClassAt(int, int) override { return valueClass; }
        std::uint8_t LandValueAt(int, int) override { return value; }
        bool HasPowerAt(int, int) override { return power; }
    };
    Lot lot; City city; city.lot = &lot;
    sim->SetSuitabilityHost(&city);
    sim->SetCheats(false, false, false);
    ConstructionProfile plan; plan.purpose = 1; plan.landValueClass = 3; plan.x = 4; plan.z = 5;
    ReasonForFailure reason = kReasonLotNotReady;
    CHECK(sim->IsSuitableForConstruction(plan, reason) && reason == kReasonLotNotReady);   // success leaves the reason alone
    city.zone = 4;   reason = kReasonLandValueRange;
    CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonNone); city.zone = 1;
    city.lot = nullptr;
    CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonNoLot); city.lot = &lot;
    city.power = false;
    CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonNoPower);
    sim->SetCheats(false, true, false);   // ignore power
    CHECK(sim->IsSuitableForConstruction(plan, reason));
    sim->SetCheats(false, false, false); city.power = true;
    city.valueClass = 2;
    CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonLandValueClass);
    plan.purpose = 8;   // purposes 7..9 skip the class comparison (and fit zone 1? no: zone 1 admits purpose 1 only)
    city.zone = 8;
    CHECK(sim->IsSuitableForConstruction(plan, reason));
    plan.purpose = 1; city.zone = 1; city.valueClass = 3;
    plan.minLandValue = 40; plan.maxLandValue = 60;
    CHECK(sim->IsSuitableForConstruction(plan, reason));
    city.value = 39; CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonLandValueRange);
    city.value = 61; CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonLandValueRange);
    city.value = 60; CHECK(sim->IsSuitableForConstruction(plan, reason));
    lot.state = 2;   CHECK(!sim->IsSuitableForConstruction(plan, reason) && reason == kReasonLotNotReady);
    sim->SetSuitabilityHost(nullptr);
    // ---- placement, developers and the decline plan
    {
        FakeServices services;
        sim->SetPlacementServices(&services);
        sim->SetCityCells(64, 64);
        CHECK(!sim->LocationIsOKForNewBuilding(0x7777, 40, 40));                   // not a building type
        CHECK(sim->LocationIsOKForNewBuilding(0x1001, 40, 40));
        services.occupied = true;
        CHECK(!sim->LocationIsOKForNewBuilding(0x1001, 40, 40));
        services.occupied = false;
        int area[4];
        const float rect[4] = {-20, -4, 2000, 1100};
        sim->GetStandardCityCellArea(rect, area);
        CHECK(area[0] == 0 && area[1] == 0 && area[2] == 63 && area[3] == 63);
        const float rect2[4] = {32, 48, 64, 80};
        sim->GetStandardCityCellArea(rect2, area);
        CHECK(area[0] == 2 && area[1] == 3 && area[2] == 4 && area[3] == 5);
        float position[3] = {40, -1, 56};
        IPlacedBuilding* placed = sim->AddNewBuildingOccupant(0x1001, position, 0);
        CHECK(placed == &services.building && services.added == 1 && services.released == 1);
        CHECK(position[1] == 33 && services.building.pos[1] == 33 && services.building.finished && services.building.type == 0x1001);
        CHECK(services.lastRect[0] == -8 && services.lastRect[1] == -4 && services.lastRect[2] == 8 && services.lastRect[3] == 4);
        position[1] = 5;
        sim->AddNewBuildingOccupant(0x1001, position, 1);                          // quarter turn swaps x and z of the footprint
        CHECK(position[1] == 5 && services.building.setMin[0] == -4 && services.building.setMax[0] == 4 &&
              services.building.setMin[2] == -8 && services.building.rotation == 1);
        services.occupied = true;
        CHECK(sim->AddNewBuildingOccupant(0x1001, position, 0) == nullptr && services.added == 2);
        services.occupied = false;
        sim->AddNewBuildingOccupant(0x1001, 2, 3, 2);                              // by cell: position from the cell, height from the terrain
        CHECK(services.building.pos[0] == 40 && services.building.pos[1] == 12 && services.building.pos[2] == 56 &&
              services.added == 3);
        ConstructionProfile plop;
        plop.buildingType = 0x1001; plop.x = 1; plop.z = 1; plop.rotation = 3;
        sim->PlopBuilding(plop);
        CHECK(services.added == 4 && services.building.rotation == 3 && services.building.pos[0] == 24);
        CHECK(sim->AddNewBuildingOccupant(&services.building) && sim->AddNewFoundationOccupant(&services.building));
        sim->DemolishFoundationOccupant(&services.building, 0);
        CHECK(services.removed == 1 && sim->GetCostOfOccupantDemolition(&services.foundation, 0) == 0);
        std::unordered_set<std::uint32_t> filtered;
        Filter filter;
        CHECK(sim->GetBuildingTypesByFilter(filtered, &filter) == 1 && filtered.count(0x1002) == 1);
        CHECK(sim->GetBuildingTypesByFilter(filtered, nullptr) == 2 && filtered.size() == 2);

        // foundations: the exemplar of the type is applied and its height property copied, the size follows the height difference
        services.foundation.holder = host.exemplars[0x1001];
        cSCPropertyHelp::SetPropertyValue(host.exemplars[0x1001], 0x88fcc3e3, 3.5f);
        cSCPropertyHelp::SetPropertyValue(host.exemplars[0x1001], 0x99afacd, std::int64_t(777));
        CHECK(!sim->LoadFoundationProperties(&services.foundation, 0x4242));          // no such foundation type
        float fpos[3] = {40, -1, 56};
        CHECK(!sim->AddNewFoundationOccupant(0x4242, 2, fpos, 16, 32, 1, 0) && services.foundationReleased == 1);
        services.building.holder = host.exemplars[0x1001];
        CHECK(sim->GetCostOfOccupantDemolition(&services.building, 0) == 777);
        {
            cGZPersistResourceKey key; key.instance = 0x1001;
            S::AddFoundationData(key);                                                 // type = the instance, so 0x1001 is now a foundation too
        }
        CHECK(sim->AddNewFoundationOccupant(0x1001, 2, fpos, 16, 32, 1, 0));
        CHECK(fpos[1] == 20 && services.foundation.height == 3.5f && services.foundation.exemplar == host.exemplars[0x1001]);
        CHECK(services.foundation.size[0] == 16 && services.foundation.size[1] == 20 && services.foundation.size[2] == 32 &&
              services.foundation.style == 2 && services.foundation.rotation == 1 && services.foundation.finished);
        float fpos2[3] = {40, 10, 56};
        CHECK(sim->AddNewFoundationOccupant(0x1001, 0, fpos2, 16, 16, 0, -1) && services.lastWhich == 1 &&
              services.foundation.size[1] == 5);                                       // top 10 - bottom 5
        float fpos3[3] = {40, 1, 56};
        sim->AddNewFoundationOccupant(0x1001, 0, fpos3, 16, 16, 0, 0.8f);
        CHECK(services.foundation.size[1] == 0.5f);                                    // never thinner than half a metre

        cSCPropertyHolder properties;
        cSCPropertyHelp::SetPropertyValue(&properties, 0x21, std::uint32_t(0x1020));
        sim->CreateDeveloper(&properties);
        CHECK(services.created == 1 && services.developer.inits == 1 && sim->GetDeveloper(0x1020) == &services.developer);
        CHECK(sim->GetDeveloper(1, 2) == &services.developer && sim->GetDeveloper(1, 3) == nullptr);
        CHECK(sim->GetDevelopers().size() == 1 && sim->GetDayOfLastSuccessfulBuild(0x1020) == 7);
        sim->CreateDeveloper(&properties);                                         // existing developer: re-initialised, not recreated
        CHECK(services.created == 1 && services.developer.inits == 2);
        cSCPropertyHolder none;
        sim->CreateDeveloper(&none);
        CHECK(sim->GetDevelopers().size() == 1);

        Tract tract;
        tract.results = {0, 1};
        sim->SetTractDeveloper(&tract);
        CHECK(sim->GetTractDeveloper() == &tract);
        Source bad(-25), fine(-5), worse(-100);
        DeclineEntry later; later.day = 99; later.source = &bad;
        DeclineEntry skipped; skipped.day = 1; skipped.source = &fine;               // due but not bad enough: dropped
        DeclineEntry first; first.day = 7; first.source = &bad;                      // executes, result 0: keep going
        DeclineEntry second; second.day = 3; second.source = &worse;                // executes, result 1: stop
        DeclineEntry untouched; untouched.day = 2; untouched.source = &bad;
        for (const DeclineEntry& e : {later, skipped, first, second, untouched}) sim->AddDeclineEntry(e);
        sim->ExecuteDeclinePlan();
        CHECK(tract.calls == 2);                                                   // `untouched` and `later` are still queued
        tract.results = {0, 0};
        tract.calls = 0;
        sim->ExecuteDeclinePlan();
        CHECK(tract.calls == 1);                                                   // only `untouched` was due; `later` waits for day 99
        sim->SetPlacementServices(nullptr);
    }
    sim->Release();

    // ---- the simulation messages (DoMessage 0x10014e84c, DoMonthlySimulation, DoIdle)
    {
        auto* growth = new cSC4BuildingDevelopmentSimulator();
        growth->AddRef();
        FakeServices services;
        growth->SetPlacementServices(&services);
        Resources resources;
        growth->SetConstructionResources(&resources);
        auto* simulatorExemplar = new cSCPropertyHolder();
        simulatorExemplar->AddRef();
        cSCPropertyHelp::SetPropertyValue(simulatorExemplar, 0x10, static_cast<std::uint32_t>(4));   // a simulator exemplar...
        cSCPropertyHelp::SetPropertyValue(simulatorExemplar, 0x21, static_cast<std::uint32_t>(0x1010));   // ... naming R$
        host.exemplars[0x7777] = simulatorExemplar;
        const int registeredBefore = host.registered;
        CHECK(growth->Init());
        CHECK(services.created == 1 && services.developer.inits >= 1);    // the developer came from the simulator exemplar
        CHECK(host.registered == registeredBefore + 1);
        CHECK(resources.inits == 1);
        // month: R + C + I supply 3000 * 0.5 = 1500 resources; every developer starts its map update; stage 1
        host.channels = {{0x1001, 1000.0f}, {0x3001, 1500.0f}, {0x4001, 500.0f}};
        cRZMessage2Standard month;
        month.SetType(0x66956816);
        growth->DoMessage(&month);
        CHECK(resources.total == 1500.0f && services.developer.mapUpdates == 1 && growth->GrowthStage() == 1);
        growth->DoMessage(&month);                                         // a second month before the stages finished: nothing
        CHECK(services.developer.mapUpdates == 1);
        // idle: the stage machine waits for the developer's map update
        services.developer.finished = false;
        cRZMessage2Standard idle;
        idle.SetType(0x6695681a);
        growth->DoMessage(&idle);
        CHECK(growth->GrowthStage() == 2);
        services.developer.finished = true;
        growth->DoMessage(&idle);                                          // finished: stage 3 -> 4 (no planned growth)
        CHECK(growth->GrowthStage() == 4);
        growth->DoMessage(&idle);                                          // the decline plan, back to 0
        CHECK(growth->GrowthStage() == 0);
        // the clamps: no demand -> the minimum, a huge demand -> the maximum
        host.channels.clear();
        growth->DoMessage(&month);
        CHECK(resources.total == 100.0f);
        for (int i = 0; i < 3; ++i) growth->DoMessage(&idle);
        host.channels = {{0x1001, 1e7f}};
        growth->DoMessage(&month);
        CHECK(resources.total == 2000.0f);
        // the cheats
        cRZMessage2Standard cheat;
        cheat.SetType(0x230e27ac);
        cheat.SetData1(0x491865e1);
        growth->DoMessage(&cheat);
        CHECK(growth->CheatIgnorePlumbing());
        cheat.SetData1(0x6ab6a2d7);
        growth->DoMessage(&cheat);
        CHECK(growth->CheatCoffeeBar());
        const int unregisteredBefore = host.unregistered;
        CHECK(growth->Shutdown() && host.unregistered == unregisteredBefore + 1);
        growth->SetPlacementServices(nullptr);
        growth->SetConstructionResources(nullptr);
        host.exemplars.erase(0x7777);
        simulatorExemplar->Release();
        growth->Release();
    }
    // ---- Grow (0x10014fb24): the first unmarked tract of the drawn developer goes to the tract developer
    {
        auto* growth = new cSC4BuildingDevelopmentSimulator();
        growth->AddRef();
        FakeServices services;
        growth->SetPlacementServices(&services);
        Tract tract;
        host.tract = &tract;
        CHECK(growth->Init());
        GrowingDeveloper developer;
        growth->SetDeveloperForTest(developer.type, &developer);
        growth->Grow();
        CHECK(tract.grown.size() == 1 && tract.grown[0] == std::make_pair(3, 4));
        CHECK(!growth->IsTractUnmarked(0x1010, 3, 4) && growth->GetDayOfLastSuccessfulBuild(0x1010) == 7);
        growth->Grow();                                                    // the next tract
        CHECK(tract.grown.size() == 2 && tract.grown[1] == std::make_pair(7, 8));
        growth->Grow();                                                    // all marked: the marks clear, the list starts over
        CHECK(tract.grown.size() == 3 && tract.grown[2] == std::make_pair(3, 4) && growth->IsTractUnmarked(0x1010, 7, 8));
        developer.demand = 5;                                              // demand not above 5: nobody grows
        growth->Grow();
        CHECK(tract.grown.size() == 3);
        growth->SetDeveloperForTest(developer.type, nullptr);
        growth->Shutdown();
        growth->SetPlacementServices(nullptr);
        host.tract = nullptr;
        growth->Release();
    }
    // ---- DoTerrainChanged (i386 0x000f77e8): lots whose ground left them are demolished, the others refitted
    {
        struct Ground final : ITerrainChangedLot {
            std::int32_t rect[4] = {0, 0, 0, 0};
            float center[3] = {0, 0, 0};
            float builtSlope = 0.0f, slopeNow = 0.0f;
            bool demolishable = true;
            void GetBoundingRect(std::int32_t out[4]) override { for (int i = 0; i < 4; ++i) out[i] = rect[i]; }
            void GetCenter(float xyz[3]) override { for (int i = 0; i < 3; ++i) xyz[i] = center[i]; }
            float GetSlopeAtTimeOfConstruction() override { return builtSlope; }
            bool IsInDemolishableState() override { return demolishable; }
        };
        struct City final : ITerrainChangedHost {
            std::vector<Ground*> lots;
            float altitude = 100.0f;
            std::int64_t demolitionCost = 250;
            std::int32_t asked[4] = {0, 0, 0, 0};
            int queries = 0, done = 0, demolitions = 0;
            std::vector<ITerrainChangedLot*> updated, demolished;
            std::vector<std::uint32_t> updateReasons;
            std::vector<std::pair<int, bool>> cleared;     // the cleared rect's x0, and whether the mayor pays
            std::vector<std::int64_t> charged;
            void LotsInCellRect(const std::int32_t rect[4], std::vector<ITerrainChangedLot*>& out) override {
                ++queries;
                for (int i = 0; i < 4; ++i) asked[i] = rect[i];
                for (Ground* lot : lots) out.push_back(lot);
            }
            void LotsDone() override { ++done; }
            float TerrainAltitude(float, float) override { return altitude; }
            float LotSlope(ITerrainChangedLot* lot) override { return static_cast<Ground*>(lot)->slopeNow; }
            float CellWidthX() override { return 16.0f; }
            void UpdateLotElements(ITerrainChangedLot* lot, std::uint32_t reason) override { updated.push_back(lot); updateReasons.push_back(reason); }
            void ClearZone(const std::int32_t rect[4], bool mayorPays) override { cleared.emplace_back(rect[0], mayorPays); }
            std::int64_t DemolishLots(const std::vector<ITerrainChangedLot*>& doomed) override {
                ++demolitions;
                demolished.insert(demolished.end(), doomed.begin(), doomed.end());
                return doomed.empty() ? 0 : demolitionCost;
            }
            void ChargeFunds(std::int64_t cost) override { charged.push_back(cost); }
        } city;
        SetTerrainChangedHost(&city);
        auto* growth = new S();
        growth->AddRef();
        growth->SetCityCells(64, 64);
        Ground level, sunk, raised, steep, vacant;
        level.center[1] = 99.5f;                           // the ground moved half a unit: fine
        sunk.center[1] = 101.5f;                           // the ground is 1.5 under the lot
        raised.center[1] = 97.5f;                          // ... 2.5 over it
        steep.center[1] = 100.0f; steep.builtSlope = 1.0f; steep.slopeNow = 3.0f;   // about 7 degrees steeper than built
        vacant.center[1] = 90.0f; vacant.demolishable = false; vacant.rect[0] = 7;
        city.lots = {&level, &sunk, &raised, &steep, &vacant};
        const std::int32_t rect[4] = {3, 4, 9, 12};
        growth->DoTerrainChanged(rect, 3);                 // a disaster: nobody pays
        CHECK(city.queries == 1 && city.done == 1 && city.asked[0] == 3 && city.asked[3] == 12);
        CHECK(city.updated.size() == 1 && city.updated[0] == &level && city.updateReasons[0] == 3);
        CHECK(city.demolished.size() == 3 && city.demolished[0] == &sunk && city.demolished[1] == &raised && city.demolished[2] == &steep);
        CHECK(city.cleared.size() == 1 && city.cleared[0].first == 7 && !city.cleared[0].second);   // nothing to demolish there: the zone goes
        CHECK(city.charged.empty());
        // the limits themselves are fine: exactly 1 down, exactly 2 up, exactly as steep as the lot may get
        Ground edgeDown, edgeUp;
        edgeDown.center[1] = 101.0f;
        edgeUp.center[1] = 98.0f;
        city.lots = {&edgeDown, &edgeUp};
        city.updated.clear(); city.demolished.clear();
        growth->DoTerrainChanged(rect, 3);
        CHECK(city.updated.size() == 2 && city.demolished.empty());
        Ground slight;
        slight.center[1] = 100.0f; slight.builtSlope = 1.0f; slight.slopeNow = 2.0f;   // about 3.5 degrees steeper: under the 5
        city.lots = {&slight};
        city.updated.clear();
        growth->DoTerrainChanged(rect, 3);
        CHECK(city.updated.size() == 1 && city.demolished.empty());
        // the mayor's own terraforming (reasons 1 and 2) is charged what the demolition cost, and the zone is cleared at
        // the mayor's expense
        city.lots = {&sunk, &vacant};
        city.demolished.clear(); city.cleared.clear();
        growth->DoTerrainChanged(rect, 1);
        CHECK(city.demolished.size() == 1 && city.charged.size() == 1 && city.charged[0] == 250);
        CHECK(city.cleared.size() == 1 && city.cleared[0].second);
        growth->DoTerrainChanged(rect, 2);
        CHECK(city.charged.size() == 2);
        city.lots = {&level};                              // nothing demolished: nothing charged
        growth->DoTerrainChanged(rect, 1);
        CHECK(city.charged.size() == 2);
        // any other reason only refits the lots, whatever happened to their ground
        city.lots = {&sunk, &vacant};
        city.updated.clear(); city.demolished.clear(); city.cleared.clear();
        const int demolitionsBefore = city.demolitions;
        growth->DoTerrainChanged(rect, 0);
        CHECK(city.updated.size() == 2 && city.updateReasons.back() == 0 && city.demolitions == demolitionsBefore && city.cleared.empty());
        // an empty rectangle asks nothing
        const int queriesBefore = city.queries;
        const std::int32_t empty[4] = {5, 5, 4, 9};
        growth->DoTerrainChanged(empty, 3);
        CHECK(city.queries == queriesBefore);
        // the message: the cells come packed, widened by one towards the origin and cut to the city; data4 is the reason
        cRZMessage2Standard moved;
        moved.SetType(kMessageTerrainChanged);
        moved.SetData1((10u << 16) | 70u);                 // x 10..70 of a 64 cell city
        moved.SetData2((0u << 16) | 20u);                  // z 0..20
        moved.SetData3(1);                                 // not the reason: read as one, the mayor would pay below
        moved.SetData4(3);
        city.lots = {&sunk};
        city.demolished.clear();
        const std::size_t chargesBefore = city.charged.size();
        growth->DoMessage(&moved);
        CHECK(city.queries == queriesBefore + 1);
        CHECK(city.asked[0] == 9 && city.asked[1] == 0 && city.asked[2] == 63 && city.asked[3] == 20);
        CHECK(city.demolished.size() == 1 && city.charged.size() == chargesBefore);
        city.lots.clear();
        SetTerrainChangedHost(nullptr);
        growth->DoTerrainChanged(rect, 3);                 // without the city's services: nothing
        growth->Release();
    }
    S::StaticShutdown();
    for (auto& kv : host.exemplars) kv.second->Release();   // the host map owns the exemplars the test made
    std::puts(failures ? "sc4_buildingdevelopment: FAILED" : "sc4_buildingdevelopment ok");
    return failures != 0;
}
