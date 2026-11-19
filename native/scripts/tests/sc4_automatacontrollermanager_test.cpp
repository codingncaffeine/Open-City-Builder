#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/DBSegment.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Occ final : cSC4Occupant {
    float pos[3], lo[3], hi[3];
    Occ(float x, float z) : pos{x, 0, z}, lo{x - 8, 0, z - 8}, hi{x + 8, 10, z + 8} {}
    void GetPosition(float* p) override { for (int i = 0; i < 3; ++i) p[i] = pos[i]; }
    void GetBoundingBox(float* a, float* b) override { for (int i = 0; i < 3; ++i) { a[i] = lo[i]; b[i] = hi[i]; } }
};
Occ* NewOcc(float x, float z) { auto* o = new Occ(x, z); o->AddRef(); return o; }

struct Ctl : cSC4AutomataController {
    std::uint32_t type = kControllerTypeGenerator;
    int shutdowns = 0, ticks = 0, timeChecks = 0, sourceChanges = 0, effectChecks = 0, viewChanges = 0;
    bool tickable = true, expired = false;
    std::uint32_t GetControllerType() const override { return type; }
    bool Shutdown() override { ++shutdowns; return cSC4AutomataController::Shutdown(); }
    bool IsTickable() const override { return tickable; }
    bool IsExpired() const override { return expired; }
    void DoAnimationTick(float, std::uint32_t, const ControllerTime&) override { ++ticks; }
    void CheckTimeOfDay(const ControllerTime&, const ControllerTime&) override { ++timeChecks; }
    void NotifySourceDataChanged() override { ++sourceChanges; }
    void CheckEffectTriggers() override { ++effectChecks; }
    bool HandleViewChanged() override { ++viewChanges; return true; }
};
struct Att : cSC4AutomataAttractor {
    std::uint32_t GetControllerType() const override { return kControllerTypeAttractor; }
};

struct Automaton final : IControllerAutomaton {
    int refs = 1;
    IControllerAutomaton* leader = reinterpret_cast<IControllerAutomaton*>(1);
    int removedFrom = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetController(void*) override {}
    void SetPosition(const float*) override {}
    IControllerLeader* GetLeader() override { return nullptr; }
    void SetLeader(IControllerAutomaton* l) override { leader = l; }
};

struct Unk : cIGZUnknown {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};
struct Clock final : IControllerClock {
    int refs = 1, hour = 6, minute = 30;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int Hour() override { return hour; }
    int Minute() override { return minute; }
};
struct GroupTemplate final : IOccupantGroupTemplate {
    std::list<tControllerTemplate*> templates;
    float chance = 1.0f;
    cISC4Occupant* matching = nullptr;
    bool Matches(cISC4Occupant* o) override { return o == matching; }
    float SpawnChance() override { return chance; }
    const std::list<tControllerTemplate*>& ControllerTemplates() override { return templates; }
};
struct Script final : IControllerScriptSystem {
    int refs = 1;
    std::list<IOccupantGroupTemplate*> groups;
    std::map<std::string, IOccupantGroupTemplate*> byName;
    std::map<std::uint32_t, IOccupantGroupTemplate*> byId;
    std::map<std::string, tControllerTemplate*> templates;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    const std::list<IOccupantGroupTemplate*>& OccupantGroupTemplates() override { return groups; }
    IOccupantGroupTemplate* FindGroupTemplate(const cIGZString& n) override { auto it = byName.find(n.ToChar()); return it == byName.end() ? nullptr : it->second; }
    IOccupantGroupTemplate* FindGroupTemplate(std::uint32_t id) override { auto it = byId.find(id); return it == byId.end() ? nullptr : it->second; }
    tControllerTemplate* FindControllerTemplate(const cIGZString& n) override { auto it = templates.find(n.ToChar()); return it == templates.end() ? nullptr : it->second; }
};
struct Server final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added, removed, sent;
    std::vector<void*> sentVoid;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2* m) override { sent.push_back(m->GetType()); sentVoid.push_back(static_cast<cIGZMessage2Standard*>(m)->GetVoid1()); return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t t) override { added.push_back(t); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t t) override { removed.push_back(t); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct CtrlHost final : IAutomataControllerHost {
    Unk city;
    bool network = true;
    std::vector<std::pair<int, int>> networkQueries;
    std::uint32_t rnd = 0;
    bool hasLot = false;
    SC4CellRect lot{0, 0, 1, 1};
    std::uint32_t facing = 0;
    cIGZUnknown* CityContext() override { return &city; }
    bool GetOccupantLot(cISC4Occupant*, SC4CellRect& r, std::uint32_t& f) override { if (!hasLot) return false; r = lot; f = facing; return true; }
    bool PointInCity(float, float) override { return true; }
    float TerrainHeight(float, float) override { return 0; }
    float CityTerrainHeight(float, float) override { return 0; }
    float GroundHeight(float, float) override { return 0; }
    float CityWidth() override { return 1024.0f; }
    float CityDepth() override { return 1024.0f; }
    IControllerQueue* GetQueue(cISC4Occupant*, bool) override { return nullptr; }
    void RegisterActiveController(void*) override {}
    bool BeginEffectQuery() override { return false; }
    bool IsEffectActive(std::uint32_t) override { return false; }
    void EndEffectQuery() override {}
    bool SoundAvailable() override { return false; }
    void PostSound(std::uint32_t, std::uint32_t, std::int64_t, std::int64_t, std::int64_t) override {}
    std::uint32_t RandomBelow(std::uint32_t) override { return rnd; }
    bool ViewAvailable() override { return false; }
    bool IsPointInViewFrustum(const float*) override { return false; }
    int CityCellsX() override { return 64; }
    int CityCellsZ() override { return 64; }
    bool HasNetworkAt(int x, int z) override { networkQueries.push_back({x, z}); return network; }
};

struct MgrHost final : IControllerManagerHost {
    Clock clock;
    Script script;
    Server server;
    std::vector<std::string> cheats, reports;
    bool cheatMgr = true, debug = true, layers = true;
    int tickAdds = 0, tickRemoves = 0, cheatListenerAdds = 0, cheatListenerRemoves = 0;
    float view[3] = {0, 0, 0};
    int zoom = 0;
    double random = 0.0;
    std::map<const cISC4Occupant*, const void*> lots;
    std::vector<Ctl*> made;
    IControllerClock* AcquireClock() override { return &clock; }
    IControllerScriptSystem* AcquireScriptSystem() override { return &script; }
    cIGZMessageServer2* MessageServer() override { return &server; }
    void AddTickNotification(cIGZMessageTarget2*, int) override { ++tickAdds; }
    void RemoveTickNotification(cIGZMessageTarget2*, int) override { ++tickRemoves; }
    bool CheatManagerAvailable() override { return cheatMgr; }
    bool DebugFunctionalityEnabled() override { return debug; }
    void RegisterCheat(std::uint32_t, const char* n) override { cheats.push_back(n); }
    void UnregisterCheat(std::uint32_t) override { cheats.push_back("-"); }
    void AddCheatListener(cIGZMessageTarget*) override { ++cheatListenerAdds; }
    void RemoveCheatListener(cIGZMessageTarget*) override { ++cheatListenerRemoves; }
    bool AutomataLayersRunning() override { return layers; }
    const float* ViewPosition() override { return view; }
    int ViewZoom() override { return zoom; }
    cSC4AutomataController* CreateController(std::uint32_t key) override {
        Ctl* c;
        if (key == kControllerTypeAttractor) { auto* a = new Att(); a->AddRef(); return a; }
        c = new Ctl();
        c->AddRef();
        made.push_back(c);
        return c;
    }
    const void* LotOf(cISC4Occupant* o) override { auto it = lots.find(o); return it == lots.end() ? nullptr : it->second; }
    double RandomDouble() override { return random; }
    void Report(const std::string& m) override { reports.push_back(m); }
};

tControllerTemplate* NewTemplate(const char* name, std::uint32_t type = kControllerTypeGenerator) {
    auto* t = new tGeneratorTemplate();
    t->name.AssignFromCString(name);
    t->classKey = type;
    t->controllerType = type;
    t->radius = 20.0f;
    return t;
}
std::string S(const cIGZString& s) { return std::string(s.ToChar(), s.Strlen()); }

cRZMessage2Standard* Msg(std::uint32_t type) {
    auto* m = new cRZMessage2Standard();
    m->AddRef();
    m->SetType(type);
    return m;
}
}  // namespace

int main() {
    CtrlHost ctrl;
    SetAutomataControllerHost(&ctrl);

    // ---- NormalizeControllerTime
    {
        ControllerTime t; t.time = -1.0f; t.days = 0;
        NormalizeControllerTime(t);
        CHECK(t.time == 23.0f && t.days == 6);
        t.time = 49.0f; t.days = 5; NormalizeControllerTime(t);
        CHECK(t.time == 1.0f && t.days == 0);    // 5 + 2 = 7 -> 0
        t.time = 3.0f; t.days = -8; NormalizeControllerTime(t);
        CHECK(t.days == 6);                      // -8 mod 7
        t.time = std::nanf(""); NormalizeControllerTime(t);
        CHECK(t.time == 0.0f);
    }

    // ---- grid
    cSC4ControllerGrid grid;
    CHECK(grid.Init() && grid.Width() == 8 && grid.Height() == 8 && grid.Shift() == 3 && grid.CellCount() == 64);
    auto* c1 = new Ctl(); c1->AddRef();
    auto* occ1 = NewOcc(100, 100);
    c1->SetOccupant(occ1);
    tControllerTemplate* templateA = NewTemplate("a");
    c1->SetTemplate(templateA);
    float lo[3], hi[3];
    c1->GetBoundingBox(lo, hi);     // occupant box +-8 + radius 20: 72..128
    grid.AddController(c1);
    std::uint8_t* rect = c1->TractRect();
    CHECK(rect[0] == 0 && rect[1] == 0 && rect[2] == 1 && rect[3] == 1);   // 72/128 = 0, 128/128 = 1
    CHECK(grid.Tract(0, 0).size() == 1 && grid.Tract(1, 1).size() == 1 && grid.Tract(2, 2).empty());
    grid.AddController(c1);
    CHECK(grid.Tract(0, 0).size() == 1);   // already listed
    occ1->pos[0] = 400; occ1->pos[2] = 400; for (int i = 0; i < 3; i += 2) { occ1->lo[i] = 392; occ1->hi[i] = 408; }
    grid.MoveController(c1);
    CHECK(grid.Tract(0, 0).empty() && grid.Tract(1, 1).empty() && grid.Tract(3, 3).size() == 1 && grid.Tract(2, 2).size() == 1 && rect[0] == 2 && rect[2] == 3);
    CHECK(&grid.GetTractByWorldPos(400.0f, 400.0f) == &grid.Tract(3, 3));
    CHECK(&grid.GetTractByWorldPos(-5.0f, 5000.0f) == &grid.Tract(0, 7));   // clamped
    grid.RemoveController(c1);
    CHECK(grid.Tract(3, 3).empty() && rect[0] == 0 && rect[3] == 0);
    {   // GetGridBounds(position, radius): quirk, depth is clamped with the width
        const float p[3] = {1000.0f, 0, 10.0f};
        TractRect r;
        grid.GetGridBounds(p, 50.0f, r);
        CHECK(r.x0 == 7 && r.x1 == 7 && r.z0 == 0 && r.z1 == 0);
        const float neg[3] = {10.0f, 0, 10.0f};
        grid.GetGridBounds(neg, 50.0f, r);
        CHECK(r.x0 == 0 && r.z0 == 0 && r.x1 == 0);
    }
    c1->Release();   // refcount 1 -> final release

    // ---- queue info
    {
        tControllerQInfo q;
        auto* occ = NewOcc(64, 64);
        ctrl.hasLot = true; ctrl.lot = {1, 1, 2, 2};    // box 16..48
        ctrl.facing = 1;                                // facing 1 -> side 2 (east edge)
        q.InitQueuePoint(occ);
        CHECK(q.valid && q.point[0] == 49.0f && q.point[2] == 32.0f && q.direction[2] == 1.0f && q.direction[0] == 0.0f);
        q.valid = false;
        ctrl.facing = 3;                                // side 0 (west edge)
        q.InitQueuePoint(occ);
        CHECK(q.valid && q.point[0] == 15.0f && q.point[2] == 32.0f && q.direction[2] == -1.0f);
        q.valid = false; ctrl.facing = 2;               // side 3: south edge
        q.InitQueuePoint(occ);
        CHECK(q.point[0] == 32.0f && q.point[2] == 49.0f && q.direction[0] == -1.0f);
        q.valid = false; ctrl.facing = 0;               // facing 0 -> side 1: north edge
        q.InitQueuePoint(occ);
        CHECK(q.point[0] == 32.0f && q.point[2] == 15.0f && q.direction[0] == 1.0f);
        // no road next to the edge: tries the four sides then gives up
        q.valid = false; ctrl.network = false; ctrl.networkQueries.clear();
        q.InitQueuePoint(occ);
        CHECK(!q.valid && ctrl.networkQueries.size() == 4);
        ctrl.network = true;
        // valid queue points are kept
        q.valid = true; ctrl.networkQueries.clear();
        q.InitQueuePoint(occ);
        CHECK(ctrl.networkQueries.empty());
        // no lot: random side and the occupant box
        ctrl.hasLot = false; ctrl.rnd = 2; q.valid = false;
        q.InitQueuePoint(occ);   // box 56..72 -> east edge 73
        CHECK(q.valid && q.point[0] == 73.0f && q.point[2] == 64.0f);
        // queue: leaders
        Automaton a, b, c;
        q.AddToQueue(&a); q.AddToQueue(&b); q.AddToQueue(&c); q.AddToQueue(&b);
        CHECK(q.queue.size() == 3 && a.leader == nullptr && b.leader == &a && c.leader == &b && b.refs == 2);
        q.RemoveFromQueue(&b);
        CHECK(q.queue.size() == 2 && c.leader == &a && b.refs == 1);
        q.RemoveFromQueue(&a);
        CHECK(c.leader == nullptr);
        q.RemoveFromQueue(&a);   // not queued: nothing
        q.Clear();
        CHECK(q.queue.empty() && c.refs == 1 && b.refs == 1);
        occ->Release();
    }

    // ---- manager
    MgrHost host;
    SetControllerManagerHost(&host);
    auto* mgr = new cSC4AutomataControllerManager();
    mgr->AddRef();
    CHECK(mgr->GetGZCLSID() == 0xca14ca03 && !mgr->Initialized() && mgr->Enabled());
    CHECK(mgr->Init() && mgr->Initialized() && mgr->Init());
    CHECK(host.server.added.size() == 12 && host.tickAdds == 1 && host.cheats.size() == 1 && host.cheats[0] == "Controllers" && host.cheatListenerAdds == 1);
    CHECK(host.clock.refs == 2 && host.script.refs == 2 && mgr->Grid().CellCount() == 64);

    tControllerTemplate* shop = NewTemplate("Shop");
    tControllerTemplate* park = NewTemplate("Park");
    tControllerTemplate* lure = NewTemplate("Lure", kControllerTypeAttractor);
    host.script.templates = {{"Shop", shop}, {"Park", park}, {"Lure", lure}};
    auto* b1 = NewOcc(200, 200);
    auto* b2 = NewOcc(210, 210);
    int lotA = 0, lotB = 0, lotC = 0;
    host.lots[b1] = &lotA;
    host.lots[b2] = &lotA;   // same lot as b1

    cSC4AutomataController* s1 = mgr->AddControllerFromTemplate(b1, shop, nullptr);
    CHECK(s1 && s1->GetTemplate() == shop && s1->GetOccupant() == b1 && mgr->OccupantEntryCount() == 1 && b1->AddRef() == 4);   // test + controller + map key
    b1->Release();
    CHECK(mgr->GetController(b1, shop) == s1 && mgr->GetController(b1, park) == nullptr && mgr->GetController(b1, cRZString("Shop")) == s1);
    CHECK(mgr->AddControllerFromTemplate(b2, shop, nullptr) == nullptr);   // same lot already has this template
    cSC4AutomataController* p2 = mgr->AddControllerFromTemplate(b2, park, nullptr);
    CHECK(p2 && mgr->OccupantEntryCount() == 2);
    host.lots[b2] = &lotB;
    cSC4AutomataController* s2 = mgr->AddControllerFromTemplate(b2, shop, nullptr);
    CHECK(s2 && s2 != s1);
    // controllers without occupant live under key 0 at an explicit position
    const float spot[3] = {500, 0, 500};
    cSC4AutomataController* free1 = mgr->AddControllerFromTemplateName(nullptr, cRZString("Park"), spot);
    CHECK(free1 && free1->GetOccupant() == nullptr);
    float got[3];
    free1->GetPosition(got);
    CHECK(got[0] == 500 && got[2] == 500 && mgr->OccupantEntryCount() == 3);
    CHECK(mgr->AddControllerFromTemplateName(nullptr, cRZString("Missing"), spot) == nullptr);
    // uniqueness radius
    park->uniqueRadius = 300.0f;
    const float nearSpot[3] = {520, 0, 520};
    CHECK(mgr->AddControllerFromTemplate(nullptr, park, nearSpot) == nullptr);
    park->uniqueRadius = 0.0f;
    CHECK(mgr->GetController(cS3DVector3{500, 0, 500}, cRZString("Park"), 5.0f) == free1);
    CHECK(mgr->GetController(cS3DVector3{500, 0, 500}, cRZString("Shop"), 5.0f) == nullptr);

    // ---- relevant controllers / attraction
    auto* shopper = NewOcc(300, 300);
    auto* shopOcc = NewOcc(310, 310);
    auto* att = new tAttractorTemplate();
    att->name.AssignFromCString("Mall"); att->classKey = kControllerTypeAttractor; att->controllerType = kControllerTypeAttractor;
    att->radius = 100.0f; att->groups = {5}; att->flags = kCtrlFlagReady | kCtrlFlagInView;
    att->strengthCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {100.0f, 1.0f}});
    att->curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {24.0f, 1.0f}});
    host.script.templates["Mall"] = att;
    shopper->AddOccupantGroup(5);
    cSC4AutomataController* mall = mgr->AddControllerFromTemplate(shopOcc, att, nullptr);
    CHECK(mall != nullptr && mall->IsActive());
    std::list<cSC4AutomataController*> found;
    CHECK(mgr->GetRelevantControllersByWorldPos(305.0f, 305.0f, kControllerTypeAttractor, found) && found.size() == 1 && found.front() == mall);
    mgr->ReleaseControllerList(found);
    CHECK(found.empty());
    cS3DVector3 pull;
    float dist = -1, strength = -1;
    ControllerTime t0; t0.time = 12.0f; t0.days = 0;
    cSC4AutomataController* best = mgr->GetCompositeAttractionEffect(shopper, pull, dist, strength);
    CHECK(best == mall && strength > 0 && dist >= 0 && pull.x > 0 && pull.z > 0 && std::fabs(pull.x - pull.z) < 1e-4f);
    shopper->AddOccupantGroup(77);   // (groups only matter for the filter)
    mgr->SetEnabled(false);
    CHECK(mgr->GetCompositeAttractionEffect(shopper, pull, dist, strength) == nullptr);
    found.clear();
    CHECK(!mgr->GetRelevantControllersByWorldPos(305.0f, 305.0f, kControllerTypeAttractor, found));
    mgr->SetEnabled(true);
    struct NetView final : INetworkOccupantView { void GetWorldPosition(float* p) override { p[0] = 305; p[1] = 0; p[2] = 305; } } net;
    CHECK(mgr->GetCompositeAttractionCommotion(&net) > 0.0f && mgr->GetCompositeAttractionCommotion(nullptr) == 0.0f);

    // ---- queue infos
    tControllerQInfo* qi = mgr->GetControllerQInfo(b1, true);
    CHECK(qi && mgr->GetControllerQInfo(b1, false) == qi && mgr->QueueEntryCount() == 1 && qi->valid);
    CHECK(mgr->GetControllerQInfo(b2, false) == nullptr);
    {   // queue reset message needs an IGZUnknown and data1
        Unk u;
        auto* m = Msg(kCtrlMsgQueueReset);
        m->SetIGZUnknown(&u);
        CHECK(mgr->DoMessage(static_cast<cIGZMessage2*>(m)) && qi->valid);   // no data1: ignored
        m->SetData1(1);
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(!qi->valid);
        m->Release();
    }

    // ---- move, view changes, source data
    CHECK(mgr->MoveControllers(b1) && mgr->MoveController(s1) && mgr->MoveController(nullptr));
    static_cast<Ctl*>(s1)->viewChanges = 0;
    for (std::uint32_t id : {kCtrlMsgCameraMoved, kCtrlMsgScrollEnd}) {
        auto* m = Msg(id);
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        m->Release();
    }
    CHECK(static_cast<Ctl*>(s1)->viewChanges == 2);
    {
        auto* m = Msg(kCtrlMsgCameraChanged);
        host.view[0] = 10.0f;   // 100 m^2 < granularity of zoom 0
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(static_cast<Ctl*>(s1)->viewChanges == 2);
        host.view[0] = 1000.0f;
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(static_cast<Ctl*>(s1)->viewChanges == 3);
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));   // same place now: below the granularity
        CHECK(static_cast<Ctl*>(s1)->viewChanges == 3);
        auto* start = Msg(kCtrlMsgScrollStart);
        mgr->DoMessage(static_cast<cIGZMessage2*>(start));
        host.view[0] = 9000.0f;
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));   // scrolling: ignored
        CHECK(static_cast<Ctl*>(s1)->viewChanges == 3);
        start->Release(); m->Release();
        auto* end = Msg(kCtrlMsgScrollEnd);
        mgr->DoMessage(static_cast<cIGZMessage2*>(end));
        end->Release();
    }
    for (std::uint32_t id : {kCtrlMsgSourceDataChanged, kCtrlMsgEffectTriggers}) {
        auto* m = Msg(id);
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        m->Release();
    }
    CHECK(static_cast<Ctl*>(s1)->sourceChanges == 1 && static_cast<Ctl*>(s1)->effectChecks == 2);   // Init checked once

    // ---- ticks
    {
        host.clock.hour = 8; host.clock.minute = 30;
        auto* m = Msg(kCtrlMsgTick);
        m->SetData2(7); m->SetData3(40);
        static_cast<Ctl*>(s1)->ticks = 0;
        CHECK(mgr->DoMessage(static_cast<cIGZMessage2*>(m)));
        CHECK(mgr->GetControllerTime().time == 8.5f && static_cast<Ctl*>(s1)->ticks == 1);
        // expired and ready: removed with a message
        static_cast<Ctl*>(s2)->expired = true;
        host.server.sent.clear();
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(mgr->GetController(b2, shop) == nullptr && host.server.sent.size() == 1 && host.server.sent[0] == kMessageControllerRemoved);
        // layers off: no ticking, but the clock still moves
        host.layers = false;
        const int ticksBefore = static_cast<Ctl*>(s1)->ticks;
        host.clock.hour = 9;
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(static_cast<Ctl*>(s1)->ticks == ticksBefore && mgr->GetControllerTime().time == 9.5f);
        host.layers = true;
        // hour passed
        auto* h = Msg(kCtrlMsgHourPassed);
        static_cast<Ctl*>(s1)->timeChecks = 0;
        mgr->DoMessage(static_cast<cIGZMessage2*>(h));
        CHECK(mgr->GetControllerTime().time == 10.5f && static_cast<Ctl*>(s1)->timeChecks == 1);
        h->Release(); m->Release();
    }

    // ---- occupant messages and groups
    GroupTemplate fruit, everyone;
    fruit.templates = {shop};
    everyone.templates = {park};
    everyone.matching = b1;
    host.script.byId[9] = &fruit;
    host.script.byName["Everyone"] = &everyone;
    host.script.groups = {&everyone};
    auto* b3 = NewOcc(600, 600);
    host.lots[b3] = &lotC;
    {
        auto* m = Msg(kCtrlMsgOccupantGroupAdded);
        m->SetVoid1(b3);
        m->SetData4(9);
        host.random = 0.5; fruit.chance = 0.1f;
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(mgr->GetController(b3, shop) == nullptr);   // chance too low
        fruit.chance = 0.9f;
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));
        CHECK(mgr->GetController(b3, shop) != nullptr);
        m->SetData4(404);
        mgr->DoMessage(static_cast<cIGZMessage2*>(m));    // unknown group: nothing
        m->Release();
    }
    CHECK(mgr->AddControllersFromOccupantGroup(9, b3, nullptr) && !mgr->AddControllersFromOccupantGroup(10, b3, nullptr));
    CHECK(mgr->AddControllersFromOccupantGroup(cRZString("Everyone"), b3, nullptr) && mgr->GetController(b3, park) != nullptr);
    CHECK(mgr->RemoveControllersFromOccupantGroup(cRZString("Everyone"), b3) && mgr->GetController(b3, park) == nullptr && mgr->GetController(b3, shop) != nullptr);
    CHECK(!mgr->RemoveControllersFromOccupantGroup(cRZString("Nope"), b3));
    CHECK(mgr->RemoveControllersFromOccupantGroup(9u, b3) && mgr->ControllersOf(b3) == nullptr);   // last one: the entry goes
    CHECK(mgr->AddControllers(b1));   // b1 matches "everyone"
    CHECK(mgr->GetController(b1, park) != nullptr);
    {
        auto* removedMsg = Msg(kCtrlMsgOccupantRemoved);
        removedMsg->SetVoid1(b1);
        mgr->DoMessage(static_cast<cIGZMessage2*>(removedMsg));
        CHECK(mgr->ControllersOf(b1) == nullptr);
        auto* inserted = Msg(kCtrlMsgOccupantInserted);
        inserted->SetVoid1(b1);
        mgr->DoMessage(static_cast<cIGZMessage2*>(inserted));
        CHECK(mgr->GetController(b1, park) != nullptr);
        removedMsg->Release(); inserted->Release();
    }
    CHECK(!mgr->RemoveController(b1, cRZString("Park")) && mgr->ControllersOf(b1) == nullptr);   // always false (kept)
    CHECK(!mgr->RemoveController(static_cast<cSC4AutomataController*>(nullptr)));
    {
        cSC4AutomataController* extra = mgr->AddControllerFromTemplate(b3, shop, nullptr);
        CHECK(extra && mgr->RemoveController(extra) && mgr->ControllersOf(b3)->empty());
    }
    // queue info disappears together with the last controller
    CHECK(mgr->GetControllerQInfo(b2, true) != nullptr && mgr->RemoveControllers(b2) && mgr->GetControllerQInfo(b2, false) == nullptr);

    // ---- cheat code
    {
        cRZString on("controllers on"), off("controllers off"), junk("other off");
        cGZMessage msg;
        msg.type = kMessageCheatCode; msg.param1 = kAutomataCheatControllers;
        msg.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&off));
        CHECK(mgr->DoMessage(msg) && !mgr->Enabled());
        msg.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&on));
        CHECK(mgr->DoMessage(msg) && mgr->Enabled());
        msg.param1 = 5;
        CHECK(!mgr->DoMessage(msg));
        (void)junk;
    }

    // ---- post city init: controllers are matched to the script templates again
    {
        auto* post = Msg(kCtrlMsgPostCityInit);
        mgr->DoMessage(static_cast<cIGZMessage2*>(post));   // not pending: nothing happens
        PortMemoryOStream os;
        CHECK(mgr->Write(os));
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        auto* other = new cSC4AutomataControllerManager();
        other->AddRef();
        CHECK(other->Read(is));   // plain streams carry no objects: the entries read back are keyed by null
        mgr->DoMessage(static_cast<cIGZMessage2*>(post));
        post->Release();
        other->SetEnabled(true);
        delete other;
    }

    // ---- shutdown / release
    CHECK(mgr->Shutdown() && !mgr->Initialized() && mgr->OccupantEntryCount() == 0 && mgr->QueueEntryCount() == 0 && mgr->Grid().CellCount() == 0);
    CHECK(host.clock.refs == 1 && host.script.refs == 1 && host.server.removed.size() == 12 && host.tickRemoves == 1);
    CHECK(host.cheats.back() == "-" && host.cheatListenerRemoves == 1);
    void* iface = nullptr;
    CHECK(mgr->QueryInterface(kGZIID_cISC4AutomataControllerManager, &iface) && iface == static_cast<cISC4AutomataControllerManager*>(mgr));
    CHECK(mgr->QueryInterface(kGZIID_cIGZMessageTarget2, &iface) && iface == static_cast<cIGZMessageTarget2*>(mgr));
    CHECK(mgr->QueryInterface(1, &iface) && iface == static_cast<cIGZSerializable*>(mgr));
    CHECK(!mgr->QueryInterface(0x1234, &iface));
    CHECK(mgr->Release() == 3 && mgr->Release() == 2 && mgr->Release() == 1);
    mgr->Release();   // final release
    for (Occ* o : {b1, b2, b3, shopper, shopOcc, occ1}) o->Release();
    for (tControllerTemplate* t : {templateA, shop, park, lure, static_cast<tControllerTemplate*>(att)}) delete t;   // the controllers only point at them
    std::printf(failures ? "FAILED\n" : "sc4_automatacontrollermanager OK\n");
    return failures ? 1 : 0;
}
