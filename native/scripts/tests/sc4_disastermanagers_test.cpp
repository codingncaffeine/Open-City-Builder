#include <cstdio>
#include <vector>

#include "sc4/DisasterManagers.h"
#include "sc4/OccupantManager.h"
#include "rz/cRZMessage2Standard.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Counted {
    std::uint32_t refs = 0;
};

class FakeFire final : public cISC4DisasterInstanceFire {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        *out = nullptr;
        if (iid == kGZIID_cISC4DisasterInstanceFire || iid == kGZIID_cISC4DisasterInstance) *out = static_cast<cISC4DisasterInstanceFire*>(this);
        if (!*out) return false;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Shutdown() override { return true; }
    cISC4DisasterManager* GetManager() const override { return nullptr; }
    std::uint32_t GetID() const override { return 1; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return 1; }
    bool Init() override { ++inits; manager->AddInstanceToList(this); return true; }
    void Update() override { ++begins; }
    bool Ignite(cISC4Occupant*, bool a, std::uint32_t b, bool c) override { flags = (a ? 1 : 0) | (b ? 2 : 0) | (c ? 4 : 0); return true; }
    bool Extinguish(cISC4Occupant*) override { return true; }
    bool BurnDown(cISC4Occupant*) override { return true; }
    int GetFireCountInCellRect(const SC4CellRect&) override { return 0; }
    int GetFireCount() override { return 0; }
    bool IgniteCell(std::uint32_t x, std::uint32_t z, bool, bool, std::uint32_t) override { cell = x * 100 + z; return true; }
    std::uint32_t ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) override { return x + z + amount; }
    bool QuickBurnCell(std::uint32_t, std::uint32_t) override { return true; }
    bool IgniteCellRadius(std::uint32_t, std::uint32_t, std::uint32_t r, std::uint32_t) override { radius = r; return true; }
    std::uint32_t SetCause(std::uint32_t c) override { cause = c; return c + 1; }
    std::uint32_t GetCause() override { return cause; }
    std::uint32_t refs = 0, inits = 0, begins = 0, cause = 0, cell = 0, radius = 0, flags = 0;
    cISC4DisasterManager* manager = nullptr;
};

class FakeVolcano final : public cISC4DisasterInstanceVolcano {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        *out = nullptr;
        if (iid == kGZIID_cISC4DisasterInstanceVolcano) *out = static_cast<cISC4DisasterInstanceVolcano*>(this);
        if (!*out) return false;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Shutdown() override { return true; }
    cISC4DisasterManager* GetManager() const override { return nullptr; }
    std::uint32_t GetID() const override { return 2; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return phase; }
    float GetLavaHeightByCityCell(int x, int z) override { return float(x * 10 + z); }
    void Erupt() override { calls += "e"; }
    void Prepare() override { calls += "p"; }
    void SetPhase(int p) override { phase = p; calls += "s"; }
    std::uint32_t refs = 0;
    int phase = 1;
    std::string calls;
};

class FakeRiot final : public cISC4DisasterInstanceRiot {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Shutdown() override { return true; }
    cISC4DisasterManager* GetManager() const override { return nullptr; }
    std::uint32_t GetID() const override { return 3; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return 1; }
    bool Init() override { ++inits; return true; }
    void AddRiotLeader(int x, int z, bool) override { leaderX = x; leaderZ = z; }
    std::uint32_t refs = 0;
    int inits = 0, leaderX = -1, leaderZ = -1;
};

class FakePipeBurst final : public cISC4DisasterInstancePipeBurst {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Shutdown() override { return true; }
    cISC4DisasterManager* GetManager() const override { return nullptr; }
    std::uint32_t GetID() const override { return 4; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return 1; }
    bool Init(cIGZUnknown* pipe) override { initPipe = pipe; return accept; }
    std::uint32_t refs = 0;
    cIGZUnknown* initPipe = nullptr;
    bool accept = true;
};

class FakeQuake final : public cISC4DisasterInstanceEarthquake {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Shutdown() override { return true; }
    cISC4DisasterManager* GetManager() const override { return nullptr; }
    std::uint32_t GetID() const override { return 5; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return phase; }
    void Init(int x, int z) override { cx = x; cz = z; }
    void BeginDeformation() override { phase = 2; ++begins; }
    std::uint32_t refs = 0;
    int phase = 1, cx = -1, cz = -1, begins = 0;
};

class Grid final : public IPlumbingGrid {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool IsWatered(int x, int z) override { return x == wx && z == wz; }
    std::uint32_t refs = 0;
    int wx = 3, wz = 4;
};

class Pipe final : public cISC4PipeOccupantView {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void GetCell(std::uint32_t& x, std::uint32_t& z) override { x = cx; z = cz; }
    std::uint32_t refs = 1, cx = 0, cz = 0;
};

class PipeOccupant final : public cSC4Occupant {
public:
    PipeOccupant(std::uint32_t t, std::uint32_t x, std::uint32_t z) { type_ = t; pipe.cx = x; pipe.cz = z; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4PipeOccupant) { *out = &pipe; pipe.AddRef(); return true; }
        return cSC4Occupant::QueryInterface(iid, out);
    }
    void GetBoundingBox(float* lo, float* hi) override {
        lo[0] = float(pipe.cx) * 16; lo[1] = 0; lo[2] = float(pipe.cz) * 16;
        hi[0] = lo[0] + 16; hi[1] = 1; hi[2] = lo[2] + 16;
    }
    Pipe pipe;
};

class FakeTarget final : public cISC4RiotNetworkOccupant {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void ClearState(std::uint32_t b) override { cleared |= b; }
    void SetState(std::uint32_t b) override { set |= b; }
    std::uint32_t refs = 0, set = 0, cleared = 0;
};

class Factory final : public IDisasterInstanceFactory {
public:
    cISC4DisasterManager* fireManager = nullptr;
    FakeFire* fire = nullptr;
    FakeVolcano* volcano = nullptr;
    FakeRiot* riot = nullptr;
    FakePipeBurst* burst = nullptr;
    FakeQuake* quake = nullptr;
    Grid grid;
    int tuneInstances = 2, tuneLeaders = 9;
    bool tuneOk = true;
    cISC4DisasterInstanceFire* CreateFire() override { fire = new FakeFire(); fire->manager = fireManager; return fire; }
    cISC4DisasterInstanceVolcano* CreateVolcano() override { volcano = new FakeVolcano(); return volcano; }
    cISC4DisasterInstanceRiot* CreateRiot() override { riot = new FakeRiot(); return riot; }
    cISC4DisasterInstancePipeBurst* CreatePipeBurst() override { burst = new FakePipeBurst(); return burst; }
    cISC4DisasterInstanceEarthquake* CreateEarthquake() override { quake = new FakeQuake(); return quake; }
    IPlumbingGrid* PlumbingGrid() override { grid.AddRef(); return &grid; }
    bool ReadRiotTuning(int& a, int& b) override { if (!tuneOk) return false; a = tuneInstances; b = tuneLeaders; return true; }
};

class Host final : public IDisasterHost {
public:
    cSC4OccupantManager* occupants = nullptr;
    cISC4DisasterLayer* DisasterLayer() override { return nullptr; }
    cISC4OccupantManager* Occupants() override { return occupants; }
    bool PositionToCell(float, float, int&, int&) override { return false; }
    bool CellCenterToPosition(int, int, float&, float&) override { return false; }
    float TerrainHeight(float, float) override { return 0; }
    int CityCellCountX() override { return 8; }
    int CityCellCountZ() override { return 8; }
    cISC4DisasterManager* CreateManager(std::uint32_t) override { return nullptr; }
};

std::vector<std::pair<int, int>> Walk(SpiralIterator it, int limit = 1000) {
    std::vector<std::pair<int, int>> cells;
    int x, z;
    while (limit-- > 0 && it.GetNext(x, z)) cells.push_back({x, z});
    return cells;
}
}  // namespace

int main() {
    Host host;
    auto* occupants = new cSC4OccupantManager();
    occupants->AddRef();
    occupants->SetCitySize(8, 8);
    occupants->Init();
    host.occupants = occupants;
    SetDisasterHost(&host);
    Factory factory;
    SetDisasterInstanceFactory(&factory);

    // ---- spiral: every cell of the rectangle exactly once, starting at the start cell
    {
        auto cells = Walk(SpiralIterator::Over(3, 3, 8, 8));
        CHECK(cells.size() == 64);
        CHECK(cells.front() == std::make_pair(3, 3));
        std::vector<int> seen(64, 0);
        for (auto& c : cells) { CHECK(c.first >= 0 && c.first < 8 && c.second >= 0 && c.second < 8); ++seen[c.second * 8 + c.first]; }
        bool once = true;
        for (int v : seen) once = once && v == 1;
        CHECK(once);
        // a corner start still covers the grid
        CHECK(Walk(SpiralIterator::Over(0, 0, 8, 8)).size() == 64);
        CHECK(Walk(SpiralIterator::Over(7, 7, 8, 8)).size() == 64);
        CHECK(Walk(SpiralIterator::Over(5, 5, 3, 7)).size() == 0);       // outside
        CHECK(Walk(SpiralIterator::Over(0, 0, 1, 1)).size() == 1);
        CHECK(Walk(SpiralIterator::Over(-1, 2, 8, 8)).empty());
        SpiralIterator done = SpiralIterator::Over(0, 0, 1, 1);
        int x, z;
        CHECK(done.GetNext(x, z) && !done.GetNext(x, z) && !done.GetNext(x, z));
        auto first = Walk(SpiralIterator::Over(4, 4, 9, 9), 5);
        CHECK(first.size() == 5);
    }

    // ---- fire
    {
        auto* m = new cSC4DisasterManagerFire();
        m->AddRef();
        factory.fireManager = m;
        CHECK(m->GetID() == kDisasterManagerIdFire && m->GetGZCLSID() == kGZCLSID_cSC4DisasterManagerFire);
        CHECK(cSC4DisasterManagerFire::Instance() == m);
        void* p = nullptr;
        CHECK(m->QueryInterface(kGZIID_cISC4DisasterManagerFire, &p) && p == static_cast<cISC4DisasterManagerFire*>(m));
        static_cast<cIGZUnknown*>(static_cast<cISC4DisasterManagerFire*>(m))->Release();
        CHECK(m->GetFireInstance() == nullptr);
        CHECK(m->ExtinguishCell(1, 2, 3) == 0);                 // never creates
        CHECK(factory.fire == nullptr);
        CHECK(m->Init());
        CHECK(factory.fire && factory.fire->inits == 1 && factory.fire->begins == 1);
        CHECK(m->GetInstances().size() == 1);
        CHECK(factory.fire->refs == 1);                          // only the list
        CHECK(m->SetCause(7) == 8 && m->GetCause() == 7);
        CHECK(m->IgniteOccupant(nullptr) && factory.fire->flags == 4);
        CHECK(m->IgniteCell(2, 3, false, false, 0) && factory.fire->cell == 203);
        CHECK(m->ExtinguishCell(1, 2, 3) == 6);
        CHECK(m->QuickBurnCell(1, 1));
        CHECK(m->IgniteCellRadius(0, 0, 5, 0) && factory.fire->radius == 5);
        CHECK(!m->AttemptDisasterStart() && !m->AttemptDisasterStart(1, 2));
        CHECK(m->OnStandardMessage(nullptr));
        CHECK(factory.fire->refs == 1);
        m->Shutdown();
        m->Release();
        CHECK(cSC4DisasterManagerFire::Instance() == nullptr);
    }

    // ---- volcano
    {
        auto* m = new cSC4DisasterManagerVolcano();
        m->AddRef();
        CHECK(m->GetID() == kDisasterManagerIdVolcano);
        CHECK(m->GetLavaHeightByCityCell(1, 2) == 0.0f);
        CHECK(m->Init());
        CHECK(m->AttemptDisasterStart(5, 5));
        CHECK(factory.volcano && factory.volcano->calls == "eps" && factory.volcano->phase == 3);
        CHECK(m->GetInstances().size() == 1);
        CHECK(m->GetLavaHeightByCityCell(3, 4) == 34.0f);
        CHECK(!m->AttemptDisasterStart(1, 1));                   // one at a time
        CHECK(!m->AttemptDisasterStart());
        m->Shutdown();
        m->Release();
    }

    // ---- riots
    {
        auto* m = new cSC4DisasterManagerRiot();
        m->AddRef();
        CHECK(m->GetID() == kDisasterManagerIdRiot && m->GetGZCLSID() == kGZCLSID_cSC4DisasterManagerRiot);
        CHECK(m->Init());
        CHECK(cSC4DisasterManagerRiot::snRiotMaxInstances == 2 && cSC4DisasterManagerRiot::snRiotMaxLeaders == 9);
        CHECK(!m->AttemptDisasterStart(6, 7));
        CHECK(factory.riot && factory.riot->inits == 1 && factory.riot->leaderX == 6 && factory.riot->leaderZ == 7);
        CHECK(factory.riot->refs == 1 && m->GetInstances().size() == 1);   // the list's reference
        FakeTarget a, b;
        m->AddRiotNetworkOccupant(&a);
        m->AddRiotNetworkOccupant(&a);
        m->AddRiotNetworkOccupant(&b);
        CHECK(a.refs == 1 && b.refs == 1 && a.set == 2);
        FakeRiot* first = factory.riot;
        CHECK(!m->AttemptDisasterStart(1, 1));
        CHECK(m->GetInstances().size() == 2);
        CHECK(!m->AttemptDisasterStart(2, 2));                   // at the limit
        CHECK(m->GetInstances().size() == 2);
        m->RemoveInstanceFromList(first);
        CHECK(a.refs == 1);                                      // still one running
        m->RemoveInstanceFromList(m->GetInstances().front());
        CHECK(a.refs == 0 && a.cleared == 2 && b.refs == 0);
        m->Shutdown();
        m->Release();
    }

    // ---- pipe bursts
    {
        auto* m = new cSC4DisasterManagerPipeBurst();
        m->AddRef();
        CHECK(m->GetID() == kDisasterManagerIdPipeBurst && m->GetGZCLSID() == kGZCLSID_cSC4DisasterManagerPipeBurst);
        CHECK(m->Init());
        CHECK(m->Filter() && m->Filter()->Grid() == &factory.grid && factory.grid.refs == 1);
        auto* water = new PipeOccupant(0xc772bf98, 3, 4);
        auto* dry = new PipeOccupant(0xc772bf98, 5, 5);
        auto* other = new PipeOccupant(0x12345678, 2, 2);
        for (auto* o : {static_cast<cSC4Occupant*>(water), static_cast<cSC4Occupant*>(dry), static_cast<cSC4Occupant*>(other)}) {
            o->AddRef();
            o->Init();
            occupants->InsertOccupant(o, 0);
        }
        CHECK(m->Filter()->IsOccupantIncluded(water) && !m->Filter()->IsOccupantIncluded(dry) && !m->Filter()->IsOccupantIncluded(other));
        CHECK(!m->AttemptDisasterStart(5, 5));
        CHECK(!m->AttemptDisasterStart(2, 2));
        CHECK(m->AttemptDisasterStart(3, 4));
        CHECK(factory.burst && factory.burst->initPipe == &water->pipe && factory.burst->refs == 0);
        CHECK(water->pipe.refs == 1);                            // the query reference was dropped
        CHECK(m->AttemptDisasterStart());                        // the spiral finds the only watered pipe
        factory.grid.wx = -1;
        CHECK(!m->AttemptDisasterStart());
        m->Shutdown();
        CHECK(m->Filter() == nullptr && factory.grid.refs == 0);
        m->Release();
    }

    // ---- earthquakes
    {
        auto* m = new cSC4DisasterManagerEarthquake();
        m->AddRef();
        CHECK(m->GetID() == kDisasterManagerIdEarthquake && m->GetGZCLSID() == kGZCLSID_cSC4DisasterManagerEarthquake);
        CHECK(m->Init());
        CHECK(!m->AttemptDisasterStart());
        CHECK(m->AttemptDisasterStart(10, 20));
        CHECK(factory.quake && factory.quake->cx == 10 && factory.quake->cz == 20 && factory.quake->begins == 1);
        CHECK(m->GetInstances().size() == 1);
        CHECK(!m->AttemptDisasterStart(1, 1));
        m->Shutdown();
        m->Release();
    }

    // ---- the small per-manager entry points: interface views, standard-message hooks, interface queries, riot
    //      occupant registration, and destruction through the last Release.
    {
        struct RiotOccupant final : cISC4RiotNetworkOccupant {
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return ++refs; }
            std::uint32_t Release() override { return --refs; }
            void ClearState(std::uint32_t bits) override { state &= ~bits; }
            void SetState(std::uint32_t bits) override { state |= bits; }
            std::uint32_t refs = 0, state = 0;
        };
        cRZMessage2Standard message;
        auto* fire = new cSC4DisasterManagerFire();
        CHECK(fire->AsISC4DisasterManager() == static_cast<cISC4DisasterManager*>(fire));
        CHECK(fire->OnStandardMessage(&message));
        static_cast<cISC4DisasterManagerFire*>(fire)->Release();     // count 0: destroys
        auto* volcano = new cSC4DisasterManagerVolcano();
        CHECK(volcano->AsISC4DisasterManager() == static_cast<cISC4DisasterManager*>(volcano));
        CHECK(volcano->OnStandardMessage(&message));
        void* view = nullptr;
        CHECK(volcano->QueryInterface(kGZIID_cISC4DisasterManagerVolcano, &view) && view == static_cast<cISC4DisasterManagerVolcano*>(volcano));
        CHECK(volcano->QueryInterface(kGZIID_cISC4DisasterManager, &view) && view != nullptr);
        CHECK(!volcano->QueryInterface(0x12345678, &view) && view == nullptr);
        while (static_cast<cISC4DisasterManagerVolcano*>(volcano)->Release() != 0) {}   // drop the two query references, then destroy
        auto* riot = new cSC4DisasterManagerRiot();
        CHECK(riot->AsISC4DisasterManager() == static_cast<cISC4DisasterManager*>(riot));
        CHECK(riot->OnStandardMessage(&message) && !riot->AttemptDisasterStart());
        RiotOccupant occupant;
        riot->AddRiotNetworkOccupant(&occupant);
        riot->AddRiotNetworkOccupant(&occupant);                        // already registered: no second reference
        CHECK(occupant.refs == 1 && (occupant.state & 2) != 0);
        riot->Release();
        auto* quake = new cSC4DisasterManagerEarthquake();
        CHECK(quake->AsISC4DisasterManager() == static_cast<cISC4DisasterManager*>(quake));
        CHECK(quake->OnStandardMessage(&message) && !quake->AttemptDisasterStart());
        quake->Release();
    }

    SetDisasterInstanceFactory(nullptr);
    if (failures == 0) std::printf("sc4_disastermanagers: all passed\n");
    return failures == 0 ? 0 : 1;
}
