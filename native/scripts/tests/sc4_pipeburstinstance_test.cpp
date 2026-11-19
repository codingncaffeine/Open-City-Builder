#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/PipeBurstInstance.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Effect final : cISC4VisualEffect {
    std::string name;
    int refs = 1, started = 0, stopped = 0, lastStop = -1;
    float transform[13] = {};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Start(int) override { ++started; }
    void Stop(int flags) override { ++stopped; lastStop = flags; }
    bool IsPlaying() override { return false; }
    void SetCompletionMessage(std::uint32_t, std::uint32_t) override {}
    void SetIntParameter(int, const void*, int) override {}
    void SetPointerParameter(int, const void*, int) override {}
    void SetTransform(const float* m) override { for (int i = 0; i < 13; ++i) transform[i] = m[i]; }
};

struct Pipe final : cISC4PipeOccupantView {
    int refs = 1;
    std::uint32_t cx, cz;
    bool connected[4] = {false, false, false, false};
    cISC4Occupant* occupant = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void GetCell(std::uint32_t& x, std::uint32_t& z) override { x = cx; z = cz; }
    void GetCenter(float* xyz) override { xyz[0] = cx * 16.0f + 8; xyz[1] = 2; xyz[2] = cz * 16.0f + 8; }
    bool IsConnected(int direction, int kind) override { return kind == 4 && connected[direction]; }
    cISC4Occupant* AsOccupant() override { return occupant; }
};

class PipeOccupant final : public cSC4Occupant {
public:
    PipeOccupant(std::uint32_t x, std::uint32_t z) { type_ = kOccupantTypeNetworkOccupant; view.cx = x; view.cz = z; view.occupant = this; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_PortPipeOccupantView) { *out = static_cast<cISC4PipeOccupantView*>(&view); view.AddRef(); return true; }
        return cSC4Occupant::QueryInterface(iid, out);
    }
    void GetBoundingBox(float* lo, float* hi) override {
        lo[0] = float(view.cx) * 16; lo[1] = 0; lo[2] = float(view.cz) * 16;
        hi[0] = lo[0] + 16; hi[1] = 1; hi[2] = lo[2] + 16;
    }
    Pipe view;
};

struct Server final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> listeners;
    std::vector<std::uint32_t> posted;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> postedCells;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* m, bool) override {
        posted.push_back(m->GetType());
        auto* standard = static_cast<cIGZMessage2Standard*>(m);
        postedCells.push_back({standard->GetData1(), standard->GetData2()});
        return true;
    }
    bool AddNotification(cIGZMessageTarget2* t, std::uint32_t type) override { listeners.push_back({t, type}); return true; }
    bool RemoveNotification(cIGZMessageTarget2* t, std::uint32_t type) override {
        for (auto it = listeners.begin(); it != listeners.end(); ++it)
            if (*it == std::make_pair(t, type)) { listeners.erase(it); return true; }
        return false;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeManager final : cSC4DisasterManager {
    explicit FakeManager(std::uint32_t id) : id_(id) {}
    std::uint32_t GetID() const override { return id_; }
    std::uint32_t id_;
};

struct Grid final : IPlumbingGrid {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool IsWatered(int, int) override { return true; }
    std::uint32_t refs = 0;
};

struct Factory final : IDisasterInstanceFactory {
    Grid grid;
    cISC4DisasterInstanceFire* CreateFire() override { return nullptr; }
    cISC4DisasterInstanceVolcano* CreateVolcano() override { return nullptr; }
    cISC4DisasterInstanceRiot* CreateRiot() override { return nullptr; }
    cISC4DisasterInstancePipeBurst* CreatePipeBurst() override { return new cSC4DisasterInstancePipeBurst(); }
    cISC4DisasterInstanceEarthquake* CreateEarthquake() override { return nullptr; }
    IPlumbingGrid* PlumbingGrid() override { grid.AddRef(); return &grid; }
    bool ReadRiotTuning(int&, int&) override { return false; }
};

struct BurstHost final : IPipeBurstHost {
    std::vector<Effect*> effects;
    std::vector<std::string> names;
    int demolished = 0, sinkHoles = 0, level = 1;
    SinkHoleDesc lastHole;
    bool demolition = true;
    bool CreateEffect(const char* name, cISC4VisualEffect** out) override {
        auto* e = new Effect();
        e->name = name;
        effects.push_back(e);
        *out = e;
        return true;
    }
    float TerrainCellSize() override { return 16.0f; }
    void AddSinkHole(int type, const SinkHoleDesc& d) override { ++sinkHoles; lastHole = d; (void)type; }
    bool HasDemolition() override { return demolition; }
    void DemolishPipe(cISC4Occupant*) override { ++demolished; }
    int ViewLevel() override { return level; }
    bool ReadTuning(float& a, float& b, int& zoom) override { a = 1.0f; b = 1.0f; zoom = 7; return true; }
};

struct Host final : IDisasterHost {
    cISC4DisasterLayer* layer = nullptr;
    cSC4OccupantManager* occupants = nullptr;
    Server server;
    int began = 0;
    cISC4DisasterLayer* DisasterLayer() override { return layer; }
    cISC4OccupantManager* Occupants() override { return occupants; }
    cIGZMessageServer2* MessageServer() override { return &server; }
    bool PositionToCell(float, float, int&, int&) override { return false; }
    bool CellCenterToPosition(int, int, float&, float&) override { return false; }
    float TerrainHeight(float, float) override { return 0; }
    int CityCellCountX() override { return 8; }
    int CityCellCountZ() override { return 8; }
    cISC4DisasterManager* CreateManager(std::uint32_t classId) override {
        cISC4DisasterManager* m = nullptr;
        if (classId == kGZCLSID_cSC4DisasterManagerToxic) m = new cSC4DisasterManagerToxic();
        else if (classId == kGZCLSID_cSC4DisasterManagerEffect) m = new cSC4DisasterManagerEffect();
        else if (classId == kGZCLSID_cSC4DisasterManagerPipeBurst) m = new cSC4DisasterManagerPipeBurst();
        else {
            static const std::pair<std::uint32_t, std::uint32_t> ids[] = {
                {kGZCLSID_cSC4DisasterManagerVolcano, kDisasterManagerIdVolcano},
                {kGZCLSID_cSC4DisasterManagerFire, kDisasterManagerIdFire},
                {kGZCLSID_cSC4DisasterManagerEarthquake, kDisasterManagerIdEarthquake},
                {kGZCLSID_cSC4DisasterManagerRiot, kDisasterManagerIdRiot}};
            for (auto& p : ids) if (p.first == classId) m = new FakeManager(p.second);
        }
        if (m) m->AddRef();
        return m;
    }
};

class ToxicFx final : public IToxicEffect {
public:
    void Release() override {}
    void Start(int) override {}
    void Stop(int) override {}
    void SetTransform(const cS3DTransform&) override {}
};
class FakeToxicHost final : public IToxicHost {
public:
    ToxicFx fx;
    IToxicEffect* CreateEffect(const char*) override { return &fx; }
    void CellToWorld(int cx, int cz, float& x, float& z) override { x = cx * 16.0f; z = cz * 16.0f; }
    float TerrainHeight(float, float) override { return 0.0f; }
};
}  // namespace

int main() {
    Host host;
    FakeToxicHost toxicHost;
    SetToxicHost(&toxicHost);
    auto* occupants = new cSC4OccupantManager();
    occupants->AddRef();
    occupants->SetCitySize(8, 8);
    occupants->Init();
    host.occupants = occupants;
    SetDisasterHost(&host);
    Factory factory;
    SetDisasterInstanceFactory(&factory);
    auto* layer = new cSC4DisasterLayer();
    layer->AddRef();
    host.layer = layer;
    CHECK(layer->Init());
    BurstHost burst;
    SetPipeBurstHost(&burst);
    cSC4DisasterInstancePipeBurst::SeedRandom(7);
    cSC4DisasterInstancePipeBurst::StaticShutdown(nullptr);

    // a cross of watered pipes around (3,4): the centre connects to all four neighbours
    std::vector<PipeOccupant*> pipes;
    const int cells[5][2] = {{3, 4}, {2, 4}, {3, 3}, {4, 4}, {3, 5}};
    for (auto& c : cells) {
        auto* p = new PipeOccupant(c[0], c[1]);
        p->AddRef();
        p->Init();
        occupants->InsertOccupant(p, 0);
        pipes.push_back(p);
    }
    for (int i = 0; i < 4; ++i) pipes[0]->view.connected[i] = true;

    auto* manager = static_cast<cSC4DisasterManagerPipeBurst*>(layer->GetDisasterManager(kDisasterManagerIdPipeBurst));
    CHECK(manager && manager->Init());

    CHECK(manager->AttemptDisasterStart(3, 4));
    CHECK(manager->GetInstances().size() == 1);
    auto* instance = static_cast<cSC4DisasterInstancePipeBurst*>(manager->GetInstances().front());
    CHECK(cSC4DisasterInstancePipeBurst::kMinLeakTimeBeforeBurst == 1000000 && cSC4DisasterInstancePipeBurst::kGoToZoomLevel == 7);
    instance->AddRef();      // the message server holds its targets while it delivers; so does the test
    CHECK(instance->GetID() == kDisasterManagerIdPipeBurst && instance->GetPhase() == 1 && instance->GetManager() == manager);
    int x = 0, z = 0;
    CHECK(!instance->GetLocation(x, z) && instance->GetDamagedOccupantsCount() == 0);
    CHECK(layer->IsADisasterOngoing());
    CHECK(instance->Leaks().size() == 1 && instance->Leaks().front().cellX == 3 && instance->Leaks().front().cellZ == 4);
    CHECK(burst.effects.size() == 1 && burst.effects[0]->name == "burst_pipe_aboveground" && burst.effects[0]->started == 1);
    CHECK(burst.effects[0]->transform[9] == 3 * 16.0f + 8 && burst.effects[0]->transform[12] == 1.0f);
    CHECK(host.server.listeners.size() >= 4);                                // three messages + the city tick (the layer listens too)
    CHECK(instance->Leaks().front().burstTime == 1000000);                   // min == max in this tuning
    CHECK(std::find(host.server.posted.begin(), host.server.posted.end(), kMessagePipeBurstCell) != host.server.posted.end());

    // a second start on the same pipe is refused (already leaking)
    cISC4PipeOccupantView* again = pipes[0]->view.AsOccupant() ? &pipes[0]->view : nullptr;
    CHECK(!instance->StartLeakAtPipe(again));

    // not yet time
    auto tick = [&](std::uint32_t ms) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kPipeBurstMessageAnimationTick);
        message->SetData2(ms);
        instance->DoMessage(message);
        message->Release();
    };
    tick(500000);
    CHECK(instance->Leaks().size() == 1 && burst.demolished == 0);
    // the view level changes: the effect is replaced
    {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kPipeBurstMessageViewLevelChanged);
        int level = 3;
        message->SetVoid2(&level);
        instance->DoMessage(message);
        message->Release();
        CHECK(instance->ViewLevelValue() == 3 && burst.effects.size() == 2 && burst.effects[1]->name == "burst_pipe_underground");
        CHECK(burst.effects[0]->stopped == 1 && burst.effects[0]->lastStop == 1 && burst.effects[0]->refs == 0);
        CHECK(instance->Leaks().front().elapsed == 0);                    // the timer restarts
    }
    tick(999999);
    CHECK(burst.demolished == 0);
    tick(1);                                                              // the burst
    CHECK(burst.demolished == 1 && burst.sinkHoles == 1 && burst.lastHole.x == 48.0f && burst.lastHole.z == 64.0f);
    CHECK(instance->Leaks().empty() && instance->Waiting().size() == 1 && instance->Waiting().front().cells.size() == 4);
    CHECK(burst.effects[1]->lastStop == 0 && burst.effects[1]->refs == 1);   // stopped, not released
    CHECK(burst.effects.size() == 3 && burst.effects[2]->name == "pipe_explodes" && burst.effects[2]->refs == 0);
    // three ticks of waiting, then the neighbours start leaking
    tick(1); tick(1); tick(1);
    CHECK(instance->Leaks().empty() && instance->Waiting().size() == 1);
    tick(1);
    CHECK(instance->Waiting().empty() && instance->Leaks().size() == 4);
    // removing a leaking pipe stops that leak
    {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMessagePipeOccupantRemoved);
        message->SetVoid1(static_cast<cISC4Occupant*>(pipes[1]));
        instance->DoMessage(message);
        message->Release();
        CHECK(instance->Leaks().size() == 3);
    }
    // everything bursts and the disaster ends
    for (int i = 0; i < 40 && !manager->GetInstances().empty(); ++i) tick(1000000);
    CHECK(manager->GetInstances().empty());
    CHECK(!layer->IsADisasterOngoing());
    for (auto& l : host.server.listeners) CHECK(l.first != static_cast<cIGZMessageTarget2*>(instance));
    instance->Release();

    // no pipe: nothing starts
    CHECK(!manager->AttemptDisasterStart(0, 0));
    CHECK(manager->AttemptDisasterStart());                               // the spiral finds one of the (still standing) pipes
    CHECK(manager->GetInstances().size() == 1);
    manager->Shutdown();
    CHECK(manager->GetInstances().empty());
    burst.demolition = false;
    SetPipeBurstHost(nullptr);
    SetDisasterInstanceFactory(nullptr);
    // every effect the instances asked for was given back (the host hands each out with one reference), except a leak's
    // effect when its pipe bursts: the original stops it (transition 0, fade out) and drops it without a release
    for (Effect* e : burst.effects) {
        CHECK(e->refs == (e->stopped > 0 && e->lastStop == 0 ? 1 : 0));
        delete e;
    }
    burst.effects.clear();
    layer->Shutdown();
    host.layer = nullptr;
    layer->Release();
    for (PipeOccupant* p : pipes) p->Release();
    occupants->Release();
    std::printf(failures ? "FAILED\n" : "sc4_pipeburstinstance ok\n");
    return failures != 0;
}
