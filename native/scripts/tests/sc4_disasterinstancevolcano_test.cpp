// cSC4DisasterInstanceVolcano: placing, the eruption effect and its finish message, the lava's start, rates and end, the
// states and phases, and how the instance ends with and without lava, against a fake city, effect and lava.
#include <cstdio>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/DisasterInstanceVolcano.h"
#include "sc4/Occupant.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

class Layer final : public cISC4DisasterLayer {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void EnableDisasters(bool) override {}
    bool DisastersEnabled() const override { return true; }
    cISC4DisasterManager* GetDisasterManager(std::uint32_t id) const override { return id == kDisasterManagerIdVolcano ? manager : nullptr; }
    void DisasterHasBegun(cISC4DisasterInstance*) override { ++begun; }
    void DisasterHasEnded() override { ++ended; }
    bool IsADisasterOngoing() const override { return begun > ended; }
    bool GoToNextDisaster() override { return false; }
    bool GetMostRecentDisasterLocation(cISC4DisasterInstance*, int*, int*) override { return false; }
    bool RegisterDisasterLocation(cISC4DisasterInstance*, int x, int z, cISC4Occupant*, int, bool) override { ++registered; lastCell = x * 1000 + z; return true; }
    bool UnregisterDisasterLocations(cISC4DisasterInstance*) override { ++unregistered; return true; }
    bool UnregisterDisasterLocationByOccupant(cISC4DisasterInstance*, cISC4Occupant*) override { return true; }
    int GetDisasterLocationCount() const override { return registered; }
    bool GetDisasterLocationByIndex(int, cISC4DisasterInstance**, int*, int*) const override { return false; }
    DisasterCost* GetDisasterCost() override { return nullptr; }
    cISC4DisasterManager* manager = nullptr;
    int begun = 0, ended = 0, registered = 0, unregistered = 0, lastCell = -1;
};

class City final : public IDisasterHost {
public:
    cISC4DisasterLayer* DisasterLayer() override { return layer; }
    cISC4OccupantManager* Occupants() override { return nullptr; }
    bool PositionToCell(float x, float z, int& cellX, int& cellZ) override { cellX = static_cast<int>(x / 16.0f); cellZ = static_cast<int>(z / 16.0f); return true; }
    bool CellCenterToPosition(int, int, float&, float&) override { return false; }
    float TerrainHeight(float, float) override { return 0; }
    int CityCellCountX() override { return 64; }
    int CityCellCountZ() override { return 64; }
    cISC4DisasterManager* CreateManager(std::uint32_t) override { return nullptr; }
    Layer* layer = nullptr;
};

struct Lava final : IVolcanoLava {
    void StartLavaFlow(float x, float z, float radius, bool burn) override { ++starts; startX = x; startZ = z; startRadius = radius; startBurn = burn; }
    void SetLavaProductionRate(float rate) override { production.push_back(rate); }
    void SetSolidificationRate(float rate) override { solidification.push_back(rate); }
    void Shutdown() override { ++shutdowns; }
    float GetLavaHeightByCityCell(int x, int z) override { return static_cast<float>(x * 10 + z); }
    void Release() override { ++releases; }
    int starts = 0, shutdowns = 0, releases = 0;
    float startX = 0, startZ = 0, startRadius = 0;
    bool startBurn = false;
    std::vector<float> production, solidification;
};

struct Host final : IVolcanoHost {
    float CitySizeX() override { return 1024.0f; }
    float CitySizeZ() override { return 1024.0f; }
    float Altitude(float, float) override { return 270.0f; }
    void* CreateEffect(const char* name, const float xyz[3], std::uint32_t message, std::uint32_t data) override {
        effectName = name;
        for (int i = 0; i < 3; ++i) effectAt[i] = xyz[i];
        finishMessage = message;
        finishData = data;
        ++effects;
        return effectExists ? this : nullptr;
    }
    void StopEffect(void*) override { ++stopped; }
    void AddTickable(cIGZMessageTarget2*, std::uint32_t priority) override { ++ticking; tickPriority = priority; }
    void RemoveTickable(cIGZMessageTarget2*, std::uint32_t) override { --ticking; }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t message) override { ++listening; listenedTo = message; }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { --listening; }
    const cISCPropertyHolder* TuningExemplar() override { return tuning; }
    IVolcanoLava* CreateLava() override { ++lavasMade; return lava; }
    std::string effectName;
    float effectAt[3] = {0, 0, 0};
    std::uint32_t finishMessage = 0, finishData = 0, tickPriority = 0, listenedTo = 0;
    int effects = 0, stopped = 0, ticking = 0, listening = 0, lavasMade = 0;
    bool effectExists = true;
    const cISCPropertyHolder* tuning = nullptr;
    Lava* lava = nullptr;
};

// The tuning exemplar: lava for 50 s, production 0 -> 1 -> 0 over the time scaled by 6, solidification rising to 2.
class TuningHolder final : public cSC4Occupant {};
TuningHolder* Tuning(bool lava) {
    auto* holder = new TuningHolder();
    holder->AddRef();
    cISCPropertyHolder* properties = holder->AsPropertyHolder();
    cSCPropertyHelp::SetPropertyValue(properties, 0xea4bc6e4, std::vector<float>{0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f});
    cSCPropertyHelp::SetPropertyValue(properties, 0x4a4bc714, std::vector<float>{0.0f, 0.0f, 1.0f, 2.0f});
    cSCPropertyHelp::SetPropertyValue(properties, 0x0a4bc742, 50.0f);
    cSCPropertyHelp::SetPropertyValue(properties, 0xaa4bc72a, 6.0f);
    cSCPropertyHelp::SetPropertyValue(properties, 0x0a4bc752, std::uint32_t{20});
    cSCPropertyHelp::SetPropertyValue(properties, 0x6a4bc8ed, lava);
    return holder;
}

void Tick(cSC4DisasterInstanceVolcano* volcano, std::uint32_t microseconds) {
    cRZMessage2Standard tick;
    tick.AddRef();                                   // the sender's reference (the message lives on the stack here)
    tick.SetType(kAnimationTickMessage);
    tick.SetData2(microseconds);
    volcano->DoMessage(&tick);
}
void Finished(cSC4DisasterInstanceVolcano* volcano, std::uint32_t data) {
    cRZMessage2Standard done;
    done.AddRef();
    done.SetType(kVolcanoEffectFinishedMessage);
    done.SetData1(data);
    volcano->DoMessage(&done);
}
bool Near(float a, float b) { return a - b < 1e-9f && b - a < 1e-9f; }

}  // namespace

int main() {
    City city;
    Layer layer;
    cSC4DisasterManagerVolcano manager;
    manager.AddRef();
    layer.manager = &manager;
    city.layer = &layer;
    SetDisasterHost(&city);
    Host host;
    SetVolcanoHost(&host);

    // ---- without lava: placed, erupting, over when the effect has finished
    {
        TuningHolder* tuning = Tuning(false);
        host.tuning = tuning->AsPropertyHolder();
        auto* volcano = new cSC4DisasterInstanceVolcano();
        volcano->AddRef();                                           // the test's own reference: a volcano that ends early fails a check
        manager.AddInstanceToList(volcano);
        CHECK(volcano->GetPhase() == 0 && volcano->GetState() == 0);
        volcano->SetLocation(2000, -5);                              // outside the city: Init brings it in
        CHECK(layer.registered == 1 && layer.lastCell == 125 * 1000 + 0);
        CHECK(volcano->Init());
        int x = 0, z = 0;
        CHECK(volcano->GetLocation(x, z) && x == 1023 && z == 0);
        CHECK(volcano->GetState() == 1 && volcano->GetPhase() == 2);
        CHECK(host.ticking == 1 && host.tickPriority == kVolcanoTickPriority && host.listening == 1 && host.listenedTo == kVolcanoEffectFinishedMessage);
        CHECK(layer.begun == 1 && layer.registered == 2 && layer.lastCell == 63 * 1000 + 0);
        CHECK(volcano->GetManager() == static_cast<cISC4DisasterManager*>(&manager));
        CHECK(volcano->GetID() == kDisasterManagerIdVolcano);
        void* view = nullptr;
        CHECK(volcano->QueryInterface(kGZIID_cISC4DisasterInstanceVolcano, &view) && view == static_cast<cISC4DisasterInstanceVolcano*>(volcano));
        volcano->Release();
        CHECK(!volcano->QueryInterface(0x12345678, &view));

        CHECK(volcano->SetState(3) && !volcano->SetState(3));        // erupting; the same state again is refused
        CHECK(host.effects == 1 && host.effectName == "volcano_control" && host.finishMessage == kVolcanoEffectFinishedMessage);
        CHECK(host.effectAt[0] == 1023.0f && host.effectAt[1] == 270.0f && host.effectAt[2] == 0.0f);
        CHECK(volcano->GetState() == 3 && volcano->GetPhase() == 1 && host.lavasMade == 0 && !volcano->LavaFlowing());
        CHECK(volcano->GetLavaHeightByCityCell(3, 4) == 0.0f);
        Tick(volcano, 1000000);                                      // no lava: the ticks do nothing
        CHECK(volcano->GetState() == 3);
        Finished(volcano, host.finishData + 1);                      // another volcano's effect
        CHECK(volcano->GetState() == 3 && manager.GetInstances().size() == 1);
        const std::uint32_t mine = host.finishData;
        Finished(volcano, mine);                                     // its own: finished, and with no lava over (the instance is gone)
        CHECK(manager.GetInstances().empty());
        CHECK(host.stopped == 1 && host.ticking == 0 && host.listening == 0);
        CHECK(layer.ended == 1 && layer.unregistered == 1);
        CHECK(volcano->GetState() == 8);
        volcano->Release();
        host.tuning = nullptr;
        tuning->Release();
    }

    // ---- with lava
    {
        TuningHolder* tuning = Tuning(true);
        host.tuning = tuning->AsPropertyHolder();
        Lava lava;
        host.lava = &lava;
        host.effects = host.stopped = 0;
        layer.begun = layer.ended = layer.registered = layer.unregistered = 0;
        auto* volcano = new cSC4DisasterInstanceVolcano();
        volcano->AddRef();                                           // the test's own reference: a volcano that ends early fails a check
        manager.AddInstanceToList(volcano);
        volcano->SetLocation(529, 900);
        volcano->Init();
        CHECK(cSC4DisasterInstanceVolcano::Tuning().lavaShouldFlow && cSC4DisasterInstanceVolcano::Tuning().totalLavaSeconds == 50.0f);
        CHECK(cSC4DisasterInstanceVolcano::Tuning().ventRadius == 20 && cSC4DisasterInstanceVolcano::Tuning().productionScale == 6.0f);
        volcano->SetState(3);                                        // the effect, the lava object, and the lava flows at once
        CHECK(host.effects == 1 && host.lavasMade == 1 && volcano->GetState() == 5 && volcano->GetPhase() == 2 && volcano->LavaFlowing());
        CHECK(lava.starts == 1 && lava.startX == 529.0f && lava.startZ == 900.0f && lava.startRadius == 20.0f && lava.startBurn);
        CHECK(lava.production.size() == 1 && lava.production[0] == 0.0f && lava.solidification.size() == 1);   // made with both rates at 0
        CHECK(volcano->GetLavaHeightByCityCell(3, 4) == 34.0f);
        // the rates follow the curves over the lava time (divided by 100000; the production is scaled)
        Tick(volcano, 12500000);                                     // a quarter of the time: production 0.5 * 6, solidification 0.5
        CHECK(lava.production.size() == 2 && Near(lava.production[1], 3.0f / 100000.0f) && Near(lava.solidification[1], 0.5f / 100000.0f));
        Tick(volcano, 12500000);                                     // half: production 1 * 6, solidification 1
        CHECK(Near(lava.production.back(), 6.0f / 100000.0f) && Near(lava.solidification.back(), 1.0f / 100000.0f));
        // the effect finishes while the lava still flows: the volcano goes on (state 7 then back to 5)
        Finished(volcano, host.finishData);
        CHECK(volcano->EffectFinished() && volcano->GetState() == 5 && manager.GetInstances().size() == 1 && lava.shutdowns == 0);
        Tick(volcano, 12500000);                                     // three quarters: still fed
        CHECK(lava.production.size() == 4 && Near(lava.production.back(), 3.0f / 100000.0f));
        Tick(volcano, 12500000);                                     // exactly the lava time: still inside it
        CHECK(lava.production.size() == 5 && lava.shutdowns == 0);
        Tick(volcano, 1000);                                         // past it: the lava ends, and with the effect finished the volcano is over
        CHECK(lava.shutdowns == 1 && lava.releases == 1);
        CHECK(manager.GetInstances().empty() && host.stopped == 1 && host.ticking == 0 && host.listening == 0);
        CHECK(layer.ended == 1 && layer.unregistered == 1);
        CHECK(volcano->GetState() == 8);
        volcano->Release();
        host.tuning = nullptr;
        tuning->Release();
    }

    // ---- the lava runs out before the effect has finished: the volcano erupts on without it, and ends with the effect
    {
        TuningHolder* tuning = Tuning(true);
        host.tuning = tuning->AsPropertyHolder();
        Lava lava;
        host.lava = &lava;
        host.effects = host.stopped = host.lavasMade = 0;
        layer.begun = layer.ended = layer.registered = layer.unregistered = 0;
        auto* volcano = new cSC4DisasterInstanceVolcano();
        volcano->AddRef();                                           // the test's own reference: a volcano that ends early fails a check
        manager.AddInstanceToList(volcano);
        volcano->SetLocation(100, 100);
        volcano->Init();
        volcano->SetState(3);
        Tick(volcano, 51000000);                                     // the lava time is up
        CHECK(lava.shutdowns == 1 && !volcano->LavaFlowing() && volcano->GetState() == 3);
        CHECK(host.effects == 1 && host.lavasMade == 1);             // no second effect, no second lava
        CHECK(manager.GetInstances().size() == 1 && layer.ended == 1);
        Tick(volcano, 1000000);
        CHECK(lava.production.size() == 1);
        Finished(volcano, host.finishData);
        CHECK(manager.GetInstances().empty() && host.stopped == 1 && layer.ended == 1);
        CHECK(volcano->GetState() == 8);
        volcano->Release();
        host.tuning = nullptr;
        tuning->Release();
    }

    // ---- a host without lava erupts without it even when the tuning asks for lava; shutting down early stops everything
    {
        TuningHolder* tuning = Tuning(true);
        host.tuning = tuning->AsPropertyHolder();
        host.lava = nullptr;
        host.effects = host.stopped = host.lavasMade = 0;
        layer.begun = layer.ended = 0;
        auto* volcano = new cSC4DisasterInstanceVolcano();
        volcano->AddRef();
        manager.AddInstanceToList(volcano);
        volcano->SetLocation(100, 100);
        volcano->Init();
        volcano->SetState(3);
        CHECK(host.lavasMade == 1 && !volcano->LavaFlowing() && volcano->GetState() == 5);
        Tick(volcano, 60000000);
        CHECK(volcano->GetState() == 5 && manager.GetInstances().size() == 1);
        CHECK(volcano->Shutdown() && !volcano->Shutdown());
        CHECK(host.stopped == 1 && host.ticking == 0 && host.listening == 0 && layer.ended == 1 && manager.GetInstances().empty());
        volcano->Release();
        host.tuning = nullptr;
        tuning->Release();
    }

    SetVolcanoHost(nullptr);
    SetDisasterHost(nullptr);
    std::printf(failures ? "%d FAILURES\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
