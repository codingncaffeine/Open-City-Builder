// cSC4ObliterateCity against OpenSC4 region-city-dialogs.md section 5: Init (blocks, save switch, pause, indicator, the two
// radio posts), the isolated removal curve (4 blocks at once, then round(4 + 1.6 t)), the sparkle rule, the smoke screen, the
// finale (everything removed, the city demolished and dezoned), the end message and Escape.
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/ObliterateCity.h"
#include "vic_fakes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

struct Server final : cIGZMessageServer2 {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> posted;
    std::set<std::uint32_t> subscribed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 2; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* m, bool) override {
        posted.push_back({m->GetType(), static_cast<cIGZMessage2Standard*>(m)->GetData1()});
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t t) override { subscribed.insert(t); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t t) override { subscribed.erase(t); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct Effect final : IObliterateEffect {
    int* stops;
    bool* hard;
    Effect(int* s, bool* h) : stops(s), hard(h) {}
    void Stop(bool h) override { ++*stops; *hard = h; }
    void Release() override { delete this; }
};

struct Host final : IObliterateCityHost {
    Server server;
    int saveDisabled = 0, paused = 0, ticks = 0, removedAll = 0, dezoned = 0, effectStops = 0;
    bool hardStop = false, csi = true;
    std::vector<std::pair<int, int>> demolished;   // x0, z0 of each call
    std::set<std::pair<int, int>> occupiedBlocks;  // block corners that report occupants
    std::vector<std::string> effects;
    std::vector<std::size_t> effectBlocks;   // how many blocks were demolished when each effect was made (its block's index)
    std::uint32_t finaleMessage = 0;
    Tuning ReadTuning() override {
        Tuning t;
        t.blockSize = 4;
        t.curve = {{0.0f, 4.0f}, {10.0f, 20.0f}};
        t.smokeSeconds = 10.0f;
        return t;
    }
    void CityCellCount(int& x, int& z) override { x = 64; z = 64; }
    void DisableSave() override { ++saveDisabled; }
    void EnableSave() override { --saveDisabled; }
    void SimulatorHiddenPause() override { ++paused; }
    void SimulatorHiddenResume() override { --paused; }
    bool SituationIndicatorVisible() override { return csi; }
    void SetSituationIndicatorVisible(bool v) override { csi = v; }
    cIGZMessageServer2* Messages() override { return &server; }
    void AddTick(cIGZMessageTarget2*, std::uint32_t priority) override { if (priority == 0xc0) ++ticks; }
    void RemoveTick(cIGZMessageTarget2*, std::uint32_t) override { --ticks; }
    void DemolishCells(int x0, int z0, int, int, cISC4OccupantFilter*) override { demolished.push_back({x0, z0}); }
    bool CellsOccupied(int x0, int z0, int, int, cISC4OccupantFilter*) override { return occupiedBlocks.count({x0, z0}) != 0; }
    void RemoveOccupants(cISC4OccupantFilter*) override { ++removedAll; }
    void DezoneCity(int maxX, int maxZ) override { if (maxX == 63 && maxZ == 63) ++dezoned; }
    IObliterateEffect* CreateEffect(const char* name, float, float, bool keep, std::uint32_t endMessage) override {
        effects.push_back(name);
        effectBlocks.push_back(demolished.size());
        if (!keep) return nullptr;
        finaleMessage = endMessage;
        return new Effect(&effectStops, &hardStop);
    }
};

struct ViewHost final : IViewInputHost {
    vic_fakes::Mgr mgr;
    IViewInputView* View3D() override { return nullptr; }
    cIGZWinMgr* WindowManager() override { return &mgr; }
    void PlaySound(int, std::uint32_t) override {}
};

void Tick(cSC4ObliterateCity* tool, std::uint64_t us) {
    cRZMessage2Standard tick;
    tick.SetType(0x86ad10ee);
    tick.SetData2(us);
    tool->DoMessage(&tick);
}

}  // namespace

int main() {
    ViewHost view;
    SetViewInputHost(&view);
    Host host;
    SetObliterateCityHost(&host);

    auto* tool = new cSC4ObliterateCity();
    tool->AddRef();
    CHECK(tool->GetState() == cSC4ObliterateCity::kIdle);
    Tick(tool, 100000);   // a tool whose Init has not run ignores ticks
    CHECK(host.demolished.empty());
    CHECK(tool->Init());
    CHECK(tool->GetState() == cSC4ObliterateCity::kIsolatedRemoval);
    CHECK(tool->BlocksLeft() == 256);                       // 64 x 64 cells in blocks of 4
    CHECK(host.saveDisabled == 1 && host.paused == 1 && host.ticks == 1 && !host.csi);
    CHECK(host.server.subscribed.count(kMessageObliterateEffectEnded) == 1);
    CHECK(host.server.posted.size() == 2 && host.server.posted[0] == std::make_pair(kMessageObliterateRadioFlag, std::uint64_t{1}) &&
          host.server.posted[1].first == kMessageObliterateRadioStop);

    // the first tick removes round(4) blocks; the first block always sparkles, then six empty ones in a row are needed
    Tick(tool, 1);
    CHECK(host.demolished.size() == 4 && tool->BlocksProcessed() == 4);
    CHECK(host.effects.size() == 1 && host.effects[0] == "sparkles_on_demolish");
    Tick(tool, 1000000);   // 1 s: round(5.6) = 6
    CHECK(tool->BlocksProcessed() == 6);
    Tick(tool, 1500000);   // 2.5 s: 8
    CHECK(tool->BlocksProcessed() == 8);
    Tick(tool, 0);         // no change: no work
    CHECK(tool->BlocksProcessed() == 8);
    CHECK(host.effects.size() == 2);   // after six empty blocks the seventh empty one sparkles (blocks 2..7 skipped, 8 sparkles)
    CHECK(host.effectBlocks.size() == 2 && host.effectBlocks[0] == 0 && host.effectBlocks[1] == 7);
    // an occupied block sparkles at once
    const std::size_t before = host.effects.size();
    for (int z = 0; z < 64; z += 4)
        for (int x = 0; x < 64; x += 4) host.occupiedBlocks.insert({x, z});
    Tick(tool, 2500000);   // 5 s: 12
    CHECK(tool->BlocksProcessed() == 12 && host.effects.size() == before + 4);

    // at 10 s the smoke screen forms: the kept finale effect with the end message
    Tick(tool, 5000000);
    CHECK(tool->GetState() == cSC4ObliterateCity::kSmokeForming);
    CHECK(host.effects.back() == "obliterate_finale" && host.finaleMessage == kMessageObliterateEffectEnded);
    CHECK(tool->BlocksProcessed() == 12);   // the deadline tick removes nothing more
    Tick(tool, 9999999);
    CHECK(tool->GetState() == cSC4ObliterateCity::kSmokeForming);
    Tick(tool, 1);         // 10 s of smoke: the finale removes everything, demolishes and dezones the whole city
    CHECK(tool->GetState() == cSC4ObliterateCity::kFinale);
    CHECK(host.removedAll == 1 && host.dezoned == 1 && host.demolished.back() == std::make_pair(0, 0));

    // the finale effect's end message ends the scene: CityObliterationSceneEnded is posted
    cRZMessage2Standard ended;
    ended.SetType(kMessageObliterateEffectEnded);
    tool->DoMessage(&ended);
    CHECK(tool->GetState() == cSC4ObliterateCity::kEnded);
    CHECK(host.server.posted.back().first == kMessageCityObliterationSceneEnded);
    const std::size_t posts = host.server.posted.size();
    Tick(tool, 1000000);   // ended: ticks are ignored, nothing posted twice
    CHECK(host.server.posted.size() == posts);
    CHECK(tool->Shutdown());
    CHECK(host.saveDisabled == 0 && host.paused == 0 && host.ticks == 0 && host.csi);
    CHECK(host.server.subscribed.count(kMessageObliterateEffectEnded) == 0);
    tool->Release();

    // Escape in the smoke phase: the scene stops where it is (no finale, no end message); the finale effect is hard stopped
    // by Shutdown
    host.server.posted.clear();
    host.effectStops = 0;
    auto* second = new cSC4ObliterateCity();
    second->AddRef();
    second->Init();
    Tick(second, 11000000);
    CHECK(second->GetState() == cSC4ObliterateCity::kSmokeForming);
    CHECK(second->OnKeyDown(0x1b, 0));
    CHECK(second->OnKeyDown('A', 0));   // every key is taken
    second->Shutdown();
    CHECK(host.effectStops == 1 && host.hardStop);
    for (const auto& post : host.server.posted) CHECK(post.first != kMessageCityObliterationSceneEnded);
    second->Release();

    SetObliterateCityHost(nullptr);
    SetViewInputHost(nullptr);
    if (failures == 0) std::printf("sc4_obliteratecity: all passed\n");
    return failures == 0 ? 0 : 1;
}
